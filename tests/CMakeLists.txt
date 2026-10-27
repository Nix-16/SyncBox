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
  unit/store_crash_test.cpp
  unit/fs_ignore_test.cpp
  unit/fs_scanner_test.cpp
  unit/fs_atomic_test.cpp
  unit/fs_suppression_test.cpp
  unit/fs_watch_test.cpp
  unit/fs_change_test.cpp
  unit/fs_debounce_test.cpp
  unit/server_api_test.cpp)

target_link_libraries(syncbox_unit_tests
  PRIVATE
    syncbox_core
    syncbox_store
    syncbox_fs
    syncbox_server
    GTest::gtest_main)

syncbox_set_warnings(syncbox_unit_tests)

# Helper process used by StoreCrashTest (FI-16 prototype).
add_executable(syncbox_crash_writer helpers/crash_writer.cpp)
target_link_libraries(syncbox_crash_writer PRIVATE syncbox_store)
syncbox_set_warnings(syncbox_crash_writer)

# Keep both executables in a flat directory for every configuration. With
# multi-config generators (Visual Studio), $<TARGET_FILE> resolves into a
# per-config subdirectory; a config-independent path keeps the crash helper
# discoverable from the test environment on all platforms.
set(SYNCBOX_TEST_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
foreach(target syncbox_unit_tests syncbox_crash_writer)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SYNCBOX_TEST_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SYNCBOX_TEST_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SYNCBOX_TEST_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SYNCBOX_TEST_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SYNCBOX_TEST_OUTPUT_DIR}")
endforeach()

gtest_discover_tests(syncbox_unit_tests
  PROPERTIES ENVIRONMENT
    "SYNCBOX_CRASH_WRITER=${SYNCBOX_TEST_OUTPUT_DIR}/syncbox_crash_writer${CMAKE_EXECUTABLE_SUFFIX}")

# Manual benchmark; not registered with CTest.
add_executable(syncbox_scan_benchmark bench/scan_benchmark.cpp)
target_link_libraries(syncbox_scan_benchmark PRIVATE syncbox_fs)
syncbox_set_warnings(syncbox_scan_benchmark)
