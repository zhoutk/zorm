// tests/TestConfig.h
// ----------------------------------------------------------------------------
// Config-driven test harness (gels-style, see refer/gels/src/config/configs.ts).
//
// ONE contract suite, MANY backends: the same test binary is registered once
// per backend in CMake/CTest. The backend under test is selected by, in order
// of precedence:
//   1. `--dialect <name>` command line argument
//   2. ZORM_DB_DIALECT environment variable
//   3. the "db_dialect" field in tests/dbconfig.json
//   4. "sqlite3-mem" (default)
//
// Switching the database under test = switching one configuration value,
// exactly like gels. No per-database test files.
//
// sqlite3 naming (gels parity):
//   sqlite3-mem   - in-memory SQLite  (connString = ":memory:")
//   sqlite3       - file-backed SQLite (connString = a file path)
//   jsonfile      - JSON file backend
//   mysql / postgres / dm8 - remote servers
// ----------------------------------------------------------------------------
#pragma once

#include "Idb.h"
#include "DbBase.h"
#include <string>
#include <vector>

namespace ZORM {
namespace contract {

// Everything the shared contract suite needs to connect, reset the schema and
// translate dialect-specific raw SQL. Filled from tests/dbconfig.json.
struct BackendConfig {
	std::string name;            // sqlite3-mem | sqlite3 | jsonfile | mysql | postgres | dm8
	std::string type;            // db type passed to DbBase
	Json options;                // connection options passed to DbBase
	std::vector<std::string> schema;   // drop + create DDL, in order
	std::string rawTable;        // table reference for raw SQL
	std::string catalogSql;      // metadata-catalog query (202 when missing)
	std::string quoteColumn;     // column quote char for raw SQL ("\"" for dm8, "`" for mysql, "" otherwise)
	// How a NULL database value surfaces in reads:
	//   "empty"     - empty string "" (sqlite3, postgres, dm8)
	//   "null-string" - the literal string "null" (mysql client)
	//   "json-null" - a real JSON null (jsonfile)
	std::string nullRendering = "empty";
	std::string placeholder;     // "?" or "$n" (informational)
	bool supportsWherePlaceholders = true;  // querySql accepts "? " WHERE clauses
	bool autoCreateTables = false;          // create()/insertBatch() auto-create the table
};

// Loads the backend config for `dialect` from tests/dbconfig.json.
BackendConfig loadConfig(const std::string& dialect);

// Resolves the backend under test: --dialect <name> | ZORM_DB_DIALECT |
// dbconfig.json "db_dialect" | "sqlite3-mem".
std::string resolveDialect(int argc, char* argv[]);

// Like resolveDialect, but searches dbconfig.json next to the executable
// first (argv[0]) - needed when CTest runs from the build directory.
std::string resolveDialectWithArgv(int argc, char* argv[]);

// Global, set once by main() before RUN_ALL_TESTS().
extern BackendConfig g_config;
extern const char* g_dialect;

}  // namespace contract
}  // namespace ZORM
