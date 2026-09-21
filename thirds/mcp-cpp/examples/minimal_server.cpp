// minimal_server - smallest useful MCP server over stdio.
//
// Serves as:
//  - the stdio end-to-end test peer (tests/test_stdio_e2e.cpp spawns it)
//  - a manual target for the MCP Inspector:
//      npx @modelcontextprotocol/inspector ./minimal_server.exe

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

using namespace mcp;
using namespace mcp::server;

int main() {
  Builder b;
  b.name("minimal").version("0.1.0")
      .instructions("Echoes messages; use for smoke-testing MCP clients.");

  b.simpleTool("echo", "Returns the input text unchanged.",
               [](Ctx& c) -> McpResult<Json> {
                 const Json args = c.params["arguments"];
                 const Json text = args["text"];
                 if (!text.isString()) {
                   return Err(McpErrorCode::InvalidParams, "arguments.text must be a string");
                 }
                 Json result;
                 Json content = Json("[]");
                 Json item;
                 item.add("type", Json::str("text"));
                 item.add("text", Json::str(text.toString()));
                 content.push_back(std::move(item));
                 result.add("content", std::move(content));
                 return Ok(std::move(result));
               });

  b.simpleTool("add", "Adds two integers.",
               [](Ctx& c) -> McpResult<Json> {
                 const Json args = c.params["arguments"];
                 const Json a = args["a"];
                 const Json bv = args["b"];
                 if (!a.isNumber() || !bv.isNumber()) {
                   return Err(McpErrorCode::InvalidParams, "arguments.a and arguments.b must be numbers");
                 }
                 const int64_t sum = a.toInt64() + bv.toInt64();
                 Json result;
                 Json content = Json("[]");
                 Json item;
                 item.add("type", Json::str("text"));
                 item.add("text", Json::str(std::to_string(sum)));
                 content.push_back(std::move(item));
                 result.add("content", std::move(content));
                 return Ok(std::move(result));
               });

  Server server = b.build();
  StdioTransport transport;
  const McpResult<void> r = server.run(transport);
  if (!r.has_value()) {
    std::cerr << "[minimal] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}
