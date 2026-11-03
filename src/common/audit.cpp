#include <syncbox/common/audit.hpp>

#include <syncbox/core/token.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>

namespace syncbox::audit {

namespace {

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;

[[nodiscard]] std::string escape_json(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += '?';
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

[[nodiscard]] std::shared_ptr<spdlog::logger> logger() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_logger;
}

}  // namespace

void configure(std::string_view log_file) {
  std::shared_ptr<spdlog::logger> created;
  if (log_file.empty()) {
    created = std::make_shared<spdlog::logger>(
        "syncbox.audit",
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
  } else {
    created = std::make_shared<spdlog::logger>(
        "syncbox.audit",
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(std::string(log_file), true));
  }
  created->set_pattern("%v");
  created->set_level(spdlog::level::info);
  created->flush_on(spdlog::level::info);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_logger) {
    g_logger->flush();
  }
  g_logger = std::move(created);
}

void reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_logger) {
    g_logger->flush();
  }
  g_logger.reset();
}

void flush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_logger) {
    g_logger->flush();
  }
}

void event(std::string_view name,
           std::vector<std::pair<std::string, std::string>> fields) {
  std::string line = "{\"event\":\"" + escape_json(name) + "\"";
  for (const auto& [key, value] : fields) {
    line += ",\"" + escape_json(key) + "\":\"" + escape_json(value) + "\"";
  }
  line += "}";
  if (auto sink = logger()) {
    sink->info(line);
  }
}

void auth_failure_for_token(std::string_view token, std::string_view reason) {
  std::string prefix = "unknown";
  auto hash = hash_token(token);
  if (hash) {
    prefix = hash.value().to_hex().substr(0, 8);
  }
  event("auth_failure", {{"token_hash_prefix", prefix}, {"reason", std::string(reason)}});
}

void device_revoked(std::string_view device_id, std::string_view actor,
                    std::string_view reason) {
  event("device_revoked", {{"device_id", std::string(device_id)},
                           {"actor", std::string(actor)},
                           {"reason", std::string(reason)}});
}

}  // namespace syncbox::audit
