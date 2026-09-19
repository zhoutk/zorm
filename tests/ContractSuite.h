// tests/ContractSuite.h
// ----------------------------------------------------------------------------
// ONE shared contract suite for every ZORM backend (sqlite3 / mysql /
// postgres / dm8 / jsonfile), driven by tests/dbconfig.json (gels-style).
//
// The suite is instantiated once per backend through ZORM_CONTRACT_TESTS() in
// a single translation unit. The backend under test is selected by
// --dialect / ZORM_DB_DIALECT / dbconfig.json "db_dialect".
//
// The bodies assert the intersection of backend behaviour (status codes and
// "data" contents), plus the features every backend now supports after the
// parity work: auto-generated ids, records/pages, structured transGo,
// single-element insertBatch. Behaviour that genuinely differs (metadata
// catalogs, placeholder syntax, null rendering) is isolated behind the
// BackendConfig hooks in dbconfig.json, so one config value switches the
// whole suite - exactly like gels' db_dialect.
//
// Seed data design (6 rows, every expected count is hand-derivable):
//   id         name         age   score
//   a1b2c3d4   Kevin 凯文    18   99.99   <- unicode, sorts before "test*"
//   a2b3c4d5   test001      19   98.88   <- contains "001" and "test"
//   a3b4c5d6   test002      20   97.77   <- the numeric pivot value
//   a4b5c6d7   test003      21   96.66
//   a5b6c7d8   test004      22   95.55
//   a6b7c8d9   test005      23   94.44   <- contains "005" and "test"
//   * age 18..23 consecutive and unique -> exact IN / range / between counts
//   * score strictly descending -> independent ordering check
//   * names give exact fuzzy / like hit counts ("test" -> 5, "001" -> 1)
// ----------------------------------------------------------------------------
#pragma once

#include <gtest/gtest.h>

#include "DbBase.h"
#include "TestConfig.h"

#include <memory>
#include <string>
#include <vector>

namespace ZORM {
namespace contract {

using std::string;
using std::vector;
using namespace ZJSON;

inline constexpr const char* kTable = "table_for_test";

inline Json seedRows() {
	Json rows(JsonType::Array);
	rows.add(Json{{"id", "a1b2c3d4"}, {"name", "Kevin 凯文"}, {"age", 18}, {"score", 99.99}});
	rows.add(Json{{"id", "a2b3c4d5"}, {"name", "test001"}, {"age", 19}, {"score", 98.88}});
	rows.add(Json{{"id", "a3b4c5d6"}, {"name", "test002"}, {"age", 20}, {"score", 97.77}});
	rows.add(Json{{"id", "a4b5c6d7"}, {"name", "test003"}, {"age", 21}, {"score", 96.66}});
	rows.add(Json{{"id", "a5b6c7d8"}, {"name", "test004"}, {"age", 22}, {"score", 95.55}});
	rows.add(Json{{"id", "a6b7c8d9"}, {"name", "test005"}, {"age", 23}, {"score", 94.44}});
	return rows;
}

// Backend adapter: connection + schema + dialect hooks, driven by config.
class Env {
public:
	virtual ~Env() = default;

	// Fresh backend instance (a DbBase or a concrete backend); owned here.
	virtual Idb* connect() = 0;
	// Full schema sequence: drop-if-exists first, then create (+ constraints).
	virtual vector<string> schemaSqls() const = 0;

	// Table reference for RAW SQL (dm8 quotes its identifiers lower-case).
	virtual string rawTable() const {
		return g_config.rawTable.empty() ? kTable : g_config.rawTable;
	}

	// Column quote char for raw SQL (dm8 quotes lower-case identifiers).
	virtual string quoteColumn() const {
		return g_config.quoteColumn;
	}

	// Raw-SQL statement builders (dialect hooks from dbconfig.json).
	virtual string rawSelectAll() const {
		return std::string("select * from ") + rawTable();
	}
	virtual string rawInsert(const string& valuesList) const {
		const string q = quoteColumn();
		return std::string("insert into ") + rawTable() + " (" + q + "id" + q + "," + q + "name" + q + "," + q + "age" + q + "," + q + "score" + q + ") values (" + valuesList + ")";
	}
	virtual string rawUpdate(const string& column, const string& sqlLiteral, const string& id) const {
		const string q = quoteColumn();
		return std::string("update ") + rawTable() + " set " + q + column + q + " = " + sqlLiteral + " where " + q + "id" + q + " = '" + id + "'";
	}

	// How a NULL database value surfaces in reads ("empty" | "null-string" |
	// "json-null"), from dbconfig.json.
	virtual string nullRendering() const {
		return g_config.nullRendering;
	}

	// Placeholder syntax ("?" or "$n"), from dbconfig.json.
	virtual string rawPlaceholder() const {
		return g_config.placeholder;
	}

	// True when querySql accepts "? " placeholders in a WHERE clause.
	// jsonfile's SQL shim only routes plain "select * from t" (conditions come
	// from `params`); execSql/transGo placeholders are fully supported.
	virtual bool supportsWherePlaceholders() const {
		return g_config.supportsWherePlaceholders;
	}

	// True when create()/insertBatch() auto-create the table (jsonfile keeps
	// an implicit schema and creates missing tables on write). SQL backends
	// need explicit DDL first, so the schema-driven Env creates the extra
	// tables in schemaSqls().
	virtual bool autoCreateTables() const {
		return g_config.autoCreateTables;
	}

	// Metadata-catalog query; a missing object yields 202.
	virtual string catalogSql() const {
		return g_config.catalogSql;
	}

	Idb& db() {
		return *db_;
	}

	void connectOnce() {
		if (!db_) {
			db_.reset(connect());
		}
	}

	// Drop + create + seed. Returns false (and fails the current test) on error.
	bool reset() {
		return schema() && seed();
	}

	// Drop + create only.
	bool schema() {
		for (const string& sql : schemaSqls()) {
			const Json rs = db_->execSql(sql);
			if (rs["status"].toInt() != 200) {
				ADD_FAILURE() << "schema sql failed: " << sql << " -> " << rs.toString();
				return false;
			}
		}
		return true;
	}

	bool seed() {
		const Json rs = db_->insertBatch(kTable, seedRows());
		if (rs["status"].toInt() != 200) {
			ADD_FAILURE() << "seed insertBatch failed: " << rs.toString();
			return false;
		}
		return true;
	}

private:
	std::unique_ptr<Idb> db_;
};

// Set by each backend's main() before RUN_ALL_TESTS().
inline Env* env = nullptr;

#define CONTRACT_RESET()                                       \
	do {                                                       \
		if (!::ZORM::contract::env->reset()) {                 \
			return;                                            \
		}                                                      \
	} while (0)

// ─────────────────────────────────────────────────────────────────────────────
// Read contract (gels read-contract parity)
// ─────────────────────────────────────────────────────────────────────────────

inline void Read() {
	CONTRACT_RESET();
	Idb& db = env->db();

	// Select by id
	Json result = db.select(kTable, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	ASSERT_GE(result["data"].size(), 1);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");
	EXPECT_EQ(result["data"][0]["age"].toInt(), 18);
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 99.99);

	// Select with multiple conditions
	result = db.select(kTable, Json{{"id", "a1b2c3d4"}, {"age", 18}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 1);

	// Reject wrong exact match / accept correct exact match
	EXPECT_EQ(db.select(kTable, Json{{"id", "a1b2c3d4"}, {"age", 18}, {"name", "Kevin"}})["status"].toInt(), 202);
	EXPECT_EQ(db.select(kTable, Json{{"id", "a1b2c3d4"}, {"age", 18}, {"name", "Kevin 凯文"}})["status"].toInt(), 200);

	// Field projection via the vector argument
	result = db.select(kTable, Json{{"id", "a2b3c4d5"}}, vector<string>{"name", "score"});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "test001");
	EXPECT_TRUE(result["data"][0]["id"].isError());

	// Fuzzy search
	result = db.select(kTable, Json{{"name", "Kevin"}, {"fuzzy", 1}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");

	// Select all
	result = db.select(kTable, Json(JsonType::Object));
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 6);

	// Non-existent id / non-existent pattern
	EXPECT_EQ(db.select(kTable, Json{{"id", "nonexistent"}})["status"].toInt(), 202);
	EXPECT_EQ(db.select(kTable, Json{{"name", "ZZZZ_NOT_FOUND"}, {"fuzzy", 1}})["status"].toInt(), 202);

	// A missing table must not report success
	EXPECT_NE(db.select("no_such_table", Json(JsonType::Object))["status"].toInt(), 200);

	// querySql without WHERE: params act as conditions
	result = db.querySql(env->rawSelectAll(), Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");
}

// ─────────────────────────────────────────────────────────────────────────────
// Write contract (gels write-contract parity + auto-id parity)
// ─────────────────────────────────────────────────────────────────────────────

inline void Write() {
	CONTRACT_RESET();
	Idb& db = env->db();

	// Update a single field, verified through a fresh read
	Json result = db.update(kTable, Json{{"id", "a1b2c3d4"}, {"score", 6.6}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 6.6);

	// Delete, then confirm the row is gone
	result = db.remove(kTable, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(db.select(kTable, Json{{"id", "a1b2c3d4"}})["status"].toInt(), 202);

	// Update multiple fields at once
	result = db.update(kTable, Json{{"id", "a5b6c7d8"}, {"name", "test888"}, {"score", 23.27}, {"age", 22}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "a5b6c7d8"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "test888");
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 23.27);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 22);

	// Update / delete of a non-existent id must not fail
	EXPECT_EQ(db.update(kTable, Json{{"id", "no_such_id"}, {"name", "ghost"}})["status"].toInt(), 200);
	EXPECT_EQ(db.remove(kTable, Json{{"id", "already_deleted"}})["status"].toInt(), 200);

	// Create with all fields, then round-trip zero / negative / unicode values
	result = db.create(kTable, Json{{"id", "w001"}, {"name", "edge-case"}, {"age", 0}, {"score", 0.0}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "w001"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 0);
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 0.0);

	result = db.create(kTable, Json{{"id", "w002"}, {"name", "negative"}, {"age", -5}, {"score", -3.14}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "w002"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), -5);
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), -3.14);

	result = db.create(kTable, Json{{"id", "w003"}, {"name", "中文测试-凯文"}, {"age", 1}, {"score", 1.0}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "w003"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "中文测试-凯文");

	// Create with a partial payload: the missing fields must default (null for
	// jsonfile, empty string for SQL clients) - asserted through config hooks.
	result = db.create(kTable, Json{{"name", "zhoutk"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	const std::string createdId = result["id"].toString();
	ASSERT_EQ(createdId.size(), static_cast<size_t>(8));
	EXPECT_TRUE(createdId.find_first_not_of("0123456789abcdef") == std::string::npos);
	result = db.select(kTable, Json{{"id", createdId}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "zhoutk");
	EXPECT_EQ(result["data"][0]["id"].toString(), createdId);
	if (env->nullRendering() == "json-null") {
		EXPECT_TRUE(result["data"][0]["age"].isNull());
		EXPECT_TRUE(result["data"][0]["score"].isNull());
	} else if (env->nullRendering() == "null-string") {
		EXPECT_EQ(result["data"][0]["age"].toString(), "null");
		EXPECT_EQ(result["data"][0]["score"].toString(), "null");
	} else {
		EXPECT_EQ(result["data"][0]["age"].toString(), "");
		EXPECT_EQ(result["data"][0]["score"].toString(), "");
	}

	// Create with a manual id, then upsert through create (gels parity: the
	// SQL backends run INSERT ... ON DUPLICATE/upsert, jsonfile upserts).
	result = db.create(kTable, Json{{"id", "manual001"}, {"name", "manual-row"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["id"].toString(), "manual001");
	result = db.create(kTable, Json{{"id", "manual001"}, {"name", "upserted-name"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "manual001"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "upserted-name");

	// Empty-string id is replaced by a generated one
	result = db.create(kTable, Json{{"id", ""}, {"name", "empty-id"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["id"].toString().size(), static_cast<size_t>(8));

	// Missing required parameter is rejected, not successful
	EXPECT_NE(db.update(kTable, Json{{"score", 1}})["status"].toInt(), 200);
	EXPECT_NE(db.create(kTable, Json(JsonType::Object))["status"].toInt(), 200);
	EXPECT_NE(db.remove(kTable, Json(JsonType::Object))["status"].toInt(), 200);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query contract (gels query-contract parity + records/pages parity)
// ─────────────────────────────────────────────────────────────────────────────

inline void Query() {
	CONTRACT_RESET();
	Idb& db = env->db();

	// Fuzzy
	Json result = db.select(kTable, Json{{"name", "test"}, {"fuzzy", 1}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 5);

	// IN / LIKE / OR
	EXPECT_EQ(db.select(kTable, Json{{"ins", "age,20,21,23"}})["data"].size(), 3);
	EXPECT_EQ(db.select(kTable, Json{{"lks", "name,001,age,23"}})["data"].size(), 2);
	EXPECT_EQ(db.select(kTable, Json{{"ors", "age,19,age,23"}})["data"].size(), 2);

	// Pagination over the plain filter path (records/pages parity)
	result = db.select(kTable, Json{{"page", 1}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);
	EXPECT_EQ(result["records"].toInt(), 6);
	EXPECT_EQ(result["pages"].toInt(), 2);
	result = db.select(kTable, Json{{"page", 2}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);
	EXPECT_EQ(db.select(kTable, Json{{"page", 999}, {"size", 3}})["status"].toInt(), 202);

	// Pagination combined with sort
	result = db.select(kTable, Json{{"page", 2}, {"size", 2}, {"sort", "age asc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	ASSERT_EQ(result["data"].size(), 2);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 20);
	EXPECT_EQ(result["data"][1]["age"].toInt(), 21);

	// Pagination combined with an aggregate
	result = db.select(kTable, Json{{"page", 1}, {"size", 2}, {"count", "*,total"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["total"].toInt(), 6);

	// Count with a projected field list
	result = db.select(kTable, Json{{"count", "1,total"}}, vector<string>{"id"});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["total"].toInt(), 6);

	// Sum with a filter
	result = db.select(kTable, Json{{"sum", "age,agesum"}, {"age", "<=,20"}}, vector<string>{"id"});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["agesum"].toInt(), 57);

	// Comparison operators (ages still 18..23 at this point)
	EXPECT_EQ(db.select(kTable, Json{{"age", ">,21"}})["data"].size(), 2);
	EXPECT_EQ(db.select(kTable, Json{{"age", ">=,19,<=,22"}})["data"].size(), 4);
	EXPECT_EQ(db.select(kTable, Json{{"age", "<,20"}})["data"].size(), 2);
	EXPECT_EQ(db.select(kTable, Json{{"age", "<>,20"}})["data"].size(), 5);
	EXPECT_EQ(db.select(kTable, Json{{"age", "=,20"}})["data"].size(), 1);

	// Sort
	result = db.select(kTable, Json{{"sort", "age asc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 18);
	result = db.select(kTable, Json{{"sort", "age desc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 23);

	// Group + count (with a tie so "total desc" is meaningful)
	result = db.update(kTable, Json{{"id", "a4b5c6d7"}, {"age", 22}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"group", "age"}, {"count", "*,total"}, {"sort", "total desc"}}, vector<string>{"age"});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["total"].toInt(), 2);

	// Empty matches
	EXPECT_EQ(db.select(kTable, Json{{"ins", "age,999"}})["status"].toInt(), 202);
	EXPECT_EQ(db.select(kTable, Json{{"lks", "name,ZZZZ"}})["status"].toInt(), 202);

	// OR conditions de-duplicate overlapping rows
	result = db.select(kTable, Json{{"ors", "age,18,age,18,age,19"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// DAO contract: insertBatch / transGo (structured + SQL text) / execSql
// ─────────────────────────────────────────────────────────────────────────────

inline void Dao() {
	CONTRACT_RESET();
	Idb& db = env->db();

	// insertBatch of two rows
	Json batchRows(JsonType::Array);
	batchRows.add(Json{{"id", "batch001"}, {"name", "batch-one"}, {"age", 40}, {"score", 1.11}});
	batchRows.add(Json{{"id", "batch002"}, {"name", "batch-two"}, {"age", 41}, {"score", 2.22}});
	Json result = db.insertBatch(kTable, batchRows);
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "batch001"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "batch-one");

	// insertBatch of exactly one element is accepted (gels/orm parity)
	Json one(JsonType::Array);
	one.add(Json{{"id", "one001"}, {"name", "one-row"}, {"age", 1}, {"score", 0.5}});
	result = db.insertBatch(kTable, one);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);

	// insertBatch with an empty array is rejected
	EXPECT_EQ(db.insertBatch(kTable, Json(JsonType::Array))["status"].toInt(), 301);

	// transGo with SQL text elements (literal values; the shared denominator)
	auto sqlElement = [](const std::string& text, Json values = Json(JsonType::Array)) {
		Json el;
		el.add("text", text);
		if (values.isArray() && values.size() > 0) {
			el.add("values", values);
		}
		return el;
	};
	Json sqlArr(JsonType::Array);
	sqlArr.add(sqlElement(env->rawInsert("'tx001','text-1',21,78.48")));
	sqlArr.add(sqlElement(env->rawInsert("'tx002','text-2',22,23.27")));
	result = db.transGo(sqlArr);
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "tx001"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "text-1");
	result = db.select(kTable, Json{{"id", "tx002"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 22);

	// Structured transGo: Insert (gels parity)
	Json structArr(JsonType::Array);
	structArr.add(Json{{"table", kTable}, {"method", "Insert"}, {"params", Json{{"id", "st01"}, {"name", "struct-1"}, {"age", 30}, {"score", 3.01}}}});
	structArr.add(Json{{"table", kTable}, {"method", "Insert"}, {"params", Json{{"id", "st02"}, {"name", "struct-2"}, {"age", 31}, {"score", 3.02}}}});
	result = db.transGo(structArr);
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "st01"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "struct-1");
	result = db.select(kTable, Json{{"id", "st02"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 31);

	// Structured Update (id is top-level, not inside params)
	Json txUpdate(JsonType::Array);
	txUpdate.add(Json{{"table", kTable}, {"method", "Update"}, {"id", "st01"}, {"params", Json{{"name", "struct-updated"}}}});
	result = db.transGo(txUpdate);
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "st01"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "struct-updated");

	// Structured Delete
	Json txDelete(JsonType::Array);
	txDelete.add(Json{{"table", kTable}, {"method", "Delete"}, {"id", "st02"}});
	result = db.transGo(txDelete);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(db.select(kTable, Json{{"id", "st02"}})["status"].toInt(), 202);

	// Structured Batch
	Json txBatch(JsonType::Array);
	Json batchParams(JsonType::Array);
	batchParams.add(Json{{"id", "bt01"}, {"name", "batch-tx-1"}, {"age", 1}, {"score", 0.1}});
	batchParams.add(Json{{"id", "bt02"}, {"name", "batch-tx-2"}, {"age", 2}, {"score", 0.2}});
	txBatch.add(Json{{"table", kTable}, {"method", "Batch"}, {"params", batchParams}});
	result = db.transGo(txBatch);
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "bt01"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "batch-tx-1");
	result = db.select(kTable, Json{{"id", "bt02"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 2);

	// transGo rolls back the whole transaction when a step fails
	Json rollbackArr(JsonType::Array);
	rollbackArr.add(sqlElement(env->rawInsert("'rb001','will-not-stay',1,1.0")));
	rollbackArr.add(sqlElement("this is not sql"));
	result = db.transGo(rollbackArr);
	EXPECT_NE(result["status"].toInt(), 200);
	EXPECT_EQ(db.select(kTable, Json{{"id", "rb001"}})["status"].toInt(), 202);

	// A structured element that matches no method fails and rolls back
	Json badStruct(JsonType::Array);
	badStruct.add(Json{{"table", kTable}, {"method", "Insert"}, {"params", Json{{"id", "rb002"}, {"name", "ok"}}}});
	badStruct.add(Json{{"table", kTable}, {"method", "Nonsense"}, {"params", Json{{"x", 1}}}});
	result = db.transGo(badStruct);
	EXPECT_NE(result["status"].toInt(), 200);
	EXPECT_EQ(db.select(kTable, Json{{"id", "rb002"}})["status"].toInt(), 202);

	// transGo with an empty array is rejected
	EXPECT_EQ(db.transGo(Json(JsonType::Array))["status"].toInt(), 301);

	// execSql with literal values, verified through reads
	result = db.execSql(env->rawUpdate("name", "'tx-updated'", "tx002"));
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "tx002"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "tx-updated");

	result = db.execSql(env->rawUpdate("age", "33", "tx002"));
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "tx002"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 33);

	// execSql with garbage must not report success
	EXPECT_NE(db.execSql("", Json(), Json(JsonType::Array))["status"].toInt(), 200);
	EXPECT_NE(db.execSql("this is not sql")["status"].toInt(), 200);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases: missing tables, batch-via-create, second table
// ─────────────────────────────────────────────────────────────────────────────

inline void EdgeCases() {
	CONTRACT_RESET();
	Idb& db = env->db();

	// Select from a non-existent table must not report success
	EXPECT_NE(db.select("empty_table", Json{{"id", "anything"}})["status"].toInt(), 200);

	// The second/third tables exist in the schema (created by the Env's DDL
	// for SQL backends; auto-created on write by jsonfile).
	Json createResult = db.create("second_table", Json{{"name", "first"}});
	ASSERT_EQ(createResult["status"].toInt(), 200);
	EXPECT_EQ(createResult["id"].toString().size(), static_cast<size_t>(8));
	Json selectResult = db.select("second_table", Json{{"id", createResult["id"].toString()}});
	ASSERT_EQ(selectResult["status"].toInt(), 200);
	EXPECT_EQ(selectResult["data"][0]["name"].toString(), "first");

	// Batch-via-create (array payload)
	Json batchViaCreate(JsonType::Array);
	batchViaCreate.add(Json{{"id", "bc01"}, {"name", "batch-create-1"}});
	batchViaCreate.add(Json{{"id", "bc02"}, {"name", "batch-create-2"}});
	Json batchResult = db.create("second_table", batchViaCreate);
	ASSERT_EQ(batchResult["status"].toInt(), 200);
	EXPECT_EQ(batchResult["affectedRows"].toInt(), 2);
	selectResult = db.select("second_table", Json{{"id", "bc02"}});
	ASSERT_EQ(selectResult["status"].toInt(), 200);
	EXPECT_EQ(selectResult["data"][0]["name"].toString(), "batch-create-2");

	// Create with an empty string id -> auto-generated
	createResult = db.create("second_table", Json{{"id", ""}, {"name", "empty-id"}});
	ASSERT_EQ(createResult["status"].toInt(), 200);
	EXPECT_EQ(createResult["id"].toString().size(), static_cast<size_t>(8));

	// Multiple tables coexist
	Json multiTableResult = db.create("third_table", Json{{"id", "s001"}, {"value", "second-data"}});
	ASSERT_EQ(multiTableResult["status"].toInt(), 200);
	Json secondSelect = db.select("third_table", Json{{"id", "s001"}});
	ASSERT_EQ(secondSelect["status"].toInt(), 200);
	EXPECT_EQ(secondSelect["data"][0]["value"].toString(), "second-data");
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata catalog: a missing view yields 202 on every backend
// ─────────────────────────────────────────────────────────────────────────────

inline void MetadataCatalog() {
	env->connectOnce();
	Idb& db = env->db();
	ASSERT_TRUE(env->reset());

	const string sql = env->catalogSql();
	if (!sql.empty()) {
		EXPECT_EQ(db.querySql(sql)["status"].toInt(), 202);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Placeholder SQL: execSql / transGo / querySql with bound parameters
// (placeholder syntax from config: "?" or "$n")
// ─────────────────────────────────────────────────────────────────────────────

inline void PlaceholderSql() {
	CONTRACT_RESET();
	Idb& db = env->db();
	const bool dollar = env->rawPlaceholder() == "$n";
	const string table = env->rawTable();
	const string q = env->quoteColumn();
	const string colId = q + "id" + q;
	const string colName = q + "name" + q;

	// execSql with placeholders
	Json values(JsonType::Array);
	values.add("placeholder-1");
	values.add("a1b2c3d4");
	Json rs;
	if (dollar) {
		rs = db.execSql("update " + table + " set " + colName + " = $1 where " + colId + " = $2", Json(), values);
	} else {
		rs = db.execSql("update " + table + " set " + colName + " = ? where " + colId + " = ?", Json(), values);
	}
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select(kTable, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "placeholder-1");

	// transGo element with placeholders and values (built via the object API
	// so quoted identifiers like dm8's "dbtest"."table_for_test" cannot break
	// a JSON round-trip)
	Json sqlArr(JsonType::Array);
	const string colScore = q + "score" + q;
	const string colAge = q + "age" + q;
	const string cols = colId + "," + colName + "," + colAge + "," + colScore;
	{
		Json el;
		if (dollar) {
			el.add("text", "insert into " + table + " (" + cols + ") values ($1,$2,$3,$4)");
		} else {
			el.add("text", "insert into " + table + " (" + cols + ") values (?,?,?,?)");
		}
		Json vals(JsonType::Array);
		vals.add("ph001");
		vals.add("ph-name");
		vals.add(9);
		vals.add(9.9);
		el.add("values", vals);
		sqlArr.add(el);
	}
	{
		Json el;
		el.add("text", "insert into " + table + " (" + cols + ") values ('ph002','ph-literal',10,10.1)");
		sqlArr.add(el);
	}
	rs = db.transGo(sqlArr);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select(kTable, Json{{"id", "ph001"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "ph-name");

	// querySql with placeholders (WHERE placeholders are a SQL-backend feature;
	// jsonfile routes "select * from t" with conditions via `params`)
	if (env->supportsWherePlaceholders()) {
		Json arrObj(JsonType::Array);
		arrObj.add("ph-name");
		const string qName = q + "name" + q;
		if (dollar) {
			rs = db.querySql("select * from " + table + " where " + qName + " = $1", Json(), arrObj);
		} else {
			rs = db.querySql("select * from " + table + " where " + qName + " = ?", Json(), arrObj);
		}
		EXPECT_EQ(rs["status"].toInt(), 200);
	} else {
		// jsonfile: conditions ride in `params` (the querySql contract)
		rs = db.querySql("select * from table_for_test", Json{{"name", "ph-name"}});
		ASSERT_EQ(rs["status"].toInt(), 200);
		EXPECT_EQ(rs["data"].size(), 1);
		EXPECT_EQ(rs["data"][0]["id"].toString(), "ph001");
	}
}

}  // namespace contract
}  // namespace ZORM

// Instantiates the shared suite in the including translation unit.
// Requires ::ZORM::contract::env to be assigned before RUN_ALL_TESTS().
#define ZORM_CONTRACT_TESTS()                                                     \
	TEST(Contract, Read) {                                                        \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::Read();                                                 \
	}                                                                             \
	TEST(Contract, Write) {                                                       \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::Write();                                                \
	}                                                                             \
	TEST(Contract, Query) {                                                       \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::Query();                                                \
	}                                                                             \
	TEST(Contract, Dao) {                                                         \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::Dao();                                                  \
	}                                                                             \
	TEST(Contract, EdgeCases) {                                                   \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::EdgeCases();                                            \
	}                                                                             \
	TEST(Contract, MetadataCatalog) {                                             \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::MetadataCatalog();                                      \
	}                                                                             \
	TEST(Contract, PlaceholderSql) {                                              \
		::ZORM::contract::env->connectOnce();                                     \
		::ZORM::contract::PlaceholderSql();                                       \
	}
