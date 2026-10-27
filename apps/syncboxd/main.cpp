#include <syncbox/common/time.hpp>
#include <syncbox/core/token.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/core/version.hpp>
#include <syncbox/server/api.hpp>
#include <syncbox/server/auth_service.hpp>
#include <syncbox/server/op_service.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
  g_stop = 1;
}

struct Options {
  std::string db_path = "server.db";
  std::string listen = "127.0.0.1";
  std::uint16_t port = 8443;
};

[[nodiscard]] std::optional<std::string> next_value(int argc, char** argv, int& index) {
  if (index + 1 >= argc) {
    return std::nullopt;
  }
  ++index;
  return std::string(argv[index]);
}

int print_usage() {
  std::cout
      << "Usage:\n"
      << "  syncboxd [--db PATH] [--listen ADDR] [--port PORT]\n"
      << "  syncboxd admin create-space --db PATH --name NAME\n"
      << "  syncboxd admin enroll --db PATH --space SPACE_ID [--ttl-hours N]\n"
      << "  syncboxd admin revoke --db PATH --device DEVICE_ID [--reason TEXT]\n"
      << "  syncboxd --version\n";
  return 0;
}

int fail(const std::string& message) {
  std::cerr << "syncboxd: " << message << '\n';
  return 1;
}

int run_admin(int argc, char** argv) {
  std::string db_path;
  std::string command;
  std::string name;
  std::string space_id;
  std::string device_id;
  std::string reason = "revoked by administrator";
  int ttl_hours = 24;

  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--db") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--db needs a value");
      db_path = *value;
    } else if (argument == "--name") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--name needs a value");
      name = *value;
    } else if (argument == "--space") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--space needs a value");
      space_id = *value;
    } else if (argument == "--device") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--device needs a value");
      device_id = *value;
    } else if (argument == "--reason") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--reason needs a value");
      reason = *value;
    } else if (argument == "--ttl-hours") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--ttl-hours needs a value");
      try {
        ttl_hours = std::stoi(*value);
      } catch (const std::exception&) {
        return fail("invalid --ttl-hours");
      }
    } else if (command.empty()) {
      command = argument;
    } else {
      return fail("unexpected argument: " + argument);
    }
  }

  if (db_path.empty()) {
    return fail("--db is required");
  }

  auto store = syncbox::ServerStore::open(db_path);
  if (!store) {
    return fail(store.error().message);
  }
  auto migrated = store.value().migrate();
  if (!migrated) {
    return fail(migrated.error().message);
  }

  if (command == "create-space") {
    if (name.empty()) {
      return fail("--name is required");
    }
    syncbox::SpaceRecord space;
    space.space_id = syncbox::generate_uuid_v7();
    space.name = name;
    space.created_at = syncbox::now_millis();
    auto inserted = store.value().insert_space(space);
    if (!inserted) {
      return fail(inserted.error().message);
    }
    std::cout << space.space_id << '\n';
    return 0;
  }

  if (command == "enroll") {
    if (space_id.empty()) {
      return fail("--space is required");
    }
    auto token = syncbox::generate_token();
    if (!token) {
      return fail(token.error().message);
    }
    auto hash = syncbox::hash_token(token.value());
    if (!hash) {
      return fail(hash.error().message);
    }
    syncbox::EnrollmentTokenRecord record;
    record.token_hash.assign(hash.value().bytes.begin(), hash.value().bytes.end());
    record.space_id = space_id;
    record.created_by = "admin-cli";
    record.expires_at = syncbox::now_millis() +
                        static_cast<std::int64_t>(ttl_hours) * 3600 * 1000;
    auto inserted = store.value().insert_enrollment_token(record);
    if (!inserted) {
      return fail(inserted.error().message);
    }
    std::cout << token.value() << '\n';
    return 0;
  }

  if (command == "revoke") {
    if (device_id.empty()) {
      return fail("--device is required");
    }
    auto revoked = store.value().set_device_status(device_id, "revoked", reason);
    if (!revoked) {
      return fail(revoked.error().message);
    }
    return 0;
  }

  return print_usage();
}

int run_server(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--version" || argument == "-v") {
      std::cout << syncbox::project_name() << " server " << syncbox::version_string() << '\n';
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      return print_usage();
    }
    if (argument == "--db") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--db needs a value");
      options.db_path = *value;
    } else if (argument == "--listen") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--listen needs a value");
      options.listen = *value;
    } else if (argument == "--port") {
      auto value = next_value(argc, argv, i);
      if (!value) return fail("--port needs a value");
      try {
        options.port = static_cast<std::uint16_t>(std::stoi(*value));
      } catch (const std::exception&) {
        return fail("invalid --port");
      }
    } else {
      return fail("unknown argument: " + argument);
    }
  }

  auto store = syncbox::ServerStore::open(options.db_path);
  if (!store) {
    return fail(store.error().message);
  }
  auto migrated = store.value().migrate();
  if (!migrated) {
    return fail(migrated.error().message);
  }

  syncbox::server::AuthService auth(store.value());
  syncbox::server::OpService ops(store.value());
  syncbox::server::Api api(store.value(), auth, ops);
  syncbox::net::HttpServer server(options.listen, options.port, api, 2);

  auto started = server.start();
  if (!started) {
    return fail(started.error().message);
  }
  std::cout << "syncboxd listening on " << options.listen << ':' << server.port() << '\n';

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  server.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "admin") {
    return run_admin(argc, argv);
  }
  return run_server(argc, argv);
}
