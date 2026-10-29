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
find_package(Boost CONFIG REQUIRED COMPONENTS asio beast system)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Core library: version model, path rules, hashing, error model.
# ---------------------------------------------------------------------------
add_library(syncbox_core STATIC
  src/core/error.cpp
  src/core/hash.cpp
  src/core/path.cpp
  src/core/token.cpp
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
# Filesystem library: scanner, ignore rules, atomic replace, watchers.
# ---------------------------------------------------------------------------
set(SYNCBOX_FS_SOURCES
  src/fs/change_detector.cpp
  src/fs/debounce.cpp
  src/fs/ignore_rules.cpp
  src/fs/scanner.cpp
  src/fs/suppression.cpp)

if(WIN32)
  list(APPEND SYNCBOX_FS_SOURCES
    src/fs/atomic_file_windows.cpp
    src/fs/file_identity_windows.cpp
    src/fs/platform_windows.cpp
    src/fs/watcher_windows.cpp)
elseif(APPLE)
  list(APPEND SYNCBOX_FS_SOURCES
    src/fs/atomic_file_posix.cpp
    src/fs/file_identity_posix.cpp
    src/fs/platform_macos.cpp
    src/fs/watcher_macos.cpp)
else()
  list(APPEND SYNCBOX_FS_SOURCES
    src/fs/atomic_file_posix.cpp
    src/fs/file_identity_posix.cpp
    src/fs/platform_linux.cpp
    src/fs/watcher_linux.cpp)
endif()

add_library(syncbox_fs STATIC ${SYNCBOX_FS_SOURCES})

target_include_directories(syncbox_fs
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(syncbox_fs
  PUBLIC syncbox_core)

if(APPLE)
  target_link_libraries(syncbox_fs PRIVATE
    "-framework CoreServices"
    "-framework CoreFoundation")
endif()

syncbox_set_warnings(syncbox_fs)

# ---------------------------------------------------------------------------
# Networking: asynchronous HTTP server (Boost.Beast).
# ---------------------------------------------------------------------------
add_library(syncbox_net STATIC
  src/net/http_client.cpp
  src/net/http_server.cpp
  src/net/ws_hub.cpp)

target_include_directories(syncbox_net
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(syncbox_net
  PUBLIC
    syncbox_core
    Boost::asio
    Boost::beast
    Boost::system
  PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json)

syncbox_set_warnings(syncbox_net)

# ---------------------------------------------------------------------------
# Server services: auth, operations, HTTP routing.
# ---------------------------------------------------------------------------
add_library(syncbox_server STATIC
  src/server/api.cpp
  src/server/auth_service.cpp
  src/server/blob_store.cpp
  src/server/op_service.cpp)

target_include_directories(syncbox_server
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(syncbox_server
  PUBLIC
    syncbox_store
    syncbox_net
    nlohmann_json::nlohmann_json)

syncbox_set_warnings(syncbox_server)

# ---------------------------------------------------------------------------
# Client sync engine.
# ---------------------------------------------------------------------------
add_library(syncbox_client STATIC
  src/client/engine.cpp)

target_include_directories(syncbox_client
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(syncbox_client
  PUBLIC
    syncbox_core
    syncbox_store
    syncbox_fs
    syncbox_net)

syncbox_set_warnings(syncbox_client)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(syncbox apps/syncbox/main.cpp)
target_link_libraries(syncbox PRIVATE syncbox_client syncbox_store syncbox_core)
syncbox_set_warnings(syncbox)

add_executable(syncboxd apps/syncboxd/main.cpp)
target_link_libraries(syncboxd PRIVATE syncbox_server syncbox_store syncbox_core)
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
