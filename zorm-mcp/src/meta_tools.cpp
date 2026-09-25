// meta_tools.cpp - read-only schema/metadata tools:
//   db_list_tables / db_describe_table / db_indexes / db_info
// db_describe_table is the "get table fields" tool: catalog-backed, so it
// answers correctly even when the table has zero rows.

#include "app.hpp"
#include "json_util.hpp"

#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace zorm_mcp {

using mcp::Json;
using mcp::McpErrorCode;
using mcp::McpResult;
using mcp::member;
using mcp::server::Ctx;

namespace {

McpResult<Json> unknownDb(const std::string& dbName) {
  return mcp::Err(McpErrorCode::InvalidParams,
                  "unknown db: " + dbName + " (available: " + app().dbListText() +
                      ") - `db` is the backend NAME, see the server instructions");
}

Json columnsJson(const std::vector<ColumnMeta>& cols) {
  Json arr = Json("[]");
  for (const auto& c : cols) {
    Json o;
    o.add("name", Json::str(c.name));
    o.add("type", Json::str(c.type.empty() ? "any" : c.type));
    o.add("nullable", Json(!c.notNull));
    o.add("default", Json::str(c.dflt));
    o.add("primary_key", Json(c.pk));
    arr.push_back(std::move(o));
  }
  return arr;
}

// ---- db_indexes dialect routing -------------------------------------------------

// Best-effort index listing. Rows are passed through (drivers report their own
// column names); jsonfile has no indexes by design.
McpResult<Json> indexRows(const std::string& dbName, const std::string& type,
                          const std::string& table) {
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  std::string sql;
  if (type == "sqlite3") {
    // TVF form - bare pragma breaks the sqlite backend's " limit 1"
    // column-discovery probe (see sqlDescribe in app.cpp).
    sql = "select * from pragma_index_list('" + table + "')";
  } else if (type == "mysql") {
    sql = "show index from `" + table + "`";
  } else if (type == "postgres") {
    sql = "select indexname, indexdef from pg_indexes "
          "where schemaname = current_schema() and tablename = '" + table + "'";
  } else if (type == "dm8") {
    sql = "select index_name from user_indexes where table_name = '" + table +
          "' or upper(table_name) = upper('" + table + "')";
  } else {
    Json result;
    result.add("indexes", Json("[]"));
    result.add("note", Json::str("backend " + type + " has no index concept"));
    return mcp::Ok(toolResult(std::move(result)));
  }
  const ZORM::Json rs = db->querySql(sql);
  if (!okStatus(rs)) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "index lookup failed: " + rs["message"].toString());
  }
  Json result;
  result.add("indexes", toMcpJson(rs["data"]));
  return mcp::Ok(toolResult(std::move(result)));
}

}  // namespace

McpResult<Json> toolListTables(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  if (app().db(dbName) == nullptr) return unknownDb(dbName);

  const std::vector<std::string> tables = listTables(dbName);
  Json arr = Json("[]");
  for (const auto& t : tables) arr.push_back(Json::str(t));
  Json result;
  result.add("tables", std::move(arr));
  result.add("count", Json(static_cast<int64_t>(tables.size())));
  return mcp::Ok(toolResult(std::move(result)));
}

McpResult<Json> toolDescribeTable(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  if (app().db(dbName) == nullptr) return unknownDb(dbName);
  if (!validIdentifier(table)) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "invalid table name: " + table +
                        " (letters/digits/underscore, not starting with a digit)");
  }

  // Unknown tables (miss in the cache AND absent from the catalog) return an
  // empty column list; surface that distinctly from a real empty-table hit.
  bool known = false;
  for (const auto& t : listTables(dbName)) {
    if (t == table) { known = true; break; }
  }
  if (!known) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "unknown table: " + dbName + "." + table +
                        " - call db_list_tables first");
  }

  const std::vector<ColumnMeta> cols = describeTable(dbName, table);
  Json result;
  result.add("db", Json::str(dbName));
  result.add("table", Json::str(table));
  result.add("columns", columnsJson(cols));
  return mcp::Ok(toolResult(std::move(result)));
}

McpResult<Json> toolIndexes(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  if (app().db(dbName) == nullptr) return unknownDb(dbName);
  if (!validIdentifier(table)) {
    return mcp::Err(McpErrorCode::InvalidParams, "invalid table name: " + table);
  }
  return indexRows(dbName, app().specs[dbName].type, table);
}

McpResult<Json> toolInfo(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  const std::string type = app().specs[dbName].type;

  Json result;
  result.add("db", Json::str(dbName));
  result.add("type", Json::str(type));
  result.add("ready", Json(true));

  // Server version, best-effort per dialect (never fails the tool).
  std::string sql;
  if (type == "sqlite3") sql = "select sqlite_version() as version";
  else if (type == "mysql" || type == "postgres") sql = "select version() as version";
  else if (type == "dm8") sql = "select banner as version from v$version";
  if (!sql.empty()) {
    const ZORM::Json rs = db->querySql(sql);
    if (okStatus(rs) && rs["data"].isArray() && rs["data"].size() > 0) {
      const Json row = toMcpJson(rs["data"][0]);
      const std::string version = rowStr(row, {"version", "VERSION"});
      if (!version.empty()) result.add("server_version", Json::str(version));
    }
  }

  const std::vector<std::string> tables = listTables(dbName);
  result.add("tables_count", Json(static_cast<int64_t>(tables.size())));
  return mcp::Ok(toolResult(std::move(result)));
}

// ---- registration -----------------------------------------------------------------

void registerMetaTools(mcp::server::Builder& b) {
  b.tool(Json(R"({"name":"db_list_tables","description":"List user tables of one backend. Start here on first contact with a database; feed the names into db_describe_table to learn each table's columns.","inputSchema":{"type":"object","required":["db"],"properties":{"db":{"type":"string","description":"backend name, e.g. sqlite3 | jsonfile | mysql | postgres | dm8"}}}})"),
         toolListTables);
  b.tool(Json(R"json({"name":"db_describe_table","description":"Get the FULL column metadata of one table: name, type, nullable, default, primary_key. Served from the backend catalog (pragma table_info / information_schema.columns / user_tab_columns / jsonfile columns array), so it works even when the table is EMPTY - never guess columns by selecting rows. Call db_list_tables first to get valid table names.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string","description":"backend name"},"table":{"type":"string","description":"table name - letters, digits, underscore"}}}})json"),
         toolDescribeTable);
  b.tool(Json(R"({"name":"db_indexes","description":"List the indexes of one table (best-effort per dialect; jsonfile has none). Use when query performance is under discussion or before adding new indexes.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"}}}})"),
         toolIndexes);
  b.tool(Json(R"({"name":"db_info","description":"Connectivity probe and server facts for one backend: type, server_version, tables_count. Use it to check a backend is alive and what it is, or to report the database engine version.","inputSchema":{"type":"object","required":["db"],"properties":{"db":{"type":"string"}}}})"),
         toolInfo);
}

}  // namespace zorm_mcp
