// demo_data - idempotent demo database setup for the Zorm Analytics MCP demo.
//
// Builds sample data into the sqlite + jsonfile backends (zero-dependency
// open-box defaults). SQL backends beyond those are optional; this file only
// seeds what a `ZORM::Idb` can create through the unified interface.
//
// Idempotency: each table is probed first (via a count query on the table
// name); if it exists and is non-empty it is left untouched. Rerunning
// `--setup` is safe (no side effects on an already-seeded database).

#include "demo_data.h"

#include "Idb.h"

#include <string>
#include <vector>

namespace zorm_demo {

namespace {

// Builds a ZJSON object from explicit key/value pairs. String literals and
// std::strings convert through ZJSON::Json(const char*), which sniffs JSON
// numbers/arrays - so numeric text like "123.45" becomes a number value.
ZORM::Json row(std::initializer_list<std::pair<const char*, ZORM::Json>> kv) {
  ZORM::Json obj;
  for (const auto& [k, v] : kv) obj.add(k, v);
  return obj;
}

// SQL DDL per table (used only for the SQL backends; jsonfile auto-creates).
const char* kCreateOrders =
    "create table if not exists orders ("
    "id varchar(32) primary key, region varchar(32), order_date varchar(16), "
    "amount double, status varchar(16), product varchar(32))";
const char* kCreateUsers =
    "create table if not exists users ("
    "id varchar(32) primary key, name varchar(32), created varchar(16))";
const char* kCreateProducts =
    "create table if not exists products ("
    "id varchar(32) primary key, name varchar(32), price double, category varchar(32))";
const char* kCreateAnalytics =
    "create table if not exists analytics ("
    "id varchar(32) primary key, note varchar(255))";

const char* ddlFor(const std::string& table) {
  if (table == "orders") return kCreateOrders;
  if (table == "users") return kCreateUsers;
  if (table == "products") return kCreateProducts;
  if (table == "analytics") return kCreateAnalytics;
  return nullptr;
}

// Inserts `rows` into `table` on `db` via insertBatch (single-call batch).
// Returns true when the call succeeded (status 200).
bool seedTable(ZORM::Idb* db, const std::string& table,
               const std::vector<ZORM::Json>& rows) {
  if (rows.empty()) return true;
  ZORM::Json elements(ZORM::JsonType::Array);
  for (const auto& r : rows) elements.push_back(ZORM::Json(r));
  const ZORM::Json rs = db->insertBatch(table, elements, "id");
  return rs["status"].toInt() == 200;
}

// True when `table` needs seeding on `db` (absent OR empty). SQL backends
// probe sqlite_master / information_schema then count; jsonfile reads the
// store's top-level keys via a select probe. On any probe failure we err
// toward seeding (conservative: rerunning setup on a broken probe is safe).
bool tableNeedsSeed(ZORM::Idb* db, const std::string& table, const std::string& dbType) {
  if (dbType == "sqlite3") {
    ZORM::Json rs = db->querySql(
        "select name from sqlite_master where type='table' and name = '" + table + "'");
    if (rs["status"].toInt() != 200 || rs["data"].isError() || rs["data"].size() == 0)
      return true;  // absent (or probe failed) - try seeding
    // Present: empty check via a plain select of one row.
    ZORM::Json params;
    params.add("size", ZORM::Json("1"));
    const ZORM::Json crs = db->select(table, params);
    if (crs["status"].toInt() != 200) return true;  // probe failed - try seeding
    return crs["data"].isArray() && crs["data"].size() == 0;
  }
  if (dbType == "jsonfile") {
    // The store is a JSON array of {table,...}; a select on a missing table
    // returns non-200. Present-but-empty also needs seeding (return true).
    ZORM::Json params;
    params.add("size", ZORM::Json("1"));
    const ZORM::Json rs = db->select(table, params);
    if (rs["status"].toInt() != 200) return true;  // absent
    return rs["data"].isArray() && rs["data"].size() == 0;
  }
  // mysql/postgres/dm8: information_schema probe, then select-one check.
  ZORM::Json rs = db->querySql(
      "select table_name from information_schema.tables where table_schema = "
      "current_schema() and table_name = '" + table + "'");
  if (rs["status"].toInt() != 200 || rs["data"].isError() || rs["data"].size() == 0)
    return true;
  ZORM::Json params;
  params.add("size", ZORM::Json("1"));
  const ZORM::Json crs = db->select(table, params);
  if (crs["status"].toInt() != 200) return true;
  return crs["data"].isArray() && crs["data"].size() == 0;
}

// Orders: 2025 + 2026 across 5 regions, with amount + status + product_ref.
// Every row carries an explicit id: the SQL backends' insertBatch does not
// auto-generate ids (only jsonfile's create does), and demo rows must be
// deterministic across runs so setup is idempotent.
std::vector<ZORM::Json> makeOrders() {
  std::vector<ZORM::Json> out;
  const char* regions[] = {"north", "south", "east", "west", "central"};
  const char* statuses[] = {"paid", "pending", "shipped", "refunded"};
  const char* products[] = {"widget", "gadget", "gizmo", "sprocket"};
  unsigned seed = 12345;
  auto rnd = [&seed]() mutable {
    seed = seed * 1103515245u + 12345u;
    return static_cast<double>((seed >> 16) & 0x7fff) / 32767.0;
  };
  for (int i = 0; i < 500; ++i) {
    const int year = (i % 2 == 0) ? 2026 : 2025;
    const int month = 1 + static_cast<int>(rnd() * 12);
    const int day = 1 + static_cast<int>(rnd() * 28);
    char date[16];
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d", year, month, day);
    const double amount = 10.0 + rnd() * 990.0;
    char amountText[32];
    std::snprintf(amountText, sizeof(amountText), "%.2f", amount);
    char id[16];
    std::snprintf(id, sizeof(id), "o%04d", i);
    out.push_back(row({
        {"id", id},
        {"region", regions[static_cast<int>(rnd() * 5)]},
        {"order_date", date},
        {"amount", ZORM::Json(amountText)},
        {"status", statuses[static_cast<int>(rnd() * 4)]},
        {"product", products[static_cast<int>(rnd() * 4)]},
    }));
  }
  return out;
}

// Users: id generated by the backend; name + created date.
std::vector<ZORM::Json> makeUsers() {
  std::vector<ZORM::Json> out;
  const char* names[] = {"ada", "bob", "carol", "dave", "erin",
                         "frank", "grace", "heidi", "ivan", "judy"};
  unsigned seed = 999;
  auto rnd = [&seed]() mutable {
    seed = seed * 1103515245u + 12345u;
    return static_cast<double>((seed >> 16) & 0x7fff) / 32767.0;
  };
  for (int i = 0; i < 50; ++i) {
    const int year = (i < 30) ? 2026 : 2025;
    const int month = 1 + static_cast<int>(rnd() * 12);
    char date[16];
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d", year, month, 1 + static_cast<int>(rnd() * 28));
    char id[16];
    std::snprintf(id, sizeof(id), "u%04d", i);
    out.push_back(row({
        {"id", id},
        {"name", names[static_cast<int>(rnd() * 10)]},
        {"created", date},
    }));
  }
  return out;
}

// Products: name + price + category.
std::vector<ZORM::Json> makeProducts() {
  std::vector<ZORM::Json> out;
  struct P { const char* name; double price; const char* category; };
  const P ps[] = {
      {"widget", 25.0, "tools"},  {"gadget", 120.0, "electronics"},
      {"gizmo", 45.5, "tools"},   {"sprocket", 8.75, "hardware"},
      {"thunder", 299.0, "electronics"}, {"bolt", 60.0, "hardware"},
      {"flux-capacitor", 99.0, "electronics"}, {"anvil", 210.0, "tools"},
      {"doohickey", 15.0, "hardware"}, {"whatsit", 155.0, "tools"},
      {"gearbox", 340.0, "electronics"}, {"pulley", 12.0, "hardware"},
      {"oscillator", 88.0, "electronics"}, {"hammer", 22.0, "tools"},
      {"latch", 6.5, "hardware"}, {"relay", 33.0, "electronics"},
      {"drill", 180.0, "tools"}, {"washer", 1.25, "hardware"},
      {"sensor", 65.0, "electronics"}, {"lever", 42.0, "tools"},
  };
  for (size_t i = 0; i < std::size(ps); ++i) {
    char price[32];
    std::snprintf(price, sizeof(price), "%.2f", ps[i].price);
    char id[16];
    std::snprintf(id, sizeof(id), "p%04zu", i);
    out.push_back(row({{"id", id}, {"name", ps[i].name}, {"price", ZORM::Json(price)},
                       {"category", ps[i].category}}));
  }
  return out;
}

// Analytics: the demo's "write target" table (initially empty).
std::vector<ZORM::Json> makeAnalytics() { return {}; }

}  // namespace

bool setupDemoData(ZORM::Idb* db, const std::string& dbType, const std::string& dbName) {
  if (db == nullptr) return false;
  // SQL backends need explicit DDL; jsonfile auto-creates tables on insert.
  const bool isSqlLike = (dbType != "jsonfile");
  bool ok = true;
  const auto seed = [&](const std::string& table, const std::vector<ZORM::Json>& rows) {
    if (!ok) return;
    if (isSqlLike) {
      const char* ddl = ddlFor(table);
      if (ddl != nullptr) {
        const ZORM::Json rs = db->execSql(ddl);
        if (rs["status"].toInt() != 200) { ok = false; return; }
      }
    }
    if (!tableNeedsSeed(db, table, dbType)) return;
    ok = seedTable(db, table, rows);
  };
  seed("orders", makeOrders());
  seed("users", makeUsers());
  seed("products", makeProducts());
  seed("analytics", makeAnalytics());
  return ok;
}

}  // namespace zorm_demo
