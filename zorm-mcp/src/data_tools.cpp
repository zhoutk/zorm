// data_tools.cpp - the data plane:
//   db_query / db_count                    (read; zorm smart query semantics)
//   db_insert / db_insert_batch / db_update / db_delete   (writes, MRTR-gated)
// plus the shared MRTR staging/execute machinery reused by sql_tools.cpp.

#include "app.hpp"
#include "json_util.hpp"
#include "tools.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

// ---- db_query argument conversion -------------------------------------------------
//
// zorm's smart-query shapes (verified against SqlBackendBase::genSql):
//   lks/ins/ors : COMMA-SEPARATED STRING "col1,val1,col2,val2" (pairs)
//   sum/count   : comma-separated "col,alias" pairs
//   group/sort  : comma-separated "a,b"
//   page/size/fuzzy : scalars
// The client-facing schema uses friendlier shapes (objects / arrays); convert
// here. Values are assumed simple (no embedded commas) - documented in the
// playbook.

void convertPairsObject(const Json& v, std::string& joined) {
  for (const auto& k : v.getAllKeys().toVector()) {
    const std::string kk = k.toString();
    const Json vv = v[kk];
    if (vv.isString() || vv.isNumber() || vv.isTrue() || vv.isFalse()) {
      if (!joined.empty()) joined += ",";
      joined += kk + "," + vv.toString();
    } else if (vv.isArray()) {
      // ins/ors accept multi-value lists: {id:["a","b"]} -> "id,a,b"
      for (const auto& item : vv.toVector()) {
        if (!joined.empty()) joined += ",";
        joined += kk + "," + item.toString();
      }
    }
    // object values are not supported by the pair encoding
  }
}

void convertStringLists(const Json& args, ZORM::Json& params, bool stripWildcard) {
  // lks/ins/ors: object {col:value} -> "col1,val1,col2,val2"
  for (const char* key : {"lks", "ins", "ors"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isObject()) {
      std::string joined;
      if (stripWildcard && std::string(key) == "lks") {
        // jsonfile's lks is a plain substring match - the portable
        // %wildcard semantics have no backend equivalent, so strip the
        // wildcards and let "contains" approximate them.
        Json stripped = Json("{}");
        for (const auto& k : v.getAllKeys().toVector()) {
          std::string val = v[k.toString()].toString();
          val.erase(std::remove(val.begin(), val.end(), '%'), val.end());
          stripped.add(k.toString(), Json::str(val));
        }
        convertPairsObject(stripped, joined);
      } else {
        convertPairsObject(v, joined);
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString() || v.isArray()) {
      params.add(key, toZormJson(v));  // pass through zorm-native shapes
    }
  }
  // group/sort: array ["a","b"] -> "a,b"
  for (const char* key : {"group", "sort"}) {
    const Json v = member(args, key);
    if (v.isError() || v.isNull()) continue;
    if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        if (!joined.empty()) joined += ",";
        joined += item.toString();
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString()) {
      params.add(key, toZormJson(v));
    }
  }
  // sum/count: {col:alias} or ["col"] or "col" -> "col,alias" pairs
  for (const char* key : {"sum", "count"}) {
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
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isArray()) {
      std::string joined;
      for (const auto& item : v.toVector()) {
        const std::string s = item.toString();
        if (s.empty()) continue;
        if (!joined.empty()) joined += ",";
        joined += s + "," + s;  // alias defaults to the column name
      }
      if (!joined.empty()) params.add(key, ZORM::Json(joined.c_str()));
    } else if (v.isString()) {
      const std::string s = v.toString();
      if (!s.empty()) params.add(key, ZORM::Json((s + "," + s).c_str()));
    }
  }
  // scalars
  for (const char* key : {"fuzzy", "page", "size"}) {
    const Json v = member(args, key);
    if (!v.isError() && !v.isNull()) params.add(key, toZormJson(v));
  }
}

// ---- MRTR machinery (shared with sql_tools.cpp) -------------------------------------

// Extracts the user's approval from an inputResponses payload. Clients echo
// elicitation answers in different shapes; accept them all:
//   {"confirm":{"approved":true}}   canonical (inputRequests key)
//   {"approved":true}               flat
//   {"content"/"value"/"params":{"approved":true}}
//   {"action":"accept"}             (or decline/reject/cancel)
// Anything unrecognized counts as NOT approved (fail-safe).
bool confirmApproved(const Json& responses) {
  static const char* kNested[] = {"confirm", "content", "value", "params"};
  for (const char* key : kNested) {
    const Json nested = member(responses, key);
    if (nested.isObject()) {
      const Json approved = member(nested, "approved");
      if (approved.isTrue() || approved.isFalse()) return approved.isTrue();
    }
  }
  const Json flat = member(responses, "approved");
  if (flat.isTrue() || flat.isFalse()) return flat.isTrue();
  const Json action = member(responses, "action");
  if (action.isString()) {
    const std::string a = action.toString();
    return a == "accept" || a == "accepted" || a == "approve" || a == "approved";
  }
  return false;
}

// Human phrase for the affected rows of a staged delete (count-first so the
// confirmation text states the impact). zorm's count aggregate lands in
// data[0]["total"] - data.size() is always 1 and must never be used here.
std::string deleteImpact(ZORM::Idb* db, const std::string& table,
                         const Json& where) {
  ZORM::Json wp;
  if (ZJSON::hasChild(where, "id")) {
    wp.add("id", toZormJson(where["id"]));
  } else {
    // top-level conditions (zorm select semantics: conditions live in params)
    for (const auto& k : where.getAllKeys().toVector()) {
      wp.add(k.toString(), toZormJson(where[k.toString()]));
    }
  }
  wp.add("count", ZORM::Json("*,total"));
  const ZORM::Json rs = db->select(table, wp);
  if (okStatus(rs) && rs["data"].isArray() && rs["data"].size() > 0) {
    const long long total = rs["data"][0]["total"].toInt64();
    return std::to_string(total) + (total == 1 ? " row" : " rows");
  }
  return "unknown number of rows";
}

}  // namespace

// Builds the input_required result asking the user to confirm a write.
Json inputRequiredConfirm(const std::string& requestState, const std::string& message) {
  Json result;
  result.add(std::string(mcp::kResultTypeKey),
             Json::str(std::string(mcp::kResultTypeInputRequired)));
  // CallToolResult requires content even for input_required results.
  Json content = Json("[]");
  Json text;
  text.add("type", Json::str("text"));
  text.add("text", Json::str(message));
  content.push_back(std::move(text));
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

// Executes an already-staged write (MRTR-approved retry / --yes fast path).
McpResult<Json> executeWrite(const std::string& dbName, const std::string& table,
                             const std::string& kind, const Json& args) {
  App& g = app();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return unknownDb(dbName);

  ZORM::Json rs;
  if (kind == "insert") {
    // Idb::create semantics: single-row insert; a missing id is
    // auto-generated, an existing id upserts (covers jsonfile parity).
    rs = db->create(table, toZormJson(member(args, "data")));
  } else if (kind == "insert_batch") {
    // Idb::insertBatch: multi-row upsert on the constraint column.
    const Json rows = member(args, "rows");
    if (!rows.isArray() || rows.size() < 1) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_insert_batch requires `rows` to be a non-empty array of row objects");
    }
    // zjson trap: member() of a missing key yields Null whose toString() is
    // the literal "null" - test presence explicitly, never via toString().
    std::string constraint = "id";
    const Json c = member(args, "constraint");
    if (c.isString() && !c.toString().empty()) constraint = c.toString();
    rs = db->insertBatch(table, toZormJson(rows), constraint);
  } else if (kind == "update") {
    // zorm update: the target id rides at the TOP level of params
    // (buildUpdateSql reads params["id"]) plus the columns to change.
    // Client shape {data:{cols}, where:{id}} is flattened here.
    const Json idv = member(member(args, "where"), "id");
    if (!(idv.isString() || idv.isNumber())) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_update requires where:{\"id\": <value>} (single-row update). "
                      "For condition-based updates use db_trans with a structured "
                      "update element, or db_exec_sql when --allow-sql is on");
    }
    ZORM::Json params = toZormJson(member(args, "data"));
    params.add("id", toZormJson(idv));
    rs = db->update(table, params);
  } else if (kind == "delete") {
    // zorm remove is id-targeted (params["id"] at the top level). where:{}
    // means "delete ALL rows" - structured remove cannot express it, so fall
    // back to execSql with a VALIDATED table name (identifier whitelist keeps
    // client-supplied names out of SQL). A non-empty where without id is
    // rejected with guidance, never treated as delete-all.
    // NOTE: member() returns Json(nullptr) for a missing key and zjson's
    // toString() renders Null as "null" - presence is tested via hasChild.
    const Json where = member(args, "where");
    const Json idv = member(where, "id");
    if (idv.isString() || idv.isNumber()) {
      ZORM::Json params;
      params.add("id", toZormJson(idv));
      rs = db->remove(table, params);
    } else if (ZJSON::isEmptyObject(where)) {
      if (!validIdentifier(table)) {
        return mcp::Err(McpErrorCode::InvalidParams, "invalid table name: " + table);
      }
      if (g.specs[dbName].type == "jsonfile") {
        // jsonfile's SQL shim only supports DELETE ... WHERE id = ?. Delete-all
        // goes through the structured interface instead: select the ids, then
        // remove row by row (adapter-level; Idb stays untouched).
        const ZORM::Json rows = db->select(table, ZORM::Json());
        if (!okStatus(rows)) {
          rs = rows;
        } else {
          long long n = 0;
          bool allOk = true;
          std::string lastErr;
          if (rows["data"].isArray()) {
            for (const auto& row : rows["data"].toVector()) {
              if (!ZJSON::hasChild(row, "id")) continue;
              ZORM::Json p;
              p.add("id", row["id"]);
              const ZORM::Json one = db->remove(table, p);
              if (!okStatus(one)) {
                allOk = false;
                lastErr = one["message"].toString();
                break;
              }
              ++n;
            }
          }
          ZORM::Json out(R"({"status":200})");
          out.add("message", allOk ? Json::str(std::string("delete all ok"))
                                   : Json::str("delete all aborted: " + lastErr));
          out.add("affectedRows", Json(static_cast<int64_t>(n)));
          rs = out;
        }
      } else {
        rs = db->execSql("delete from " + table);
      }
    } else {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_delete supports only where:{\"id\": <value>} (single row) "
                      "or an empty where:{} (delete ALL rows). For condition-based "
                      "deletes stage them in db_trans (structured delete elements), "
                      "or query ids with db_query and delete them one by one");
    }
  } else if (kind == "exec_sql") {
    rs = db->execSql(member(args, "sql").toString());
  } else if (kind == "trans") {
    const Json sqls = member(args, "sqls");
    if (!sqls.isArray() || sqls.size() < 1) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "db_trans requires `sqls`: a non-empty array of transaction elements");
    }
    // zorm's structured elements use capitalized methods ("Insert" /
    // "Update" / "Delete" / "Batch"); the client-facing schema documents
    // lowercase - normalize here so agents are never punished for case.
    ZORM::Json zsqls(R"([])");
    for (const auto& item : sqls.toVector()) {
      ZORM::Json el = toZormJson(item);
      if (ZJSON::hasChild(el, "method")) {
        std::string m = el["method"].toString();
        for (auto& ch : m)
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string mapped;
        if (m == "insert") mapped = "Insert";
        else if (m == "update") mapped = "Update";
        else if (m == "delete") mapped = "Delete";
        else if (m == "batch") mapped = "Batch";
        if (!mapped.empty()) ZJSON::setChild(el, "method", ZORM::Json(mapped.c_str()));
      }
      zsqls.push_back(el);
    }
    rs = db->transGo(zsqls);
  } else {
    return mcp::Err(McpErrorCode::InternalError, "unknown write kind: " + kind);
  }

  if (!okStatus(rs)) {
    // Raw driver codes are opaque to AI clients; map the common sqlite ones
    // and always name the exact file that was being written (connString is
    // relative - the resolved path is what matters when the client-spawned
    // cwd differs from the repo root).
    std::string msg = rs["message"].toString();
    const std::string raw = "Error Code : ";
    const size_t pos = msg.find(raw);
    if (pos != std::string::npos) {
      const std::string code = msg.substr(pos + raw.size());
      if (code == "8")
        msg += " (SQLITE_READONLY: the database file or its directory is read-only - "
               "check the file attribute, disk, and the server process's working directory)";
      else if (code == "5")
        msg += " (SQLITE_BUSY: the database is locked by another connection/process)";
      else if (code == "11")
        msg += " (SQLITE_CORRUPT: the database file is corrupted)";
      else if (code == "13")
        msg += " (SQLITE_FULL: disk is full or quota exceeded)";
      else if (code == "14")
        msg += " (SQLITE_CANTOPEN: the database file cannot be opened - wrong path or no permission)";
    }
    std::string file = g.specs[dbName].options["connString"].toString();
    try {
      file = std::filesystem::absolute(file).string();
    } catch (...) {
    }
    return mcp::Err(McpErrorCode::InternalError, "write failed: " + msg + " [file: " + file + "]");
  }

  // Notify resource subscribers that this table changed (mcp-cpp fan-out).
  if (auto srv = g.server.lock()) {
    if (!table.empty()) {
      srv->notifyResourceUpdated("db://schema/" + dbName + "/" + table);
    }
    srv->notifyListChanged("resources");
  }
  return mcp::Ok(toolResult(toMcpJson(rs)));
}

// Stages a write and returns input_required (or executes directly with --yes
// / on an approved retry). Shared by every write tool; `kind` selects the
// executor and the confirmation wording.
McpResult<Json> stageWrite(Ctx& c, const std::string& kind) {
  App& g = app();
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = g.db(dbName);
  if (db == nullptr) return unknownDb(dbName);

  // MRTR retry detection. Clients are unreliable about echoing the exact
  // shape we ask for, so a retry is ANY of:
  //   - inputResponses present (object; shape-tolerant, see confirmApproved)
  //   - requestState present (echoed token)
  //   - a bare top-level "approved" argument (clients that reply in place)
  const Json responses = member(args, "inputResponses");
  const Json reqStateArg = member(args, "requestState");
  const Json approvedArg = member(args, "approved");
  const bool isRetry = responses.isObject() || reqStateArg.isString() ||
                       approvedArg.isTrue() || approvedArg.isFalse();
  if (isRetry) {
    const bool approved =
        responses.isObject() ? confirmApproved(responses) : approvedArg.isTrue();
    // Locate the pending op: prefer the echoed requestState; when the client
    // dropped it, fall back to the pending op for THIS (db, table, kind) -
    // with several staged (the client looping before confirming), the latest
    // one wins.
    std::string state;
    PendingOp op;
    bool found = false;
    if (reqStateArg.isString()) {
      const std::string s = reqStateArg.toString();
      const auto it = g.pending.find(s);
      if (it != g.pending.end()) {
        state = s;
        op = it->second;
        found = true;
      }
    }
    if (!found) {
      int bestN = -1;
      for (const auto& [s, po] : g.pending) {
        if (po.db != dbName || po.table != table || po.kind != kind) continue;
        int n = 0;
        try {
          n = std::stoi(s.substr(s.find_last_of('-') + 1));
        } catch (...) {
          continue;
        }
        if (n > bestN) {
          bestN = n;
          state = s;
          op = po;
          found = true;
        }
      }
    }
    if (!found) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "no pending " + kind + " op for " + dbName +
                          (table.empty() ? "" : "." + table) +
                          " (requestState " + reqStateArg.toString() +
                          " is unknown or expired) - call again without "
                          "requestState/inputResponses to stage a new one");
    }
    g.pending.erase(state);
    if (!approved) {
      Json result;
      result.add(std::string(mcp::kResultTypeKey),
                 Json::str(std::string(mcp::kResultTypeComplete)));
      result.add("content", Json(R"([{"type":"text","text":"Write cancelled by user."}])"));
      return mcp::Ok(std::move(result));
    }
    return executeWrite(op.db, op.table, op.kind, op.args);
  }

  if (g.confirmWrites) {
    const std::string state = "st-" + std::to_string(g.nextState++);
    // Impact estimate for the confirmation text.
    std::string impact;
    if (kind == "delete" && member(args, "where").isObject()) {
      impact = deleteImpact(db, table, member(args, "where"));
    } else if (kind == "insert_batch" && member(args, "rows").isArray()) {
      impact = std::to_string(member(args, "rows").size()) + " rows";
    } else if (kind == "insert") {
      impact = "1 row";
    } else if (kind == "update") {
      impact = "row id=" + member(member(args, "where"), "id").toString();
    } else if (kind == "trans" && member(args, "sqls").isArray()) {
      impact = std::to_string(member(args, "sqls").size()) + " statements";
    } else if (kind == "exec_sql") {
      impact = "arbitrary SQL";
    }
    // A new stage for the same (db,table,kind) replaces the old pending -
    // stale confirm requests are meaningless and would pile up.
    for (auto it = g.pending.begin(); it != g.pending.end();) {
      if (it->second.db == dbName && it->second.table == table && it->second.kind == kind)
        it = g.pending.erase(it);
      else
        ++it;
    }
    g.pending[state] = PendingOp{dbName, table, kind, args};
    // requestState rides in the confirmation text so strict text-only
    // clients can echo it back verbatim on the retry.
    const std::string msg = "Confirm " + kind + " on " + dbName +
                            (table.empty() ? "" : "." + table) + " (" + impact +
                            ")? Reply with approved:true to proceed (requestState: " +
                            state + ").";
    return mcp::Ok(inputRequiredConfirm(state, msg));
  }
  // --yes: execute immediately (tests / automation).
  return executeWrite(dbName, table, kind, args);
}

// ---- read handlers -----------------------------------------------------------------

McpResult<Json> toolQuery(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  if (table.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "table is required");
  }

  ZORM::Json params;
  const bool jsonfileBackend = app().specs[dbName].type == "jsonfile";
  convertStringLists(args, params, jsonfileBackend);
  // Remaining keys pass through as plain column conditions: {"price":100}
  // equal, or a compare-prefix string {"price":">,100"} (zorm prefixes
  // >, >=, <, <=, <>, =).
  for (const auto& k : args.getAllKeys().toVector()) {
    const std::string kk = k.toString();
    static const char* kReserved[] = {"db",       "table",  "fields", "lks",
                                      "ins",      "ors",    "group",  "sort",
                                      "sum",      "count",  "fuzzy",  "page",
                                      "size",     "requestState", "inputResponses",
                                      "approved", "constraint", "rows"};
    if (std::any_of(std::begin(kReserved), std::end(kReserved),
                    [&](const char* r) { return kk == r; })) {
      continue;
    }
    const Json v = member(args, kk);
    if (v.isError() || v.isNull()) continue;
    params.add(kk, toZormJson(v));
  }

  std::vector<std::string> fields;
  const Json f = member(args, "fields");
  if (f.isArray()) {
    for (const auto& item : f.toVector()) fields.push_back(item.toString());
  }

  const ZORM::Json rs = db->select(table, params, fields);
  if (!okStatus(rs)) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "query failed: " + rs["message"].toString());
  }
  return mcp::Ok(toolResult(toMcpJson(rs)));
}

McpResult<Json> toolCount(Ctx& c) {
  const Json args = member(c.params, "arguments");
  const std::string dbName = args["db"].toString();
  const std::string table = args["table"].toString();
  ZORM::Idb* db = app().db(dbName);
  if (db == nullptr) return unknownDb(dbName);
  if (table.empty()) {
    return mcp::Err(McpErrorCode::InvalidParams, "table is required");
  }

  // zorm count expects a "col,alias" pair (e.g. "*,total"); optional where
  // keys merge into params as top-level conditions (zorm select semantics).
  ZORM::Json params;
  const Json where = member(args, "where");
  if (where.isObject()) {
    for (const auto& k : where.getAllKeys().toVector()) {
      const std::string kk = k.toString();
      params.add(kk, toZormJson(where[kk]));
    }
  }
  params.add("count", ZORM::Json("*,total"));
  const ZORM::Json rs = db->select(table, params);
  if (!okStatus(rs)) {
    return mcp::Err(McpErrorCode::InvalidParams,
                    "count failed: " + rs["message"].toString());
  }
  return mcp::Ok(toolResult(toMcpJson(rs)));
}

McpResult<Json> toolInsert(Ctx& c) { return stageWrite(c, "insert"); }
McpResult<Json> toolInsertBatch(Ctx& c) { return stageWrite(c, "insert_batch"); }
McpResult<Json> toolUpdate(Ctx& c) { return stageWrite(c, "update"); }
McpResult<Json> toolDelete(Ctx& c) { return stageWrite(c, "delete"); }

// ---- registration -------------------------------------------------------------------

void registerDataTools(mcp::server::Builder& b) {
  b.tool(Json(R"({"name":"db_query","description":"PREFERRED data query - structured JSON, no SQL, same semantics on every backend. Supports: fields (projection), lks (fuzzy, % wildcards), ins (IN-list), ors (OR), group+sum (aggregation), sort, page/size (pagination), plus plain column conditions: {\"price\":100} equals, {\"price\":\">,100\"} compares (prefixes >, >=, <, <=, <>, =). Use db_query_sql only when JOINs or dialect functions are unavoidable.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string","description":"backend name"},"table":{"type":"string"},"fields":{"type":"array","items":{"type":"string"},"description":"column projection"},"lks":{"type":"object","description":"fuzzy matches, % wildcards, e.g. {\"name\":\"%abc%\"}"},"ins":{"type":"object","description":"IN-list matches"},"ors":{"type":"object","description":"OR conditions"},"group":{"type":"array","items":{"type":"string"},"description":"GROUP BY columns"},"sum":{"oneOf":[{"type":"object","additionalProperties":{"type":"string"}},{"type":"array","items":{"type":"string"}}],"description":"SUM aggregates, e.g. {\"amount\":\"total\"} or [\"amount\"]"},"count":{"type":"string","description":"COUNT column alias pair"},"sort":{"oneOf":[{"type":"array","items":{"type":"string"}},{"type":"string"}],"description":"ORDER BY"},"page":{"type":"integer","minimum":1},"size":{"type":"integer","minimum":1,"maximum":500}},"additionalProperties":{"description":"plain column condition: value equals, or \">,100\"-style compare prefix"}}})"),
         toolQuery);
  b.tool(Json(R"({"name":"db_count","description":"Count rows in a table (optionally matching a where object whose keys are column conditions). Cheaper and clearer than db_query when only the number matters.","inputSchema":{"type":"object","required":["db","table"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object","description":"column conditions, e.g. {\"status\":\"paid\"}"}}}})"),
         toolCount);
  b.tool(Json(R"({"name":"db_insert","description":"Insert ONE row (MRTR-confirmed). data is a column->value object; a missing id is auto-generated, an existing id upserts. For many rows use db_insert_batch.","inputSchema":{"type":"object","required":["db","table","data"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object","description":"column->value row"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolInsert);
  b.tool(Json(R"({"name":"db_insert_batch","description":"Insert/upsert MULTIPLE rows in one statement (MRTR-confirmed). rows is a non-empty array of column->value objects; on duplicate constraint values existing rows are updated (upsert). Prefer this over repeated db_insert for bulk loads.","inputSchema":{"type":"object","required":["db","table","rows"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"rows":{"type":"array","items":{"type":"object"},"minItems":1,"description":"row objects; keys of the first row define the column list"},"constraint":{"type":"string","description":"upsert conflict column, default id"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolInsertBatch);
  b.tool(Json(R"({"name":"db_update","description":"Update ONE row by id (MRTR-confirmed). data holds the columns to change; where MUST be {\"id\": <value>}. For condition-based updates use db_trans with a structured update element.","inputSchema":{"type":"object","required":["db","table","data","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"data":{"type":"object","description":"columns to change"},"where":{"type":"object","description":"must carry {\"id\": <value>}"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolUpdate);
  b.tool(Json(R"({"name":"db_delete","description":"Delete rows (MRTR-confirmed, destructive). where:{\"id\": v} deletes that row; where:{} deletes ALL rows in the table (the confirmation states the affected count). Other conditions are rejected - query ids with db_query then delete by id, or use db_trans for condition-based deletes.","inputSchema":{"type":"object","required":["db","table","where"],"properties":{"db":{"type":"string"},"table":{"type":"string"},"where":{"type":"object","description":"{\"id\": v} for one row, {} for all"},"requestState":{"type":"string"},"inputResponses":{"type":"object"}}}})"),
         toolDelete);
}

}  // namespace zorm_mcp
