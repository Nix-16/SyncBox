#include <syncbox/server/op_service.hpp>

#include <syncbox/common/time.hpp>
#include <syncbox/core/path.hpp>
#include <syncbox/core/uuid.hpp>

#include <string>
#include <utility>

namespace syncbox::server {

namespace {

[[nodiscard]] nlohmann::json entry_json(const EntryRow& entry) {
  nlohmann::json value;
  value["file_id"] = entry.file_id.value;
  value["revision"] = entry.revision.value;
  value["path"] = entry.path;
  value["deleted"] = entry.deleted;
  value["content_hash"] = entry.content_hash.has_value()
                              ? nlohmann::json(entry.content_hash->to_hex())
                              : nlohmann::json(nullptr);
  value["size"] = entry.size;
  value["editor_device"] = entry.editor_device;
  return value;
}

[[nodiscard]] CommitFailure fail(ErrorCode code, std::string message,
                                 nlohmann::json details = nlohmann::json::object()) {
  CommitFailure failure;
  failure.code = code;
  failure.message = std::move(message);
  failure.details = std::move(details);
  return failure;
}

[[nodiscard]] CommitFailure fail_from(const Error& error) {
  return fail(error.code, error.message);
}

[[nodiscard]] bool requires_content(OpKind kind) noexcept {
  return kind == OpKind::Create || kind == OpKind::Update || kind == OpKind::Restore;
}

}  // namespace

CommitOutcome OpService::commit(const CommitRequest& request) {
  auto normalized = normalize_relative_path(request.path);
  if (!normalized) {
    return fail_from(normalized.error());
  }
  const std::string& path = normalized.value();

  auto path_key_result = make_path_key(path);
  if (!path_key_result) {
    return fail_from(path_key_result.error());
  }
  const std::string& path_key = path_key_result.value();

  if (requires_content(request.kind) && !request.content_hash.has_value()) {
    return fail(ErrorCode::InvalidOperation, "content_hash is required for this operation");
  }
  if (!requires_content(request.kind) && request.content_hash.has_value()) {
    return fail(ErrorCode::InvalidOperation, "content_hash is not allowed for this operation");
  }
  if (request.kind == OpKind::Create && request.file_id.has_value()) {
    return fail(ErrorCode::InvalidOperation, "file_id must be null for create");
  }
  if (request.kind != OpKind::Create && !request.file_id.has_value()) {
    return fail(ErrorCode::InvalidOperation, "file_id is required");
  }
  if (request.kind == OpKind::Create && request.base_revision.value != 0) {
    return fail(ErrorCode::InvalidOperation, "base_revision must be 0 for create");
  }

  auto transaction = Transaction::begin(store_.db());
  if (!transaction) {
    return fail_from(transaction.error());
  }

  // --- idempotency -------------------------------------------------------
  auto existing = store_.find_operation(request.op_id);
  if (!existing) {
    return fail_from(existing.error());
  }
  if (existing.value().has_value()) {
    const OperationRecord& operation = *existing.value();
    if (operation.request_hash != request.request_hash) {
      return fail(ErrorCode::IdempotencyMismatch,
                  "op_id was already used with a different request body");
    }
    auto body = nlohmann::json::parse(operation.response_body, nullptr, false);
    if (body.is_discarded()) {
      return fail(ErrorCode::Internal, "stored idempotent response is not valid JSON");
    }
    return CommitSuccess{std::move(body), true};
  }

  // --- load current entry ------------------------------------------------
  std::optional<EntryRow> current;
  if (request.kind != OpKind::Create) {
    auto found = store_.find_entry(request.space_id, *request.file_id);
    if (!found) {
      return fail_from(found.error());
    }
    if (!found.value().has_value()) {
      return fail(ErrorCode::FileNotFound, "file_id does not exist");
    }
    current = *found.value();

    if (current->revision.value != request.base_revision.value) {
      nlohmann::json details;
      details["current"] = entry_json(*current);
      return fail(ErrorCode::RevisionConflict,
                  "base_revision does not match current revision", std::move(details));
    }
    if (request.kind == OpKind::Delete && current->deleted) {
      return fail(ErrorCode::RevisionConflict, "file is already deleted",
                  nlohmann::json{{"current", entry_json(*current)}});
    }
    if ((request.kind == OpKind::Update || request.kind == OpKind::Delete) &&
        current->path != path) {
      return fail(ErrorCode::InvalidOperation, "path does not match the current entry path");
    }
    if (request.kind == OpKind::Restore && !current->deleted) {
      return fail(ErrorCode::InvalidOperation, "restore requires a deleted entry");
    }
  }

  // --- resurrect blocking (create) ---------------------------------------
  if (request.kind == OpKind::Create) {
    auto tombstone = store_.find_tombstone_by_path_key(request.space_id, path_key);
    if (!tombstone) {
      return fail_from(tombstone.error());
    }
    if (tombstone.value().has_value() &&
        tombstone.value()->updated_seq.value > request.client_seq.value) {
      nlohmann::json details;
      details["tombstone"] = entry_json(*tombstone.value());
      details["min_client_seq"] = tombstone.value()->updated_seq.value;
      return fail(ErrorCode::ResurrectBlocked,
                  "path was deleted after the client's last seen sequence",
                  std::move(details));
    }
  }

  // --- live path collision ----------------------------------------------
  {
    auto live = store_.find_live_entry_by_path_key(request.space_id, path_key);
    if (!live) {
      return fail_from(live.error());
    }
    if (live.value().has_value()) {
      const EntryRow& other = *live.value();
      const bool same_file = current.has_value() && other.file_id == current->file_id;
      if (!same_file) {
        return fail(ErrorCode::PathCaseCollision, "path is already in use",
                    nlohmann::json{{"entry", entry_json(other)}});
      }
    }
  }

  // --- blob validation ---------------------------------------------------
  if (request.content_hash.has_value()) {
    auto blob = store_.find_blob(request.content_hash->to_hex());
    if (!blob) {
      return fail_from(blob.error());
    }
    if (!blob.value().has_value() || blob.value()->state != "complete") {
      return fail(ErrorCode::BlobMissing, "content blob is not complete",
                  nlohmann::json{{"content_hash", request.content_hash->to_hex()}});
    }
    if (request.size.has_value() && request.size.value() != blob.value()->size) {
      return fail(ErrorCode::InvalidOperation, "size does not match the stored blob");
    }
  }

  // --- compute target state ---------------------------------------------
  const std::int64_t now = now_millis();
  const FileId target_file =
      request.kind == OpKind::Create ? FileId{generate_uuid_v7()} : *request.file_id;
  const Revision new_revision =
      request.kind == OpKind::Create ? Revision{1} : current->revision.next();

  std::optional<ContentHash> content_hash;
  std::int64_t size = 0;
  std::int64_t mtime_ms = request.mtime_ms;
  if (request.kind == OpKind::Delete || request.kind == OpKind::Rename) {
    content_hash = current->content_hash;
    size = current->size;
    mtime_ms = current->mtime_ms;
  } else {
    content_hash = request.content_hash;
    size = request.size.value_or(0);
  }
  const bool deleted = request.kind == OpKind::Delete;

  ChangeRecord change;
  change.space_id = request.space_id;
  change.file_id = target_file;
  change.revision = new_revision;
  change.op = request.kind;
  change.path = path;
  change.content_hash = content_hash;
  change.size = size;
  change.deleted = deleted;
  change.conflict = request.conflict;
  change.device_id = request.device_id;
  change.created_at = now;

  auto seq = store_.append_change(change);
  if (!seq) {
    return fail_from(seq.error());
  }

  EntryRow entry;
  entry.file_id = target_file;
  entry.path = path;
  entry.path_key = path_key;
  entry.revision = new_revision;
  entry.content_hash = content_hash;
  entry.size = size;
  entry.mtime_ms = mtime_ms;
  entry.deleted = deleted;
  entry.editor_device = request.device_id;
  entry.updated_seq = seq.value();
  entry.updated_at = now;

  auto upserted = store_.upsert_entry(request.space_id, entry);
  if (!upserted) {
    return fail_from(upserted.error());
  }

  VersionRecord version;
  version.space_id = request.space_id;
  version.file_id = target_file;
  version.revision = new_revision;
  version.parent_revision = request.base_revision;
  version.op = request.kind;
  version.path = path;
  version.content_hash = content_hash;
  version.size = size;
  version.mtime_ms = mtime_ms;
  version.device_id = request.device_id;
  version.op_id = request.op_id;
  version.created_at = now;

  auto version_inserted = store_.insert_version(version);
  if (!version_inserted) {
    return fail_from(version_inserted.error());
  }

  if (request.kind == OpKind::Create || request.kind == OpKind::Update ||
      request.kind == OpKind::Restore) {
    if (content_hash.has_value()) {
      auto ref = store_.increment_blob_ref(content_hash->to_hex());
      if (!ref) {
        return fail_from(ref.error());
      }
    }
  }

  nlohmann::json response;
  response["op_id"] = request.op_id.value;
  response["file_id"] = target_file.value;
  response["revision"] = new_revision.value;
  response["parent_revision"] = request.base_revision.value;
  response["path"] = path;
  response["content_hash"] =
      content_hash.has_value() ? nlohmann::json(content_hash->to_hex()) : nlohmann::json(nullptr);
  response["size"] = size;
  response["deleted"] = deleted;
  response["conflict"] = request.conflict;
  response["seq"] = seq.value().value;
  response["applied_at"] = now;

  OperationRecord operation;
  operation.op_id = request.op_id;
  operation.space_id = request.space_id;
  operation.device_id = request.device_id;
  operation.request_hash = request.request_hash;
  operation.response_code = 200;
  operation.response_body = response.dump();
  operation.seq = seq.value();
  operation.created_at = now;

  auto operation_inserted = store_.insert_operation(operation);
  if (!operation_inserted) {
    return fail_from(operation_inserted.error());
  }

  auto committed = transaction.value().commit();
  if (!committed) {
    return fail_from(committed.error());
  }

  return CommitSuccess{std::move(response), false};
}

}  // namespace syncbox::server
