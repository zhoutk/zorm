#pragma once
// app.hpp - zorm-mcp application state: backend registry, per-backend config,
// schema metadata cache, and the MRTR write-confirmation pending table.

#include "Idb.h"
#include <mcp/mcp.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zorm_mcp {

struct DbSpec {
  std::string type;    // sqlite3 | jsonfile | mysql | postgres | dm8
  ZORM::Json options;  // passed to ZORM::DbBase verbatim
};

// One column of one table, as reported by the backend's catalog. This is the
// payload of db_describe_table and of the db://schema resources - it works on
// EMPTY tables (catalog/metadata SQL + the jsonfile storage format), which is
// exactly what "query the table and guess columns" cannot do.
struct ColumnMeta {
  std::string name;
  std::string type;      // declared type ("" when the backend is typeless)
  bool notNull = false;
  std::string dflt;      // default expression, "" when none
  bool pk = false;
};

// MRTR write-confirmation state: requestState string -> staged operation.
struct PendingOp {
  std::string db;
  std::string table;  // "" for db_trans / db_exec_sql
  std::string kind;   // insert | insert_batch | update | delete | exec_sql | trans
  mcp::Json args;     // the original tools/call arguments (verbatim)
};

// Global application state. mcp-cpp dispatches tools on a single thread, so
// plain members need no locking.
struct App {
  // db name -> backend (each owns its driver connections).
  std::map<std::string, std::unique_ptr<ZORM::Idb>> backends;
  std::map<std::string, DbSpec> specs;

  // db name -> table -> columns (filled lazily by describeTable).
  std::map<std::string, std::map<std::string, std::vector<ColumnMeta>>> schemaCache;

  std::map<std::string, PendingOp> pending;
  int nextState = 1;

  bool confirmWrites = true;  // --yes disables the MRTR gate (automation only)
  bool allowSql = false;      // --allow-sql exposes db_exec_sql

  // Set after buildServer(); write handlers notify resource subscribers.
  std::weak_ptr<mcp::server::Server> server;

  ZORM::Idb* db(const std::string& name) {
    const auto it = backends.find(name);
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

App& app();  // the single global instance (defined in app.cpp)

// ---- startup (app.cpp) ---------------------------------------------------------

// Loads backend specs: built-in defaults (sqlite3 + jsonfile in the working
// directory) or the "databases" object of a --config JSON file. Returns false
// on a malformed config file. `allowLogs` keeps user-configured zorm SQL logs
// on (HTTP debug mode); stdio mode always silences them.
bool loadSpecs(const std::string& configPath, bool allowLogs);

// Opens every configured backend and logs to stderr. Returns false when any
// backend fails to open.
bool openBackends();

// Discovers tables + columns for every backend at startup so resources and
// completion have data from the first request (best-effort: a backend that
// yields nothing simply starts with an empty cache).
void discoverAllSchemas();

// ---- schema metadata (app.cpp) -------------------------------------------------

// Lists user tables of one backend. Dialect-routed:
//   sqlite3   : sqlite_master
//   jsonfile  : the storage file's table entries
//   mysql     : information_schema.tables (table_schema = database())
//   postgres  : information_schema.tables (table_schema = current_schema())
//   dm8       : user_tables
std::vector<std::string> listTables(const std::string& dbName);

// Full column metadata of one table (see ColumnMeta). Validates the table
// name, routes to the dialect's catalog, caches the result, and returns an
// empty vector for an unknown table (caller reports the error).
std::vector<ColumnMeta> describeTable(const std::string& dbName,
                                      const std::string& table);

// describeTable against the cache without catalog access (miss = empty).
std::vector<ColumnMeta> cachedColumns(const std::string& dbName,
                                      const std::string& table);

}  // namespace zorm_mcp
