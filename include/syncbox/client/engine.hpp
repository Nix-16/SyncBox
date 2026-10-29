#pragma once

#include <syncbox/core/result.hpp>
#include <syncbox/fs/ignore_rules.hpp>
#include <syncbox/net/http_client.hpp>
#include <syncbox/store/client_store.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace syncbox::client {

struct EngineConfig {
  std::string root;
  std::string server_url;
  std::string device_token;
  std::string space_id;
  std::string device_id;
  std::vector<std::string> ignore_patterns;
  std::int64_t chunk_size = 4 * 1024 * 1024;
};

// Single-shot synchronization engine (M5). Each call to run_once() scans the
// local tree, pushes queued operations, then applies remote changes. Threading
// and continuous watching are layered on top in later milestones.
class SyncEngine {
 public:
  SyncEngine(EngineConfig config, ClientStore& store);

  [[nodiscard]] Result<void> run_once();
  [[nodiscard]] Result<void> recover();

  [[nodiscard]] Result<void> scan_local_changes();
  [[nodiscard]] Result<void> push_pending();
  [[nodiscard]] Result<void> pull_remote();
  [[nodiscard]] Result<void> reconcile();

 private:
  struct StagedContent {
    std::filesystem::path path;
    ContentHash hash;
    std::int64_t size = 0;
  };

  [[nodiscard]] Result<StagedContent> stage_file(const std::filesystem::path& source);
  [[nodiscard]] Result<void> upload_blob(const StagedContent& content);
  [[nodiscard]] Result<void> enqueue(const OpId& op_id, OpKind kind, const FileId& file_id,
                                     Revision base, std::string path,
                                     std::optional<StagedContent> content);
  struct RemoteState {
    Revision revision;
    bool deleted = false;
    std::string path;
    std::optional<ContentHash> hash;
    std::int64_t size = 0;
  };

  [[nodiscard]] Result<StagedContent> content_for_op(const PendingOpRow& op);
  [[nodiscard]] Result<void> handle_conflict(const PendingOpRow& op,
                                             const nlohmann::json& error_body);
  [[nodiscard]] Result<void> handle_create_collision(const PendingOpRow& op,
                                                     const nlohmann::json& error_body);
  [[nodiscard]] Result<void> apply_remote_state(const FileId& file_id,
                                                const RemoteState& state);
  [[nodiscard]] Result<void> apply_change(const nlohmann::json& change, Seq seq);
  [[nodiscard]] Result<std::string> conflict_copy_path(const std::string& path);
  [[nodiscard]] Result<void> copy_staged_to(const std::filesystem::path& staged,
                                            const std::filesystem::path& target);
  void record_conflict(std::string kind, std::string file_id, std::string local_path,
                       std::string remote_path, std::string resolution, std::string detail);
  [[nodiscard]] Result<void> download_blob(const std::string& hash_hex,
                                           const std::filesystem::path& target);
  [[nodiscard]] std::string space_prefix() const;

  EngineConfig config_;
  ClientStore& store_;
  net::HttpClient http_;
  fs::IgnoreRules ignore_;
};

}  // namespace syncbox::client
