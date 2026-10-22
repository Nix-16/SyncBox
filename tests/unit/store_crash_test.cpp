#include <gtest/gtest.h>

#include <syncbox/store/client_store.hpp>

#include <cstdlib>
#include <string>

#include "test_support.hpp"

using syncbox::test::scalar_int;
using syncbox::test::scalar_text;
using syncbox::test::TempDirectory;

// FI-16 prototype: a child process dies inside an open transaction. The
// uncommitted row must not survive, and the database must remain intact.
TEST(StoreCrashTest, UncommittedTransactionDoesNotSurviveProcessDeath) {
  const char* writer = std::getenv("SYNCBOX_CRASH_WRITER");
  ASSERT_NE(writer, nullptr) << "SYNCBOX_CRASH_WRITER is not set by the test harness";
  ASSERT_TRUE(std::filesystem::exists(writer)) << "crash writer not found: " << writer;

  TempDirectory directory;
  const auto db_path = directory.file("client.db");

  const std::string command =
      std::string("\"") + writer + "\" \"" + db_path.string() + "\"";

  // cmd.exe strips the first and last quote of a command line that starts with
  // a quote, so the whole command needs one extra pair of quotes on Windows.
#ifdef _WIN32
  const std::string shell_command = "\"" + command + "\"";
#else
  const std::string shell_command = command;
#endif

  const int exit_code = std::system(shell_command.c_str());
  EXPECT_NE(exit_code, 0) << "crash writer should terminate without committing; command: "
                          << shell_command;
  ASSERT_TRUE(std::filesystem::exists(db_path))
      << "database was not created; exit code " << exit_code << ", command: " << shell_command;

  auto reopened = syncbox::ClientStore::open(db_path.string());
  ASSERT_TRUE(reopened);

  auto& db = reopened.value().db();
  EXPECT_EQ(scalar_text(db, "PRAGMA integrity_check;"), "ok");
  EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM meta WHERE key='crash';"), 0);
}
