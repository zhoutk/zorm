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
//   --auto      true (default) = 全自动演示; false = 每步暂停等按键 + 验证提示
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
#ifdef _WIN32
#include <io.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
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

// Wraps a structured tool result in the MCP-spec tools/call envelope:
//   {"content":[{type:"text", text:"<json>"}], <structured fields>...}
// The `content` array is REQUIRED by the spec (AI clients iterate it - a
// bare structured result makes them fail with "r.content is not iterable").
// The structured fields are also flattened at the top level so programmatic
// clients (and the e2e tests) keep their old access paths.
Json toolResult(const Json& structured) {
  Json result = textResult(structured.toString());
  if (structured.isObject()) {
    for (const auto& k : structured.getAllKeys().toVector()) {
      const std::string kk = k.toString();
      result.add(kk, structured[kk]);
    }
  }
  return result;
}

// Converts a zorm Json to the mcp Json by re-parsing its wire text.
Json toMcpJson(const ZORM::Json& zj) {
  std::string err;
  Json parsed = Json::ParseJson(zj.toString(), err);
  return parsed.isError() ? Json(nullptr) : parsed;
}

// zorm 的"成功"状态码: STSUCCESS(200) 正常返回, STQUERYEMPTY(202) 查无结果。
// 202 不是错误 - jsonfile 后端对"查无结果"返回 202 (SQL 后端统一返回 200),
// AI 客户端应当把 202 视为空结果集而不是失败。所有工具判断状态时都走这里。
bool okStatus(const ZORM::Json& rs) {
  const int code = rs["status"].toInt();
  return code == 200 || code == 202;
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
  // parameterized:true - 使用参数化 SQL(占位符绑定), 否则非参数化路径会把
  // ins/lks 的字符串值裸拼进 SQL(如 in (electronics)), 导致 "no such column"。
  dbs["sqlite"] = {"sqlite3", ZORM::Json(R"({"connString":"demo.db","DbLogClose":true,"parameterized":true})")};
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

// ---- jsonfile schema helpers ---------------------------------------------------

// jsonfile 后端没有"列出所有表"的 API(metadataQueryResult 只查单表), 而输出库
// 的表是运行时动态创建的(sqlite 测试数据 -> 汇总结果写入 jsonfile)。这里直接读
// 存储文件(demo 是纯 JSON 数组 [{table, rows}, ...]), 返回实际存在的表名。
// 空/缺失文件返回空表(与后端语义一致)。
std::vector<std::string> jsonfileTableNames(const ZORM::Json& options) {
  std::vector<std::string> tables;
  const std::string file = options["connString"].toString();
  std::ifstream in(file, std::ios::binary);
  if (!in) return tables;  // 文件不存在/不可读 -> 空表
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string err;
  Json doc = Json::ParseJson(raw, err);
  if (doc.isError() || !doc.isArray()) return tables;
  for (const auto& item : doc.toVector()) {
    if (item.isObject() && item.contains("table")) tables.push_back(item["table"].toString());
  }
  return tables;
}

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
    // jsonfile 是"输出库": 表由运行时写入自动创建(首次 insert 建表)。动态
    // 读取存储文件实际存在的表 - 空文件返回空清单。
    tables = jsonfileTableNames(g.specs[dbName].options);
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
  return Ok(toolResult(std::move(result)));
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
  // 透传其余 key 作为普通列条件: 等值 {price:100} 或带比较操作符的
  // 字符串 {price:">,100"}(zorm 比较前缀: >, >=, <, <=, <>, =)。
  // 这样 AI 客户端可以写 {price:">,100"} 查单价大于 100 的产品。
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string kk = k.toString();
    if (kk == "db" || kk == "table" || kk == "fields" || kk == "lks" || kk == "ins" ||
        kk == "ors" || kk == "group" || kk == "sort" || kk == "sum" || kk == "count" ||
        kk == "fuzzy" || kk == "page" || kk == "size") {
      continue;  // 保留 key 已处理
    }
    const Json v = member(args, kk);
    if (v.isError() || v.isNull()) continue;
    // 数值/布尔直接透传; 字符串若带比较前缀(>, >= 等)原样透传, 否则当等值。
    params.add(kk, toZormJson(v));
  }
  std::vector<std::string> fields;
  const Json f = member(args, "fields");
  if (f.isArray()) {
    for (const auto& item : f.toVector()) fields.push_back(item.toString());
  }

  const ZORM::Json rs = db->select(table, params, fields);
  if (!okStatus(rs)) {
    return Err(McpErrorCode::InvalidParams,
               "query failed: " + rs["message"].toString());
  }
  return Ok(toolResult(toMcpJson(rs)));
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
  if (!okStatus(rs))
    return Err(McpErrorCode::InvalidParams, "count failed: " + rs["message"].toString());
  return Ok(toolResult(toMcpJson(rs)));
}

// ---- MRTR write confirmation ------------------------------------------------

// Builds the input_required result that asks the user to confirm a write.
Json inputRequiredConfirm(const std::string& requestState, const std::string& message) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeInputRequired)));
  // CallToolResult requires content even for input_required results.
  Json content = Json(R"([])");
  Json text;
  text.add("type", Json::str("text"));
  text.add("text", Json::str(message));
  content.add(text);
  result.add("content", std::move(content));
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
      // 预计影响行数: select 的 where 条件在顶层(与 zorm 语义一致),
      // count 是保留字, 由后端聚合。
      ZORM::Json wp;
      const Json where = member(args, "where");
      if (where.contains("id")) {
        wp.add("id", toZormJson(where["id"]));
      } else {
        wp.add("where", toZormJson(where));
      }
      wp.add("count", ZORM::Json("*,total"));
      const ZORM::Json rs = db->select(table, wp);
      if (okStatus(rs) && !rs["data"].isError()) {
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
    // zorm update: params must carry the target id at the TOP level
    // (buildUpdateSql reads params["id"]) plus the columns to change.
    // The client-facing shape is {data:{cols}, where:{id}} - flatten here.
    ZORM::Json params = toZormJson(member(args, "data"));
    const Json where = member(args, "where");
    const std::string id = where["id"].toString();
    if (!id.empty()) params.add("id", ZORM::Json(id.c_str()));
    rs = db->update(table, params);
  } else if (kind == "delete") {
    // zorm remove: params["id"] at the TOP level (buildDeleteSql / jsonfile
    // remove both require it). The client-facing where:{id} flattens here.
    ZORM::Json params;
    const Json where = member(args, "where");
    params.add("id", toZormJson(where["id"]));
    rs = db->remove(table, params);
  } else if (kind == "exec_sql") {
    rs = db->execSql(member(args, "sql").toString());
  }
  if (!okStatus(rs))
    return Err(McpErrorCode::InternalError, "write failed: " + rs["message"].toString());

  // Notify resource subscribers that this table changed (mcp-cpp fan-out).
  if (auto srv = g.server.lock()) {
    srv->notifyResourceUpdated("db://schema/" + dbName + "/" + table);
    srv->notifyListChanged("resources");
  }
  return Ok(toolResult(toMcpJson(rs)));
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
  b.tool(Json(R"({"name":"db_query","description":"Smart query over a table. Structured JSON, no SQL needed. lks/ins/ors are objects mapping columns to values; group/sum/sort accept arrays or strings; any other key is a plain column condition (equal, or a string with a compare prefix like \">,100\" for greater-than).","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"fields":{"type":"array","items":{"type":"string"}},"lks":{"type":"object","description":"fuzzy matches with percent-wildcards"},"ins":{"type":"object","description":"IN-list matches"},"ors":{"type":"object","description":"OR matches"},"group":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"sum":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"sort":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}]},"page":{"type":"integer","minimum":1},"size":{"type":"integer","minimum":1,"maximum":500},"count":{"type":"string"}},"additionalProperties":true}})"),
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
    if (seed && spec.type != "jsonfile") {
      // 设计: 测试数据全部放在 sqlite; jsonfile 是"输出库", 保持空文件,
      // 由演示步骤在运行时写入(首次写入自动建表)。
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
      // 输出库: 表运行时动态创建, 从存储文件读取实际表名(空文件 -> 无表)。
      tables = jsonfileTableNames(spec.options);
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
  b.acceptLegacyClients(true);  // Claude Code / VS Code speak legacy initialize
  // Tools are version-agnostic; speak every protocol revision so legacy
  // clients (initialize with 2025-06-18 etc.) negotiate successfully.
  b.supports(ProtocolVersion::V2024_11_05)
      .supports(ProtocolVersion::V2025_03_26)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::V2025_11_25)
      .supports(ProtocolVersion::Latest);
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
  bool autoMode = true;  // --auto 默认 true: 自动完成演示
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--setup") doSetup = true;
    else if (a == "--demo") doDemo = true;
    else if (a == "--http") { doHttp = true; if (i + 1 < argc) httpPort = std::atoi(argv[++i]); }
    else if (a == "--yes") g.confirmWrites = false;
    else if (a == "--allow-sql") g.allowSql = true;
    else if (a == "--auto" && i + 1 < argc) {
      const std::string v = argv[++i];
      // 接受 "true/false" 或 "1/0" 或 "yes/no"
      autoMode = (v == "true" || v == "1" || v == "yes");
    }
    else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
    else if (a == "--child") { /* internal: spawned by --demo */ }
    else {
      std::cerr << "usage: zorm_analytics_server [--setup] [--demo] [--auto true|false] "
                   "[--http <port>] [--yes] [--allow-sql] [--config <file>]\n";
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
    // --setup: sqlite 已 seed; jsonfile 是"输出库", 重置为空文件(写空数组),
    // 保证每次演示从干净的输出起点开始。
    for (const auto& [dbName, spec] : g.specs) {
      if (spec.type != "jsonfile") continue;
      std::ofstream out(spec.options["connString"].toString(),
                        std::ios::binary | std::ios::trunc);
      if (out) out << "[]";
    }
    std::cout << "[zorm-analytics] setup complete. databases: " << g.dbListText() << std::endl;
    return 0;
  }

  auto server = buildServer();

  // --demo: self-driven scripted demo over real stdio pipes (spawn self as child).
  if (doDemo) {
    // Forward declaration defined below.
    extern int runScriptedDemo(const std::shared_ptr<Server>&, bool autoMode);
    return runScriptedDemo(server, autoMode);
  }

  // --http: HTTP-ONLY serving (stdio not run).
  std::unique_ptr<mcp::http::HttpServerTransport> http;
  if (doHttp) {
    try {
      http = std::make_unique<mcp::http::HttpServerTransport>(*server);
      if (http->runAsync("127.0.0.1", httpPort).has_value()) {
        std::cerr << "[zorm-analytics] HTTP serving on 127.0.0.1:" << httpPort << "/mcp"
                  << std::endl;
      } else {
        std::cerr << "[zorm-analytics] HTTP listen failed" << std::endl;
        http.reset();
      }
    } catch (...) {
      std::cerr << "[zorm-analytics] HTTP transport unavailable (built without httplib?)"
                << std::endl;
    }
    // Deterministic: --http means HTTP-ONLY serving (the stdio loop would
    // exit on immediate EOF when stdin is /dev/null, killing the process).
    if (http) {
      while (http->running()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return 0;
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
      if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
      }
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
  void closeStdin() {
    if (inWrite) {
      CloseHandle(inWrite);
      inWrite = nullptr;
    }
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
  void closeStdin() {
    if (inWrite >= 0) {
      close(inWrite);
      inWrite = -1;
    }
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

#ifdef _WIN32
// Windows 10+ terminals support ANSI/VT once enabled on the stdout handle.
// Also switch the console code page to UTF-8 (CP_UTF8 / 65001) so the
// Chinese demo text displays correctly in cmd / PowerShell / mintty alike.
// Notes:
//  - SetConsoleOutputCP affects the console output codepage; without it,
//    cmd (cp936/GBK) renders the UTF-8 byte stream as mojibake.
//  - Only call when attached to a real console (GetConsoleMode succeeds);
//    piped stdout (e.g. run-demo.sh | cat, the --child server) must NOT be
//    touched - the parent terminal owns the console.
void enableVirtualTerminal() {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (GetConsoleMode(out, &mode)) {
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // Best-effort: switch both input and output to UTF-8. The console may
    // refuse (older Windows) - output stays readable in mintty regardless.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
  }
}
#else
void enableVirtualTerminal() {}
#endif

namespace demo_color {
const char* kReset = "[0m";
const char* kCyan = "[36m";
const char* kGreen = "[32m";
const char* kYellow = "[33m";
const char* kMagenta = "[35m";
const char* kBold = "[1m";
}  // namespace demo_color

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

int runScriptedDemo(const std::shared_ptr<Server>&, bool autoMode) {
  using namespace demo_color;
  enableVirtualTerminal();

  // 演示节奏控制: 默认模拟真人操作节奏(每步停顿), 设 ZORM_DEMO_FAST=1
  // 可加速(CI/自动化), 设 ZORM_DEMO_STEP_MS=<毫秒> 自定义每步停顿。
  auto demoDelayMs = []() -> long {
    static const long step = []() -> long {
      const char* v = std::getenv("ZORM_DEMO_STEP_MS");
      if (v != nullptr && *v != '\0') return std::strtol(v, nullptr, 10);
      return 1200L;  // 默认每步约 1.2 秒
    }();
    return step;
  };
  const bool fastMode = std::getenv("ZORM_DEMO_FAST") != nullptr;
  auto pause = [&](long ms) {
    if (!fastMode) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  // 模拟"AI 正在处理请求"的间隔(请求发出后到结果返回前的思考时间)。
  auto thinking = [&]() {
    if (fastMode) return;
    std::cout << kYellow << "      ··· 调用中" << kReset << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::cout << "\r      \r" << std::flush;  // 清掉占位符
  };
  // 分步模式(--auto false): 每步结束后暂停, 提示用户如何验证刚才这一步。
  // 用户按键继续; 显示"q"可提前结束演示。
  PipeChild child = spawnSelfChild();
  PipeTransport transport(child);
  Client client("zorm-analytics-demo", "0.1.0");
  client.connect(transport);

  // 分步模式(--auto false): 每步结束后暂停, 提示用户如何验证刚才这一步。
  // 用户按键继续; 输入 q 提前结束演示(返回 false)。自动模式直接返回 true。
  auto stepDone = [&](const std::string& verifyHint) -> bool {
    if (autoMode) return true;  // 自动模式: 不停顿, 继续
    std::cout << "\n" << kMagenta << "  ── 本步已完成 ──" << kReset << "\n";
    if (!verifyHint.empty()) {
      std::cout << kYellow << "  验证方法: " << kReset << verifyHint << "\n";
    }
    std::cout << kCyan << "  按回车键继续下一演示步骤 (输入 q 退出)... " << kReset << std::flush;
    std::string line;
    std::getline(std::cin, line);
    if (line == "q" || line == "Q" || line == "quit") {
      std::cout << kYellow << "  演示提前结束。\n" << kReset;
      return false;
    }
    return true;
  };
  // 每步调用 stepDone 后的统一检查: 返回 false 则优雅关停子进程并结束演示。
  auto afterStep = [&](bool keepGoing) {
    if (!keepGoing) {
      child.closeStdin();
#ifdef _WIN32
      WaitForSingleObject(child.pi.hProcess, 3000);
#else
      waitpid(child.pid, nullptr, 0);
#endif
      std::cout << kYellow << "(演示已由用户提前结束。)\n" << kReset;
      return true;  // 需要结束整个函数
    }
    return false;
  };

  std::cout << "\n" << kBold << "=== Zorm Analytics MCP - 自驱动演示 ===" << kReset << "\n";
  std::cout << kCyan << "一个 AI 助手，多种数据库，零 SQL 方言差异。\n" << kReset;
  std::cout << "(通过真实 OS 管道驱动一个 MCP 子进程服务器，由 mcp-cpp 客户端 API 调用 - "
               "与 AI 客户端完全相同的调用路径)\n\n";
  // 数据库文件位置: 与当前工作目录相关, 用绝对路径打印, 方便用户去查看/验证。
  std::cout << kYellow << "  两个演示数据库文件:\n" << kReset;
  std::error_code ec;
  const auto sqlitePath = std::filesystem::absolute("demo.db", ec);
  const auto jsonPath = std::filesystem::absolute("analytics.json", ec);
  std::cout << "    sqlite   : " << (ec ? "demo.db" : sqlitePath.string())
            << "  [测试数据] orders/users/products 3 张表\n";
  std::cout << "    jsonfile : " << (ec ? "analytics.json" : jsonPath.string())
            << "  [输出库] 初始为空, 演示过程写入汇总结果\n";
  std::cout << kReset;
  if (!autoMode) {
    std::cout << kCyan << "\n  当前为【分步演示模式】: 每步结束后会暂停, 可按提示打开数据库文件验证.\n"
              << kReset;
  } else {
    std::cout << kYellow << "  自动模式: 如需逐步验证, 用 ./demo/run-demo.sh --auto false 重新运行\n"
              << kReset;
  }
  pause(demoDelayMs() * 2);  // 开场让观众读完说明

  int step = 0;
  auto headline = [&](const char* text) {
    std::cout << "\n" << kBold << "[" << ++step << "] " << text << kReset << "\n";
    pause(demoDelayMs());  // 标题后停顿, 让观众读
  };

  // [1] discover
  headline("server/discover - 协议版本协商与能力发现");
  std::cout << kYellow << "  请求: server/discover  <- 服务器告知支持的协议版本与能力\n" << kReset;
  thinking();
  auto disc = client.discover();
  if (disc.has_value()) {
    const Json versions = member(*disc, "supportedVersions");
    std::cout << kGreen << "  -> ok" << kReset << "  支持 " << versions.size()
              << " 个协议版本: ";
    for (const auto& v : versions.toVector()) std::cout << v.toString() << " ";
    std::cout << "\n";
    std::cout << kYellow << "  说明: 新版(2026)客户端无需握手, 每请求自带协议元数据;\n"
              << "       旧版(Claude Code 等)走 legacy initialize - 服务器两者都兼容。\n" << kReset;
    pause(demoDelayMs());
    if (afterStep(stepDone("这是一个协议层步骤, 无需查数据库; 观察上方返回的 5 个版本号即可。"))) return 0;
  } else {
    std::cout << kYellow << "  discover failed: " << disc.error().message << "\n" << kReset;
  }

  // [2] tools/list
  headline("tools/list - 整个数据库操作面, 不写一行 SQL");
  thinking();
  auto tools = client.listTools();
  if (tools.has_value()) {
    const Json arr = member(*tools, "tools");
    std::cout << kGreen << "  -> ok" << kReset << "  共 " << arr.size() << " 个工具: ";
    for (const auto& t : arr.toVector()) std::cout << kCyan << t["name"].toString() << kReset << " ";
    std::cout << "\n";
    std::cout << kYellow << "  说明: 这 6 个工具覆盖 查表/查询/计数/增/改/删, 对所有后端一致。\n" << kReset;
    pause(demoDelayMs());
    if (afterStep(stepDone("协议层步骤; 观察上方的工具清单即可。"))) return 0;
  } else {
    std::cout << kYellow << "  tools/list failed\n" << kReset;
  }

  // [3] db_list_tables
  headline("db_list_tables(sqlite) - schema discovery 表清单发现");
  std::cout << kYellow << "  请求: db_list_tables {db: \"sqlite\"}  <- 一个方法，不写任何 SQL\n" << kReset;
  thinking();
  auto tables = client.callTool("db_list_tables", Json(R"({"db":"sqlite"})"));
  if (tables.has_value()) {
    const Json arr = member(*tables, "tables");
    std::cout << kGreen << "  -> ok" << kReset << "  共发现 " << arr.size() << " 张表:\n";
    // 真实返回的表清单（与 AI 客户端看到的一致），附中文说明。
    for (const auto& t : arr.toVector()) {
      const std::string name = t.toString();
      std::cout << "      - " << kCyan << name << kReset;
      if (name == "orders") std::cout << "  订单表 (500 行: 地区/日期/金额/状态/商品)";
      else if (name == "users") std::cout << "  用户表 (50 行: 姓名/注册日期)";
      else if (name == "products") std::cout << "  产品表 (20 行: 名称/价格/类别)";
      else if (name == "analytics") std::cout << "  分析结果表 (演示写操作的落点)";
      std::cout << "\n";
    }
    std::cout << kYellow << "  说明: 同一句调用换 db 参数即可查其他库 - 五种后端零差异\n" << kReset;
    pause(demoDelayMs() * 2);  // 表清单信息量大, 多停一会儿
    if (afterStep(stepDone("打开 demo.db 查看: 终端执行  sqlite3 demo.db \".tables\"  (应看到 4 张表: "
                             "orders/users/products/analytics)。"))) return 0;
  } else {
    std::cout << kYellow << "  db_list_tables failed: " << tables.error().message << "\n" << kReset;
  }

  // [4] smart query: group + sum (基于 sqlite 测试数据)
  headline("db_query - 智能查询: 2026 年订单按地区汇总销售额 (数据在 sqlite)");
  std::cout << kYellow << "  请求: db_query {db:\"sqlite\", table:\"orders\", lks:{order_date:\"2026\"}, "
               "group:\"region\", sum:\"amount\"}\n"
            << kReset
            << kYellow << "  含义: 测试数据放在 sqlite - 过滤 2026 年订单 -> 按地区分组 -> 求和金额。"
               "全程无 SQL。\n" << kReset;
  Json qargs;
  qargs.add("db", Json::str("sqlite"));
  qargs.add("table", Json::str("orders"));
  qargs.add("lks", Json(R"({"order_date":"2026"})"));
  qargs.add("group", Json::str("region"));
  qargs.add("sum", Json::str("amount"));
  thinking();
  auto query = client.callTool("db_query", qargs);
  if (query.has_value()) {
    const Json data = member(*query, "data");
    double total = 0.0;
    std::cout << kGreen << "  -> ok" << kReset << "  2026 年各地区销售额汇总:\n";
    for (const auto& row : data.toVector()) {
      char sumBuf[64];
      std::snprintf(sumBuf, sizeof(sumBuf), "%.2f", row["amount"].toDouble());
      total += row["amount"].toDouble();
      std::cout << "      - " << kCyan << row["region"].toString() << kReset
                << " 地区: " << kMagenta << sumBuf << kReset << "\n";
      pause(demoDelayMs() / 2);  // 逐行显示, 模拟 AI 逐条输出
    }
    char totalBuf[64];
    std::snprintf(totalBuf, sizeof(totalBuf), "%.2f", total);
    std::cout << kYellow << "      合计: " << totalBuf << "  (等价 SQL: select region, sum(amount) "
                 "from orders where order_date like '%2026%' group by region)\n"
              << kReset;
    pause(demoDelayMs() * 2);
    if (afterStep(stepDone("用 SQL 交叉验证(sqlite 测试库): 终端执行  sqlite3 demo.db \"select region, "
                             "sum(amount) from orders where order_date like '%2026%' group by region;\"  "
                             "应得到相同的各地区金额。下一步会把结果写入 jsonfile 输出库。"))) return 0;
  } else {
    std::cout << kYellow << "  db_query failed: " << query.error().message << "\n" << kReset;
  }

  // [4b] 把汇总结果写入 jsonfile 的 region_sales 表 (MRTR 确认后落盘)
  headline("db_insert - 汇总结果写入 jsonfile.region_sales (输出库落盘)");
  std::cout << kYellow << "  含义: 测试数据在 sqlite, jsonfile 是输出库。把刚才的汇总结果写进去, "
               "json 内容少、方便查看。\n" << kReset;
  if (query.has_value()) {
    const Json data = member(*query, "data");
    // 逐地区写入 (每行一条, 演示多行批量插入的真实感; 数据量小, 逐条确认太啰嗦,
    // 用一次 MRTR 确认 + 循环 retry 的简化方式: 先插第一条看确认流程, 其余跟进)。
    bool firstRow = true;
    int writtenCount = 0;  // 实际写入成功的行数(不是查询结果行数!)
    for (const auto& row : data.toVector()) {
      Json iargs;
      iargs.add("db", Json::str("jsonfile"));
      iargs.add("table", Json::str("region_sales"));
      Json rowData;
      rowData.add("region", Json::str(row["region"].toString()));
      char sumBuf[64];
      std::snprintf(sumBuf, sizeof(sumBuf), "%.2f", row["amount"].toDouble());
      rowData.add("total_amount", Json::str(sumBuf));
      iargs.add("data", std::move(rowData));
      thinking();
      auto ires = client.callTool("db_insert", iargs);
      if (ires.has_value() && (*ires)["resultType"].toString() == "input_required") {
        const std::string state = (*ires)["requestState"].toString();
        if (firstRow) {
          std::cout << kGreen << "  -> 服务器请求确认 (MRTR)" << kReset << "\n";
          std::cout << kYellow << "  提示: " << kReset
                    << (*ires)["inputRequests"]["confirm"]["params"]["message"].toString() << "\n";
          pause(demoDelayMs());
          std::cout << kMagenta << "  用户点击『批准』 -> 带 inputResponses 重发" << kReset << "\n";
          pause(demoDelayMs());
          firstRow = false;
        }
        Json iretry;
        iretry.add("db", Json::str("jsonfile"));
        iretry.add("table", Json::str("region_sales"));
        Json ird;
        ird.add("region", Json::str(row["region"].toString()));
        char sBuf[64];
        std::snprintf(sBuf, sizeof(sBuf), "%.2f", row["amount"].toDouble());
        ird.add("total_amount", Json::str(sBuf));
        iretry.add("data", std::move(ird));
        iretry.add("requestState", Json::str(state));
        Json iresp;
        Json icf;
        icf.add("approved", Json(true));
        iresp.add("confirm", std::move(icf));
        iretry.add("inputResponses", std::move(iresp));
        thinking();
        auto isecond = client.callTool("db_insert", iretry);
        if (isecond.has_value()) {
          // executeWrite 返回成功(200/202)才算真正落盘; has_value() 只说明
          // MCP 响应正常, 写入可能被 zorm 拒绝(如文件被占用)。
          const Json res = *isecond;
          const bool ok = res["status"].isNumber() && res["status"].toInt() == 200;
          if (ok) {
            ++writtenCount;
            std::cout << kGreen << "  -> 已写入 " << kCyan << row["region"].toString()
                      << kReset << " 地区: " << sBuf << "\n";
          } else {
            std::cout << kYellow << "  -> 写入失败: " << res["message"].toString() << "\n" << kReset;
          }
        } else {
          std::cout << kYellow << "  -> 写入失败: " << isecond.error().message << "\n" << kReset;
        }
        pause(demoDelayMs() / 2);
      }
    }
    if (writtenCount > 0) {
      std::cout << kGreen << "  -> ok" << kReset << "  " << writtenCount
                << " 个地区报告已写入, 用 db_count 确认实际落盘行数\n";
      // 主动验证: 实际查询 jsonfile 里的 region_sales 行数, 与提示一致。
      auto verifyCnt = client.callTool("db_count",
                                       Json(R"({"db":"jsonfile","table":"region_sales"})"));
      long long actualRows = 0;
      if (verifyCnt.has_value()) {
        const Json vdata = member(*verifyCnt, "data");
        if (vdata.isArray() && vdata.size() > 0) actualRows = vdata[0]["total"].toInt64();
      }
      std::cout << kGreen << "  -> 实际落盘 " << kMagenta << actualRows << kReset
                << " 行 (db_count 验证)\n";
      pause(demoDelayMs());
      if (afterStep(stepDone("打开 analytics.json: 现在应有 region_sales 表, " +
                               std::to_string(actualRows) + " 行(每个地区一行), "
                               "字段 {region, total_amount}。"))) return 0;
    } else {
      std::cout << kYellow << "  -> 没有任何地区写入成功! 检查 analytics.json 是否被其他程序打开占用。\n"
                << kReset;
      pause(demoDelayMs());
    }
  }

  // [5] resources
  headline("resources/read - 服务器自描述 schema (资源读取)");
  std::cout << kYellow << "  请求: resources/read db://schema/sqlite/orders\n" << kReset;
  thinking();
  auto schema = client.readResource("db://schema/sqlite/orders");
  if (schema.has_value()) {
    const Json contents = member(*schema, "contents");
    if (contents.isArray() && contents.size() > 0) {
      std::cout << kGreen << "  -> ok" << kReset << "  服务器返回 orders 表结构:\n";
      std::cout << "      " << contents[0]["text"].toString() << "\n";
    }
    std::cout << kYellow << "  说明: 资源(MCP resources)让客户端在动手查询前先了解数据结构。\n" << kReset;
    pause(demoDelayMs());
    if (afterStep(stepDone("用 SQL 交叉验证: 终端执行  sqlite3 demo.db \".schema orders\"  应显示 orders 表的 "
                             "6 个字段 (id/region/order_date/amount/status/product)。"))) return 0;
  } else {
    std::cout << kYellow << "  resources/read failed: " << schema.error().message << "\n" << kReset;
  }

  // [6] 完整操作: sqlite 2026 订单按地区汇总 + sqlite 高价产品 -> 对比 -> 写 jsonfile
  headline("db_query 完整对比 - 2026 订单汇总 vs 单价>100 产品 (数据都在 sqlite)");
  std::cout << kYellow << "  请求1: db_query {db:\"sqlite\", table:\"orders\", lks:{order_date:\"2026\"}, "
               "group:\"region\", sum:\"amount\"}\n"
            << kReset
            << kYellow << "  请求2: db_query {db:\"sqlite\", table:\"products\", price:\">,100\"}\n"
            << kReset
            << kYellow << "  含义: 2026 年订单按地区分组汇总金额, 再查出单价超过 100 的产品做对比。\n"
            << kReset;
  // 高价产品查询 (products 在 sqlite, 用比较操作符 >,100)
  Json pargs;
  pargs.add("db", Json::str("sqlite"));
  pargs.add("table", Json::str("products"));
  pargs.add("price", Json::str(">,100"));
  thinking();
  auto products = client.callTool("db_query", pargs);
  int expensiveCount = 0;
  double expensiveTotal = 0.0;
  if (products.has_value()) {
    const Json data = member(*products, "data");
    expensiveCount = static_cast<int>(data.size());
    std::cout << kGreen << "  -> ok" << kReset << "  单价 > 100 的产品 " << kCyan
              << expensiveCount << kReset << " 件: ";
    std::string names;
    for (const auto& p : data.toVector()) {
      if (!names.empty()) names += ", ";
      names += p["name"].toString();
      expensiveTotal += p["price"].toDouble();
    }
    std::cout << names << "\n";
    pause(demoDelayMs());
  } else {
    std::cout << kYellow << "  db_query(products) failed: " << products.error().message << "\n" << kReset;
  }

  // 对比结论 + 写入 jsonfile analytics 表 (MRTR 确认)
  headline("db_insert - 对比结论写入 jsonfile.analytics (MRTR 人机确认)");
  std::cout << kYellow << "  请求: db_insert {db:\"jsonfile\", table:\"analytics\", data:{...}}\n"
            << kReset
            << kYellow << "  含义: 把两边的统计对比结果存到输出库。写操作不立即执行 - "
               "先返回 input_required, 由人工确认后落盘。\n" << kReset;
  Json args;
  args.add("db", Json::str("jsonfile"));
  args.add("table", Json::str("analytics"));
  Json data;
  char expTotalBuf[64];
  std::snprintf(expTotalBuf, sizeof(expTotalBuf), "%.2f", expensiveTotal);
  data.add("summary", Json::str("2026 orders by region vs expensive products"));
  data.add("expensive_product_count", Json(expensiveCount));
  data.add("expensive_product_total", Json::str(expTotalBuf));
  args.add("data", std::move(data));
  thinking();
  auto first = client.callTool("db_insert", args);
  if (first.has_value() && (*first)["resultType"].toString() == "input_required") {
    const std::string state = (*first)["requestState"].toString();
    std::cout << kGreen << "  -> 服务器请求确认 (resultType=input_required)" << kReset << "\n";
    std::cout << kYellow << "  提示: " << kReset
              << (*first)["inputRequests"]["confirm"]["params"]["message"].toString() << "\n";
    std::cout << "     (真实 AI 客户端此时会把这个确认请求展示给用户...)\n";
    pause(demoDelayMs() * 2);  // 展示确认框, 让观众读
    std::cout << kMagenta << "  用户点击『批准』 -> 带 inputResponses 重发" << kReset << "\n";
    pause(demoDelayMs());  // 模拟用户思考后点击
    Json retry;
    retry.add("db", Json::str("jsonfile"));
    retry.add("table", Json::str("analytics"));
    Json data2;
    data2.add("summary", Json::str("2026 orders by region vs expensive products"));
    data2.add("expensive_product_count", Json(expensiveCount));
    data2.add("expensive_product_total", Json::str(expTotalBuf));
    retry.add("data", std::move(data2));
    retry.add("requestState", Json::str(state));
    Json responses;
    Json confirm;
    confirm.add("approved", Json(true));
    responses.add("confirm", std::move(confirm));
    retry.add("inputResponses", std::move(responses));
    thinking();
    auto second = client.callTool("db_insert", retry);
    if (second.has_value() && second->isObject() &&
        second->contains("status") && (*second)["status"].toInt() == 200) {
      std::cout << kGreen << "  -> ok" << kReset << "  对比结论已写入 jsonfile.analytics (用户批准后真正落库)\n";
      pause(demoDelayMs());
      if (afterStep(stepDone("打开 analytics.json: 现在应有 2 张表 - region_sales(5 行地区汇总) 和 "
                               "analytics(对比结论, 含 expensive_product_count 字段)。"))) return 0;
    } else {
      const std::string errMsg = second.has_value()
          ? (*second)["message"].toString()
          : second.error().message;
      std::cout << kYellow << "  db_insert retry failed: " << errMsg << "\n" << kReset;
    }
  } else if (first.has_value()) {
    std::cout << kYellow << "  (unexpected: executed without confirmation)\n" << kReset;
  } else {
    std::cout << kYellow << "  db_insert failed: " << first.error().message << "\n" << kReset;
  }

  // [7] count as proof (验证两个 jsonfile 输出表)
  headline("db_count - 验证写入真的生效 (两个输出表的计数)");
  std::cout << kYellow << "  请求: db_count {db:\"jsonfile\", table:\"region_sales\"}  /  "
               "db_count {db:\"jsonfile\", table:\"analytics\"}\n" << kReset;
  auto cntRegion = client.callTool("db_count", Json(R"({"db":"jsonfile","table":"region_sales"})"));
  auto cntAnalytics = client.callTool("db_count", Json(R"({"db":"jsonfile","table":"analytics"})"));
  long long regionTotal = 0, analyticsTotal = 0;
  if (cntRegion.has_value()) {
    const Json data = member(*cntRegion, "data");
    if (data.isArray() && data.size() > 0) regionTotal = data[0]["total"].toInt64();
  }
  if (cntAnalytics.has_value()) {
    const Json data = member(*cntAnalytics, "data");
    if (data.isArray() && data.size() > 0) analyticsTotal = data[0]["total"].toInt64();
  }
  std::cout << kGreen << "  -> ok" << kReset << "  region_sales 表 " << kMagenta << regionTotal
            << kReset << " 行 (2026 各地区汇总) | analytics 表 " << kMagenta << analyticsTotal
            << kReset << " 行 (对比结论)\n";
  pause(demoDelayMs() * 2);
  if (afterStep(stepDone("打开 analytics.json: 应看到 2 张表 - region_sales(" + std::to_string(regionTotal) +
                           " 行) 和 analytics(" + std::to_string(analyticsTotal) + " 行)。"))) return 0;

  // [8] 收尾: 清空 analytics 统计表, 保留 region_sales 汇总表
  // 最终 jsonfile = 2026 订单按地区汇总销售额表(有数据) + 统计空表(输出容器)。
  headline("db_delete - 收尾: 清空 analytics 统计表 (保留 region_sales 汇总)");
  std::cout << kYellow << "  含义: 演示结束, analytics 作为『统计空表』容器清空; "
               "region_sales 汇总结果保留, 方便查看。\n" << kReset;
  Json delQuery;
  delQuery.add("db", Json::str("jsonfile"));
  delQuery.add("table", Json::str("analytics"));
  delQuery.add("lks", Json(R"({"summary":"2026"})"));
  thinking();
  auto delRows = client.callTool("db_query", delQuery);
  bool cleaned = true;
  if (delRows.has_value()) {
    const Json rows = member(*delRows, "data");
    if (rows.size() > 0) {
      std::cout << kGreen << "  -> ok" << kReset << "  找到 " << rows.size()
                << " 条 analytics 记录, 逐条删除\n";
      pause(demoDelayMs());
    }
    for (const auto& row : rows.toVector()) {
      Json delArgs;
      delArgs.add("db", Json::str("jsonfile"));
      delArgs.add("table", Json::str("analytics"));
      Json where;
      where.add("id", Json::str(row["id"].toString()));
      delArgs.add("where", std::move(where));
      thinking();
      auto delFirst = client.callTool("db_delete", delArgs);
      if (delFirst.has_value() && (*delFirst)["resultType"].toString() == "input_required") {
        const std::string state = (*delFirst)["requestState"].toString();
        std::cout << kGreen << "  -> 请求确认删除 id=" << row["id"].toString() << kReset << "\n";
        pause(demoDelayMs() / 2);
        std::cout << kMagenta << "  用户点击『批准』" << kReset << "\n";
        Json delRetry;
        delRetry.add("db", Json::str("jsonfile"));
        delRetry.add("table", Json::str("analytics"));
        Json where2;
        where2.add("id", Json::str(row["id"].toString()));
        delRetry.add("where", std::move(where2));
        delRetry.add("requestState", Json::str(state));
        Json delResponses;
        Json delConfirm;
        delConfirm.add("approved", Json(true));
        delResponses.add("confirm", std::move(delConfirm));
        delRetry.add("inputResponses", std::move(delResponses));
        auto delSecond = client.callTool("db_delete", delRetry);
        if (delSecond.has_value()) {
          std::cout << kGreen << "  -> 已删除 id=" << row["id"].toString() << kReset << "\n";
        } else {
          std::cout << kYellow << "  -> 删除失败: " << delSecond.error().message << "\n" << kReset;
          cleaned = false;
        }
        pause(demoDelayMs() / 2);
      }
    }
  }
  if (cleaned) {
    std::cout << kGreen << "  -> analytics 已清空(统计空表) | region_sales 汇总保留\n" << kReset;
    pause(demoDelayMs());
    if (afterStep(stepDone("最终 analytics.json: region_sales 表(5 行地区汇总) + analytics 表(空数组 [] 统计空表)。"
                             "测试数据仍在 sqlite demo.db。"))) return 0;
  } else {
    std::cout << kYellow << "  (清理失败或无需清理)\n" << kReset;
  }

  // graceful shutdown
  headline("stdin EOF - 优雅关停 (客户端断开即退出)");
  pause(demoDelayMs() / 2);
  child.closeStdin();
#ifdef _WIN32
  WaitForSingleObject(child.pi.hProcess, 5000);
#else
  waitpid(child.pid, nullptr, 0);
#endif

  std::cout << "\n" << kBold << "=== demo complete 演示完成 ===" << kReset << "\n";
  std::cout << "数据流: sqlite 测试数据 -> 查询/统计 -> 结果写入 jsonfile 输出库。\n";
  std::cout << "最终 jsonfile(analytics.json) 含 2 张表:\n";
  std::cout << "  - region_sales  2026 年订单按地区汇总销售额 (5 行)\n";
  std::cout << "  - analytics     统计空表 (输出容器)\n";
  std::cout << "测试数据仍在 sqlite(demo.db): orders/users/products。\n";
  std::cout << "\n覆盖的协议能力: 版本协商 / 工具发现 / 智能查询(聚合/比较) /\n";
  std::cout << "资源读取 / MRTR 写入确认 / 优雅关停。\n";
  std::cout << "\n用真实 AI 客户端连接 (见 README_demo.md):\n";
  std::cout << kCyan << "  claude mcp add zorm-analytics -- <path-to>/zorm_analytics_server.exe\n" << kReset;
  return 0;
}
