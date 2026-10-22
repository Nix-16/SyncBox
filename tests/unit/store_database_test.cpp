#include <gtest/gtest.h>

#include <syncbox/store/client_store.hpp>
#include <syncbox/store/database.hpp>
#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include "test_support.hpp"

using syncbox::test::scalar_int;
using syncbox::test::scalar_text;
using syncbox::test::TempDirectory;

TEST(StoreDatabaseTest, ServerMigrationCreatesSchemaAndIsIdempotent) {
  TempDirectory directory;
  auto store = syncbox::ServerStore::open(directory.file("server.db").string());
  ASSERT_TRUE(store);

  ASSERT_TRUE(store.value().migrate());
  ASSERT_TRUE(store.value().migrate());

  auto& db = store.value().db();
  EXPECT_EQ(db.user_version(), syncbox::kSchemaVersion);
  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM schema_migrations;"), 1);
  EXPECT_EQ(scalar_text(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='entries';"),
            "entries");
  EXPECT_EQ(scalar_text(db, "PRAGMA integrity_check;"), "ok");
}

TEST(StoreDatabaseTest, ClientMigrationCreatesSchemaAndIsIdempotent) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);

  ASSERT_TRUE(store.value().migrate());
  ASSERT_TRUE(store.value().migrate());

  auto& db = store.value().db();
  EXPECT_EQ(db.user_version(), syncbox::kSchemaVersion);
  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM schema_migrations;"), 1);
  EXPECT_EQ(scalar_text(db,
                        "SELECT name FROM sqlite_master WHERE type='table' AND name='pending_ops';"),
            "pending_ops");
  EXPECT_EQ(scalar_text(db, "PRAGMA integrity_check;"), "ok");
}

TEST(StoreDatabaseTest, TransactionRollsBackWhenNotCommitted) {
  TempDirectory directory;
  auto store = syncbox::ClientStore::open(directory.file("client.db").string());
  ASSERT_TRUE(store);
  ASSERT_TRUE(store.value().migrate());

  auto& db = store.value().db();
  {
    auto transaction = syncbox::Transaction::begin(db);
    ASSERT_TRUE(transaction);
    ASSERT_TRUE(db.execute("INSERT INTO meta(key,value) VALUES('rolled_back','1');"));
    // Leaving scope without commit must roll back.
  }

  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM meta WHERE key='rolled_back';"), 0);

  {
    auto transaction = syncbox::Transaction::begin(db);
    ASSERT_TRUE(transaction);
    ASSERT_TRUE(db.execute("INSERT INTO meta(key,value) VALUES('committed','1');"));
    ASSERT_TRUE(transaction.value().commit());
  }
  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM meta WHERE key='committed';"), 1);
}
