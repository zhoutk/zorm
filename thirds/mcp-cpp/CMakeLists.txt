# ---------------------------------------------------------------------------
# mcp-cpp: independent MCP (Model Context Protocol) C++ SDK.
#
# Builds two ways:
#   1. standalone:  cmake -S thirds/mcp-cpp -B build && cmake --build build
#      (needs a zjson single-header copy under ./thirds/zjson)
#   2. as a subdirectory of the zorm project: the root CMakeLists adds this
#      directory; zjson is then found in the repo sibling dir and googletest
#      is reused from the parent build when already present.
#
# Targets: mcp (static) + mcp::mcp alias; mcp tests registered with CTest.
# See PLAN.md for the roadmap and decision log.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.18)
project(mcp-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MCP_BUILD_TESTS "Build mcp-cpp unit tests" ON)
# Reserved now, effective from M3 (Streamable HTTP transport); a pure-stdio
# build for low-footprint hosts must stay possible.
option(MCP_DISABLE_HTTP "Build without the Streamable HTTP transport" OFF)

# ---- zjson discovery -------------------------------------------------------
# Default: a copy bundled under ./thirds/zjson (standalone layout); falls back
# to the zorm repo sibling dir when built in-tree. Override with
# -DMCP_ZJSON_DIR=<path> to point anywhere else.
if(NOT DEFINED MCP_ZJSON_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirds/zjson/zjson.hpp")
    set(MCP_ZJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirds/zjson")
  elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../zjson/zjson.hpp")
    set(MCP_ZJSON_DIR "${CMAKE_CURRENT_LIST_DIR}/../zjson")
  endif()
endif()
if(NOT DEFINED MCP_ZJSON_DIR)
  message(FATAL_ERROR
    "mcp-cpp: zjson not found - copy the zjson single header under "
    "${CMAKE_CURRENT_SOURCE_DIR}/thirds/zjson or pass -DMCP_ZJSON_DIR=<dir>")
endif()
message(STATUS "mcp-cpp: zjson at ${MCP_ZJSON_DIR}")

# ---- library ---------------------------------------------------------------

add_library(mcp STATIC
  src/version.cpp
  src/types.cpp
  src/jsonrpc.cpp
  src/transport.cpp
  src/server.cpp
  src/client.cpp
  src/schema.cpp
  src/base64.cpp
  src/transport_http.cpp)
add_library(mcp::mcp ALIAS mcp)

target_include_directories(mcp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MCP_ZJSON_DIR}>)
target_compile_features(mcp PUBLIC cxx_std_17)

set(MCP_CXX_WARNINGS -Wall -Wextra -Wpedantic)
if(MSVC)
  set(MCP_CXX_WARNINGS /W4)
  target_compile_options(mcp PRIVATE /Zc:__cplusplus)
else()
  # Match the host project convention: pin char signedness for x86/ARM parity.
  target_compile_options(mcp PRIVATE -fsigned-char)
endif()
target_compile_options(mcp PRIVATE ${MCP_CXX_WARNINGS})

# ---- Streamable HTTP (httplib) ----------------------------------------------
# Skipped entirely when MCP_DISABLE_HTTP is on; degrades to a pure-stdio
# build with a warning when httplib is unavailable.
if(NOT MCP_DISABLE_HTTP)
  if(NOT TARGET httplib)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../httplib/CMakeLists.txt")
      add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../httplib" httplib-build)
    endif()
  endif()
  if(TARGET httplib)
    target_compile_definitions(mcp PUBLIC MCP_HAS_HTTP)
    target_link_libraries(mcp PUBLIC httplib)
    message(STATUS "mcp-cpp: HTTP transport enabled (httplib)")
  else()
    message(WARNING "mcp-cpp: httplib not found, building without HTTP transport")
  endif()
endif()

# ---- examples ---------------------------------------------------------------

add_executable(minimal_server examples/minimal_server.cpp)
target_link_libraries(minimal_server PRIVATE mcp)

# ---- benchmarks (not registered with CTest; run manually) --------------------

add_executable(mcp_bench_parse bench/bench_parse.cpp)
add_executable(mcp_bench_dispatch bench/bench_dispatch.cpp)
add_executable(mcp_bench_e2e bench/bench_e2e.cpp)
foreach(t mcp_bench_parse mcp_bench_dispatch mcp_bench_e2e)
  target_link_libraries(${t} PRIVATE mcp)
  target_compile_options(${t} PRIVATE ${MCP_CXX_WARNINGS})
endforeach()

# ---- tests -----------------------------------------------------------------
# googletest discovery, in order:
#   1. already added by the parent project (in-tree integration)
#   2. the zorm repo copy next to this directory (also covers standalone
#      builds made from inside the repo checkout)
#   3. a system GTest package; if none exists, tests are skipped with a
#      warning - the library itself has no test dependency.

set(MCP_TEST_EXECUTABLES "")
if(MCP_BUILD_TESTS)
  if(NOT TARGET gtest)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../googletest-1.18.0/CMakeLists.txt")
      add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../googletest-1.18.0"
                       "${CMAKE_BINARY_DIR}/mcp-googletest" EXCLUDE_FROM_ALL)
    else()
      find_package(GTest QUIET)
      if(NOT GTest_FOUND)
        message(WARNING "mcp-cpp: googletest not found, building without tests")
        set(MCP_BUILD_TESTS OFF)
      endif()
    endif()
  endif()
endif()

if(MCP_BUILD_TESTS)
  enable_testing()

  add_executable(mcp_test_version tests/test_version.cpp)
  add_executable(mcp_test_error tests/test_error.cpp)
  add_executable(mcp_test_jsonrpc tests/test_jsonrpc.cpp)
  add_executable(mcp_test_types tests/test_types.cpp)
  add_executable(mcp_test_transport tests/test_transport.cpp)
  add_executable(mcp_test_server tests/test_server.cpp)
  add_executable(mcp_test_client tests/test_client.cpp)
  add_executable(mcp_test_schema tests/test_schema.cpp)
  # Owns its main(): doubles as the stdio e2e child server.
  add_executable(mcp_test_stdio_e2e tests/test_stdio_e2e.cpp)
  foreach(t mcp_test_version mcp_test_error mcp_test_jsonrpc mcp_test_types mcp_test_transport
           mcp_test_server mcp_test_client mcp_test_schema)
    target_link_libraries(${t} PRIVATE mcp gtest gtest_main)
    target_compile_options(${t} PRIVATE ${MCP_CXX_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
    list(APPEND MCP_TEST_EXECUTABLES ${t})
  endforeach()
  if(TARGET httplib)
    add_executable(mcp_test_http_e2e tests/test_http_e2e.cpp)
    target_link_libraries(mcp_test_http_e2e PRIVATE mcp gtest gtest_main)
    target_compile_options(mcp_test_http_e2e PRIVATE ${MCP_CXX_WARNINGS})
    add_test(NAME mcp_test_http_e2e COMMAND mcp_test_http_e2e)
    list(APPEND MCP_TEST_EXECUTABLES mcp_test_http_e2e)
  endif()

  target_link_libraries(mcp_test_stdio_e2e PRIVATE mcp gtest)
  target_compile_options(mcp_test_stdio_e2e PRIVATE ${MCP_CXX_WARNINGS})
  add_test(NAME mcp_test_stdio_e2e COMMAND mcp_test_stdio_e2e)
  list(APPEND MCP_TEST_EXECUTABLES mcp_test_stdio_e2e)

  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Standalone build: keep artifacts inside this project's build dir.
    set_target_properties(${MCP_TEST_EXECUTABLES} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
endif()
