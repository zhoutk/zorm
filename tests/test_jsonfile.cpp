// jsonfile backend: shared contract suite + jsonfile-specific behaviour
// (auto-generated ids, upsert semantics, affectedRows/records/pages, the SQL
// text shims, structured transactions and all file-hardening guarantees:
// corrupt-file protection, cross-process lock, atomic write, id index).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ContractSuite.h"
#include "FileLock.h"
#include "JsonFileDb.h"
#include "DbBase.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace ZORM;
using namespace ZORM::JsonFile;
using namespace ZJSON;

namespace jfd = ZORM::JsonFile::detail;

namespace {

namespace fs = std::filesystem;

constexpr const char* kTableName = contract::kTable;
constexpr const char* kCreateTableSql =
	"CREATE TABLE table_for_test (id text NOT NULL, name text DEFAULT NULL, "
	"age integer DEFAULT NULL, score real DEFAULT NULL, "
	"price decimal(10,2) DEFAULT NULL, ts datetime DEFAULT NULL, PRIMARY KEY (id))";

// Directory of the running executable, computed once from argv[0].
std::string gExecutableDirectory;

std::string executableDirectoryFromArgv(const char* argv0) {
	if (argv0 == nullptr || *argv0 == '\0') {
		return fs::current_path().generic_string();
	}
	std::error_code error;
	const fs::path absolute = fs::absolute(jfd::fsPath(argv0), error);
	if (error) {
		return fs::current_path().generic_string();
	}
	return jfd::genericUtf8(absolute.lexically_normal().parent_path());
}

std::string joinPath(const std::string& directory, const std::string& leaf) {
	std::string base = directory;
	while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
		base.pop_back();
	}
	if (base.empty()) {
		return leaf;
	}
	return base + "/" + leaf;
}

int scratchProcessId() {
#ifdef _WIN32
	return ::_getpid();
#else
	return static_cast<int>(::getpid());
#endif
}

// Dedicated scratch database per test binary run. Never point at a fixed file
// a developer's data may live in.
std::string scratchPath(const std::string& tag) {
	return joinPath(gExecutableDirectory,
					"zorm_jsonfile_" + tag + "_" + std::to_string(scratchProcessId()) + ".json");
}

bool fileExists(const std::string& path) {
	std::error_code error;
	return fs::exists(jfd::fsPath(path), error);
}

void writeFile(const std::string& path, const std::string& content) {
	std::ofstream out(jfd::fsPath(path), std::ios::binary | std::ios::trunc);
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::string& path) {
	std::string content;
	jfd::readWholeFile(jfd::fsPath(path), content);
	return content;
}

void removePath(const std::string& path) {
	std::error_code error;
	fs::remove(jfd::fsPath(path), error);
}

// Removes the database file plus everything a test run may leave next to it
// (lock file, atomic-write temp files, corruption backups).
void removeDatabaseFiles(const std::string& filePath) {
	removePath(filePath);
	removePath(filePath + ".lock");
	std::error_code error;
	const fs::path directory = jfd::fsPath(filePath).parent_path();
	const std::string name = jfd::genericUtf8(jfd::fsPath(filePath).filename());
	if (directory.empty()) {
		return;
	}
	for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
		const std::string candidate = jfd::genericUtf8(it->path().filename());
		if (candidate == name + ".lock" || candidate.rfind(name + ".tmp.", 0) == 0 ||
			candidate.rfind(name + ".corrupt.", 0) == 0) {
			std::error_code removeError;
			fs::remove(it->path(), removeError);
		}
	}
}

bool hasCorruptionBackup(const std::string& filePath) {
	const std::string prefix = jfd::genericUtf8(jfd::fsPath(filePath).filename()) + ".corrupt.";
	std::error_code error;
	for (fs::directory_iterator it(jfd::fsPath(filePath).parent_path(), error), end;
		 !error && it != end; it.increment(error)) {
		if (jfd::genericUtf8(it->path().filename()).rfind(prefix, 0) == 0) {
			return true;
		}
	}
	return false;
}

// - Fixture for jsonfile-specific tests --------------------------------------

class JsonFileDbTest : public ::testing::Test {
protected:
	void SetUp() override {
		filePath_ = scratchPath("fixture");
	}

	void TearDown() override {
		db_.reset();
		removeDatabaseFiles(filePath_);
	}

	// Fresh db with schema + seed data (CREATE TABLE then insertBatch).
	void reset() {
		db_.reset();
		removeDatabaseFiles(filePath_);
		db_ = JsonFileDb::createShared(filePath_);
		ASSERT_EQ(db_->execSql(kCreateTableSql)["status"].toInt(), 200);
		Json insertResult = db_->insertBatch(kTableName, contract::seedRows());
		ASSERT_EQ(insertResult["status"].toInt(), 200);
		ASSERT_EQ(insertResult["affectedRows"].toInt(), 6);
	}

	// Fresh db with schema only (no seed data).
	void resetEmpty() {
		db_.reset();
		removeDatabaseFiles(filePath_);
		db_ = JsonFileDb::createShared(filePath_);
		ASSERT_EQ(db_->execSql(kCreateTableSql)["status"].toInt(), 200);
	}

	std::shared_ptr<JsonFileDb>& db() {
		return db_;
	}

	const std::string& dbPath() const {
		return filePath_;
	}

private:
	std::string filePath_;
	std::shared_ptr<JsonFileDb> db_;
};

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only write semantics: auto id, upsert, affectedRows/insertId,
// records/pages, schema null defaults, single-element batches, swap-pop order
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, JsonFileOnlyWriteContract) {
	reset();
	Idb& idb = *db();

	// Row order after a delete: swap-pop (last row moves into the hole).
	// Checked first, while the table still holds exactly the seed rows.
	Json all = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(all["status"].toInt(), 200);
	ASSERT_EQ(all["data"].size(), 6);
	ASSERT_EQ(idb.remove(kTableName, Json{{"id", "a1b2c3d4"}})["status"].toInt(), 200);
	all = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(all["data"].size(), 5);
	EXPECT_EQ(all["data"][0]["id"].toString(), "a6b7c8d9");  // last row swapped in
	EXPECT_EQ(all["data"][1]["id"].toString(), "a2b3c4d5");  // second row untouched

	// Create with auto-generated id
	Json result = idb.create(kTableName, Json{{"name", "zhoutk"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	const std::string createdId = result["id"].toString();
	ASSERT_EQ(createdId.size(), static_cast<size_t>(8));
	EXPECT_TRUE(createdId.find_first_not_of("0123456789abcdef") == std::string::npos);
	Json row = idb.select(kTableName, Json{{"id", createdId}});
	ASSERT_EQ(row["status"].toInt(), 200);
	EXPECT_EQ(row["data"][0]["name"].toString(), "zhoutk");
	// With schema, missing fields default to null (SQL backends default to '')
	EXPECT_TRUE(row["data"][0]["age"].isNull());
	EXPECT_EQ(row["data"][0]["id"].toString(), createdId);

	// Create with duplicate id (upsert, not a PK violation)
	result = idb.create(kTableName, Json{{"id", "manual001"}, {"name", "manual-row"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["id"].toString(), "manual001");
	result = idb.create(kTableName, Json{{"id", "manual001"}, {"name", "upserted-name"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "manual001"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "upserted-name");

	// Delete reports affected rows, including 0 for a missing id
	result = idb.remove(kTableName, Json{{"id", "already_deleted"}});
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 0);

	// Update of a non-existent id reports 0 affected rows
	result = idb.update(kTableName, Json{{"id", "no_such_id"}, {"name", "ghost"}});
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 0);

	// create() with a single-element array goes through the single-row path
	Json single = idb.create(kTableName, Json(JsonType::Array).add(Json{{"id", "arr001"}, {"name", "single-array"}}));
	ASSERT_EQ(single["status"].toInt(), 200);
	EXPECT_EQ(single["id"].toString(), "arr001");

	// insertBatch with exactly one element is accepted
	Json one(JsonType::Array);
	one.add(Json{{"id", "one001"}, {"name", "one-row"}});
	Json batchResult = idb.insertBatch(kTableName, one);
	ASSERT_EQ(batchResult["status"].toInt(), 200);
	EXPECT_EQ(batchResult["affectedRows"].toInt(), 1);

	// Empty / malformed input
	EXPECT_EQ(idb.create(kTableName, Json(JsonType::Object))["status"].toInt(), 301);
	EXPECT_EQ(idb.insertBatch(kTableName, Json(JsonType::Array))["status"].toInt(), 301);

	// Empty string id is replaced by a generated one
	result = idb.create(kTableName, Json{{"id", ""}, {"name", "empty-id"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["id"].toString().size(), static_cast<size_t>(8));
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only response shape: records / pages on select results
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, ResponseShapeContract) {
	reset();
	Idb& idb = *db();

	Json all = idb.select(kTableName, Json(JsonType::Object));
	EXPECT_EQ(all["records"].toInt(), 6);
	EXPECT_EQ(all["pages"].toInt(), 1);

	Json paged = idb.select(kTableName, Json{{"page", 1}, {"size", 3}});
	EXPECT_EQ(paged["records"].toInt(), 6);
	EXPECT_EQ(paged["pages"].toInt(), 2);

	Json empty = idb.select(kTableName, Json{{"id", "missing"}});
	EXPECT_EQ(empty["records"].toInt(), 0);
	EXPECT_EQ(empty["pages"].toInt(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile querySql semantics: metadata shims and plain-select routing
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, QuerySqlContract) {
	reset();
	Idb& idb = *db();

	// sqlite_master existence check: table exists / missing -> 202
	Json values(JsonType::Array);
	values.add(kTableName);
	Json result = idb.querySql("SELECT name FROM sqlite_master WHERE type='table' AND name=?", Json(), values);
	ASSERT_EQ(result["status"].toInt(), 200);
	ASSERT_EQ(result["data"].size(), 1);
	EXPECT_EQ(result["data"][0]["TABLE_NAME"].toString(), kTableName);

	result = idb.querySql("SELECT name FROM sqlite_master WHERE type='table' AND name=?", Json(), Json(JsonType::Array).add("no_such_table"));
	EXPECT_EQ(result["status"].toInt(), 202);

	// information_schema.views / columns are not supported -> empty
	EXPECT_EQ(idb.querySql("SELECT * FROM information_schema.views")["status"].toInt(), 202);

	// information_schema.tables with the table name as the last bound value
	result = idb.querySql("SELECT * FROM information_schema.tables WHERE table_name = ?", Json(), values);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 1);

	// Without a bound table name nothing matches -> empty result
	result = idb.querySql("SELECT * FROM information_schema.tables WHERE table_name = 'table_for_test'");
	EXPECT_EQ(result["status"].toInt(), 202);

	// Plain select without WHERE routes to the table with params as conditions
	result = idb.querySql("select * from table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");

	result = idb.querySql("select * from table_for_test");
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 6);

	result = idb.querySql("select * from missing_table");
	EXPECT_EQ(result["status"].toInt(), 202);

	// SELECT with a literal WHERE clause is not supported -> empty result
	result = idb.querySql("select * from table_for_test where age = 18");
	EXPECT_EQ(result["status"].toInt(), 202);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only SQL shims: literals, drop table, placeholders, rejections
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, SqlEdgeContract) {
	reset();
	Idb& idb = *db();

	// Transaction control shims are no-ops that report success
	EXPECT_EQ(idb.execSql("begin")["status"].toInt(), 200);
	EXPECT_EQ(idb.execSql("COMMIT")["status"].toInt(), 200);
	EXPECT_EQ(idb.execSql("  rollback ")["status"].toInt(), 200);

	// String literals with doubled-quote escaping, boolean and null literals
	Json result = idb.execSql(
		"insert into table_for_test (id,name,age,score) values ('lit001','it''s ok',true,null)");
	ASSERT_EQ(result["status"].toInt(), 200);
	Json row = idb.select(kTableName, Json{{"id", "lit001"}});
	ASSERT_EQ(row["status"].toInt(), 200);
	EXPECT_EQ(row["data"][0]["name"].toString(), "it's ok");
	EXPECT_TRUE(row["data"][0]["age"].isTrue());
	EXPECT_TRUE(row["data"][0]["score"].isNull());

	// insert with fewer bound values than placeholders is rejected
	EXPECT_NE(idb.execSql("insert into table_for_test (id,name) values (?,?)",
						  Json(), Json(JsonType::Array).add("x"))["status"].toInt(), 200);

	// INSERT INTO t ? with a non-object payload is rejected
	EXPECT_NE(idb.execSql("INSERT INTO table_for_test ?", Json(),
						  Json(JsonType::Array).add("not-an-object"))["status"].toInt(), 200);

	// Garbled literal is rejected
	EXPECT_NE(idb.execSql("insert into table_for_test (id,name) values ('lit002','unterminated)"
						  )["status"].toInt(), 200);

	// DELETE with ?? table placeholder
	result = idb.execSql("DELETE FROM ?? WHERE id = ?", Json(),
						 Json(JsonType::Array).add(kTableName).add("lit001"));
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "lit001"}})["status"].toInt(), 202);

	// UPDATE / DELETE against a missing table report 0 affected rows
	result = idb.execSql("update missing_table set name = 'x' where id = 'y'");
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 0);
	result = idb.execSql("delete from missing_table where id = 'y'");
	EXPECT_EQ(result["affectedRows"].toInt(), 0);

	// drop table removes it entirely; dropping again reports 0
	ASSERT_EQ(idb.create("temp_table", Json{{"id", "t1"}, {"v", 1}})["status"].toInt(), 200);
	result = idb.execSql("DROP TABLE temp_table");
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	EXPECT_EQ(idb.select("temp_table", Json(JsonType::Object))["status"].toInt(), 202);
	EXPECT_EQ(idb.execSql("DROP TABLE temp_table")["affectedRows"].toInt(), 0);
	EXPECT_EQ(idb.execSql("drop table if exists no_such_table")["affectedRows"].toInt(), 0);

	// UPDATE / DELETE without a WHERE clause are not supported
	EXPECT_EQ(idb.execSql("update table_for_test set name = 'x'")["status"].toInt(), 701);
	EXPECT_EQ(idb.execSql("delete from table_for_test")["status"].toInt(), 701);

	// INSERT without column list / values section is not supported
	EXPECT_EQ(idb.execSql("insert into table_for_test")["status"].toInt(), 701);

	// ?? placeholders demand the table name as the first bound value
	EXPECT_EQ(idb.execSql("INSERT INTO ?? (id) values ('x')")["status"].toInt(), 701);
	EXPECT_EQ(idb.execSql("UPDATE ?? SET id = 'x' WHERE id = 'y'")["status"].toInt(), 701);
	EXPECT_EQ(idb.execSql("DELETE FROM ?? WHERE id = 'y'")["status"].toInt(), 701);

	// CREATE TABLE / DROP TABLE with a ?? table placeholder
	Json phValues(JsonType::Array);
	phValues.add("ph_table");
	ASSERT_EQ(idb.execSql("CREATE TABLE ?? (id text NOT NULL, val real DEFAULT NULL)", Json(), phValues)["status"].toInt(), 200);
	ASSERT_EQ(idb.create("ph_table", Json{{"id", "p1"}, {"val", false}})["status"].toInt(), 200);
	// boolean false literal survives a round-trip
	row = idb.select("ph_table", Json{{"id", "p1"}});
	ASSERT_EQ(row["status"].toInt(), 200);
	EXPECT_TRUE(row["data"][0]["val"].isFalse());
	ASSERT_EQ(idb.execSql("DROP TABLE ??", Json(), Json(JsonType::Array).add("ph_table"))["status"].toInt(), 200);
	EXPECT_EQ(idb.select("ph_table", Json(JsonType::Object))["status"].toInt(), 202);
}

// A hand-crafted data file (no CREATE TABLE) is indexed on load: rows with
// ids are fully mutable through the SQL shims.
TEST_F(JsonFileDbTest, HandcraftedStoreContract) {
	const std::string dbPath = scratchPath("handcrafted");
	removeDatabaseFiles(dbPath);
	writeFile(dbPath, R"([{"table":"t","rows":[{"id":"a","n":1},{"id":"b","n":2}]}])");
	auto db = JsonFileDb::createShared(dbPath);

	// select-all works without any schema
	Json all = db->select("t", Json(JsonType::Object));
	ASSERT_EQ(all["status"].toInt(), 200);
	EXPECT_EQ(all["data"].size(), 2);

	// the id -> row index was rebuilt from the file: indexed update works
	Json result = db->execSql("update t set n = 9 where id = 'b'");
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	all = db->select("t", Json{{"id", "b"}});
	ASSERT_EQ(all["status"].toInt(), 200);
	EXPECT_EQ(all["data"][0]["n"].toInt(), 9);

	// indexed delete works on the hand-crafted store
	result = db->execSql("delete from t where id = 'a'");
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	EXPECT_EQ(db->select("t", Json{{"id", "a"}})["status"].toInt(), 202);
	EXPECT_EQ(db->select("t", Json(JsonType::Object))["data"].size(), 1);

	removeDatabaseFiles(dbPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only structured transactions: Batch / Update / Delete methods
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, TransGoStructuredContract) {
	reset();
	Idb& idb = *db();

	// Batch method upserts every element
	Json batchTx(JsonType::Array);
	batchTx.add(Json{{"table", kTableName}, {"method", "Batch"}, {"params", Json(JsonType::Array)
		.add(Json{{"id", "bt01"}, {"name", "batch-tx-1"}, {"age", 1}, {"score", 0.1}})
		.add(Json{{"id", "bt02"}, {"name", "batch-tx-2"}, {"age", 2}, {"score", 0.2}})}});
	Json txResult = idb.transGo(batchTx);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "bt01"}})["data"][0]["name"].toString(), "batch-tx-1");
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "bt02"}})["data"][0]["age"].toInt(), 2);

	// Update method (id is top-level, not inside params)
	Json txUpdate(JsonType::Array);
	txUpdate.add(Json{{"table", kTableName}, {"method", "Update"}, {"id", "bt01"}, {"params", Json{{"name", "tx-updated"}}}});
	txResult = idb.transGo(txUpdate, false);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "bt01"}})["data"][0]["name"].toString(), "tx-updated");

	// Delete method
	Json txDelete(JsonType::Array);
	txDelete.add(Json{{"table", kTableName}, {"method", "Delete"}, {"id", "bt02"}});
	txResult = idb.transGo(txDelete, false);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "bt02"}})["status"].toInt(), 202);

	// A structured element that matches no method fails and rolls back
	Json badTx(JsonType::Array);
	badTx.add(Json{{"table", kTableName}, {"method", "Insert"}, {"params", Json{{"id", "bt03"}, {"name", "ok"}}}});
	badTx.add(Json{{"table", kTableName}, {"method", "Nonsense"}, {"params", Json{{"x", 1}}}});
	txResult = idb.transGo(badTx);
	EXPECT_NE(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "bt03"}})["status"].toInt(), 202);

	// transGo with empty array is rejected
	EXPECT_EQ(idb.transGo(Json(JsonType::Array))["status"].toInt(), 301);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only value types: null / boolean / numeric-string rows
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, ValueTypesContract) {
	reset();
	Idb& idb = *db();

	ASSERT_EQ(idb.create(kTableName, Json{{"id", "vt01"}, {"name", "nuller"}, {"age", nullptr}, {"score", nullptr}})["status"].toInt(), 200);
	ASSERT_EQ(idb.create(kTableName, Json{{"id", "vt02"}, {"name", "boolean"}, {"age", true}, {"score", false}})["status"].toInt(), 200);
	ASSERT_EQ(idb.create(kTableName, Json{{"id", "vt03"}, {"name", "numeric-string"}, {"age", "20"}, {"score", nullptr}})["status"].toInt(), 200);

	// SQL-style "null" condition matches the explicit-null row
	// (vt03's age is the string "20", which does not match the null check).
	Json result = idb.select(kTableName, Json{{"age", "null"}, {"ins", "id,vt01,vt03"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	ASSERT_EQ(result["data"].size(), 1);
	EXPECT_EQ(result["data"][0]["id"].toString(), "vt01");

	// Numeric cross-type comparison: string "20" equals number 20
	result = idb.select(kTableName, Json{{"age", 20}, {"ins", "id,vt03"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 1);

	// Boolean round-trips and is queryable
	result = idb.select(kTableName, Json{{"id", "vt02"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_TRUE(result["data"][0]["age"].isTrue());
	EXPECT_TRUE(result["data"][0]["score"].isFalse());

	// sum skips null/boolean rows and accepts numeric strings
	result = idb.select(kTableName, Json{{"sum", "age,agesum"}, {"ins", "id,vt01,vt03"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["agesum"].toInt(), 20);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsonfile-only query parameter validation and string-range comparisons
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, QueryValidationContract) {
	reset();
	Idb& idb = *db();

	// Wrong aggregate shapes are rejected
	EXPECT_EQ(idb.select(kTableName, Json{{"count", "age"}})["status"].toInt(), 301);
	EXPECT_EQ(idb.select(kTableName, Json{{"sum", "age"}})["status"].toInt(), 301);

	// Wrong comparison shape (3 parts) is rejected
	EXPECT_EQ(idb.select(kTableName, Json{{"age", ">,1,2"}})["status"].toInt(), 301);

	// Lexicographic comparison on string fields
	Json result = idb.select(kTableName, Json{{"name", ">,test002"}, {"ins", "id,a3b4c5d6,a4b5c6d7,a5b6c7d8,a6b7c8d9"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);  // test003, test004, test005

	result = idb.select(kTableName, Json{{"name", "<=,test001"}, {"ins", "id,a1b2c3d4,a2b3c4d5"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);  // "Kevin 凯文" (K < t) and "test001" itself

	// group + explicit fields projection keeps the group field
	result = idb.select(kTableName, Json{{"group", "age"}, {"count", "*,total"}}, vector<string>{"total"});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_FALSE(result["data"][0]["age"].isError());
	EXPECT_FALSE(result["data"][0]["total"].isError());

	// sum over rows where the column is missing/null yields null
	result = idb.select(kTableName, Json{{"sum", "nonexistent,s"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_TRUE(result["data"][0]["s"].isNull());

	// Field projection through a "fields" key inside params (jsonfile
	// extension ported from the orm project; SQL backends only take the
	// vector<string> argument).
	Json fieldsParam{{"id", "a2b3c4d5"}};
	Json fields(JsonType::Array);
	fields.add("name");
	fields.add("score");
	fieldsParam.add("fields", fields);
	result = idb.select(kTableName, fieldsParam);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "test001");
	EXPECT_TRUE(result["data"][0]["id"].isError());

	// Invalid fields definition is rejected
	Json badFields{{"id", "a2b3c4d5"}};
	Json badFieldArray(JsonType::Array);
	badFieldArray.add(1);
	badFieldArray.add(2);
	badFields.add("fields", badFieldArray);
	EXPECT_EQ(idb.select(kTableName, badFields)["status"].toInt(), 301);

	// remove with an empty object is rejected (SQL backends would run
	// "delete ... where id = ''" and report success)
	EXPECT_EQ(idb.remove(kTableName, Json(JsonType::Object))["status"].toInt(), 301);
}

// ─────────────────────────────────────────────────────────────────────────────
// File hygiene: empty file is an empty database; stale temp files of dead
// processes are swept; corrupt file reports through querySql as first call
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, FileHygieneContract) {
	const std::string dbPath = scratchPath("hygiene");

	// An empty file is treated as an empty database (not corruption)
	removeDatabaseFiles(dbPath);
	writeFile(dbPath, "");
	{
		auto db = JsonFileDb::createShared(dbPath);
		EXPECT_EQ(db->select("t", Json(JsonType::Object))["status"].toInt(), 202);
		EXPECT_EQ(db->create("t", Json{{"id", "e1"}})["status"].toInt(), 200);
	}
	removeDatabaseFiles(dbPath);

	// Stale temp files whose writer is gone are removed by a fresh instance
	const std::string staleTemp = dbPath + ".tmp.999999999.deadbeef";
	writeFile(staleTemp, "junk");
	const std::string liveTemp = dbPath + ".tmp." + std::to_string(jfd::currentProcessId()) + ".cafebabe";
	writeFile(liveTemp, "junk");
	{
		auto db = JsonFileDb::createShared(dbPath);
		(void)db;
	}
	EXPECT_FALSE(fileExists(staleTemp));   // dead writer -> swept
	EXPECT_TRUE(fileExists(liveTemp));     // live writer (this process) -> kept
	removePath(liveTemp);
	removeDatabaseFiles(dbPath);

	// querySql as the very first call on a corrupt file reports 701
	writeFile(dbPath, "{ broken");
	{
		auto db = JsonFileDb::createShared(dbPath);
		EXPECT_EQ(db->querySql("select * from t")["status"].toInt(), 701);
	}
	removeDatabaseFiles(dbPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Durability: a fresh instance sees what a previous instance wrote
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, PersistenceContract) {
	reset();
	const std::string path = dbPath();
	db().reset();  // drop the instance, file remains

	auto reopened = JsonFileDb::createShared(path);
	Json result = reopened->select(kTableName, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");
	EXPECT_EQ(result["data"].size(), 1);

	// Relative paths resolve against the working directory and survive a
	// fresh instance at the same path.
	const std::string relativeName =
		"zorm_jsonfile_rel_" + std::to_string(jfd::currentProcessId()) + ".json";
	removeDatabaseFiles(relativeName);
	const std::string expectedAbsolute = jfd::genericUtf8(
		jfd::fsPath(fs::current_path().generic_string() + "/" + relativeName).lexically_normal());
	{
		auto relativeDb = std::make_shared<JsonFileDb>(relativeName);
		ASSERT_EQ(relativeDb->storagePath(), expectedAbsolute);
		ASSERT_EQ(relativeDb->create("t", Json{{"id", "r1"}, {"name", "rel"}})["status"].toInt(), 200);
	}
	auto relativeDb2 = JsonFileDb::createShared(expectedAbsolute);
	Json relRow = relativeDb2->select("t", Json{{"id", "r1"}});
	ASSERT_EQ(relRow["status"].toInt(), 200);
	EXPECT_EQ(relRow["data"][0]["name"].toString(), "rel");
	removeDatabaseFiles(expectedAbsolute);
}

// ─────────────────────────────────────────────────────────────────────────────
// createShared: one shared instance per resolved file path
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, SharedInstanceContract) {
	const std::string path = scratchPath("shared");
	removeDatabaseFiles(path);
	std::shared_ptr<JsonFileDb> a = JsonFileDb::createShared(path);
	std::shared_ptr<JsonFileDb> b = JsonFileDb::createShared(path);
	EXPECT_EQ(a.get(), b.get());

	// When all references are released the registry entry expires and a fresh
	// instance is created.
	std::weak_ptr<JsonFileDb> weak = a;
	a.reset();
	b.reset();
	EXPECT_TRUE(weak.expired());
	std::shared_ptr<JsonFileDb> d = JsonFileDb::createShared(path);
	EXPECT_NE(d.get(), weak.lock().get());
	ASSERT_EQ(d->create("t", Json{{"id", "x"}, {"name", "shared"}})["status"].toInt(), 200);
	removeDatabaseFiles(path);
}

// ─────────────────────────────────────────────────────────────────────────────
// Memory contract: a failed reload must not wipe the in-memory database
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, MemoryContract) {
	reset();
	Idb& idb = *db();

	const Json before = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(before["status"].toInt(), 200);
	ASSERT_EQ(before["data"].size(), 6);

	// Break the file behind the live instance's back, then attempt a write.
	writeFile(dbPath(), "{ this is not valid json");
	const Json updateResult = idb.update(kTableName, Json{{"id", "a1b2c3d4"}, {"score", 1.5}});
	EXPECT_NE(updateResult["status"].toInt(), 200);

	// Reads keep serving the last known good state.
	const Json after = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(after["status"].toInt(), 200);
	EXPECT_EQ(after["data"].size(), 6);
	EXPECT_EQ(after["data"][0]["name"].toString(), "Kevin 凯文");

	const Json byId = idb.select(kTableName, Json{{"id", "a6b7c8d9"}});
	EXPECT_EQ(byId["status"].toInt(), 200);
	EXPECT_EQ(byId["data"][0]["name"].toString(), "test005");
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoding contract: invalid UTF-8 is corruption; valid UTF-8 round-trips
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, EncodingContract) {
	const std::string dbPath = scratchPath("encoding");
	removeDatabaseFiles(dbPath);
	auto db = JsonFileDb::createShared(dbPath);

	// A file with an invalid UTF-8 byte inside a string is corrupt: reads
	// report 701, a backup is taken, writes are refused.
	writeFile(dbPath, std::string("{\"table\":\"t\",\"rows\":[{\"name\":\"") + "\xff\xfe" + "\"}]}");
	const Json corruptRead = db->select("t", Json(JsonType::Object));
	EXPECT_EQ(corruptRead["status"].toInt(), 701);
	EXPECT_TRUE(hasCorruptionBackup(dbPath));
	const Json refused = db->create("t", Json{{"id", "x1"}, {"name", "n"}});
	EXPECT_NE(refused["status"].toInt(), 200);

	// Repair the file externally: the next write reloads, succeeds and clears
	// the failure state (self-healing).
	removeDatabaseFiles(dbPath);
	ASSERT_EQ(db->create("t", Json{{"id", "u1"}, {"name", "中文测试 🌊 ⛵"}})["status"].toInt(), 200);
	const Json healed = db->select("t", Json{{"id", "u1"}});
	ASSERT_EQ(healed["status"].toInt(), 200);
	EXPECT_EQ(healed["data"][0]["name"].toString(), "中文测试 🌊 ⛵");

	// The stored document is valid UTF-8 JSON with the expected shape.
	const std::string raw = readFile(dbPath);
	std::string parseError;
	const Json reparsed = Json::ParseJsonStrictUtf8(raw, parseError);
	EXPECT_FALSE(reparsed.isError()) << parseError;
	EXPECT_TRUE(reparsed.isArray());

	removeDatabaseFiles(dbPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// FileLock: ownership, stale reclaim and idempotent unlock
// ─────────────────────────────────────────────────────────────────────────────

TEST(FileLockTest, OwnershipAndStaleReclaim) {
	const std::string lockPath = scratchPath("lock") + ".lock";
	removePath(lockPath);

	// (a) acquire: the file records us, and unlock() removes our own file
	{
		FileLock lock(lockPath);
		EXPECT_TRUE(lock.tryLock(1000));
		EXPECT_TRUE(fileExists(lockPath));
		long long pid = 0;
		std::string host;
		std::string app;
		EXPECT_TRUE(lock.readLockInfo(&pid, &host, &app));
		EXPECT_EQ(pid, jfd::currentProcessId());
		EXPECT_EQ(app, std::string(kLockAppName));
		lock.unlock();
		EXPECT_FALSE(fileExists(lockPath));
		lock.unlock();  // idempotent
	}

	// (b) a lock file that has been taken over by somebody else must survive
	{
		FileLock lock(lockPath);
		ASSERT_TRUE(lock.tryLock(1000));
		// Same pid, different host == reclaimed elsewhere: not ours any more.
		writeFile(lockPath, FileLock::formatLockInfo(jfd::currentProcessId(), "some-other-host",
													 "other-app", jfd::nowMillis()));
		lock.unlock();
		EXPECT_TRUE(fileExists(lockPath));
		removePath(lockPath);
	}

	// (c) a stale lock (dead pid + old timestamp) is reclaimed
	{
		writeFile(lockPath, FileLock::formatLockInfo(999999999LL, jfd::hostName(), "dead-app",
													 jfd::nowMillis() - 60000));
		FileLock lock(lockPath);
		EXPECT_TRUE(lock.tryLock(2000));
		lock.unlock();
		EXPECT_FALSE(fileExists(lockPath));
	}

	// (d) a lock held by a live process is respected (timeout expires)
	{
		writeFile(lockPath, FileLock::formatLockInfo(jfd::currentProcessId(), jfd::hostName(),
													 "other-app", jfd::nowMillis()));
		FileLock lock(lockPath);
		EXPECT_FALSE(lock.tryLock(100));
		removePath(lockPath);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// A writer waiting for the cross-process lock must not block readers
// ─────────────────────────────────────────────────────────────────────────────

TEST(WriteWaitTest, ReaderNotBlockedByWaitingWriter) {
	const std::string dbPath = scratchPath("lockwait");
	removeDatabaseFiles(dbPath);
	{
		auto db = JsonFileDb::createShared(dbPath);
		ASSERT_EQ(db->execSql("CREATE TABLE t (id text NOT NULL, PRIMARY KEY (id))")["status"].toInt(), 200);
		ASSERT_EQ(db->create("t", Json{{"id", "seed"}})["status"].toInt(), 200);

		// A fresh lock owned by a live process makes the next write wait rather
		// than steal the lock.
		writeFile(dbPath + ".lock", FileLock::formatLockInfo(jfd::currentProcessId(), jfd::hostName(),
															 "other-app", jfd::nowMillis()));

		std::atomic<long long> writeMs{-1};
		std::thread writer([&]() {
			const auto started = std::chrono::steady_clock::now();
			const Json result = db->create("t", Json{{"id", "late"}});
			(void)result;
			writeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
							  std::chrono::steady_clock::now() - started)
							  .count());
		});

		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		const auto readStart = std::chrono::steady_clock::now();
		const Json read = db->select("t", Json(JsonType::Object));
		const long long readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
									 std::chrono::steady_clock::now() - readStart)
									 .count();

		removePath(dbPath + ".lock");  // let the waiting writer in
		writer.join();

		EXPECT_EQ(read["status"].toInt(), 200);
		EXPECT_LT(readMs, 200) << "reader was blocked by the waiting writer";
		EXPECT_GT(writeMs.load(), 300) << "writer did not wait for the lock";

		// The late write eventually landed.
		const Json late = db->select("t", Json{{"id", "late"}});
		EXPECT_EQ(late["status"].toInt(), 200);
	}
	removeDatabaseFiles(dbPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// DbBase integration: routing and error rejection
// ─────────────────────────────────────────────────────────────────────────────

TEST(DbBaseTest, UnknownDbTypeRejected) {
	Json options;
	options.add("connString", scratchPath("dbbase"));
	// DbBase throws a const char* for unsupported db types.
	EXPECT_THROW(DbBase("no_such_db", options), const char*);
}

TEST(DefaultPathTest, UsesExecutableDirectory) {
	const std::string expected = joinPath(gExecutableDirectory, "data.json");
	EXPECT_EQ(JsonFileDb::defaultStoragePath(), jfd::genericUtf8(jfd::fsPath(expected).lexically_normal()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared contract suite: the jsonfile backend accessed through DbBase, exactly
// like the SQL backends exercise it.
// ─────────────────────────────────────────────────────────────────────────────

class JsonFileContractEnv final : public contract::Env {
public:
	~JsonFileContractEnv() override {
		removeDatabaseFiles(path_);
	}

	Idb* connect() override {
		Json options;
		options.add("connString", path_);
		options.add("DbLogClose", true);
		return new DbBase("jsonfile", options);
	}

	vector<string> schemaSqls() const override {
		return {
			"DROP TABLE IF EXISTS table_for_test",
			kCreateTableSql,
		};
	}

	// jsonfile keeps real JSON nulls.
	string nullRendering() const override {
		return "json-null";
	}

	// jsonfile's SQL shim uses "?" placeholders.
	string rawPlaceholder() const override {
		return "?";
	}

	// jsonfile querySql routes plain selects (conditions via params), not
	// "? " WHERE clauses.
	bool supportsWherePlaceholders() const override {
		return false;
	}

	// jsonfile auto-creates tables on write (implicit schema).
	bool autoCreateTables() const override {
		return true;
	}

private:
	std::string path_ = scratchPath("contract");
};

// Instantiate the shared contract suite against the jsonfile backend.
ZORM_CONTRACT_TESTS()

}  // namespace

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	gExecutableDirectory = executableDirectoryFromArgv(argc > 0 ? argv[0] : nullptr);
	// Default config for the Env hooks used by the shared suite; the jsonfile
	// Env above overrides the backend-specific bits.
	contract::g_config.name = "jsonfile";
	contract::g_config.type = "jsonfile";
	contract::g_config.nullRendering = "json-null";
	contract::g_config.placeholder = "?";
	// jsonfile's SQL shim accepts `?` placeholders in execSql/transGo.
	contract::g_config.options.add("parameterized", true);
	static JsonFileContractEnv env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
