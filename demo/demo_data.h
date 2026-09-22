// demo_data - idempotent demo database setup for the Zorm Analytics MCP demo.
//
// Seed tables (orders/users/products/analytics) into any zorm backend through
// the unified Idb interface. Calling setupDemoData twice is safe: existing
// tables are left untouched (probed via a count query first).

#pragma once

#include "Idb.h"

#include <string>

namespace zorm_demo {

// Creates demo tables (orders/users/products/analytics) with sample rows on
// `db`. `dbType` is the zorm backend type ("sqlite3" | "jsonfile" | ...) and
// selects whether DDL is needed (SQL backends) or tables auto-create
// (jsonfile). `dbName` is only used for logging. Returns false on error.
bool setupDemoData(ZORM::Idb* db, const std::string& dbType, const std::string& dbName);

}  // namespace zorm_demo
