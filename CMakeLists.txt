cmake_minimum_required(VERSION 3.26)

project(StrongKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STRONGKV_BUILD_TESTS "Build StrongKV tests" ON)
option(STRONGKV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(STRONGKV_NURAFT_SOURCE "" CACHE PATH
    "Existing NuRaft v3.0.0 source tree; empty fetches the pinned tag")

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(DISABLE_SSL ON CACHE BOOL "" FORCE)

if(STRONGKV_NURAFT_SOURCE)
    get_filename_component(STRONGKV_NURAFT_SOURCE
        "${STRONGKV_NURAFT_SOURCE}" ABSOLUTE)
    if(NOT EXISTS "${STRONGKV_NURAFT_SOURCE}/include/libnuraft/nuraft.hxx")
        message(FATAL_ERROR
            "STRONGKV_NURAFT_SOURCE is not a NuRaft source tree: "
            "${STRONGKV_NURAFT_SOURCE}")
    endif()
    set(_strongkv_nuraft_source "${STRONGKV_NURAFT_SOURCE}")
    add_subdirectory("${STRONGKV_NURAFT_SOURCE}"
        "${CMAKE_BINARY_DIR}/_deps/nuraft-build" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(nuraft
        GIT_REPOSITORY https://github.com/eBay/NuRaft.git
        GIT_TAG 0563f31059cae5b45d2cb57576916984d9513630
        GIT_SHALLOW FALSE
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(nuraft)
    set(_strongkv_nuraft_source "${nuraft_SOURCE_DIR}")
endif()

find_package(Threads REQUIRED)

add_library(strongkv_core
    src/common/binary_codec.cpp
    src/client/client.cpp
    src/config/config.cpp
    src/logging/logger.cpp
    src/protocol/resp.cpp
    src/raft/raft_node.cpp
    src/security/password.cpp
    src/server/command_service.cpp
    src/server/tcp_server.cpp
    src/storage/file_util.cpp
    src/storage/file_log_store.cpp
    src/storage/file_state_manager.cpp
    src/state_machine/command.cpp
    src/state_machine/kv_state_machine.cpp)

target_include_directories(strongkv_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(strongkv_core SYSTEM PRIVATE
    "${_strongkv_nuraft_source}/include"
    "${_strongkv_nuraft_source}/asio/asio/include")

target_compile_definitions(strongkv_core PUBLIC ASIO_STANDALONE)
target_link_libraries(strongkv_core PUBLIC NuRaft::static_lib Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(strongkv_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(STRONGKV_WARNINGS_AS_ERRORS)
        target_compile_options(strongkv_core PRIVATE -Werror)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(strongkv_core PUBLIC stdc++fs)
endif()

add_executable(strongkv-server src/main.cpp)
target_include_directories(strongkv-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(strongkv-server SYSTEM PRIVATE
    "${_strongkv_nuraft_source}/asio/asio/include")
target_link_libraries(strongkv-server PRIVATE strongkv_core)

add_executable(strongkv-cli tools/strongkv-cli/main.cpp)
target_link_libraries(strongkv-cli PRIVATE strongkv_core)

if(STRONGKV_BUILD_TESTS)
    enable_testing()
    add_executable(strongkv_unit_tests
        tests/unit/test_main.cpp
        tests/unit/command_test.cpp
        tests/unit/config_test.cpp
        tests/unit/file_log_store_test.cpp
        tests/unit/kv_state_machine_test.cpp
        tests/unit/security_test.cpp)
    target_sources(strongkv_unit_tests PRIVATE
        tests/unit/resp_test.cpp)
    target_include_directories(strongkv_unit_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(strongkv_unit_tests PRIVATE strongkv_core)
    add_test(NAME strongkv_unit_tests COMMAND strongkv_unit_tests)

    add_executable(strongkv-concurrent-incr
        tests/integration/concurrent_incr.cpp)
    target_link_libraries(strongkv-concurrent-incr PRIVATE strongkv_core)
endif()
