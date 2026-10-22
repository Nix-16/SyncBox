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
find_package(OpenSSL REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Core library: version model, path rules, hashing, error model.
# ---------------------------------------------------------------------------
add_library(syncbox_core STATIC
  src/core/error.cpp
  src/core/hash.cpp
  src/core/path.cpp
  src/core/uuid.cpp
  src/core/version.cpp
  src/core/version_model.cpp)

target_include_directories(syncbox_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(syncbox_core
  PRIVATE SYNCBOX_VERSION="${PROJECT_VERSION}")

target_link_libraries(syncbox_core
  PUBLIC
    spdlog::spdlog
    OpenSSL::Crypto
    utf8proc::utf8proc
  PRIVATE Threads::Threads)

syncbox_set_warnings(syncbox_core)

# ---------------------------------------------------------------------------
# Storage library: SQLite wrapper, migrations, repositories.
# ---------------------------------------------------------------------------
add_library(syncbox_store STATIC
  src/store/client_store.cpp
  src/store/database.cpp
  src/store/migrations.cpp
  src/store/server_store.cpp)

target_include_directories(syncbox_store
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(syncbox_store
  PUBLIC
    syncbox_core
    unofficial::sqlite3::sqlite3)

syncbox_set_warnings(syncbox_store)

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
