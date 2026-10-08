# zorm-mcp/CMakeLists.txt - the production multi-backend database MCP server.
#
# zorm-mcp is the formal product born from the demo/ validation: one MCP
# server exposing ALL of zorm's Idb surface plus schema/SQL/transaction tools
# over every backend. Links mcp + zormlib like any consumer would.
#
# Targets:
#   zorm_mcp_server      the MCP server (stdio by default, --http optional)
#   mcp_test_zorm_mcp    e2e test (spawns the server as a child over real pipes)

add_executable(zorm_mcp_server
  src/app.cpp
  src/json_util.cpp
  src/guide.cpp
  src/meta_tools.cpp
  src/data_tools.cpp
  src/sql_tools.cpp
  src/resources.cpp
  src/main.cpp)
target_link_libraries(zorm_mcp_server PRIVATE mcp zormlib)
target_include_directories(zorm_mcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Runtime DLLs (libpq/sqlite3/dmdpi/...) must resolve when an AI client spawns
# the server - Claude Code does NOT have run-test's PATH setup. Copy them next
# to the executable (Windows searches the exe dir first).
zorm_bundle_runtime_deps(zorm_mcp_server)

# ---- e2e test ---------------------------------------------------------------

if(TARGET gtest)
  add_executable(mcp_test_zorm_mcp test_zorm_mcp.cpp)
  target_link_libraries(mcp_test_zorm_mcp PRIVATE mcp zormlib gtest gtest_main)
  # The spawned server child needs the zorm runtime DLLs (libpq/sqlite3/
  # dmdpi/...). The test prepends this dir to its own PATH before CreateProcess
  # (ctest ENVIRONMENT escaping proved unreliable for PATH on Windows).
  if(WIN32)
    target_compile_definitions(mcp_test_zorm_mcp PRIVATE
        ZORM_DLL_DIR="${CMAKE_SOURCE_DIR}/thirds/bin/win10")
  endif()
  add_test(NAME mcp_test_zorm_mcp COMMAND mcp_test_zorm_mcp
      WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_mcp>)
endif()
