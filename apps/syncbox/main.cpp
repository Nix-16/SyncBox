#include <syncbox/client/daemon.hpp>
#include <syncbox/client/engine.hpp>
#include <syncbox/common/time.hpp>
#include <syncbox/core/version.hpp>
#include <syncbox/net/http_client.hpp>
#include <syncbox/store/client_store.hpp>
#include <syncbox/store/migrations.hpp>

#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace http = boost::beast::http;

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
  g_stop = 1;
}

struct Config {
  std::string server;
  std::string token;
  std::string space_id;
  std::string root;
  std::string ca_file;
  bool verify_tls = true;
  std::string tls_server_name;
};

struct Arguments {
  std::string command;
  std::map<std::string, std::string> options;
};

[[nodiscard]] std::filesystem::path default_config_path() {
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home == nullptr) {
    return std::filesystem::path("syncbox.json");
  }
  return std::filesystem::path(home) / ".config" / "syncbox" / "config.json";
}

[[nodiscard]] Arguments parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--", 0) == 0) {
      if (i + 1 >= argc) {
        arguments.options[argument] = "";
        continue;
      }
      arguments.options[argument] = argv[++i];
    } else if (arguments.command.empty()) {
      arguments.command = argument;
    }
  }
  return arguments;
}

[[nodiscard]] std::string option(const Arguments& arguments, const std::string& name,
                                 const std::string& fallback = {}) {
  const auto found = arguments.options.find(name);
  return found == arguments.options.end() ? fallback : found->second;
}

[[nodiscard]] std::optional<Config> load_config(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    return std::nullopt;
  }
  auto parsed = nlohmann::json::parse(stream, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  Config config;
  config.server = parsed.value("server", std::string{});
  config.token = parsed.value("token", std::string{});
  config.space_id = parsed.value("space_id", std::string{});
  config.root = parsed.value("root", std::string{});
  config.ca_file = parsed.value("ca_file", std::string{});
  config.verify_tls = parsed.value("verify_tls", true);
  config.tls_server_name = parsed.value("tls_server_name", std::string{});
  return config;
}

[[nodiscard]] bool save_config(const std::filesystem::path& path, const Config& config) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    return false;
  }
  nlohmann::json body{{"server", config.server},
                      {"token", config.token},
                      {"space_id", config.space_id},
                      {"root", config.root},
                      {"ca_file", config.ca_file},
                      {"verify_tls", config.verify_tls},
                      {"tls_server_name", config.tls_server_name}};
  stream << body.dump(2) << '\n';
  stream.close();
#ifdef _WIN32
  // Windows ACLs are managed by the parent directory.
#else
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
#endif
  return true;
}

[[nodiscard]] int fail(const std::string& message) {
  std::cerr << "syncbox: " << message << '\n';
  return 1;
}

[[nodiscard]] int usage() {
  std::cout
      << "Usage:\n"
      << "  syncbox login --server URL --token TOKEN [--root DIR] [--config PATH]\n"
      << "                [--ca-file FILE] [--insecure] [--tls-server-name NAME]\n"
      << "  syncbox sync [--config PATH] [--server URL] [--token TOKEN] [--root DIR]\n"
      << "                [--space ID] [--db PATH]\n"
      << "  syncbox watch [--config PATH] [--poll SECONDS]\n"
      << "  syncbox status [--config PATH] [--db PATH]\n"
      << "  syncbox conflicts [--config PATH] [--db PATH]\n"
      << "  syncbox --version\n";
  return 0;
}

[[nodiscard]] std::filesystem::path database_path(const std::string& root,
                                                  const std::string& explicit_db) {
  if (!explicit_db.empty()) {
    return explicit_db;
  }
  return std::filesystem::path(root) / ".syncbox" / "client.db";
}

[[nodiscard]] syncbox::net::TlsOptions tls_options_for(const Config& config) {
  syncbox::net::TlsOptions options;
  options.verify = config.verify_tls;
  options.ca_file = config.ca_file;
  options.server_name = config.tls_server_name;
  return options;
}

[[nodiscard]] std::optional<std::string> fetch_space_id(const std::string& server,
                                                        const std::string& token,
                                                        const Config& config) {
  syncbox::net::HttpClient client(server, tls_options_for(config));
  auto response = client.request(http::verb::get, "/v1/me", {}, token);
  if (!response || response.value().status != 200) {
    return std::nullopt;
  }
  auto body = nlohmann::json::parse(response.value().body, nullptr, false);
  if (body.is_discarded() || !body.contains("space_id")) {
    return std::nullopt;
  }
  return body["space_id"].get<std::string>();
}

int run_login(const Arguments& arguments) {
  Config config;
  config.server = option(arguments, "--server");
  config.token = option(arguments, "--token");
  config.root = option(arguments, "--root", ".");
  config.ca_file = option(arguments, "--ca-file");
  config.verify_tls = arguments.options.count("--insecure") == 0;
  config.tls_server_name = option(arguments, "--tls-server-name");
  if (config.server.empty() || config.token.empty()) {
    return fail("login requires --server and --token");
  }
  auto space = fetch_space_id(config.server, config.token, config);
  if (!space.has_value()) {
    return fail("could not authenticate with the server");
  }
  config.space_id = *space;

  const std::filesystem::path config_path =
      option(arguments, "--config", default_config_path().string());
  if (!save_config(config_path, config)) {
    return fail("cannot write config: " + config_path.string());
  }
  std::cout << "space " << config.space_id << " configured at " << config_path.string()
            << '\n';
  return 0;
}

[[nodiscard]] std::optional<Config> resolve_config(const Arguments& arguments) {
  Config config;
  const std::filesystem::path config_path =
      option(arguments, "--config", default_config_path().string());
  if (auto loaded = load_config(config_path); loaded.has_value()) {
    config = *loaded;
  }
  if (arguments.options.count("--server") != 0) {
    config.server = option(arguments, "--server");
  }
  if (arguments.options.count("--token") != 0) {
    config.token = option(arguments, "--token");
  }
  if (arguments.options.count("--root") != 0) {
    config.root = option(arguments, "--root");
  }
  if (arguments.options.count("--space") != 0) {
    config.space_id = option(arguments, "--space");
  }
  if (arguments.options.count("--ca-file") != 0) {
    config.ca_file = option(arguments, "--ca-file");
  }
  if (arguments.options.count("--insecure") != 0) {
    config.verify_tls = false;
  }
  if (arguments.options.count("--tls-server-name") != 0) {
    config.tls_server_name = option(arguments, "--tls-server-name");
  }
  if (config.server.empty() || config.token.empty() || config.root.empty()) {
    return std::nullopt;
  }
  if (config.space_id.empty()) {
    auto space = fetch_space_id(config.server, config.token, config);
    if (!space.has_value()) {
      return std::nullopt;
    }
    config.space_id = *space;
  }
  return config;
}

int run_sync(const Arguments& arguments) {
  auto config = resolve_config(arguments);
  if (!config.has_value()) {
    return fail("sync requires server, token, and root (see `syncbox login`)");
  }

  std::error_code error;
  std::filesystem::create_directories(config->root, error);
  if (error) {
    return fail("cannot create sync root");
  }
  const auto db_path =
      database_path(config->root, option(arguments, "--db"));
  std::filesystem::create_directories(db_path.parent_path(), error);

  auto store = syncbox::ClientStore::open(db_path.string());
  if (!store) {
    return fail(store.error().message);
  }
  auto migrated = store.value().migrate();
  if (!migrated) {
    return fail(migrated.error().message);
  }

  syncbox::client::EngineConfig engine_config;
  engine_config.root = config->root;
  engine_config.server_url = config->server;
  engine_config.device_token = config->token;
  engine_config.space_id = config->space_id;
  engine_config.verify_tls = config->verify_tls;
  engine_config.ca_file = config->ca_file;
  engine_config.tls_server_name = config->tls_server_name;

  syncbox::client::SyncEngine engine(engine_config, store.value());
  auto result = engine.run_once();
  if (!result) {
    return fail(std::string(syncbox::to_string(result.error().code)) + ": " +
                result.error().message);
  }
  std::cout << "sync complete\n";
  return 0;
}

int run_watch(const Arguments& arguments) {
  auto config = resolve_config(arguments);
  if (!config.has_value()) {
    return fail("watch requires server, token, and root (see `syncbox login`)");
  }

  std::error_code error;
  std::filesystem::create_directories(config->root, error);
  const auto db_path = database_path(config->root, option(arguments, "--db"));
  std::filesystem::create_directories(db_path.parent_path(), error);

  auto store = syncbox::ClientStore::open(db_path.string());
  if (!store) {
    return fail(store.error().message);
  }
  auto migrated = store.value().migrate();
  if (!migrated) {
    return fail(migrated.error().message);
  }

  int poll_seconds = 30;
  if (const auto value = option(arguments, "--poll"); !value.empty()) {
    try {
      poll_seconds = std::stoi(value);
    } catch (const std::exception&) {
      return fail("invalid --poll value");
    }
  }

  syncbox::client::EngineConfig engine_config;
  engine_config.root = config->root;
  engine_config.server_url = config->server;
  engine_config.device_token = config->token;
  engine_config.space_id = config->space_id;
  engine_config.verify_tls = config->verify_tls;
  engine_config.ca_file = config->ca_file;
  engine_config.tls_server_name = config->tls_server_name;

  syncbox::client::SyncDaemon daemon(engine_config, store.value(),
                                     std::chrono::seconds(poll_seconds));
  auto started = daemon.start();
  if (!started) {
    return fail(started.error().message);
  }
  std::cout << "watching " << config->root << " (poll " << poll_seconds << "s)\n";

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  daemon.stop();
  return 0;
}

int run_status(const Arguments& arguments) {
  auto config = resolve_config(arguments);
  std::string root = config.has_value() ? config->root : option(arguments, "--root", ".");
  const auto db_path = database_path(root, option(arguments, "--db"));

  auto store = syncbox::ClientStore::open(db_path.string());
  if (!store) {
    return fail(store.error().message);
  }
  auto migrated = store.value().migrate();
  if (!migrated) {
    return fail(migrated.error().message);
  }

  auto cursor = store.value().cursor();
  auto pending = store.value().list_pending_ops();
  auto conflicts = store.value().list_unresolved_conflicts();
  if (!cursor || !pending || !conflicts) {
    return fail("cannot read client database");
  }

  std::map<std::string, int> counts;
  for (const auto& op : pending.value()) {
    ++counts[op.state];
  }

  std::cout << "database: " << db_path.string() << '\n'
            << "cursor: " << cursor.value().value << '\n'
            << "pending: " << pending.value().size() << '\n';
  for (const auto& [state, count] : counts) {
    std::cout << "  " << state << ": " << count << '\n';
  }
  std::cout << "conflicts: " << conflicts.value().size() << '\n';
  return 0;
}

int run_conflicts(const Arguments& arguments) {
  auto config = resolve_config(arguments);
  std::string root = config.has_value() ? config->root : option(arguments, "--root", ".");
  const auto db_path = database_path(root, option(arguments, "--db"));

  auto store = syncbox::ClientStore::open(db_path.string());
  if (!store) {
    return fail(store.error().message);
  }
  auto conflicts = store.value().list_unresolved_conflicts();
  if (!conflicts) {
    return fail("cannot read client database");
  }
  for (const auto& conflict : conflicts.value()) {
    std::cout << conflict.conflict_id << " " << conflict.kind << " "
              << conflict.local_path.value_or("?") << " -> "
              << conflict.remote_path.value_or("?") << " (" << conflict.resolution << ")\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::string argument = argv[1];
    if (argument == "--version" || argument == "-v") {
      std::cout << syncbox::project_name() << " " << syncbox::version_string() << '\n';
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      return usage();
    }
  }
  if (argc == 1) {
    return usage();
  }

  const Arguments arguments = parse_arguments(argc, argv);
  if (arguments.command == "login") {
    return run_login(arguments);
  }
  if (arguments.command == "sync") {
    return run_sync(arguments);
  }
  if (arguments.command == "watch") {
    return run_watch(arguments);
  }
  if (arguments.command == "status") {
    return run_status(arguments);
  }
  if (arguments.command == "conflicts") {
    return run_conflicts(arguments);
  }
  return usage();
}
