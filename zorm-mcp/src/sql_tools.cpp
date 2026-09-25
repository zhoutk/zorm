// sql_tools.cpp - the SQL escape hatches:
//   db_query_sql  READ-ONLY parameterized SQL (always registered; statement-
//                 level allowlist keeps it SELECT/PRAGMA/SHOW/EXPLAIN/WITH).
//   db_exec_sql   write/DDL SQL (registered ONLY with --allow-sql; MRTR).
//   db_trans      atomic multi-statement batch via Idb::transGo (MRTR).

#include "app.hpp"
#include "json_util.hpp"
#include "tools.hpp"

#include <cctype>
#include <string>

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

// Read-only statement gate. Allowed first keywords: SELECT / WITH (read
// queries and CTEs), PRAGMA / SHOW / EXPLAIN / DESCRIBE / DESC (introspection
// and plans). Semicolons are rejected outright - one statement per call, no
// statement smuggling. Anything else belongs in db_exec_sql (if enabled).
McpResult<Json> checkReadOnlySql(const std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
  static const char* kAllowed[] = {"select", "with", "pragma", "show",
                                   "explain", "describe", "desc"};
  for (const char* kw : kAllowed) {
    size_t j = 0;
    while (kw[j] != '\0') {
      if (i + j >= sql.size() ||
          std::tolower(static_cast<unsigned char>(sql[i + j])) != kw[j]) break;
      ++j;
    }
    if (kw[j] == '\0' &&
        (i + j >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[i + j])))) {
      // keyword matched and is not a prefix of a longer identifier
      if (sql.find(';') != std::string::npos) {
        return mcp::Err(McpErrorCode::InvalidParams,
                        "db_query_sql is READ-ONLY and accepts a single statement "
                        "(no ';'). Multi-statement work belongs in db_trans, write "
                        "SQL in db_exec_sql (--allow-sql)");
      }
      return mcp::Ok(Json());
    }
  }
  return mcp::Err(McpErrorCode::InvalidParams,
                  "db_query_sql is READ-ONLY: only SELECT / WITH / PRAGMA / SHOW / "
                  "EXPLAIN / DESCRIBE statements are allowed. To write data use "
                  "db_insert / db_update / db_delete / db_trans, or db_exec_sql "
                  "when the server runs with --allow-sql");
}

}  // namespace

McpResult<Json> toolQuerySql(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  const std::string sql = member(args, "sql").toString();
  if (sql.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "sql is required");
  }
  if (const McpResult<Json> gate = checkReadOnlySql(sql); gate.has_value() == false) {
    return gate;
  }

  // values: positional array bound to the placeholders in order (zorm
  // querySql(sql, params, values) - named params stay available for power
  // users via `params`).
  Json values = member(args, "values");
  if (!values.isArray()) values = Json("[]");
  // DbBase's facade requires params to be an OBJECT - a missing `params`
  // argument must become an empty object, never a null Json.
  ZORM::Json zparams;
  const Json namedParams = member(args, "params");
  if (namedParams.isObject()) zparams = toZormJson(namedParams);
  const ZORM::Json rs = db->querySql(sql, zparams, toZormJson(values));
  if (!okStatus(rs)) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "query failed: " + rs["message"].toString());
  }
  return mcp::Ok(toolResult(toMcpJson(rs)));
}

McpResult<Json> toolExecSql(Ctx& c) {
  if (!app().allowSql) {
    return mcp::Err(McpErrorCode::MethodNotFound,
                    "db_exec_sql is not enabled - restart the server with "
                    "--allow-sql. Structured writes (db_insert/db_update/db_delete/"
                    "db_trans) work without it");
  }
  return stageWrite(c, "exec_sql");
}

McpResult<Json> toolTrans(Ctx& c) { return stageWrite(c, "trans"); }

void registerSqlTools(mcp::server::Builder& b) {
  b.tool(Json(R"json({"name":"db_query_sql","description":"Ad-hoc READ-ONLY SQL for what db_query cannot express: JOINs, dialect functions, EXPLAIN plans. Only SELECT / WITH / PRAGMA / SHOW / EXPLAIN / DESCRIBE, one statement, no ';'. Bind values via the values array (? placeholders, in order) instead of string concatenation. Writes belong in db_insert / db_update / db_delete / db_trans.","inputSchema":{"type":"object","required":["db","sql"],"properties":{"db":{"type":"string","description":"backend name"},"sql":{"type":"string","description":"single read-only statement"},"values":{"type":"array","description":"positional bindings for ? placeholders"},"params":{"type":"object","description":"named parameters (zorm querySql semantics)"}}}})json"),
         toolQuerySql);
  b.tool(Json(R"({"name":"db_trans","description":"ATOMIC multi-statement transaction (MRTR-confirmed). sqls is a non-empty array; each element is either {\"text\": \"sql\", \"values\": [...]} (or \"sql\" instead of \"text\") or structured {\"table\", \"method\": \"insert\"|\"update\"|\"delete\"|\"batch\", \"params\": {...}, \"id\": v}. All statements commit together or all roll back. This is also the escape hatch for condition-based updates/deletes that the single-row write tools cannot express.","inputSchema":{"type":"object","required":["db","sqls"],"properties":{"db":{"type":"string"},"sqls":{"type":"array","items":{"type":"object"},"minItems":1},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolTrans);
  // db_exec_sql is registered separately by buildServer() when --allow-sql is
  // on - hidden by default so AI clients never see a raw-write tool unless the
  // operator opted in.
}

}  // namespace zorm_mcp
