# demo/CMakeLists.txt - zorm showcase executables.
#
# zorm is the protagonist; mcp-cpp is the third-party MCP protocol library
# it links (like any other consumer would). Built only when the zorm root
# CMakeLists includes this directory (TARGET mcp present).
#
# Targets:
#   zorm_server             single-backend minimal MCP server (four tools)
#   zorm_analytics_server   multi-backend "Zorm Analytics" MCP demo
#   mcp_test_zorm_analytics e2e test (spawns the analytics server as a child)

# ---- zorm_server: single-backend minimal example ---------------------------

add_executable(zorm_server zorm_server.cpp)
target_link_libraries(zorm_server PRIVATE mcp zormlib)

# ---- zorm_analytics_server: the multi-backend demo --------------------------

add_executable(zorm_analytics_server
  zorm_analytics_server.cpp
  demo_data.cpp
  demo_data.h)
target_link_libraries(zorm_analytics_server PRIVATE mcp zormlib)

# ---- e2e test (added in milestone 3) ----------------------------------------

# if(BUILD_TESTING AND TARGET gtest)
#   add_executable(mcp_test_zorm_analytics test_zorm_analytics.cpp)
#   target_link_libraries(mcp_test_zorm_analytics PRIVATE mcp zormlib gtest)
#   add_test(NAME mcp_test_zorm_analytics COMMAND mcp_test_zorm_analytics
#       WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_analytics>)
# endif()
