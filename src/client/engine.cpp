#include <syncbox/client/engine.hpp>

#include <syncbox/common/time.hpp>
#include <syncbox/core/hash.hpp>
#include <syncbox/core/path.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/fs/atomic_file.hpp>
#include <syncbox/fs/scanner.hpp>

#include <boost/beast/http/verb.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace syncbox::client {

namespace {

namespace http = boost::beast::http;

constexpr std::size_t kCopyBlock = 64 * 1024;

[[nodiscard]] bool pending_blocks_apply(const std::string& state) {
  return state != "done" && state != "dead";
}

[[nodiscard]] Result<nlohmann::json> parse_body(const std::string& body) {
  auto parsed = nlohmann::json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return make_error(ErrorCode::Internal, "server returned invalid JSON");
  }
  return parsed;
}

[[nodiscard]] std::optional<ContentHash> optional_hash(const nlohmann::json& value,
                                                       const char* field) {
  if (!value.contains(field) || value[field].is_null()) {
    return std::nullopt;
  }
  return ContentHash::from_hex(value[field].get<std::string>());
}

[[nodiscard]] Result<std::string> read_chunk(const std::filesystem::path& path,
                                             std::int64_t offset, std::int64_t size) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoError, "cannot open staged file");
  }
  stream.seekg(offset);
  if (!stream) {
    return make_error(ErrorCode::IoError, "cannot seek staged file");
  }
  std::string data(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    stream.read(data.data(), size);
    if (stream.gcount() != size) {
      return make_error(ErrorCode::IoError, "cannot read staged file");
    }
  }
  return data;
}

}  // namespace

SyncEngine::SyncEngine(EngineConfig config, ClientStore& store)
    : config_(std::move(config)), store_(store), http_(config_.server_url),
      ignore_(config_.ignore_patterns) {}

std::string SyncEngine::space_prefix() const {
  return "/v1/spaces/" + config_.space_id;
}

Result<void> SyncEngine::run_once() {
  auto scanned = scan_local_changes();
  if (!scanned) {
    return scanned;
  }
  auto pushed = push_pending();
  if (!pushed) {
    return pushed;
  }
  return pull_remote();
}

Result<SyncEngine::StagedContent> SyncEngine::stage_file(
    const std::filesystem::path& source) {
  auto temp = fs::create_temp_file(config_.root);
  if (!temp) {
    return temp.error();
  }
  const std::filesystem::path staged = temp.value();

  std::ifstream input(source, std::ios::binary);
  if (!input) {
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
    return make_error(ErrorCode::IoError, "cannot open file: " + source.u8string());
  }
  std::ofstream output(staged, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
    return make_error(ErrorCode::IoError, "cannot create staged file");
  }

  Sha256Hasher hasher;
  std::array<char, kCopyBlock> buffer{};
  std::int64_t size = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = input.gcount();
    if (read > 0) {
      auto updated = hasher.update(buffer.data(), static_cast<std::size_t>(read));
      if (!updated) {
        std::error_code ignored;
        std::filesystem::remove(staged, ignored);
        return updated.error();
      }
      output.write(buffer.data(), read);
      size += read;
    }
  }
  output.flush();
  if (!input.eof() || !output) {
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
    return make_error(ErrorCode::IoError, "failed while staging file");
  }

  auto hash = hasher.finalize();
  if (!hash) {
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
    return hash.error();
  }
  return StagedContent{staged, hash.value(), size};
}

Result<void> SyncEngine::enqueue(const OpId& op_id, OpKind kind, const FileId& file_id,
                                 Revision base, std::string path,
                                 std::optional<StagedContent> content) {
  auto cursor = store_.cursor();
  if (!cursor) {
    return cursor.error();
  }

  PendingOpRow op;
  op.op_id = op_id;
  if (kind != OpKind::Create) {
    op.file_id = file_id;
  }
  op.kind = kind;
  op.base_revision = base;
  op.path = std::move(path);
  op.client_seq = cursor.value();
  op.state = "staged";
  op.created_at = now_millis();
  op.updated_at = op.created_at;
  if (content.has_value()) {
    op.content_hash = content->hash;
    op.size = content->size;
    op.staged_path = content->path.u8string();
  }
  return store_.enqueue_pending_op(op);
}

Result<void> SyncEngine::scan_local_changes() {
  auto scan = fs::scan_directory(config_.root, ignore_);
  if (!scan) {
    return scan.error();
  }
  auto locals_result = store_.list_local_entries(false);
  if (!locals_result) {
    return locals_result.error();
  }
  auto pending_result = store_.list_pending_ops();
  if (!pending_result) {
    return pending_result.error();
  }
  auto cursor = store_.cursor();
  if (!cursor) {
    return cursor.error();
  }

  std::vector<LocalEntryRow> locals = std::move(locals_result.value());
  std::set<std::string> active_files;
  for (const auto& op : pending_result.value()) {
    if (op.file_id.has_value() && pending_blocks_apply(op.state)) {
      active_files.insert(op.file_id->value);
    }
  }

  std::map<std::string, std::size_t> local_by_key;
  for (std::size_t i = 0; i < locals.size(); ++i) {
    local_by_key.emplace(locals[i].path_key, i);
  }

  std::set<std::string> matched;
  std::vector<fs::ScannedEntry> unmatched_scan;

  for (const auto& entry : scan.value().entries) {
    auto key_result = make_path_key(entry.path);
    if (!key_result) {
      continue;
    }
    const std::string& key = key_result.value();
    const auto found = local_by_key.find(key);
    if (found == local_by_key.end()) {
      unmatched_scan.push_back(entry);
      continue;
    }

    LocalEntryRow& local = locals[found->second];
    matched.insert(local.file_id.value);

    bool changed = local.size != entry.size || local.mtime_ms != entry.mtime_ms;
    if (local.fs_ino.has_value() && entry.identity.inode != 0 &&
        static_cast<std::uint64_t>(*local.fs_ino) != entry.identity.inode) {
      changed = true;
    }
    if (!changed) {
      continue;
    }
    if (active_files.count(local.file_id.value) != 0) {
      continue;
    }

    const auto absolute = std::filesystem::path(config_.root) / entry.path;
    auto hash = sha256_file(absolute);
    if (!hash) {
      continue;
    }
    if (local.content_hash.has_value() && local.content_hash.value() == hash.value()) {
      local.size = static_cast<std::int64_t>(entry.size);
      local.mtime_ms = entry.mtime_ms;
      if (entry.identity.valid()) {
        local.fs_ino = static_cast<std::int64_t>(entry.identity.inode);
        local.fs_dev = static_cast<std::int64_t>(entry.identity.device);
      }
      local.updated_at = now_millis();
      auto touched = store_.upsert_local_entry(local);
      if (!touched) {
        return touched.error();
      }
      continue;
    }

    auto staged = stage_file(absolute);
    if (!staged) {
      return staged.error();
    }
    auto op_id = OpId{generate_uuid_v7()};
    auto queued =
        enqueue(op_id, OpKind::Update, local.file_id, local.server_revision, entry.path,
                staged.value());
    if (!queued) {
      return queued;
    }

    local.content_hash = staged.value().hash;
    local.size = staged.value().size;
    local.mtime_ms = entry.mtime_ms;
    if (entry.identity.valid()) {
      local.fs_ino = static_cast<std::int64_t>(entry.identity.inode);
      local.fs_dev = static_cast<std::int64_t>(entry.identity.device);
    }
    local.dirty = true;
    local.updated_at = now_millis();
    auto updated = store_.upsert_local_entry(local);
    if (!updated) {
      return updated.error();
    }
  }

  std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> by_identity;
  for (std::size_t i = 0; i < locals.size(); ++i) {
    const auto& local = locals[i];
    if (local.deleted || matched.count(local.file_id.value) != 0 ||
        !local.fs_ino.has_value() || !local.fs_dev.has_value()) {
      continue;
    }
    by_identity.emplace(
        std::make_pair(static_cast<std::uint64_t>(*local.fs_ino),
                       static_cast<std::uint64_t>(*local.fs_dev)),
        i);
  }

  for (const auto& entry : unmatched_scan) {
    bool renamed = false;
    if (entry.identity.valid()) {
      const auto identity = std::make_pair(entry.identity.inode, entry.identity.device);
      const auto found = by_identity.find(identity);
      if (found != by_identity.end()) {
        LocalEntryRow& local = locals[found->second];
        auto renamed_key = make_path_key(entry.path);
        if (!renamed_key) {
          continue;
        }
        if (active_files.count(local.file_id.value) == 0) {
          local.path = entry.path;
          local.path_key = renamed_key.value();
          local.size = static_cast<std::int64_t>(entry.size);
          local.mtime_ms = entry.mtime_ms;
          local.dirty = true;
          local.updated_at = now_millis();
          auto updated = store_.upsert_local_entry(local);
          if (!updated) {
            return updated.error();
          }
          auto queued = enqueue(OpId{generate_uuid_v7()}, OpKind::Rename, local.file_id,
                                local.server_revision, entry.path, std::nullopt);
          if (!queued) {
            return queued;
          }
          matched.insert(local.file_id.value);
          by_identity.erase(found);
          renamed = true;
        }
      }
    }
    if (renamed) {
      continue;
    }

    auto staged = stage_file(std::filesystem::path(config_.root) / entry.path);
    if (!staged) {
      return staged.error();
    }
    auto op_id = OpId{generate_uuid_v7()};
    const FileId temporary{"local:" + op_id.value};

    auto created_key = make_path_key(entry.path);
    if (!created_key) {
      return created_key.error();
    }
    LocalEntryRow row;
    row.file_id = temporary;
    row.path = entry.path;
    row.path_key = created_key.value();
    row.server_revision = Revision::none();
    row.content_hash = staged.value().hash;
    row.size = staged.value().size;
    row.mtime_ms = entry.mtime_ms;
    row.deleted = false;
    row.dirty = true;
    row.fs_ino = entry.identity.inode == 0 ? std::nullopt
                                           : std::optional<std::int64_t>(entry.identity.inode);
    row.fs_dev = entry.identity.device == 0 ? std::nullopt
                                            : std::optional<std::int64_t>(entry.identity.device);
    row.applied_seq = cursor.value();
    row.updated_at = now_millis();
    auto inserted = store_.upsert_local_entry(row);
    if (!inserted) {
      return inserted.error();
    }
    auto queued = enqueue(op_id, OpKind::Create, temporary, Revision::none(), entry.path,
                          staged.value());
    if (!queued) {
      return queued;
    }
    matched.insert(temporary.value);
  }

  for (auto& local : locals) {
    if (local.deleted || matched.count(local.file_id.value) != 0) {
      continue;
    }
    if (active_files.count(local.file_id.value) != 0) {
      continue;
    }
    auto queued = enqueue(OpId{generate_uuid_v7()}, OpKind::Delete, local.file_id,
                          local.server_revision, local.path, std::nullopt);
    if (!queued) {
      return queued;
    }
    local.deleted = true;
    local.dirty = true;
    local.updated_at = now_millis();
    auto updated = store_.upsert_local_entry(local);
    if (!updated) {
      return updated.error();
    }
  }

  return Result<void>{};
}

Result<void> SyncEngine::upload_blob(const StagedContent& content) {
  const std::int64_t chunk_size = std::max<std::int64_t>(1, config_.chunk_size);
  const int chunk_count =
      content.size == 0
          ? 1
          : static_cast<int>(content.size / chunk_size + (content.size % chunk_size ? 1 : 0));

  std::vector<std::string> chunk_hashes;
  for (int index = 0; index < chunk_count; ++index) {
    const std::int64_t offset = static_cast<std::int64_t>(index) * chunk_size;
    const std::int64_t length =
        std::min(chunk_size, std::max<std::int64_t>(0, content.size - offset));
    auto data = read_chunk(content.path, offset, length);
    if (!data) {
      return data.error();
    }
    auto hash = sha256(data.value());
    if (!hash) {
      return hash.error();
    }
    chunk_hashes.push_back(hash.value().to_hex());
  }

  nlohmann::json init{{"content_hash", content.hash.to_hex()},
                      {"size", content.size},
                      {"chunk_size", chunk_size},
                      {"chunk_hashes", chunk_hashes}};
  auto init_response =
      http_.request(http::verb::post, "/v1/blobs", init.dump(), config_.device_token);
  if (!init_response) {
    return init_response.error();
  }
  if (init_response.value().status == 200) {
    return Result<void>{};
  }
  if (init_response.value().status != 201) {
    return make_error(ErrorCode::Unavailable, "blob init failed: " + init_response.value().body);
  }

  auto init_body = parse_body(init_response.value().body);
  if (!init_body) {
    return init_body.error();
  }
  for (const auto& index : init_body.value()["missing_chunks"]) {
    const int chunk_index = index.get<int>();
    const std::int64_t offset = static_cast<std::int64_t>(chunk_index) * chunk_size;
    const std::int64_t length =
        std::min(chunk_size, std::max<std::int64_t>(0, content.size - offset));
    auto data = read_chunk(content.path, offset, length);
    if (!data) {
      return data.error();
    }
    auto uploaded = http_.request(
        http::verb::put,
        "/v1/blobs/" + content.hash.to_hex() + "/chunks/" + std::to_string(chunk_index),
        data.value(), config_.device_token,
        {{"X-Chunk-SHA256", chunk_hashes[static_cast<std::size_t>(chunk_index)]},
         {"Content-Type", "application/octet-stream"}});
    if (!uploaded) {
      return uploaded.error();
    }
    if (uploaded.value().status != 204) {
      return make_error(ErrorCode::Unavailable, "chunk upload failed");
    }
  }

  auto completed = http_.request(http::verb::post,
                                 "/v1/blobs/" + content.hash.to_hex() + "/complete", {},
                                 config_.device_token);
  if (!completed) {
    return completed.error();
  }
  if (completed.value().status != 200) {
    return make_error(ErrorCode::Unavailable, "blob completion failed");
  }
  return Result<void>{};
}

Result<void> SyncEngine::push_pending() {
  auto pending = store_.list_pending_ops();
  if (!pending) {
    return pending.error();
  }

  for (const auto& op : pending.value()) {
    if (op.state != "staged" && op.state != "retry") {
      continue;
    }

    if (op.content_hash.has_value() && op.staged_path.has_value()) {
      StagedContent content;
      content.path = std::filesystem::path(*op.staged_path);
      content.hash = *op.content_hash;
      content.size = op.size.value_or(0);
      auto uploaded = upload_blob(content);
      if (!uploaded) {
        (void)store_.update_pending_state(op.op_id, "retry", uploaded.error().message,
                                         now_millis() + 1000);
        return uploaded;
      }
    }

    nlohmann::json body;
    body["op_id"] = op.op_id.value;
    body["kind"] = std::string(to_string(op.kind));
    body["file_id"] = op.file_id.has_value() ? nlohmann::json(op.file_id->value)
                                             : nlohmann::json(nullptr);
    body["base_revision"] = op.base_revision.value;
    body["path"] = op.path;
    body["client_seq"] = op.client_seq.value;
    body["conflict"] = false;
    if (op.content_hash.has_value()) {
      body["content_hash"] = op.content_hash->to_hex();
      body["size"] = op.size.value_or(0);
      body["mtime_ms"] = 0;
    } else {
      body["content_hash"] = nullptr;
      body["size"] = nullptr;
      body["mtime_ms"] = nullptr;
    }

    auto response = http_.request(http::verb::post, space_prefix() + "/ops", body.dump(),
                                  config_.device_token);
    if (!response) {
      (void)store_.update_pending_state(op.op_id, "retry", response.error().message,
                                       now_millis() + 1000);
      return response.error();
    }

    const unsigned status = response.value().status;
    if (status == 200) {
      auto parsed = parse_body(response.value().body);
      if (!parsed) {
        return parsed.error();
      }
      const auto& result = parsed.value();
      const FileId server_id{result["file_id"].get<std::string>()};
      const Revision revision{result["revision"].get<std::int64_t>()};
      const std::string path = result["path"].get<std::string>();
      const auto hash = optional_hash(result, "content_hash");
      const std::int64_t size = result["size"].get<std::int64_t>();
      const bool deleted = result["deleted"].get<bool>();

      if (op.kind == OpKind::Create) {
        const FileId temporary{"local:" + op.op_id.value};
        auto previous = store_.find_local_entry(temporary);
        LocalEntryRow row;
        row.file_id = server_id;
        row.path = path;
        row.path_key = make_path_key(path).value();
        row.server_revision = revision;
        row.content_hash = hash;
        row.size = size;
        row.deleted = deleted;
        row.dirty = false;
        row.applied_seq = op.client_seq;
        row.updated_at = now_millis();
        if (previous && previous.value().has_value()) {
          row.mtime_ms = previous.value()->mtime_ms;
          row.fs_ino = previous.value()->fs_ino;
          row.fs_dev = previous.value()->fs_dev;
        }
        (void)store_.delete_local_entry(temporary);
        auto stored = store_.upsert_local_entry(row);
        if (!stored) {
          return stored.error();
        }
      } else if (op.file_id.has_value()) {
        auto local = store_.find_local_entry(*op.file_id);
        if (local && local.value().has_value()) {
          LocalEntryRow row = *local.value();
          row.path = path;
          row.path_key = make_path_key(path).value();
          row.server_revision = revision;
          row.content_hash = hash;
          row.size = size;
          row.deleted = deleted;
          row.dirty = false;
          row.updated_at = now_millis();
          auto stored = store_.upsert_local_entry(row);
          if (!stored) {
            return stored.error();
          }
        }
      }

      if (op.staged_path.has_value()) {
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::path(*op.staged_path), ignored);
      }
      auto removed = store_.delete_pending_op(op.op_id);
      if (!removed) {
        return removed.error();
      }
      continue;
    }

    if (status == 409) {
      auto parsed = parse_body(response.value().body);
      if (!parsed) {
        return parsed.error();
      }
      ConflictRow conflict;
      conflict.conflict_id = generate_uuid_v7();
      conflict.file_id = op.file_id.value_or(FileId{"local:" + op.op_id.value});
      conflict.kind = op.kind == OpKind::Delete ? "modify_delete" : "modify_modify";
      conflict.local_path = op.path;
      if (parsed.value().contains("error") && parsed.value()["error"].contains("details") &&
          parsed.value()["error"]["details"].contains("current")) {
        conflict.remote_path =
            parsed.value()["error"]["details"]["current"]["path"].get<std::string>();
      }
      conflict.resolution = "manual_pending";
      conflict.detail = response.value().body;
      conflict.created_at = now_millis();
      (void)store_.insert_conflict(conflict);
      (void)store_.update_pending_state(op.op_id, "conflict", "revision conflict", std::nullopt);
      continue;
    }

    if (status == 401 || status == 403) {
      (void)store_.update_pending_state(op.op_id, "dead", response.value().body, std::nullopt);
      return make_error(status == 401 ? ErrorCode::AuthRequired : ErrorCode::DeviceRevoked,
                        "server rejected the device token");
    }

    (void)store_.update_pending_state(op.op_id, "retry", response.value().body,
                                     now_millis() + 1000);
    return make_error(ErrorCode::Unavailable, "operation failed: " + response.value().body);
  }

  return Result<void>{};
}

Result<void> SyncEngine::download_blob(const std::string& hash_hex,
                                       const std::filesystem::path& target) {
  auto response = http_.request(http::verb::get, "/v1/blobs/" + hash_hex, {},
                                config_.device_token);
  if (!response) {
    return response.error();
  }
  if (response.value().status != 200) {
    return make_error(ErrorCode::Unavailable, "blob download failed: " + response.value().body);
  }

  auto digest = sha256(response.value().body);
  if (!digest) {
    return digest.error();
  }
  if (digest.value().to_hex() != hash_hex) {
    return make_error(ErrorCode::BlobHashMismatch, "downloaded blob hash mismatch");
  }

  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create target directory");
  }

  auto temp = fs::create_temp_file(config_.root);
  if (!temp) {
    return temp.error();
  }
  {
    std::ofstream stream(temp.value(), std::ios::binary | std::ios::trunc);
    if (!stream) {
      return make_error(ErrorCode::IoError, "cannot create download temp file");
    }
    stream.write(response.value().body.data(),
                 static_cast<std::streamsize>(response.value().body.size()));
    stream.flush();
    if (!stream) {
      return make_error(ErrorCode::IoError, "cannot write download temp file");
    }
  }
  auto synced = fs::sync_file(temp.value());
  if (!synced) {
    return synced;
  }
  return fs::atomic_replace(target, temp.value());
}

Result<void> SyncEngine::apply_change(const nlohmann::json& change, Seq seq) {
  const FileId file_id{change["file_id"].get<std::string>()};
  const std::string path = change["path"].get<std::string>();
  const Revision revision{change["revision"].get<std::int64_t>()};
  const bool deleted = change["deleted"].get<bool>();
  const auto hash = optional_hash(change, "content_hash");
  const std::int64_t size = change["size"].get<std::int64_t>();

  auto local = store_.find_local_entry(file_id);
  if (!local) {
    return local.error();
  }

  const auto target = std::filesystem::path(config_.root) / path;
  if (deleted) {
    std::error_code error;
    std::filesystem::remove(target, error);
  } else {
    if (local.value().has_value() && local.value()->path != path) {
      std::error_code error;
      std::filesystem::remove(std::filesystem::path(config_.root) / local.value()->path, error);
    }
    if (!hash.has_value()) {
      return make_error(ErrorCode::Internal, "change is missing content_hash");
    }
    auto downloaded = download_blob(hash->to_hex(), target);
    if (!downloaded) {
      return downloaded;
    }
  }

  LocalEntryRow row;
  if (local.value().has_value()) {
    row = *local.value();
  } else {
    row.file_id = file_id;
  }
  row.path = path;
  row.path_key = make_path_key(path).value();
  row.server_revision = revision;
  row.content_hash = hash;
  row.size = size;
  row.deleted = deleted;
  row.dirty = false;
  row.applied_seq = seq;
  row.updated_at = now_millis();
  if (!deleted) {
    const auto identity = fs::file_identity(target.u8string());
    if (identity.valid()) {
      row.fs_ino = static_cast<std::int64_t>(identity.inode);
      row.fs_dev = static_cast<std::int64_t>(identity.device);
    }
  }
  return store_.upsert_local_entry(row);
}

Result<void> SyncEngine::pull_remote() {
  auto pending = store_.list_pending_ops();
  if (!pending) {
    return pending.error();
  }
  std::set<std::string> blocked;
  for (const auto& op : pending.value()) {
    if (op.file_id.has_value() && pending_blocks_apply(op.state)) {
      blocked.insert(op.file_id->value);
    }
  }

  auto cursor = store_.cursor();
  if (!cursor) {
    return cursor.error();
  }
  Seq position = cursor.value();

  for (int page = 0; page < 50; ++page) {
    auto response = http_.request(
        http::verb::get,
        space_prefix() + "/changes?since=" + std::to_string(position.value) + "&limit=200", {},
        config_.device_token);
    if (!response) {
      return response.error();
    }
    if (response.value().status != 200) {
      return make_error(ErrorCode::Unavailable,
                        "changes request failed: " + response.value().body);
    }
    auto parsed = parse_body(response.value().body);
    if (!parsed) {
      return parsed.error();
    }
    const auto& changes = parsed.value()["changes"];
    if (changes.empty()) {
      return Result<void>{};
    }

    for (const auto& change : changes) {
      const std::string file_id = change["file_id"].get<std::string>();
      if (blocked.count(file_id) != 0) {
        return Result<void>{};
      }
      const Seq seq{change["seq"].get<std::int64_t>()};
      auto applied = apply_change(change, seq);
      if (!applied) {
        return applied;
      }
      auto advanced = store_.set_cursor(seq);
      if (!advanced) {
        return advanced.error();
      }
      position = seq;
    }

    if (!parsed.value()["has_more"].get<bool>()) {
      break;
    }
  }
  return Result<void>{};
}

}  // namespace syncbox::client
