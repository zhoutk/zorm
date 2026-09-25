#pragma once
// json_util.hpp - zjson (zorm) <-> mcp-cpp Json bridging and the tools/call
// result envelope helpers shared by all zorm-mcp tool handlers.
//
// zjson conventions this file exists for (see repo CLAUDE.md):
//   - member(json, key) returns Json(nullptr) for a missing key, and zjson's
//     toString() renders Null as the literal "null" - presence must be tested
//     with isNull()/isError()/hasChild, never via toString().empty().
//   - json[key] DEEP-SEARCHES nested layers when the key is missing at the top
//     level - structured reads must go through hasChild, never [key] blindly.

#include "Idb.h"
#include <mcp/mcp.hpp>

#include <initializer_list>
#include <string>

namespace zorm_mcp {

// zorm's "success" codes: STSUCCESS(200) normal, STQUERYEMPTY(202) no rows
// (jsonfile returns 202 for an empty result; SQL backends return 200).
// 202 is an empty result set, not an error - all tool handlers judge via this.
inline bool okStatus(const ZORM::Json& rs) {
  const int code = rs["status"].toInt();
  return code == 200 || code == 202;
}

// Bridge helpers. mcp::Json currently ALIASES ZJSON::Json, but the wire-text
// round trip is kept deliberately: it detaches values from the other side's
// parse arena and keeps the two layers independently replaceable.
inline mcp::Json toMcpJson(const ZORM::Json& zj) {
  std::string err;
  mcp::Json parsed = mcp::Json::ParseJson(zj.toString(), err);
  return parsed.isError() ? mcp::Json(nullptr) : parsed;
}
inline ZORM::Json toZormJson(const mcp::Json& j) {
  return ZORM::Json(j.toString().c_str());
}

// tools/call result envelope. The `content` array is REQUIRED by the spec
// (AI clients iterate it - a bare structured result makes them fail with
// "r.content is not iterable"). Structured fields are also flattened at the
// top level so programmatic clients (and the e2e tests) keep direct paths.
mcp::Json textResult(const std::string& text);
mcp::Json toolResult(const mcp::Json& structured);

// Reads row[key] WITHOUT deep search; tries each candidate key in order
// (drivers fold unquoted identifiers to upper case on some backends, e.g.
// dm8, so callers pass {"table_name", "TABLE_NAME"} style lists).
// Returns Json(nullptr) when no candidate is present.
mcp::Json rowVal(const mcp::Json& row, std::initializer_list<const char*> keys);

// rowVal + toString, empty string when absent.
std::string rowStr(const mcp::Json& row, std::initializer_list<const char*> keys);

// A conservative SQL identifier: letters/digits/underscore, not starting with
// a digit. Metadata SQL and the delete-all execSql fallback inline table names
// ONLY after this check (values are parameterized everywhere else).
bool validIdentifier(const std::string& name);

}  // namespace zorm_mcp
