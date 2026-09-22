// zorm_server - exposes a zorm database as MCP tools (stdio transport).
//
// Built ONLY inside the zorm repo (the root CMakeLists configures the
// target), so the library itself stays zorm-free and independently
// publishable. Copy this file as a template for "database as MCP server".
//
// Tools:
//   db_select  - smart query: {tablename, fields?, lks?, ins?, page?, size?, sort?}
//   db_insert  - insert one row: {tablename, data}
//   db_update  - update rows:    {tablename, data, where}
//   db_delete  - delete rows:    {tablename, where}
//
// Run: zorm_server <dbType> [options passed to DbBase]
// e.g. zorm_server sqlite3 or zorm_server jsonfile ./data/db.json

#include "DbBase.h"
#include "Idb.h"
#include <mcp/mcp.hpp>

#include <iostream>
#include <memory>
#include <string>

using namespace mcp;
using namespace mcp::server;

namespace {

std::unique_ptr<ZORM::Idb> g_db;  // single connection; single-threaded dispatch

Json toJson(const ZORM::Json& zj) {
  // zjson is shared by both libraries: reparse the wire text.
  std::string err;
  Json parsed = Json::ParseJson(zj.toString(), err);
  return parsed.isError() ? Json(nullptr) : parsed;
}

ZORM::Json toZorm(const Json& j) { return ZORM::Json(j.toString().c_str()); }

McpResult<Json> textResult(const std::string& text) {
  Json result;
  Json content = Json("[]");
  Json item;
  item.add("type", Json::str("text"));
  item.add("text", Json::str(text));
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  return Ok(std::move(result));
}

// All tools share the guard: db must exist and ops need table+data sanity.
McpResult<Json> queryTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  ZORM::Json rows = g_db->select(table, toZorm(args));
  return textResult(rows.toString());
}

McpResult<Json> insertTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  // Single-row insert via insertBatch: elements is a one-element array.
  ZORM::Json elements = toZorm(member(args, "data"));
  ZORM::Json inserted = g_db->insertBatch(table, elements, "id");
  return textResult(inserted.toString());
}

McpResult<Json> updateTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  // zorm update: params = data fields + reserved "where" key.
  ZORM::Json params = toZorm(member(args, "data"));
  params.add("where", toZorm(member(args, "where")));
  ZORM::Json updated = g_db->update(table, params);
  return textResult(updated.toString());
}

McpResult<Json> deleteTool(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string table = args["tablename"].toString();
  ZORM::Json params;
  params.add("where", toZorm(member(args, "where")));
  ZORM::Json deleted = g_db->remove(table, params);
  return textResult(deleted.toString());
}

Json schema(const std::string& json) {
  std::string err;
  Json parsed = Json::ParseJson(json, err);
  return parsed.isError() ? Json(R"({"type":"object"})") : parsed;
}

Json toolDef(const std::string& name, const std::string& description, const std::string& schemaJson) {
  Json def;
  def.add("name", Json::str(name));
  def.add("description", Json::str(description));
  def.add("inputSchema", schema(schemaJson));
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: zorm_server <dbType> [dboptions...]\n"
              << "  e.g. zorm_server sqlite3\n"
              << "       zorm_server jsonfile ./data/db.json\n";
    return 2;
  }
  ZORM::Json options(argc > 2 ? argv[2] : "");
  g_db.reset(new ZORM::DbBase(argv[1], options));
  if (!g_db) {
    std::cerr << "unknown dbType: " << argv[1] << std::endl;
    return 2;
  }

  Builder b;
  b.name("zorm-mcp").version("0.1.0")
      .instructions(std::string("MCP facade over a ") + argv[1] + " database (zorm).");

  b.tool(toolDef("db_select", "Smart query over a table.",
                 R"({"type":"object","required":["tablename"],
                     "properties":{"tablename":{"type":"string"},
                                   "fields":{"type":"array","items":{"type":"string"}},
                                   "lks":{"type":"object"},"ins":{"type":"object"},
                                   "page":{"type":"integer","minimum":1},
                                   "size":{"type":"integer","minimum":1,"maximum":500},
                                   "sort":{"type":"array","items":{"type":"string"}}}})"),
         [](Ctx& c) { return queryTool(c); });

  b.tool(toolDef("db_insert", "Insert one row.",
                 R"({"type":"object","required":["tablename","data"],
                     "properties":{"tablename":{"type":"string"},
                                   "data":{"type":"object"}}})"),
         [](Ctx& c) { return insertTool(c); });

  b.tool(toolDef("db_update", "Update rows matching where.",
                 R"({"type":"object","required":["tablename","data","where"],
                     "properties":{"tablename":{"type":"string"},
                                   "data":{"type":"object"},
                                   "where":{"type":"object"}}})"),
         [](Ctx& c) { return updateTool(c); });

  b.tool(toolDef("db_delete", "Delete rows matching where.",
                 R"({"type":"object","required":["tablename","where"],
                     "properties":{"tablename":{"type":"string"},
                                   "where":{"type":"object"}}})"),
         [](Ctx& c) { return deleteTool(c); });

  b.acceptLegacyClients(true);  // Claude Code / VS Code speak legacy initialize
  b.supports(ProtocolVersion::V2024_11_05)
      .supports(ProtocolVersion::V2025_03_26)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::V2025_11_25)
      .supports(ProtocolVersion::Latest);

  auto server = b.build();
  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  if (!r.has_value()) {
    std::cerr << "[zorm-mcp] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}
