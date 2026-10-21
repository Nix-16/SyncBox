find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(syncbox_unit_tests
  unit/core_smoke_test.cpp)

target_link_libraries(syncbox_unit_tests
  PRIVATE
    syncbox_core
    GTest::gtest_main)

syncbox_set_warnings(syncbox_unit_tests)

gtest_discover_tests(syncbox_unit_tests)
