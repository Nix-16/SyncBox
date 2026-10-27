#include <gtest/gtest.h>

#include <syncbox/store/migrations.hpp>
#include <syncbox/store/server_store.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "test_support.hpp"

TEST(ServerCrashTest, UncommittedTransactionDoesNotSurviveProcessDeath) {
  const char* client_writer = std::getenv("SYNCBOX_CRASH_WRITER");
  ASSERT_NE(client_writer, nullptr) << "SYNCBOX_CRASH_WRITER is not set";

  std::filesystem::path writer_path(client_writer);
  writer_path.replace_filename("syncbox_server_crash_writer" +
                               writer_path.extension().string());
  const std::string writer = writer_path.string();

  syncbox::test::TempDirectory directory;
  const std::string db_path = directory.file("server.db").string();

  std::string command = std::string("\"") + writer + "\" \"" + db_path + "\"";
#ifdef _WIN32
  command = "\"" + command + "\"";
#endif
  const int exit_code = std::system(command.c_str());
  EXPECT_NE(exit_code, 0);

  auto reopened = syncbox::ServerStore::open(db_path);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(syncbox::test::scalar_text(reopened.value().db(), "PRAGMA integrity_check;"), "ok");
  EXPECT_EQ(syncbox::test::scalar_int(reopened.value().db(), "SELECT COUNT(*) FROM change_log;"), 0);
  EXPECT_EQ(syncbox::test::scalar_int(reopened.value().db(), "SELECT COUNT(*) FROM spaces;"), 1);
}
