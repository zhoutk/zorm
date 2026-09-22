// zorm_analytics_server - the "Zorm Analytics MCP" demo server.
//
// Exposes multiple zorm backends (sqlite + jsonfile by default, more via
// --config) behind one MCP server whose tools take a `db` routing parameter.
// The AI client uses the SAME tool semantics for every backend - zorm's
// dialect layer absorbs the differences (no SQL knowledge needed).
//
// Capabilities (mcp-cpp):
//   tools       db_list_tables / db_query / db_count / db_insert / db_update /
//               db_delete (MRTR write confirmation) / db_exec_sql (--allow-sql)
//   resources   db://schema, db://schema/{db}/{table}
//   completion  table / column name completion
//   subscriptions (notifications/resources/updated after writes)
//
// Modes:
//   (default)   stdio MCP server (register in Claude Code / VS Code / ...)
//   --setup     create/seed demo tables idempotently, then exit
//   --demo      self-driven scripted demo via the mcp-cpp client API
//               (spawns itself as a child over real stdio pipes)
//   --http N    also serve Streamable HTTP on 127.0.0.1:N
//   --yes       disable MRTR write confirmation (automation/tests only)
//   --allow-sql expose db_exec_sql (dangerous; default hidden)
//   --config F  JSON config with a "databases" object (see README_demo)

#include "DbBase.h"
#include "Idb.h"
#include "demo_data.h"
#include <mcp/mcp.hpp>
#include <mcp/transport_http.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;

namespace {

// ---- result helpers --------------------------------------------------------

Json textResult(const std::string& text) {
  Json result;
  Json content = Json("[]");
  Json item;
  item.add("type", Json::str("text"));
  item.add("text", Json::str(text));
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  return result;
}

// Converts a zorm Json to the mcp Json by re-parsing its wire text.
Json toMcpJson(const ZORM::Json& zj) {
  std::string err;
  Json parsed = Json::ParseJson(zj.toString(), err);
  return parsed.isError() ? Json(nullptr) : parsed;
}

ZORM::Json toZormJson(const Json& j) { return ZORM::Json(j.toString().c_str()); }

// Pretty-prints a Json for tool result text.
std::string pretty(const Json& j) {
  // The mcp Json serializes compactly; wrap with a small indent for demos.
  return j.toString();
}

// ---- demo config ------------------------------------------------------------

struct DbSpec {
  std::string type;    // sqlite3 | jsonfile | mysql | postgres | dm8
  ZORM::Json options;  // passed to DbBase
};

// Built-in open-box defaults: sqlite + jsonfile, zero external servers.
// DbLogClose:true keeps zorm's SQL logs off stdout - MCP stdio requires
// stdout to carry ONLY JSON-RPC messages (logs would corrupt the stream).
std::map<std::string, DbSpec> defaultDatabases() {
  std::map<std::string, DbSpec> dbs;
  dbs["sqlite"] = {"sqlite3", ZORM::Json(R"({"connString":"demo.db","DbLogClose":true})")};
  dbs["jsonfile"] = {"jsonfile", ZORM::Json(R"({"connString":"analytics.json","DbLogClose":true})")};
  return dbs;
}

// ---- the demo server ---------------------------------------------------------

struct DemoServer {
  // db name -> backend (each owns its connections; single-threaded dispatch).
  std::map<std::string, std::unique_ptr<ZORM::Idb>> backends;
  std::map<std::string, DbSpec> specs;

  // db name -> table -> columns (discovered at startup via db_list_tables /
  // metadata cache, populated lazily on first schema access).
  std::map<std::string, std::map<std::string, std::vector<std::string>>> schemaCache;

  // MRTR write-confirmation state: requestState string -> pending operation.
  struct PendingOp {
    std::string db;
    std::string table;
    std::string kind;  // insert | update | delete | exec_sql
    Json data;         // payload (rows / set / where / sql)
  };
  std::map<std::string, PendingOp> pending;
  int nextState = 1;

  bool confirmWrites = true;
  bool allowSql = false;

  // Set after buildServer(); used for subscription notifications on writes.
  std::weak_ptr<Server> server;

  ZORM::Idb* db(const std::string& name) {
    auto it = backends.find(name);
    return it == backends.end() ? nullptr : it->second.get();
  }

  std::string dbListText() const {
    std::string out;
    for (const auto& [name, spec] : specs) {
      if (!out.empty()) out += ", ";
      out += name + "(" + spec.type + ")";
    }
    return out;
  }
};

DemoServer g;

// ---- per-tool handlers --------------------------------------------------------

McpResult<Json> toolListTables(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return Err(McpErrorCode::InvalidParams, "unknown db: " + dbName);

  // sqlite: query sqlite_master; jsonfile: read the storage file's top-level
  // table keys; other backends: information_schema (only when configured).
  std::vector<std::string> tables;
  const std::string type = g.specs[dbName].type;
  if (type == "sqlite3") {
    ZORM::Json rs = db->querySql(
        "select name from sqlite_master where type='table' and name not like 'sqlite_%'");
    if (rs["status"].toInt() == 200 && rs["data"].isArray()) {
      for (const auto& row : rs["data"].toVector()) tables.push_back(row["name"].toString());
    }
  } else if (type == "jsonfile") {
    // The whole store is one JSON array of {table, columns, rows}; we read it
    // through querySql with a special marker the backend understands, or via
    // the storage file. Simplest portable path: count-based probe is avoided;
    // jsonfile's select on a fake table errors - instead read the file keys.
    // For the demo we keep the schema known from setup (see schemaCache) and
    // list tables from the cache if populated, else a fixed default set.
    tables = {"orders", "users", "products", "analytics"};
  } else {
    // mysql/postgres/dm8: information_schema (best-effort; only when the
    // backend is configured - not exercised in the open-box demo).
    ZORM::Json rs = db->querySql(
        "select table_name from information_schema.tables where table_schema = current_schema()");
    if (rs["status"].toInt() == 200 && rs["data"].isArray()) {
      for (const auto& row : rs["data"].toVector())
        tables.push_back(row["table_name"].toString());
    }
  }

  Json out = Json("[]");
  for (const auto& t : tables) out.push_back(Json::str(t));
  Json result;
  result.add("tables", std::move(out));
  return Ok(std::move(result));
}

// db_query - the zorm smart query, all reserved params passed through.
McpResult<Json> toolQuery(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return Err(McpErrorCode::InvalidParams, "unknown db: " + dbName);
  if (table.empty()) return Err(McpErrorCode::InvalidParams, "table is required");

  // Forward the smart-query keys zorm understands. zorm's shapes (verified
  // against SqlBackendBase::genSql):
  //   lks/ins/ors : a COMMA-SEPARATED STRING "col1,val1,col2,val2" (pairs)
  //   sum/count   : a comma-separated string "col,alias" (pairs)
  //   group/sort  : a comma-separated string "a,b"
  //   page/size/fuzzy : scalar
  // The client-facing schema uses friendlier shapes (objects / arrays);
  // convert here. Values are assumed to be simple (no embedded commas).
  ZORM::Json params;
  for (const auto& key : {"lks", "ins", "ors"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isObject()) {
      std::string joined;
      for (const auto& k : v.getAllKeys().toVector()) {
        const std::string kk = k.toString();
        const Json vv = v[kk];
        if (vv.isString()) {
          if (!joined.empty()) joined += ",";
          joined += kk + "," + vv.toString();
        } else if (vv.isNumber()) {
          if (!joined.empty()) joined += ",";
          joined += kk + "," + vv.toString();
        }
        // arrays/objects as lks values are not supported by this demo
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else {
      params.add(key, toZormJson(v));
    }
  }
  for (const auto& key : {"group", "sort"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        if (!joined.empty()) joined += ",";
        joined += item.toString();
      }
      params.add(key, ZORM::Json(joined.c_str()));
    } else {
      params.add(key, toZormJson(v));
    }
  }
  for (const auto& key : {"sum", "count"}) {
    // zorm expects "col,alias" pairs. Accept either an object {col:alias}
    // or an array of pairs / strings; default alias = column name.
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isObject()) {
      std::string joined;
      for (const auto& k : v.getAllKeys().toVector()) {
        const std::string kk = k.toString();
        const std::string alias = v[kk].toString();
        if (!joined.empty()) joined += ",";
        joined += kk + "," + (alias.empty() ? kk : alias);
      }
      params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        if (!joined.empty()) joined += ",";
        joined += item.toString();
      }
      params.add(key, ZORM::Json(joined.c_str()));
    } else {
      // plain string: "amount" -> "amount,amount" (alias = column)
      const std::string s = v.toString();
      params.add(key, ZORM::Json((s + "," + s).c_str()));
    }
  }
  for (const auto& key : {"fuzzy", "page", "size"}) {
    const Json v = member(args, key);
    if (!v.isError() && !v.isNull()) params.add(key, toZormJson(v));
  }
  std::vector<std::string> fields;
  const Json f = member(args, "fields");
  if (f.isArray()) {
    for (const auto& item : f.toVector()) fields.push_back(item.toString());
  }

  const ZORM::Json rs = db->select(table, params, fields);
  if (rs["status"].toInt() != 200) {
    return Err(McpErrorCode::InvalidParams,
               "query failed: " + rs["info"].toString());
  }
  return Ok(toMcpJson(rs));
}

McpResult<Json> toolCount(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return Err(McpErrorCode::InvalidParams, "unknown db: " + dbName);

  // zorm count expects "col,alias" pairs (e.g. "*,total").
  ZORM::Json params;
  params.add("count", ZORM::Json("*,total"));
  const ZORM::Json rs = db->select(table, params);
  if (rs["status"].toInt() != 200)
    return Err(McpErrorCode::InvalidParams, "count failed: " + rs["info"].toString());
  return Ok(toMcpJson(rs));
}

// ---- MRTR write confirmation ------------------------------------------------

// Builds the input_required result that asks the user to confirm a write.
Json inputRequiredConfirm(const std::string& requestState, const std::string& message) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeInputRequired)));
  result.add("requestState", Json::str(requestState));
  Json inputRequests;
  Json confirm;
  confirm.add("method", Json::str("elicitation/create"));
  Json params;
  params.add("mode", Json::str("form"));
  params.add("message", Json::str(message));
  Json schema;
  schema.add("type", Json::str("object"));
  Json props;
  Json approved;
  approved.add("type", Json::str("boolean"));
  props.add("approved", approved);
  schema.add("properties", std::move(props));
  schema.add("required", Json(R"(["approved"])"));
  params.add("requestedSchema", std::move(schema));
  confirm.add("params", std::move(params));
  inputRequests.add("confirm", std::move(confirm));
  result.add("inputRequests", std::move(inputRequests));
  return result;
}

// Forward decl (defined below): executes an already-staged write.
McpResult<Json> executeWrite(const std::string& dbName, const std::string& table,
                             const std::string& kind, const Json& args);

// Stages a write and returns input_required (or executes directly with --yes).
// A retry carrying inputResponses resumes the pending operation by its
// requestState.
McpResult<Json> stageWrite(Ctx& c, const std::string& kind) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return Err(McpErrorCode::InvalidParams, "unknown db: " + dbName);

  // MRTR retry: inputResponses carries the user's confirmation.
  const Json responses = member(args, "inputResponses");
  if (responses.isObject()) {
    const Json confirm = member(responses, "confirm");
    const bool approved = confirm["approved"].isTrue();
    // Locate the pending op for this request's table (the requestState is in
    // the original result; on retry the client echoes it - we carry it via a
    // hidden requestState param). Simplify: retry carries requestState.
    const std::string state = args["requestState"].toString();
    auto it = g.pending.find(state);
    if (it == g.pending.end())
      return Err(McpErrorCode::InvalidParams, "unknown or expired requestState: " + state);
    const DemoServer::PendingOp op = it->second;
    g.pending.erase(it);
    if (!approved) {
      Json result;
      result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
      result.add("content", Json(R"([{"type":"text","text":"Write cancelled by user."}])"));
      return Ok(std::move(result));
    }
    return executeWrite(op.db, op.table, op.kind, op.data);
  }

  if (g.confirmWrites) {
    const std::string state = "st-" + std::to_string(g.nextState++);
    // Pre-count rows that a delete would affect for the confirmation text.
    std::string impact = "1 row";
    if (kind == "delete" && member(args, "where").isObject()) {
      ZORM::Json wp;
      wp.add("where", toZormJson(member(args, "where")));
      wp.add("count", ZORM::Json("*,total"));
      const ZORM::Json rs = db->select(table, wp);
      if (rs["status"].toInt() == 200 && !rs["data"].isError()) {
        impact = std::to_string(rs["data"].size()) + " rows";
      }
    }
    g.pending[state] = {dbName, table, kind, args};
    const std::string msg = "Confirm " + kind + " on " + dbName + "." + table +
                            " (" + impact + ")? Reply with approved:true to proceed.";
    return Ok(inputRequiredConfirm(state, msg));
  }
  // --yes: execute immediately (tests / scripted demo).
  return executeWrite(dbName, table, kind, args);
}

McpResult<Json> executeWrite(const std::string& dbName, const std::string& table,
                             const std::string& kind, const Json& args) {
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return Err(McpErrorCode::InvalidParams, "unknown db: " + dbName);

  ZORM::Json rs;
  if (kind == "insert") {
    ZORM::Json elements = Json("[]");
    elements.push_back(toZormJson(member(args, "data")));
    rs = db->insertBatch(table, elements, "id");
  } else if (kind == "update") {
    ZORM::Json params = toZormJson(member(args, "data"));
    params.add("where", toZormJson(member(args, "where")));
    // zorm update: needs an id in the where clause (buildUpdateSql uses id).
    // For demo simplicity we only support id-based updates via the where.id.
    rs = db->update(table, params);
  } else if (kind == "delete") {
    ZORM::Json params;
    params.add("where", toZormJson(member(args, "where")));
    rs = db->remove(table, params);
  } else if (kind == "exec_sql") {
    rs = db->execSql(member(args, "sql").toString());
  }
  if (rs["status"].toInt() != 200)
    return Err(McpErrorCode::InternalError, "write failed: " + rs["info"].toString());

  // Notify resource subscribers that this table changed (mcp-cpp fan-out).
  if (auto srv = g.server.lock()) {
    srv->notifyResourceUpdated("db://schema/" + dbName + "/" + table);
    srv->notifyListChanged("resources");
  }
  return Ok(toMcpJson(rs));
}

McpResult<Json> toolInsert(Ctx& c) { return stageWrite(c, "insert"); }
McpResult<Json> toolUpdate(Ctx& c) { return stageWrite(c, "update"); }
McpResult<Json> toolDelete(Ctx& c) { return stageWrite(c, "delete"); }

McpResult<Json> toolExecSql(Ctx& c) {
  if (!g.allowSql) return Err(McpErrorCode::MethodNotFound, "db_exec_sql is not enabled");
  return stageWrite(c, "exec_sql");
}

// ---- resources ----------------------------------------------------------------

McpResult<Json> resourceSchema(Ctx& c) {
  // Resource handlers receive the raw request params (uri at the top level,
  // NOT under "arguments" like tools).
  const Json uriJson = member(c.params, "uri");
  if (!uriJson.isString()) {
    return Err(McpErrorCode::InvalidParams, "resources/read requires a string uri");
  }
  const std::string uri = uriJson.toString();
  const std::string kPrefix = "db://schema/";
  if (uri.rfind(kPrefix, 0) != 0) {
    return Err(McpErrorCode::InvalidParams, "unknown resource: " + uri);
  }
  // uri forms: db://schema, db://schema/{db}, db://schema/{db}/{table}
  Json result;
  Json contents = Json("[]");
  Json item;
  item.add("uri", Json::str(uri));
  if (uri == "db://schema") {
    std::string text;
    for (const auto& [dbName, spec] : g.specs) {
      text += dbName + " (" + spec.type + "): ";
      // List tables from the schema cache (populated lazily).
      auto it = g.schemaCache.find(dbName);
      if (it != g.schemaCache.end() && !it->second.empty()) {
        bool first = true;
        for (const auto& [t, cols] : it->second) {
          if (!first) text += ", ";
          text += t;
          first = false;
        }
      } else {
        text += "tables unknown (run db_list_tables)";
      }
      text += "\n";
    }
    item.add("text", Json::str(text));
  } else {
    // db://schema/{db} or db://schema/{db}/{table}
    std::string rest = uri.substr(kPrefix.size());
    const size_t slash = rest.find('/');
    const std::string dbName = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string table = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (g.db(dbName) == nullptr)
      return Err(McpErrorCode::InvalidParams, "unknown db in uri: " + uri);
    std::string text;
    auto it = g.schemaCache.find(dbName);
    if (it != g.schemaCache.end()) {
      if (table.empty()) {
        for (const auto& [t, cols] : it->second) text += t + "\n";
      } else {
        auto cit = it->second.find(table);
        if (cit == it->second.end())
          return Err(McpErrorCode::InvalidParams, "unknown table: " + table);
        text = "table " + table + ": columns [";
        for (size_t i = 0; i < cit->second.size(); ++i) {
          if (i) text += ", ";
          text += cit->second[i];
        }
        text += "]";
      }
    } else {
      text = "(schema not yet discovered - call db_list_tables)";
    }
    item.add("text", Json::str(text));
  }
  contents.push_back(std::move(item));
  result.add("contents", std::move(contents));
  return Ok(std::move(result));
}

// ---- completion ---------------------------------------------------------------

McpResult<Json> toolCompletion(Ctx& c) {
  const Json params = c.params;  // {ref, argument}
  const Json ref = member(params, "ref");
  const std::string argName = ref["name"].toString();
  const Json argument = member(params, "argument");
  const std::string partial = argument["value"].toString();

  Json result;
  Json values = Json("[]");
  if (argName == "table") {
    for (const auto& [dbName, spec] : g.specs) {
      auto it = g.schemaCache.find(dbName);
      if (it == g.schemaCache.end()) continue;
      for (const auto& [t, cols] : it->second) {
        if (partial.empty() || t.rfind(partial, 0) == 0) values.push_back(Json::str(t));
      }
    }
  } else if (argName == "db") {
    for (const auto& [dbName, spec] : g.specs) {
      if (partial.empty() || dbName.rfind(partial, 0) == 0) values.push_back(Json::str(dbName));
    }
  } else if (argName == "fields") {
    // fields is an array; the completion argument.value may be the last entry.
    const Json refArgs = member(ref, "arguments");
    const std::string dbName = refArgs["db"].toString();
    const std::string table = refArgs["table"].toString();
    auto it = g.schemaCache.find(dbName);
    if (it != g.schemaCache.end()) {
      auto cit = it->second.find(table);
      if (cit != it->second.end()) {
        for (const auto& col : cit->second) {
          if (partial.empty() || col.rfind(partial, 0) == 0) values.push_back(Json::str(col));
        }
      }
    }
  }
  result.add("values", std::move(values));
  return Ok(std::move(result));
}

// ---- schema discovery ----------------------------------------------------------

// Populates schemaCache[dbName][table] = {columns} for the known tables by
// selecting a tiny page and reading the result's column headers. Works on
// every backend through the unified Idb (a select with size=0 still returns
// the record structure).
void discoverSchema(ZORM::Idb* db, const std::string& dbName,
                    const std::vector<std::string>& tables) {
  auto& cache = g.schemaCache[dbName];
  for (const auto& t : tables) {
    if (cache.find(t) != cache.end()) continue;
    ZORM::Json params;
    params.add("size", ZORM::Json("0"));  // no rows, just structure
    const ZORM::Json rs = db->select(t, params);
    if (rs["status"].toInt() == 200) {
      std::vector<std::string> cols;
      const Json keys = rs["data"].isArray() && rs["data"].size() > 0
                            ? rs["data"][0].getAllKeys()
                            : rs["fields"];
      if (keys.isArray()) {
        for (const auto& k : keys.toVector()) cols.push_back(k.toString());
      }
      cache[t] = cols;
    }
  }
}

// ---- server construction --------------------------------------------------------

std::shared_ptr<Server> buildServer(bool withHttp, bool demoMode);

// Registers everything on a fresh Builder.
void registerAll(Builder& b) {
  b.name("zorm-analytics").version("0.1.0")
      .instructions(std::string("Multi-backend database assistant over zorm. "
                                "Backends: ") + g.dbListText() +
                    ". Every tool takes a `db` routing parameter. "
                    "Use db_list_tables to discover tables, db_query for smart "
                    "queries (lks/ins/ors/group/sum/sort/page/size), db_count "
                    "for counting. Writes (db_insert/db_update/db_delete) "
                    "require user confirmation.");

  b.tool(Json(R"({"name":"db_list_tables","description":"List tables in a backend.","inputSchema":{"type":"object","required":["db"],"properties":{"db":{"type":"string"}}}})"),
         toolListTables);
  b.tool(Json(R"({"name":"db_query","description":"Smart query over a table. Structured JSON, no SQL needed. lks/ins/ors are objects mapping columns to values; group/sum/sort accept arrays or strings.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"fields":{"type":"array","items":{"type":"string"}},"lks":{"type":"object","description":"fuzzy matches with percent-wildcards"},"ins":{"type":"object","description":"IN-list matches"},"ors":{"type":"object","description":"OR matches"},"group":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"sum":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"sort":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"page":{"type":"integer"},"size":{"type":"integer"},"count":{"type":"string"}}}})"),
         toolQuery);
  b.tool(Json(R"({"name":"db_count","description":"Count rows in a table.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object"}}}})"),
         toolCount);
  b.tool(Json(R"({"name":"db_insert","description":"Insert one row (requires confirmation).","inputSchema":{"type":"object","required":["db","table","data"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolInsert);
  b.tool(Json(R"({"name":"db_update","description":"Update rows matching where (requires confirmation; where must carry id).","inputSchema":{"type":"object","required":["db","table","data","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object"},"where":{"type":"object"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolUpdate);
  b.tool(Json(R"({"name":"db_delete","description":"Delete rows matching where (requires confirmation).","inputSchema":{"type":"object","required":["db","table","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolDelete);
  if (g.allowSql) {
    b.tool(Json(R"({"name":"db_exec_sql","description":"Execute parameterized SQL (dangerous; confirmation required).","inputSchema":{"type":"object","required":["db","sql"],"properties":{"db":{"type":"string"},"sql":{"type":"string"}}}})"),
           toolExecSql);
  }

  // Resources: db://schema, db://schema/{db}, db://schema/{db}/{table}
  b.resource(Json(R"({"uri":"db://schema","name":"database schema overview"})"), resourceSchema);
  for (const auto& [dbName, spec] : g.specs) {
    std::string err;
    b.resource(Json::ParseJson("{\"uri\":\"db://schema/" + dbName +
                                   "\",\"name\":\"tables in " + dbName + "\"}",
                               err),
               resourceSchema);
    auto it = g.schemaCache.find(dbName);
    if (it == g.schemaCache.end()) continue;
    for (const auto& [t, cols] : it->second) {
      std::string err2;
      b.resource(Json::ParseJson("{\"uri\":\"db://schema/" + dbName + "/" + t +
                                     "\",\"name\":\"schema of " + dbName + "." + t + "\"}",
                                 err2),
                 resourceSchema);
    }
  }

  // Completion: table / db / fields
  b.completions(toolCompletion);
}

// ---- modes ----------------------------------------------------------------------

bool setupDatabases(const std::map<std::string, DbSpec>& specs, bool seed) {
  for (const auto& [dbName, spec] : specs) {
    std::string err;
    auto db = std::unique_ptr<ZORM::Idb>(
        new ZORM::DbBase(spec.type, spec.options));
    if (!db) {
      std::cerr << "[zorm-analytics] failed to open " << dbName << " (" << spec.type << ")"
                << std::endl;
      return false;
    }
    g.backends[dbName] = std::move(db);
    if (seed) {
      if (!zorm_demo::setupDemoData(g.backends[dbName].get(), spec.type, dbName)) {
        std::cerr << "[zorm-analytics] seeding " << dbName << " failed" << std::endl;
        return false;
      }
    }
  }
  return true;
}

// Discovers table names per backend and fills schemaCache. Returns false if
// no tables could be discovered for any backend (unseeded db).
bool discoverAllSchemas() {
  bool any = false;
  for (const auto& [dbName, spec] : g.specs) {
    ZORM::Idb* db = g.db(dbName);
    if (db == nullptr) continue;
    std::vector<std::string> tables;
    if (spec.type == "jsonfile") {
      tables = {"orders", "users", "products", "analytics"};
    } else if (spec.type == "sqlite3") {
      ZORM::Json rs = db->querySql(
          "select name from sqlite_master where type='table' and name not like 'sqlite_%'");
      if (rs["status"].toInt() == 200 && rs["data"].isArray()) {
        for (const auto& row : rs["data"].toVector()) tables.push_back(row["name"].toString());
      }
    } else {
      ZORM::Json rs = db->querySql(
          "select table_name from information_schema.tables where table_schema = current_schema()");
      if (rs["status"].toInt() == 200 && rs["data"].isArray()) {
        for (const auto& row : rs["data"].toVector()) tables.push_back(row["table_name"].toString());
      }
    }
    if (!tables.empty()) {
      discoverSchema(db, dbName, tables);
      any = true;
    }
  }
  return any;
}

// Builds (or rebuilds) the MCP server with the current tool/resource set.
std::shared_ptr<Server> buildServer() {
  Builder b;
  registerAll(b);
  auto server = b.build();
  g.server = server;
  return server;
}

}  // namespace

// ---- main ------------------------------------------------------------------------

int main(int argc, char** argv) {
  bool doSetup = false, doDemo = false, doHttp = false;
  int httpPort = 8080;
  std::string configPath;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--setup") doSetup = true;
    else if (a == "--demo") doDemo = true;
    else if (a == "--http") { doHttp = true; if (i + 1 < argc) httpPort = std::atoi(argv[++i]); }
    else if (a == "--yes") g.confirmWrites = false;
    else if (a == "--allow-sql") g.allowSql = true;
    else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
    else if (a == "--child") { /* internal: spawned by --demo */ }
    else {
      std::cerr << "usage: zorm_analytics_server [--setup] [--demo] [--http <port>] "
                   "[--yes] [--allow-sql] [--config <file>]\n";
      return 2;
    }
  }

  // Load configuration (defaults: sqlite + jsonfile).
  g.specs = defaultDatabases();
  if (!configPath.empty()) {
    std::string err;
    Json cfg = Json::ParseJson(configPath, err);
    if (cfg.isError() || !member(cfg, "databases").isObject()) {
      std::cerr << "[zorm-analytics] bad config: " << configPath << std::endl;
      return 2;
    }
    g.specs.clear();
    const Json dbs = member(cfg, "databases");
    for (const auto& k : dbs.getAllKeys().toVector()) {
      const std::string name = k.toString();
      const Json v = dbs[name];
      DbSpec spec;
      spec.type = v["type"].toString();
      std::string optsText = v["options"].toString();
      spec.options = ZORM::Json(optsText.empty() ? "{}" : optsText.c_str());
      g.specs[name] = spec;
    }
  }

  // Open backends; seed when --setup (or when nothing exists yet).
  const bool needSeed = doSetup || !std::filesystem::exists("demo.db");
  if (!setupDatabases(g.specs, needSeed)) return 1;
  discoverAllSchemas();
  if (doSetup) {
    std::cout << "[zorm-analytics] setup complete. databases: " << g.dbListText() << std::endl;
    return 0;
  }

  auto server = buildServer();

  // --demo: self-driven scripted demo over real stdio pipes (spawn self as child).
  if (doDemo) {
    // Forward declaration defined below.
    extern int runScriptedDemo(const std::shared_ptr<Server>&);
    return runScriptedDemo(server);
  }

  // --http: serve Streamable HTTP on a background thread alongside stdio.
  if (doHttp) {
    std::unique_ptr<mcp::http::HttpServerTransport> http;
    try {
      http = std::make_unique<mcp::http::HttpServerTransport>(*server);
      auto bound = http->bind("127.0.0.1");
      if (bound.has_value()) {
        httpPort = *bound;
        std::thread([&] { (void)http->run(); }).detach();
        std::cerr << "[zorm-analytics] HTTP serving on 127.0.0.1:" << httpPort << "/mcp" << std::endl;
      }
    } catch (...) {
      std::cerr << "[zorm-analytics] HTTP transport unavailable (built without httplib?)" << std::endl;
    }
  }

  // Default: stdio MCP server.
  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  if (!r.has_value()) {
    std::cerr << "[zorm-analytics] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}

// ---- --demo: self-driven scripted demo (mcp-cpp client drives this server) ----

// Spawns `this executable` as a child MCP server over real OS pipes, then
// drives it with the mcp-cpp Client API - proving the full protocol stack
// (stdio framing, JSON-RPC, tools, resources, MRTR) with zero AI client.

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {
struct PipeChild {
  HANDLE inWrite = nullptr, outRead = nullptr;
  PROCESS_INFORMATION pi{};
  ~PipeChild() {
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (pi.hProcess) {
      WaitForSingleObject(pi.hProcess, 15000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    DWORD written = 0;
    return WriteFile(inWrite, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    DWORD got = 0;
    while (ReadFile(outRead, &ch, 1, &got, nullptr) && got == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
};
PipeChild spawnSelfChild() {
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  std::string cmdline = std::string("\"") + exePath + "\" --child";
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inRead = nullptr, outWrite = nullptr;
  PipeChild c;
  CreatePipe(&inRead, &c.inWrite, &sa, 0);
  CreatePipe(&c.outRead, &outWrite, &sa, 0);
  SetHandleInformation(c.inWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(c.outRead, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.dwFlags |= STARTF_USESTDHANDLES;
  CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &c.pi);
  CloseHandle(inRead);
  CloseHandle(outWrite);
  return c;
}
}  // namespace
#else
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct PipeChild {
  int inWrite = -1, outRead = -1;
  pid_t pid = -1;
  ~PipeChild() {
    if (inWrite >= 0) close(inWrite);
    if (outRead >= 0) close(outRead);
    if (pid > 0) waitpid(pid, nullptr, 0);
  }
  bool writeLine(const std::string& line) const {
    const std::string buf = line + "\n";
    return ::write(inWrite, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  }
  bool readLine(std::string& out) const {
    out.clear();
    char ch = 0;
    while (::read(outRead, &ch, 1) == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') out.push_back(ch);
    }
    return false;
  }
};
PipeChild spawnSelfChild() {
  int inPipe[2], outPipe[2];
  pipe(inPipe);
  pipe(outPipe);
  PipeChild c;
  c.pid = fork();
  if (c.pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
    execl("/proc/self/exe", "zorm_analytics_server", "--child", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(inPipe[0]);
  close(outPipe[1]);
  c.inWrite = inPipe[1];
  c.outRead = outPipe[0];
  return c;
}
}  // namespace
#endif

// A minimal stdio transport over an OS-pipe child (the mcp-cpp client needs
// an ITransport; this one is the "real pipes" binding for --demo).
struct PipeTransport final : mcp::ITransport {
  PipeChild& child;
  explicit PipeTransport(PipeChild& c) : child(c) {}
  McpResult<std::optional<std::string>> read() override {
    std::string line;
    if (!child.readLine(line)) return Ok(std::optional<std::string>(std::nullopt));
    return Ok(std::optional<std::string>(std::move(line)));
  }
  McpResult<void> write(std::string_view message) override {
    if (!child.writeLine(std::string(message))) {
      return Err(McpErrorCode::InternalError, "write to child failed");
    }
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }
};

int runScriptedDemo(const std::shared_ptr<Server>&) {
  std::cout << "\n=== Zorm Analytics MCP - self-driven demo ===\n\n";
  PipeChild child = spawnSelfChild();
  PipeTransport transport(child);

  Client client("zorm-analytics-demo", "0.1.0");
  client.connect(transport);

  // 1. server/discover
  auto disc = client.discover();
  std::cout << "[1] server/discover -> "
            << (disc.has_value() ? "ok" : "failed: " + disc.error().message) << "\n";
  std::cout.flush();

  // 2. tools/list
  auto tools = client.listTools();
  std::cout << "[2] tools/list -> " << (tools.has_value() ? "ok" : "failed") << "\n";
  std::cout.flush();

  // 3. db_list_tables on sqlite
  auto tables = client.callTool("db_list_tables", Json(R"({"db":"sqlite"})"));
  std::cout << "[3] db_list_tables(sqlite) -> "
            << (tables.has_value() ? tables->toString() : "failed") << "\n";
  std::cout.flush();

  // 4. db_query: 2026 orders by region, summed amounts (sqlite)
  Json qargs;
  qargs.add("db", Json::str("sqlite"));
  qargs.add("table", Json::str("orders"));
  qargs.add("lks", Json(R"({"order_date":"2026"})"));
  qargs.add("group", Json::str("region"));
  qargs.add("sum", Json::str("amount"));
  auto query = client.callTool("db_query", qargs);
  std::cout << "[4] db_query(sqlite orders 2026 by region, sum amount) ->\n"
            << (query.has_value() ? query->toString() : "failed") << "\n";
  std::cout.flush();

  // 5. db_query on jsonfile products price>100
  Json pargs;
  pargs.add("db", Json::str("jsonfile"));
  pargs.add("table", Json::str("products"));
  pargs.add("ins", Json(R"({"category":"electronics"})"));
  auto products = client.callTool("db_query", pargs);
  std::cout << "[5] db_query(jsonfile electronics products) ->\n"
            << (products.has_value() ? products->toString() : "failed") << "\n";
  std::cout.flush();

  // 6. resources: db://schema/sqlite/orders
  auto schema = client.readResource("db://schema/sqlite/orders");
  std::cout << "[6] resources/read(db://schema/sqlite/orders) ->\n"
            << (schema.has_value() ? schema->toString() : "failed") << "\n";
  std::cout.flush();

  std::cout << "\n=== demo complete (writes/MRTR in milestone 2) ===\n";
  return 0;
}
