// Test helper: opens a client database, begins a transaction, inserts a row,
// then terminates the process without committing or running destructors.
// Used by StoreCrashTest to verify WAL crash recovery.
#include <syncbox/store/client_store.hpp>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: syncbox_crash_writer <db-path>\n";
    return 2;
  }

  auto store = syncbox::ClientStore::open(argv[1]);
  if (!store) {
    std::cerr << "open failed: " << store.error().message << '\n';
    return 3;
  }
  if (auto migrated = store.value().migrate(); !migrated) {
    std::cerr << "migrate failed: " << migrated.error().message << '\n';
    return 4;
  }

  auto transaction = syncbox::Transaction::begin(store.value().db());
  if (!transaction) {
    std::cerr << "begin failed: " << transaction.error().message << '\n';
    return 5;
  }

  auto inserted =
      store.value().db().execute("INSERT INTO meta(key,value) VALUES('crash','1');");
  if (!inserted) {
    std::cerr << "insert failed: " << inserted.error().message << '\n';
    return 6;
  }

  std::_Exit(42);  // simulated abrupt death: no commit, no destructors
}
