#pragma once

// Shared contract test suite for every ZORM backend (sqlite3 / mysql /
// postgres / dm8 / jsonfile).
//
// ONE suite, MANY instances: the test bodies below are written once against
// the Idb.h interface and instantiated per backend through ZORM_CONTRACT_TESTS()
// with a small Env subclass that only supplies connection options and the DDL
// dialect (drop/create of the shared table).
//
// The bodies assert the *intersection* of backend behaviour:
//   * status codes and "data" contents, which every backend reports;
//   * fields that only some backends return (affectedRows / insertId /
//     records / pages) are deliberately NOT asserted here - backend-specific
//     suites cover them.
// Behaviour that legitimately differs (metadata catalogs, auto-id, upsert,
// structured transactions, ...) is tested in each backend's own file.
//
// Seed data design (6 rows, chosen so every expected count is hand-derivable):
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

#include <gtest/gtest.h>

#include "Idb.h"
#include "DbBase.h"

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

// Backend adapter: connection + DDL dialect. One instance per test binary.
class Env {
public:
	virtual ~Env() = default;

	// Fresh backend instance (a DbBase or a concrete backend); owned here.
	virtual Idb* connect() = 0;
	// Full schema sequence: drop-if-exists first, then create (+ constraints).
	virtual vector<string> schemaSqls() const = 0;

	// Table reference for the RAW SQL in the shared bodies. DM8 quotes its
	// identifiers lower-case (see Dm8Db's generated SQL); on a case-sensitive
	// DM8 server an unquoted name folds to upper case and never matches, so
	// the dm8 env overrides this with the quoted form.
	virtual string rawTable() const {
		return kTable;
	}

	// Raw-SQL statement builders (dialect hooks). Defaults use the unquoted
	// names accepted by sqlite3 / mysql / postgres / jsonfile; the dm8 env
	// overrides them with quoted identifiers (see rawTable above).
	virtual string rawSelectAll() const {
		return std::string("select * from ") + kTable;
	}
	virtual string rawInsert(const string& valuesList) const {
		return std::string("insert into ") + kTable + " (id,name,age,score) values (" + valuesList + ")";
	}
	virtual string rawUpdate(const string& column, const string& sqlLiteral, const string& id) const {
		return std::string("update ") + kTable + " set " + column + " = " + sqlLiteral + " where id = '" + id + "'";
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
// Read contract
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

	// Field projection via the vector argument (note: projecting through a
	// "fields" key inside params is a jsonfile extension, tested there)
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

	// A missing table must not report success (202 for schemaless backends,
	// 701 "operation failed" for SQL backends).
	EXPECT_NE(db.select("no_such_table", Json(JsonType::Object))["status"].toInt(), 200);

	// querySql without WHERE: params act as conditions
	result = db.querySql(env->rawSelectAll(), Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");
}

// ─────────────────────────────────────────────────────────────────────────────
// Write contract
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

	// Note: plain BMP Chinese is the shared denominator here. 4-byte UTF-8
	// (emoji) is rejected by DM8 servers running a GBK-family database
	// charset; the emoji round-trip is asserted by the backends that store it
	// (sqlite3 / mysql / postgres / jsonfile).
	result = db.create(kTable, Json{{"id", "w003"}, {"name", "中文测试-凯文"}, {"age", 1}, {"score", 1.0}});
	ASSERT_EQ(result["status"].toInt(), 200);
	result = db.select(kTable, Json{{"id", "w003"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "中文测试-凯文");

	// Missing required parameter is rejected, not successful
	// (remove with an empty object is rejected by the jsonfile backend; the
	// SQL backends happily run "delete ... where id = ''", so it is not a
	// shared assertion.)
	EXPECT_NE(db.update(kTable, Json{{"score", 1}})["status"].toInt(), 200);
	EXPECT_NE(db.create(kTable, Json(JsonType::Object))["status"].toInt(), 200);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query contract: fuzzy / ins / lks / ors / pagination / aggregates / sort
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

	// Pagination over the plain filter path
	result = db.select(kTable, Json{{"page", 1}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);
	result = db.select(kTable, Json{{"page", 2}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);
	EXPECT_EQ(db.select(kTable, Json{{"page", 999}, {"size", 3}})["status"].toInt(), 202);

	// Pagination combined with sort (general path: filter -> sort -> slice)
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
// DAO contract: insertBatch / transGo (SQL text style) / execSql
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

	// transGo with SQL text elements carrying literal values (the shared
	// denominator: postgres speaks $n, sqlite3/mysql/dm8/jsonfile speak `?`,
	// so placeholder binding is asserted per backend).
	// Elements are built via the object API (not string parsing) because the
	// dm8 table reference itself contains double quotes.
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

	// transGo rolls back the whole transaction when a step fails
	Json rollbackArr(JsonType::Array);
	rollbackArr.add(sqlElement(env->rawInsert("'rb001','will-not-stay',1,1.0")));
	rollbackArr.add(sqlElement("this is not sql"));
	result = db.transGo(rollbackArr);
	EXPECT_NE(result["status"].toInt(), 200);
	EXPECT_EQ(db.select(kTable, Json{{"id", "rb001"}})["status"].toInt(), 202);

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
	}
