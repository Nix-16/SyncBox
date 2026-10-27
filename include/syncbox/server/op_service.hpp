#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/core/version_model.hpp>
#include <syncbox/store/server_store.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>

namespace syncbox::server {

struct CommitRequest {
  std::string space_id;
  std::string device_id;
  OpId op_id;
  OpKind kind = OpKind::Create;
  std::optional<FileId> file_id;
  Revision base_revision;
  std::string path;
  std::optional<ContentHash> content_hash;
  std::optional<std::int64_t> size;
  std::int64_t mtime_ms = 0;
  Seq client_seq;
  bool conflict = false;
  std::string request_hash;
};

struct CommitSuccess {
  nlohmann::json response;
  bool replayed = false;
};

struct CommitFailure {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  nlohmann::json details = nlohmann::json::object();
};

using CommitOutcome = std::variant<CommitSuccess, CommitFailure>;

class OpService {
 public:
  explicit OpService(ServerStore& store) : store_(store) {}

  // Implements PROTOCOL.md section 3.7 and DATABASE.md section 2.4.1:
  // CAS, resurrect blocking, path policy, blob check, version/log/entry/
  // idempotency writes in one BEGIN IMMEDIATE transaction.
  [[nodiscard]] CommitOutcome commit(const CommitRequest& request);

 private:
  ServerStore& store_;
};

}  // namespace syncbox::server
