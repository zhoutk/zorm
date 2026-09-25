// resources.cpp - MCP resources (schema self-description) and completion.
//
//   db://guide                   full usage manual (pull channel, see guide.hpp)
//   db://schema                  all backends overview
//   db://schema/{db}             table list of one backend
//   db://schema/{db}/{table}     full column metadata (type/nullable/pk)
//
// Per-table resources are registered from the startup schema discovery;
// handlers describe on demand, so tables created later still resolve (and
// write tools ping subscribers with notifyResourceUpdated).

#include "app.hpp"
#include "guide.hpp"
#include "json_util.hpp"

#include <string>
#include <vector>

namespace zorm_mcp {

using mcp::Json;
using mcp::McpErrorCode;
using mcp::McpResult;
using mcp::member;
using mcp::server::Builder;
using mcp::server::Ctx;

namespace {

// Renders ColumnMeta rows as a compact readable text block.
std::string columnsText(const std::vector<ColumnMeta>& cols) {
  std::string text = "columns:\n";
  if (cols.empty()) text += "  (no columns discovered)\n";
  for (const auto& c : cols) {
    text += "  - " + c.name;
    if (!c.type.empty()) text += " : " + c.type;
    if (c.pk) text += "  [primary key]";
    if (c.notNull && !c.pk) text += "  [not null]";
    if (!c.dflt.empty()) text += "  [default: " + c.dflt + "]";
    text += "\n";
  }
  return text;
}

}  // namespace

McpResult<Json> resourceSchema(Ctx& c) {
  // Resource handlers receive the raw request params (uri at the top level,
  // NOT under "arguments" like tools).
  const Json uriJson = member(c.params, "uri");
  if (!uriJson.isString()) {
    return mcp::Err(McpErrorCode::InvalidParams, "resources/read requires a string uri");
  }
  const std::string uri = uriJson.toString();
  static const std::string kPrefix = "db://schema";
  if (uri.rfind(kPrefix, 0) != 0) {
    return mcp::Err(McpErrorCode::InvalidParams, "unknown resource: " + uri);
  }

  Json result;
  Json contents = Json("[]");
  Json item;
  item.add("uri", Json::str(uri));

  // uri forms: db://schema, db://schema/{db}, db://schema/{db}/{table}
  std::string rest = uri.substr(kPrefix.size());
  if (rest.empty()) {
    std::string text;
    for (const auto& [dbName, spec] : app().specs) {
      text += dbName + " (" + spec.type + "):";
      for (const auto& t : listTables(dbName)) text += " " + t;
      text += "\n";
    }
    item.add("text", Json::str(text));
  } else {
    if (rest[0] == '/') rest.erase(0, 1);
    const size_t slash = rest.find('/');
    const std::string dbName = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string table = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (app().db(dbName) == nullptr) {
      return mcp::Err(McpErrorCode::InvalidParams,
                      "unknown db in uri: " + uri + " (available: " +
                          app().dbListText() + ")");
    }
    if (table.empty()) {
      std::string text;
      for (const auto& t : listTables(dbName)) text += t + "\n";
      item.add("text", Json::str(text));
    } else {
      // Unknown table reports distinctly from a real empty-table hit.
      bool known = false;
      for (const auto& t : listTables(dbName)) {
        if (t == table) { known = true; break; }
      }
      if (!known) {
        return mcp::Err(McpErrorCode::InvalidParams, "unknown table: " + table);
      }
      item.add("text", Json::str("table " + table + "\n" +
                                 columnsText(describeTable(dbName, table))));
    }
  }
  contents.push_back(std::move(item));
  result.add("contents", std::move(contents));
  return mcp::Ok(std::move(result));
}

// ---- completion -----------------------------------------------------------------

McpResult<Json> toolCompletion(Ctx& c) {
  // params: {ref: {type, name, arguments?...}, argument: {name, value}}
  const Json ref = member(c.params, "ref");
  const std::string argName = ref["name"].toString();
  const Json argument = member(c.params, "argument");
  const std::string partial = argument["value"].toString();

  Json result;
  Json values = Json("[]");
  if (argName == "db") {
    for (const auto& [dbName, spec] : app().specs) {
      if (partial.empty() || dbName.rfind(partial, 0) == 0)
        values.push_back(Json::str(dbName));
    }
  } else if (argName == "table") {
    // ref.arguments carries the already-chosen db (when the client sends it).
    const std::string dbName = member(member(ref, "arguments"), "db").toString();
    std::vector<std::string> tables;
    if (app().db(dbName) != nullptr) {
      tables = listTables(dbName);
    } else {
      for (const auto& [name, spec] : app().specs) {
        for (const auto& t : listTables(name)) tables.push_back(t);
      }
    }
    for (const auto& t : tables) {
      if (partial.empty() || t.rfind(partial, 0) == 0) values.push_back(Json::str(t));
    }
  } else if (argName == "fields") {
    // fields is an array; argument.value is the entry being typed.
    const Json refArgs = member(ref, "arguments");
    const std::string dbName = refArgs["db"].toString();
    const std::string table = refArgs["table"].toString();
    for (const auto& col : describeTable(dbName, table)) {
      if (partial.empty() || col.name.rfind(partial, 0) == 0)
        values.push_back(Json::str(col.name));
    }
  }
  result.add("values", std::move(values));
  return mcp::Ok(std::move(result));
}

// ---- registration -----------------------------------------------------------------

void registerResourcesAndCompletion(Builder& b) {
  b.resource(Json(R"({"uri":"db://guide","name":"zorm-mcp usage manual - playbook"})"),
             resourceGuide);
  b.resource(Json(R"({"uri":"db://schema","name":"all backends overview"})"),
             resourceSchema);
  for (const auto& [dbName, spec] : app().specs) {
    std::string err;
    b.resource(Json::ParseJson(
                   "{\"uri\":\"db://schema/" + dbName +
                       "\",\"name\":\"tables in " + dbName + "\"}",
                   err),
               resourceSchema);
    for (const auto& t : listTables(dbName)) {
      std::string err2;
      b.resource(Json::ParseJson(
                     "{\"uri\":\"db://schema/" + dbName + "/" + t +
                         "\",\"name\":\"schema of " + dbName + "." + t + "\"}",
                     err2),
                 resourceSchema);
    }
  }
  b.completions(toolCompletion);
}

}  // namespace zorm_mcp
