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
#include <ctime>
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

[[nodiscard]] std::int64_t backoff_delay_ms(int attempts) {
  const int shift = std::min(std::max(attempts, 0), 6);
  return std::min<std::int64_t>(60000, static_cast<std::int64_t>(1000) << shift);
}

void schedule_retry(ClientStore& store, const OpId& op_id, int attempts,
                    const std::string& error) {
  (void)store.update_pending_retry(op_id, error, now_millis() + backoff_delay_ms(attempts));
}

[[nodiscard]] Seq current_cursor(ClientStore& store) {
  auto cursor = store.cursor();
  return cursor ? cursor.value() : Seq{0};
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
    : config_(std::move(config)), store_(store),
      http_(config_.server_url, net::TlsOptions{config_.verify_tls, config_.ca_file,
                                                config_.tls_server_name}),
      ignore_(config_.ignore_patterns) {}

std::string SyncEngine::space_prefix() const {
  return "/v1/spaces/" + config_.space_id;
}

Result<void> SyncEngine::recover() {
  auto pending = store_.list_pending_ops();
  if (!pending) {
    return pending.error();
  }
  for (const auto& op : pending.value()) {
    if (op.state == "uploading" || op.state == "committing") {
      (void)store_.update_pending_state(op.op_id, "retry", "recovered after restart",
                                       now_millis());
    }
  }

  auto journal = store_.list_incomplete_apply_journal();
  if (!journal) {
    return journal.error();
  }
  for (const auto& row : journal.value()) {
    std::error_code ignored;
    if (!row.temp_path.empty()) {
      std::filesystem::remove(row.temp_path, ignored);
    }
    (void)store_.update_apply_journal_state(row.id, "failed");
  }
  return Result<void>{};
}

Result<void> SyncEngine::run_once() {
  auto recovered = recover();
  if (!recovered) {
    return recovered;
  }
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

Result<SyncEngine::StagedContent> SyncEngine::content_for_op(const PendingOpRow& op) {
  if (op.staged_path.has_value() && op.content_hash.has_value()) {
    return StagedContent{std::filesystem::path(*op.staged_path), *op.content_hash,
                         op.size.value_or(0)};
  }
  if (!op.file_id.has_value()) {
    return make_error(ErrorCode::Internal, "operation has no content source");
  }
  auto local = store_.find_local_entry(*op.file_id);
  if (!local || !local.value().has_value()) {
    return make_error(ErrorCode::Internal, "local entry is missing");
  }
  return stage_file(std::filesystem::path(config_.root) / local.value()->path);
}

Result<std::string> SyncEngine::conflict_copy_path(const std::string& path) {
  const std::filesystem::path relative(path);
  const std::string filename = relative.filename().u8string();
  const auto dot = filename.find_last_of('.');
  std::string stem = filename;
  std::string extension;
  if (dot != std::string::npos && dot != 0) {
    stem = filename.substr(0, dot);
    extension = filename.substr(dot);
  }

  std::string device = config_.device_id;
  if (device.empty()) {
    device = "device";
  } else if (device.size() > 8) {
    device.resize(8);
  }

  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);

  std::string candidate =
      stem + " (conflict " + device + " " + stamp + ")" + extension;
  int suffix = 2;
  std::filesystem::path result = relative.parent_path() / candidate;
  while (std::filesystem::exists(std::filesystem::path(config_.root) / result)) {
    candidate = stem + " (conflict " + device + " " + stamp + ")-" +
                std::to_string(suffix) + extension;
    result = relative.parent_path() / candidate;
    ++suffix;
  }
  return result.generic_u8string();
}

Result<void> SyncEngine::copy_staged_to(const std::filesystem::path& staged,
                                        const std::filesystem::path& target) {
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create conflict directory");
  }
  std::filesystem::copy_file(staged, target, std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create conflict copy: " + error.message());
  }
  return Result<void>{};
}

void SyncEngine::record_conflict(std::string kind, std::string file_id, std::string local_path,
                                 std::string remote_path, std::string resolution,
                                 std::string detail) {
  ConflictRow conflict;
  conflict.conflict_id = generate_uuid_v7();
  conflict.file_id = FileId{std::move(file_id)};
  conflict.kind = std::move(kind);
  conflict.local_path = std::move(local_path);
  conflict.remote_path = std::move(remote_path);
  conflict.resolution = std::move(resolution);
  conflict.detail = std::move(detail);
  conflict.created_at = now_millis();
  (void)store_.insert_conflict(conflict);
}

Result<void> SyncEngine::apply_remote_state(const FileId& file_id, const RemoteState& state) {
  auto local = store_.find_local_entry(file_id);
  if (!local) {
    return local.error();
  }

  const auto target = std::filesystem::path(config_.root) / state.path;
  if (state.deleted) {
    std::error_code error;
    std::filesystem::remove(target, error);
    if (local.value().has_value() && local.value()->path != state.path) {
      std::filesystem::remove(std::filesystem::path(config_.root) / local.value()->path, error);
    }
  } else {
    if (local.value().has_value() && local.value()->path != state.path) {
      std::error_code error;
      std::filesystem::remove(std::filesystem::path(config_.root) / local.value()->path, error);
    }
    if (!state.hash.has_value()) {
      return make_error(ErrorCode::Internal, "remote entry has no content hash");
    }
    auto downloaded = download_blob(state.hash->to_hex(), target);
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
  auto cursor = store_.cursor();
  row.path = state.path;
  row.path_key = make_path_key(state.path).value();
  row.server_revision = state.revision;
  row.content_hash = state.hash;
  row.size = state.size;
  row.deleted = state.deleted;
  row.dirty = false;
  row.applied_seq = cursor ? cursor.value() : Seq{0};
  row.updated_at = now_millis();
  if (!state.deleted) {
    const auto identity = fs::file_identity(target.u8string());
    if (identity.valid()) {
      row.fs_ino = static_cast<std::int64_t>(identity.inode);
      row.fs_dev = static_cast<std::int64_t>(identity.device);
    }
  }
  return store_.upsert_local_entry(row);
}

Result<void> SyncEngine::handle_conflict(const PendingOpRow& op,
                                         const nlohmann::json& error_body) {
  if (!error_body.contains("error") || !error_body["error"].contains("details") ||
      !error_body["error"]["details"].contains("current")) {
    schedule_retry(store_, op.op_id, op.attempts, "conflict without current state");
    return make_error(ErrorCode::Internal, "conflict response is missing current state");
  }
  const auto& current = error_body["error"]["details"]["current"];
  RemoteState state;
  state.revision = Revision{current["revision"].get<std::int64_t>()};
  state.deleted = current["deleted"].get<bool>();
  state.path = current["path"].get<std::string>();
  state.hash = optional_hash(current, "content_hash");
  state.size = current["size"].get<std::int64_t>();

  const FileId file_id = op.file_id.value_or(FileId{"local:" + op.op_id.value});
  const bool local_delete = op.kind == OpKind::Delete;

  if (state.deleted) {
    if (local_delete) {
      auto removed = store_.delete_pending_op(op.op_id);
      if (!removed) {
        return removed.error();
      }
      return apply_remote_state(file_id, state);
    }

    // P-R: local modification wins; restore the file.
    auto content = content_for_op(op);
    if (!content) {
      return content.error();
    }
    auto local_entry = store_.find_local_entry(file_id);
    const std::string restore_path =
        local_entry && local_entry.value().has_value() ? local_entry.value()->path : state.path;

    PendingOpRow restore;
    restore.op_id = OpId{generate_uuid_v7()};
    restore.file_id = file_id;
    restore.kind = OpKind::Restore;
    restore.base_revision = state.revision;
    restore.path = restore_path;
    restore.content_hash = content.value().hash;
    restore.size = content.value().size;
    restore.staged_path = content.value().path.u8string();
    restore.client_seq = current_cursor(store_);
    restore.state = "staged";
    restore.created_at = now_millis();
    restore.updated_at = restore.created_at;

    (void)store_.delete_pending_op(op.op_id);
    auto queued = store_.enqueue_pending_op(restore);
    if (!queued) {
      return queued;
    }
    record_conflict("modify_delete", file_id.value, op.path, state.path, "restored",
                    error_body.dump());
    return Result<void>{};
  }

  if (local_delete) {
    // P-D: the modification wins over the deletion.
    (void)store_.delete_pending_op(op.op_id);
    auto applied = apply_remote_state(file_id, state);
    if (!applied) {
      return applied;
    }
    record_conflict("modify_delete", file_id.value, op.path, state.path, "remote_wins",
                    error_body.dump());
    return Result<void>{};
  }

  if (op.kind == OpKind::Rename) {
    auto local_entry = store_.find_local_entry(file_id);
    if (!local_entry || !local_entry.value().has_value()) {
      return make_error(ErrorCode::Internal, "local entry is missing during rename conflict");
    }
    LocalEntryRow row = *local_entry.value();

    if (state.hash.has_value() && row.content_hash.has_value() &&
        state.hash.value() == row.content_hash.value()) {
      // P-N: both sides renamed; the remote path wins, content is identical.
      const auto source = std::filesystem::path(config_.root) / row.path;
      const auto destination = std::filesystem::path(config_.root) / state.path;
      std::error_code error;
      std::filesystem::create_directories(destination.parent_path(), error);
      if (source != destination) {
        std::filesystem::rename(source, destination, error);
        if (error) {
          std::filesystem::copy_file(source, destination,
                                     std::filesystem::copy_options::overwrite_existing, error);
          if (error) {
            return make_error(ErrorCode::IoError, "cannot adopt the remote rename");
          }
          std::filesystem::remove(source, error);
        }
      }
      row.path = state.path;
      row.path_key = make_path_key(state.path).value();
      row.server_revision = state.revision;
      row.dirty = false;
      row.updated_at = now_millis();
      (void)store_.delete_pending_op(op.op_id);
      auto stored = store_.upsert_local_entry(row);
      if (!stored) {
        return stored;
      }
      record_conflict("rename_conflict", file_id.value, row.path, state.path,
                      "remote_wins", error_body.dump());
      return Result<void>{};
    }

    // P-M: the remote changed content while we renamed; merge both.
    if (!state.hash.has_value()) {
      return make_error(ErrorCode::Internal, "remote rename conflict has no content hash");
    }
    const auto target = std::filesystem::path(config_.root) / op.path;
    auto downloaded = download_blob(state.hash->to_hex(), target);
    if (!downloaded) {
      return downloaded;
    }
    row.path = op.path;
    row.path_key = make_path_key(op.path).value();
    row.content_hash = state.hash;
    row.size = state.size;
    row.server_revision = state.revision;
    row.dirty = false;
    row.updated_at = now_millis();
    (void)store_.delete_pending_op(op.op_id);
    auto stored = store_.upsert_local_entry(row);
    if (!stored) {
      return stored;
    }
    auto queued = enqueue(OpId{generate_uuid_v7()}, OpKind::Rename, file_id,
                          state.revision, op.path, std::nullopt);
    if (!queued) {
      return queued;
    }
    record_conflict("rename_conflict", file_id.value, op.path, state.path, "remote_wins",
                    error_body.dump());
    return Result<void>{};
  }

  if (op.content_hash.has_value() && state.hash.has_value() &&
      op.content_hash.value() == state.hash.value()) {
    (void)store_.delete_pending_op(op.op_id);
    return apply_remote_state(file_id, state);
  }

  // P-C: keep the local content as a conflict copy, then accept the remote.
  auto content = content_for_op(op);
  if (!content) {
    return content.error();
  }
  auto copy_path = conflict_copy_path(op.path);
  if (!copy_path) {
    return copy_path.error();
  }
  auto copied = copy_staged_to(content.value().path,
                               std::filesystem::path(config_.root) / copy_path.value());
  if (!copied) {
    return copied;
  }

  const OpId copy_op_id{generate_uuid_v7()};
  const FileId temporary{"local:" + copy_op_id.value};
  LocalEntryRow copy_entry;
  copy_entry.file_id = temporary;
  copy_entry.path = copy_path.value();
  copy_entry.path_key = make_path_key(copy_path.value()).value();
  copy_entry.server_revision = Revision::none();
  copy_entry.content_hash = content.value().hash;
  copy_entry.size = content.value().size;
  copy_entry.deleted = false;
  copy_entry.dirty = true;
  copy_entry.applied_seq = current_cursor(store_);
  copy_entry.updated_at = now_millis();
  auto inserted = store_.upsert_local_entry(copy_entry);
  if (!inserted) {
    return inserted.error();
  }

  (void)store_.delete_pending_op(op.op_id);
  auto queued = enqueue(copy_op_id, OpKind::Create, temporary, Revision::none(),
                        copy_path.value(), content.value());
  if (!queued) {
    return queued;
  }

  auto applied = apply_remote_state(file_id, state);
  if (!applied) {
    return applied;
  }
  record_conflict("modify_modify", file_id.value, op.path, state.path, "conflict_copy",
                  error_body.dump());
  return Result<void>{};
}

Result<void> SyncEngine::handle_create_collision(const PendingOpRow& op,
                                                 const nlohmann::json& error_body) {
  if (error_body.contains("error") && error_body["error"].contains("details")) {
    const auto& details = error_body["error"]["details"];
    const nlohmann::json* current_ptr = nullptr;
    if (details.contains("current")) {
      current_ptr = &details["current"];
    } else if (details.contains("entry")) {
      current_ptr = &details["entry"];
    }
    if (current_ptr == nullptr) {
      // fall through to the conflict-copy path
    } else {
    const auto& current = *current_ptr;
    const auto remote_hash = optional_hash(current, "content_hash");
    if (op.content_hash.has_value() && remote_hash.has_value() &&
        op.content_hash.value() == remote_hash.value() &&
        op.size.value_or(0) == current["size"].get<std::int64_t>()) {
      const FileId remote_id{current["file_id"].get<std::string>()};
      const std::string remote_path = current["path"].get<std::string>();
      const FileId temporary{"local:" + op.op_id.value};
      auto previous = store_.find_local_entry(temporary);

      const auto source = std::filesystem::path(config_.root) / op.path;
      const auto destination = std::filesystem::path(config_.root) / remote_path;
      if (source != destination) {
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        std::filesystem::rename(source, destination, error);
        if (error) {
          std::filesystem::copy_file(source, destination,
                                     std::filesystem::copy_options::overwrite_existing, error);
          if (error) {
            return make_error(ErrorCode::IoError, "cannot adopt the remote create");
          }
          std::filesystem::remove(source, error);
        }
      }

      LocalEntryRow row;
      row.file_id = remote_id;
      row.path = remote_path;
      row.path_key = make_path_key(remote_path).value();
      row.server_revision = Revision{current["revision"].get<std::int64_t>()};
      row.content_hash = remote_hash;
      row.size = current["size"].get<std::int64_t>();
      row.deleted = false;
      row.dirty = false;
      row.applied_seq = current_cursor(store_);
      row.updated_at = now_millis();
      if (previous && previous.value().has_value()) {
        row.mtime_ms = previous.value()->mtime_ms;
        row.fs_ino = previous.value()->fs_ino;
        row.fs_dev = previous.value()->fs_dev;
      }
      (void)store_.delete_local_entry(temporary);
      auto stored = store_.upsert_local_entry(row);
      if (!stored) {
        return stored;
      }
      (void)store_.delete_pending_op(op.op_id);
      return Result<void>{};
    }
    }
  }

  auto content = content_for_op(op);
  if (!content) {
    return content.error();
  }
  auto copy_path = conflict_copy_path(op.path);
  if (!copy_path) {
    return copy_path.error();
  }

  const auto source = std::filesystem::path(config_.root) / op.path;
  const auto target = std::filesystem::path(config_.root) / copy_path.value();
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  std::filesystem::rename(source, target, error);
  if (error) {
    auto copied = copy_staged_to(content.value().path, target);
    if (!copied) {
      return copied;
    }
    std::filesystem::remove(source, error);
  }

  const FileId old_temporary{"local:" + op.op_id.value};
  const OpId new_op_id{generate_uuid_v7()};
  const FileId new_temporary{"local:" + new_op_id.value};
  (void)store_.update_local_entry_file_id(old_temporary, new_temporary);
  auto local = store_.find_local_entry(new_temporary);
  if (local && local.value().has_value()) {
    LocalEntryRow row = *local.value();
    row.path = copy_path.value();
    row.path_key = make_path_key(copy_path.value()).value();
    row.updated_at = now_millis();
    (void)store_.upsert_local_entry(row);
  }

  (void)store_.delete_pending_op(op.op_id);
  auto queued = enqueue(new_op_id, OpKind::Create, new_temporary, Revision::none(),
                        copy_path.value(), content.value());
  if (!queued) {
    return queued;
  }
  record_conflict("path_collision", old_temporary.value, op.path, op.path, "conflict_copy",
                  error_body.dump());
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
    if (op.state == "retry" && op.next_attempt_at.has_value() &&
        op.next_attempt_at.value() > now_millis()) {
      continue;
    }

    if (op.content_hash.has_value() && op.staged_path.has_value()) {
      StagedContent content;
      content.path = std::filesystem::path(*op.staged_path);
      content.hash = *op.content_hash;
      content.size = op.size.value_or(0);
      auto uploaded = upload_blob(content);
      if (!uploaded) {
        schedule_retry(store_, op.op_id, op.attempts, uploaded.error().message);
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
      schedule_retry(store_, op.op_id, op.attempts, response.error().message);
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
      const std::string code =
          parsed.value().contains("error") && parsed.value()["error"].contains("code")
              ? parsed.value()["error"]["code"].get<std::string>()
              : std::string{};
      if (code == "REVISION_CONFLICT") {
        auto resolved = handle_conflict(op, parsed.value());
        if (!resolved) {
          return resolved;
        }
        continue;
      }
      if (code == "PATH_CASE_COLLISION" && op.kind == OpKind::Create) {
        auto resolved = handle_create_collision(op, parsed.value());
        if (!resolved) {
          return resolved;
        }
        continue;
      }
      if (code == "RESURRECT_BLOCKED") {
        record_conflict("path_collision",
                        op.file_id.value_or(FileId{"local:" + op.op_id.value}).value, op.path,
                        op.path, "manual_pending", response.value().body);
        (void)store_.update_pending_state(op.op_id, "conflict", "resurrect blocked",
                                         std::nullopt);
        continue;
      }
      schedule_retry(store_, op.op_id, op.attempts, response.value().body);
      return make_error(ErrorCode::Unavailable, "operation failed: " + response.value().body);
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
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    return make_error(ErrorCode::IoError, "cannot create target directory");
  }

  auto temp = fs::create_temp_file(config_.root);
  if (!temp) {
    return temp.error();
  }

  auto status = http_.download_to_file("/v1/blobs/" + hash_hex, config_.device_token,
                                       temp.value());
  if (!status) {
    std::filesystem::remove(temp.value(), error);
    return status.error();
  }
  if (status.value() != 200) {
    std::filesystem::remove(temp.value(), error);
    return make_error(ErrorCode::Unavailable, "blob download failed");
  }

  auto digest = sha256_file(temp.value());
  if (!digest) {
    std::filesystem::remove(temp.value(), error);
    return digest.error();
  }
  if (digest.value().to_hex() != hash_hex) {
    std::filesystem::remove(temp.value(), error);
    return make_error(ErrorCode::BlobHashMismatch, "downloaded blob hash mismatch");
  }

  auto synced = fs::sync_file(temp.value());
  if (!synced) {
    std::filesystem::remove(temp.value(), error);
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

Result<void> SyncEngine::reconcile() {
  std::string page_token;
  Seq snapshot_seq = current_cursor(store_);

  do {
    std::string target = space_prefix() + "/snapshot?limit=1000";
    if (!page_token.empty()) {
      target += "&page_token=" + page_token;
    }
    auto response = http_.request(http::verb::get, target, {}, config_.device_token);
    if (!response) {
      return response.error();
    }
    if (response.value().status != 200) {
      return make_error(ErrorCode::Unavailable,
                        "snapshot request failed: " + response.value().body);
    }
    auto parsed = parse_body(response.value().body);
    if (!parsed) {
      return parsed.error();
    }
    snapshot_seq = Seq{parsed.value()["snapshot_seq"].get<std::int64_t>()};

    for (const auto& entry : parsed.value()["entries"]) {
      const FileId file_id{entry["file_id"].get<std::string>()};
      auto local = store_.find_local_entry(file_id);
      if (!local) {
        return local.error();
      }
      const bool deleted = entry["deleted"].get<bool>();
      if (!local.value().has_value() && deleted) {
        continue;
      }

      RemoteState state;
      state.revision = Revision{entry["revision"].get<std::int64_t>()};
      state.deleted = deleted;
      state.path = entry["path"].get<std::string>();
      state.hash = optional_hash(entry, "content_hash");
      state.size = entry["size"].get<std::int64_t>();

      auto applied = apply_remote_state(file_id, state);
      if (!applied) {
        return applied;
      }
    }

    if (!parsed.value().contains("next_page_token") ||
        parsed.value()["next_page_token"].is_null()) {
      page_token.clear();
    } else {
      page_token = parsed.value()["next_page_token"].get<std::string>();
    }
  } while (!page_token.empty());

  return store_.set_cursor(snapshot_seq);
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
    if (response.value().status == 410) {
      return reconcile();
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
