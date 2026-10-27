// Test helper (FI-16): opens a server database, starts a transaction, appends
// a change-log row, then dies without committing or running destructors.
#include <syncbox/common/time.hpp>
#include <syncbox/core/uuid.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: syncbox_server_crash_writer <db-path>\n";
    return 2;
  }

  auto store = syncbox::ServerStore::open(argv[1]);
  if (!store) {
    std::cerr << "open failed: " << store.error().message << '\n';
    return 3;
  }
  if (auto migrated = store.value().migrate(); !migrated) {
    std::cerr << "migrate failed: " << migrated.error().message << '\n';
    return 4;
  }

  const std::string space_id = syncbox::generate_uuid_v7();
  syncbox::SpaceRecord space{space_id, "crash", syncbox::now_millis(), "active"};
  if (auto inserted = store.value().insert_space(space); !inserted) {
    std::cerr << "space failed: " << inserted.error().message << '\n';
    return 5;
  }

  auto transaction = syncbox::Transaction::begin(store.value().db());
  if (!transaction) {
    std::cerr << "begin failed: " << transaction.error().message << '\n';
    return 6;
  }

  syncbox::ChangeRecord change;
  change.space_id = space_id;
  change.file_id = syncbox::FileId{syncbox::generate_uuid_v7()};
  change.revision = syncbox::Revision{1};
  change.op = syncbox::OpKind::Create;
  change.path = "crash.txt";
  change.size = 1;
  change.deleted = false;
  change.device_id = "crash-device";
  change.created_at = syncbox::now_millis();

  if (auto appended = store.value().append_change(change); !appended) {
    std::cerr << "append failed: " << appended.error().message << '\n';
    return 7;
  }

  std::_Exit(42);  // abrupt death: transaction never commits
}
