find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(syncbox_unit_tests
  unit/core_smoke_test.cpp
  unit/core_model_test.cpp
  unit/core_path_test.cpp
  unit/core_hash_test.cpp
  unit/store_database_test.cpp
  unit/store_server_test.cpp
  unit/store_client_test.cpp
  unit/store_crash_test.cpp)

target_link_libraries(syncbox_unit_tests
  PRIVATE
    syncbox_core
    syncbox_store
    GTest::gtest_main)

syncbox_set_warnings(syncbox_unit_tests)

# Helper process used by StoreCrashTest (FI-16 prototype).
add_executable(syncbox_crash_writer helpers/crash_writer.cpp)
target_link_libraries(syncbox_crash_writer PRIVATE syncbox_store)
syncbox_set_warnings(syncbox_crash_writer)

gtest_discover_tests(syncbox_unit_tests
  PROPERTIES ENVIRONMENT "SYNCBOX_CRASH_WRITER=$<TARGET_FILE:syncbox_crash_writer>")
