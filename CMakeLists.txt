cmake_minimum_required(VERSION 3.22)

project(SyncBox
  VERSION 0.1.0
  DESCRIPTION "Cross-platform bidirectional file synchronization tool"
  LANGUAGES CXX)

include(cmake/CompilerWarnings.cmake)

option(SYNCBOX_BUILD_TESTS "Build SyncBox unit tests" ON)
option(SYNCBOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(SYNCBOX_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(SYNCBOX_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "SYNCBOX_SANITIZE is only supported with GCC/Clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Core library: version, error model, result type.
# Business modules (proto/store/fs/net/client/server) are added in M2-M5.
# ---------------------------------------------------------------------------
add_library(syncbox_core STATIC
  src/core/error.cpp
  src/core/version.cpp)

target_include_directories(syncbox_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(syncbox_core
  PRIVATE SYNCBOX_VERSION="${PROJECT_VERSION}")

target_link_libraries(syncbox_core
  PUBLIC spdlog::spdlog
  PRIVATE Threads::Threads)

syncbox_set_warnings(syncbox_core)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(syncbox apps/syncbox/main.cpp)
target_link_libraries(syncbox PRIVATE syncbox_core)
syncbox_set_warnings(syncbox)

add_executable(syncboxd apps/syncboxd/main.cpp)
target_link_libraries(syncboxd PRIVATE syncbox_core)
syncbox_set_warnings(syncboxd)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SYNCBOX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS syncbox syncboxd
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
