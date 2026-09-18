// Unit tests for the JSON file backend (ZORM::JsonFile::JsonFileDb).
//
// Coverage follows the contracts proven by our reference projects:
//   * refer/orm/tests/orm_contract_tests.cpp  - read/write/query/dao/edge/
//     memory(order, corrupt-file, cross-process lock, encoding) contracts;
//   * refer/gels/test - the cross-language semantics of the query parameters
//     (fuzzy/ins/lks/ors, pagination, aggregates, group, sort, transactions).
//
// Passing this suite is the gate for committing changes: every behaviour the
// other backends expose through Idb.h is exercised here against the file
// backend, including the {"text": sql, "values": [...]} style used by
// transGo/execSql throughout ZORM.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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

constexpr const char* kTableName = "table_for_test";
constexpr const char* kCreateTableSql =
	"CREATE TABLE table_for_test (id text NOT NULL, name text DEFAULT NULL, "
	"age integer DEFAULT NULL, score real DEFAULT NULL, PRIMARY KEY (id))";

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

Json makeSeedRows() {
	Json rows(JsonType::Array);
	rows.add(Json{{"id", "a1b2c3d4"}, {"name", "Kevin 凯文"}, {"age", 18}, {"score", 99.99}});
	rows.add(Json{{"id", "a2b3c4d5"}, {"name", "test001"}, {"age", 19}, {"score", 98.88}});
	rows.add(Json{{"id", "a3b4c5d6"}, {"name", "test002"}, {"age", 20}, {"score", 97.77}});
	rows.add(Json{{"id", "a4b5c6d7"}, {"name", "test003"}, {"age", 21}, {"score", 96.66}});
	rows.add(Json{{"id", "a5b6c7d8"}, {"name", "test004"}, {"age", 22}, {"score", 95.55}});
	rows.add(Json{{"id", "a6b7c8d9"}, {"name", "test005"}, {"age", 23}, {"score", 94.44}});
	return rows;
}

// - Fixture ------------------------------------------------------------------

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
		Json insertResult = db_->insertBatch(kTableName, makeSeedRows());
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
// Read contract
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, ReadContract) {
	reset();
	Idb& idb = *db();

	// Select by id
	Json result = idb.select(kTableName, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");
	EXPECT_EQ(result["data"][0]["age"].toInt(), 18);
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 99.99);

	// Select with multiple conditions
	result = idb.select(kTableName, Json{{"id", "a1b2c3d4"}, {"age", 18}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 1);

	// Reject wrong exact match
	result = idb.select(kTableName, Json{{"id", "a1b2c3d4"}, {"age", 18}, {"name", "Kevin"}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// Accept correct exact match
	result = idb.select(kTableName, Json{{"id", "a1b2c3d4"}, {"age", 18}, {"name", "Kevin 凯文"}});
	EXPECT_EQ(result["status"].toInt(), 200);

	// Field projection (fields as a JSON array inside params)
	Json fieldsParam{{"id", "a2b3c4d5"}};
	Json fields(JsonType::Array);
	fields.add("name");
	fields.add("score");
	fieldsParam.add("fields", fields);
	result = idb.select(kTableName, fieldsParam);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "test001");
	EXPECT_DOUBLE_EQ(result["data"][0]["score"].toDouble(), 98.88);
	EXPECT_TRUE(result["data"][0]["id"].isError());

	// Field projection (fields as the vector<string> argument)
	std::vector<string> fieldVector{"name", "score"};
	result = idb.select(kTableName, Json{{"id", "a2b3c4d5"}}, fieldVector);
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "test001");
	EXPECT_TRUE(result["data"][0]["id"].isError());

	// Invalid fields definition
	Json badFields{{"id", "a2b3c4d5"}};
	Json badFieldArray(JsonType::Array);
	badFieldArray.add(1);
	badFieldArray.add(2);
	badFields.add("fields", badFieldArray);
	result = idb.select(kTableName, badFields);
	EXPECT_EQ(result["status"].toInt(), 301);

	// Fuzzy search
	result = idb.select(kTableName, Json{{"name", "Kevin"}, {"fuzzy", 1}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");

	// Select all (no conditions)
	result = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 6);
	EXPECT_EQ(result["records"].toInt(), 6);
	EXPECT_EQ(result["pages"].toInt(), 1);

	// Select by non-existent id
	result = idb.select(kTableName, Json{{"id", "nonexistent"}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// Fuzzy with non-existent pattern
	result = idb.select(kTableName, Json{{"name", "ZZZZ_NOT_FOUND"}, {"fuzzy", 1}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// Select from a non-existent table
	result = idb.select("no_such_table", Json(JsonType::Object));
	EXPECT_EQ(result["status"].toInt(), 202);
}

// ─────────────────────────────────────────────────────────────────────────────
// Write contract
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, WriteContract) {
	reset();
	Idb& idb = *db();

	// Update score
	Json result = idb.update(kTableName, Json{{"id", "a1b2c3d4"}, {"score", 6.6}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	Json row = idb.select(kTableName, Json{{"id", "a1b2c3d4"}});
	EXPECT_DOUBLE_EQ(row["data"][0]["score"].toDouble(), 6.6);

	// Delete row
	result = idb.remove(kTableName, Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 1);
	row = idb.select(kTableName, Json{{"id", "a1b2c3d4"}});
	EXPECT_EQ(row["status"].toInt(), 202);

	// Delete non-existent row
	result = idb.remove(kTableName, Json{{"id", "already_deleted"}});
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 0);

	// Update multiple fields
	result = idb.update(kTableName, Json{{"id", "a5b6c7d8"}, {"name", "test888"}, {"score", 23.27}, {"age", 22}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "a5b6c7d8"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "test888");
	EXPECT_DOUBLE_EQ(row["data"][0]["score"].toDouble(), 23.27);
	EXPECT_EQ(row["data"][0]["age"].toInt(), 22);

	// Update non-existent id
	result = idb.update(kTableName, Json{{"id", "no_such_id"}, {"name", "ghost"}});
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["affectedRows"].toInt(), 0);

	// Create with auto-generated id
	result = idb.create(kTableName, Json{{"name", "zhoutk"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	const std::string createdId = result["id"].toString();
	ASSERT_EQ(createdId.size(), static_cast<size_t>(8));
	EXPECT_TRUE(createdId.find_first_not_of("0123456789abcdef") == std::string::npos);
	row = idb.select(kTableName, Json{{"id", createdId}});
	ASSERT_EQ(row["status"].toInt(), 200);
	EXPECT_EQ(row["data"][0]["name"].toString(), "zhoutk");
	// With schema, missing fields default to null
	EXPECT_TRUE(row["data"][0]["age"].isNull());
	EXPECT_TRUE(row["data"][0]["score"].isNull());
	EXPECT_EQ(row["data"][0]["id"].toString(), createdId);

	// Create with manual id
	result = idb.create(kTableName, Json{{"id", "manual001"}, {"name", "manual-row"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["id"].toString(), "manual001");
	row = idb.select(kTableName, Json{{"id", "manual001"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "manual-row");
	EXPECT_TRUE(row["data"][0]["age"].isNull());

	// Create with duplicate id (upsert)
	result = idb.create(kTableName, Json{{"id", "manual001"}, {"name", "upserted-name"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "manual001"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "upserted-name");

	// Reject empty create
	EXPECT_EQ(idb.create(kTableName, Json(JsonType::Object))["status"].toInt(), 301);

	// Reject update without id
	EXPECT_EQ(idb.update(kTableName, Json{{"score", 1}})["status"].toInt(), 301);

	// Reject delete without id
	EXPECT_EQ(idb.remove(kTableName, Json(JsonType::Object))["status"].toInt(), 301);

	// Create with zero / negative / unicode values
	result = idb.create(kTableName, Json{{"name", "edge-case"}, {"age", 0}, {"score", 0.0}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", result["id"].toString()}});
	EXPECT_EQ(row["data"][0]["age"].toInt(), 0);
	EXPECT_DOUBLE_EQ(row["data"][0]["score"].toDouble(), 0.0);

	result = idb.create(kTableName, Json{{"name", "negative"}, {"age", -5}, {"score", -3.14}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", result["id"].toString()}});
	EXPECT_EQ(row["data"][0]["age"].toInt(), -5);
	EXPECT_DOUBLE_EQ(row["data"][0]["score"].toDouble(), -3.14);

	result = idb.create(kTableName, Json{{"name", "中文测试 🌊 ⛵"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", result["id"].toString()}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "中文测试 🌊 ⛵");
}

// ─────────────────────────────────────────────────────────────────────────────
// Query contract (fuzzy / ins / lks / ors / pagination / aggregates / sort)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, QueryContract) {
	reset();
	Idb& idb = *db();

	// Multi-row fuzzy query
	Json result = idb.select(kTableName, Json{{"name", "test"}, {"fuzzy", 1}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 5);

	// IN query
	result = idb.select(kTableName, Json{{"ins", "age,20,21,23"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);

	// LIKE query (lks)
	result = idb.select(kTableName, Json{{"lks", "name,001,age,23"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);

	// OR query
	result = idb.select(kTableName, Json{{"ors", "age,19,age,23"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);

	// Pagination: page 1
	result = idb.select(kTableName, Json{{"page", 1}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);
	EXPECT_EQ(result["pages"].toInt(), 2);
	EXPECT_EQ(result["records"].toInt(), 6);

	// Pagination: page 2
	result = idb.select(kTableName, Json{{"page", 2}, {"size", 3}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);

	// Pagination: page beyond end
	result = idb.select(kTableName, Json{{"page", 999}, {"size", 3}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// Pagination with negative page/size (treated as 0 -> no pagination)
	result = idb.select(kTableName, Json{{"page", -1}, {"size", -1}});
	EXPECT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 6);

	// Count query
	result = idb.select(kTableName, Json{{"count", "1,total"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["total"].toInt(), 6);

	// Sum query with a comparison filter
	result = idb.select(kTableName, Json{{"sum", "age,agesum"}, {"age", "<=,20"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["agesum"].toInt(), 57);

	// Group query
	Json updateResult = idb.update(kTableName, Json{{"id", "a4b5c6d7"}, {"age", 22}});
	ASSERT_EQ(updateResult["status"].toInt(), 200);
	result = idb.select(kTableName, Json{{"group", "age"}, {"count", "*,total"}, {"sort", "total desc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["total"].toInt(), 2);

	// Greater-than query
	result = idb.select(kTableName, Json{{"age", ">,21"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 3);

	// Between query (>=,<=)
	result = idb.select(kTableName, Json{{"age", ">=,19,<=,22"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 4);

	// Less-than query
	result = idb.select(kTableName, Json{{"age", "<,20"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);

	// Not-equal query
	result = idb.select(kTableName, Json{{"age", "<>,20"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 5);

	// Sort ascending / descending
	result = idb.select(kTableName, Json{{"sort", "age asc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 18);
	result = idb.select(kTableName, Json{{"sort", "age desc"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["age"].toInt(), 23);

	// Empty ins query
	result = idb.select(kTableName, Json{{"ins", "age,999"}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// lks query with non-matching pattern
	result = idb.select(kTableName, Json{{"lks", "name,ZZZZ"}});
	EXPECT_EQ(result["status"].toInt(), 202);

	// Multiple lks conditions
	result = idb.select(kTableName, Json{{"lks", "name,test,age,2"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 5);

	// ors with overlapping matches (deduplicated rows)
	result = idb.select(kTableName, Json{{"ors", "age,18,age,18,age,19"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 2);

	// Wrong ins / lks shape is rejected
	EXPECT_EQ(idb.select(kTableName, Json{{"ins", "age"}})["status"].toInt(), 301);
	EXPECT_EQ(idb.select(kTableName, Json{{"lks", "name"}})["status"].toInt(), 301);
}

// ─────────────────────────────────────────────────────────────────────────────
// DAO contract: execSql / insertBatch / transGo
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, DaoContract) {
	reset();
	Idb& idb = *db();

	// execSql UPDATE with ?? table placeholder (orm style)
	Json values(JsonType::Array);
	values.add(kTableName);
	values.add(77.77);
	values.add("dao001");
	ASSERT_EQ(idb.create(kTableName, Json{{"id", "dao001"}, {"name", "dao-row"}, {"age", 31}, {"score", 11.11}})["status"].toInt(), 200);
	Json execResult = idb.execSql("UPDATE ?? SET score = ? WHERE id = ?", Json(), values);
	ASSERT_EQ(execResult["status"].toInt(), 200);
	EXPECT_EQ(execResult["affectedRows"].toInt(), 1);
	Json row = idb.select(kTableName, Json{{"id", "dao001"}});
	EXPECT_DOUBLE_EQ(row["data"][0]["score"].toDouble(), 77.77);

	// execSql UPDATE with plain table name and ? placeholders (zorm style)
	values = Json(JsonType::Array);
	values.add("dao-row-2");
	values.add("dao001");
	execResult = idb.execSql("update table_for_test set name = ? where id = ?", Json(), values);
	ASSERT_EQ(execResult["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "dao001"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "dao-row-2");

	// execSql UPDATE with literal values (no placeholders)
	execResult = idb.execSql("update table_for_test set age = 33 where id = 'dao001'");
	ASSERT_EQ(execResult["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "dao001"}});
	EXPECT_EQ(row["data"][0]["age"].toInt(), 33);

	// execSql INSERT with ?? + row-object payload (orm style)
	values = Json(JsonType::Array);
	values.add(kTableName);
	values.add(Json{{"id", "exec001"}, {"name", "exec-sql-row"}, {"age", 99}});
	execResult = idb.execSql("INSERT INTO ?? ?", Json(), values);
	ASSERT_EQ(execResult["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "exec001"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "exec-sql-row");

	// execSql INSERT with column list + placeholders (zorm style)
	values = Json(JsonType::Array);
	values.add("exec002");
	values.add("exec-sql-row-2");
	values.add(44);
	values.add(4.4);
	execResult = idb.execSql("insert into table_for_test (id,name,age,score) values (?,?,?,?)", Json(), values);
	ASSERT_EQ(execResult["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "exec002"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "exec-sql-row-2");
	EXPECT_EQ(row["data"][0]["age"].toInt(), 44);

	// execSql INSERT with literal values
	execResult = idb.execSql("insert into table_for_test (id,name,age,score) values ('exec003','literal-row',55,5.5)");
	ASSERT_EQ(execResult["status"].toInt(), 200);
	row = idb.select(kTableName, Json{{"id", "exec003"}});
	EXPECT_EQ(row["data"][0]["name"].toString(), "literal-row");
	EXPECT_EQ(row["data"][0]["age"].toInt(), 55);

	// execSql DELETE
	values = Json(JsonType::Array);
	values.add("exec003");
	execResult = idb.execSql("delete from table_for_test where id = ?", Json(), values);
	ASSERT_EQ(execResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "exec003"}})["status"].toInt(), 202);

	// execSql with unsupported / empty SQL
	EXPECT_NE(idb.execSql("", Json(), Json(JsonType::Array))["status"].toInt(), 200);
	EXPECT_NE(idb.execSql("select * from nowhere", Json(), Json(JsonType::Array))["status"].toInt(), 200);

	// insertBatch
	Json batchRows(JsonType::Array);
	batchRows.add(Json{{"id", "batch001"}, {"name", "batch-one"}, {"age", 40}, {"score", 1.11}});
	batchRows.add(Json{{"id", "batch002"}, {"name", "batch-two"}, {"age", 41}, {"score", 2.22}});
	Json batchResult = idb.insertBatch(kTableName, batchRows);
	ASSERT_EQ(batchResult["status"].toInt(), 200);
	EXPECT_EQ(batchResult["affectedRows"].toInt(), 2);

	// insertBatch with empty array should fail
	EXPECT_EQ(idb.insertBatch(kTableName, Json(JsonType::Array))["status"].toInt(), 301);

	// transGo structured style (async + sync flags are accepted)
	Json txElements(JsonType::Array);
	txElements.add(Json{{"table", kTableName}, {"method", "Insert"}, {"params", Json{{"id", "txa001"}, {"name", "tx-a-one"}, {"age", 50}, {"score", 5.01}}}});
	txElements.add(Json{{"table", kTableName}, {"method", "Insert"}, {"params", Json{{"id", "txa002"}, {"name", "tx-a-two"}, {"age", 51}, {"score", 5.02}}}});
	Json txResult = idb.transGo(txElements, true);
	ASSERT_EQ(txResult["status"].toInt(), 200);

	Json syncElements(JsonType::Array);
	syncElements.add(Json{{"table", kTableName}, {"method", "Insert"}, {"params", Json{{"id", "txs001"}, {"name", "tx-s-one"}, {"age", 60}, {"score", 6.01}}}});
	syncElements.add(Json{{"table", kTableName}, {"method", "Insert"}, {"params", Json{{"id", "txs002"}, {"name", "tx-s-two"}, {"age", 61}, {"score", 6.02}}}});
	txResult = idb.transGo(syncElements, false);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "txa001"}})["data"][0]["name"].toString(), "tx-a-one");
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "txs002"}})["data"][0]["name"].toString(), "tx-s-two");

	// transGo with Update method (id is top-level, not inside params)
	Json txUpdate(JsonType::Array);
	txUpdate.add(Json{{"table", kTableName}, {"method", "Update"}, {"id", "batch001"}, {"params", Json{{"name", "tx-updated"}}}});
	txResult = idb.transGo(txUpdate, false);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "batch001"}})["data"][0]["name"].toString(), "tx-updated");

	// transGo with Delete method
	Json txDelete(JsonType::Array);
	txDelete.add(Json{{"table", kTableName}, {"method", "Delete"}, {"id", "batch002"}});
	txResult = idb.transGo(txDelete, false);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "batch002"}})["status"].toInt(), 202);

	// transGo with zorm SQL-text style ({"text": ..., "values": [...]})
	Json sqlArr(JsonType::Array);
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('txt001','text-1',21,78.48)\"}"));
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values (?,?,?,?)\",\"values\":[\"txt002\",\"text-2\",22,23.27]}"));
	txResult = idb.transGo(sqlArr);
	ASSERT_EQ(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "txt001"}})["data"][0]["name"].toString(), "text-1");
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "txt002"}})["data"][0]["age"].toInt(), 22);

	// transGo rollback: a failing step rolls back the whole transaction
	Json rollbackArr(JsonType::Array);
	rollbackArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('rb001','will-not-stay',1,1.0)\"}"));
	rollbackArr.add(Json("{\"text\":\"this is not sql\"}"));
	txResult = idb.transGo(rollbackArr);
	EXPECT_NE(txResult["status"].toInt(), 200);
	EXPECT_EQ(idb.select(kTableName, Json{{"id", "rb001"}})["status"].toInt(), 202);

	// transGo with empty array is rejected
	EXPECT_EQ(idb.transGo(Json(JsonType::Array))["status"].toInt(), 301);

	// Multiple tables
	ASSERT_EQ(idb.create("second_table", Json{{"id", "s001"}, {"value", "second-data"}})["status"].toInt(), 200);
	Json secondSelect = idb.select("second_table", Json{{"id", "s001"}});
	ASSERT_EQ(secondSelect["status"].toInt(), 200);
	EXPECT_EQ(secondSelect["data"][0]["value"].toString(), "second-data");
}

// ─────────────────────────────────────────────────────────────────────────────
// querySql contract: metadata shims + plain select routing
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, QuerySqlContract) {
	reset();
	Idb& idb = *db();

	// sqlite_master existence check: table exists
	Json result = idb.querySql("SELECT name FROM sqlite_master WHERE type='table' AND name=?", Json(), Json(JsonType::Array).add(kTableName));
	ASSERT_EQ(result["status"].toInt(), 200);
	ASSERT_EQ(result["data"].size(), 1);
	EXPECT_EQ(result["data"][0]["TABLE_NAME"].toString(), kTableName);

	// sqlite_master existence check: table missing -> 202
	result = idb.querySql("SELECT name FROM sqlite_master WHERE type='table' AND name=?", Json(), Json(JsonType::Array).add("no_such_table"));
	EXPECT_EQ(result["status"].toInt(), 202);

	// information_schema.views / columns are not supported -> empty
	result = idb.querySql("SELECT * FROM information_schema.views");
	EXPECT_EQ(result["status"].toInt(), 202);

	// Plain select without WHERE routes to the table with params as conditions
	result = idb.querySql("select * from table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"][0]["name"].toString(), "Kevin 凯文");

	// Plain select all
	result = idb.querySql("select * from table_for_test");
	ASSERT_EQ(result["status"].toInt(), 200);
	EXPECT_EQ(result["data"].size(), 6);

	// select ... from a missing table -> 202
	result = idb.querySql("select * from missing_table");
	EXPECT_EQ(result["status"].toInt(), 202);

	// SELECT with a literal WHERE clause is not supported -> empty result
	result = idb.querySql("select * from table_for_test where age = 18");
	EXPECT_EQ(result["status"].toInt(), 202);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(JsonFileDbTest, EdgeCases) {
	resetEmpty();
	Idb& idb = *db();

	// Create in an empty table
	Json createResult = idb.create("empty_table", Json{{"name", "first"}});
	ASSERT_EQ(createResult["status"].toInt(), 200);
	const std::string firstId = createResult["id"].toString();
	ASSERT_EQ(firstId.size(), static_cast<size_t>(8));

	// Verify round-trip
	Json selectResult = idb.select("empty_table", Json{{"id", firstId}});
	ASSERT_EQ(selectResult["status"].toInt(), 200);
	EXPECT_EQ(selectResult["data"][0]["name"].toString(), "first");

	// Create with array (batch insert via create)
	Json batchViaCreate(JsonType::Array);
	batchViaCreate.add(Json{{"id", "bc01"}, {"name", "batch-create-1"}});
	batchViaCreate.add(Json{{"id", "bc02"}, {"name", "batch-create-2"}});
	Json batchResult = idb.create("empty_table", batchViaCreate);
	ASSERT_EQ(batchResult["status"].toInt(), 200);
	EXPECT_EQ(batchResult["affectedRows"].toInt(), 2);
	EXPECT_EQ(idb.select("empty_table", Json(JsonType::Object))["data"].size(), 3);

	// Create with empty string id (should auto-generate)
	createResult = idb.create("empty_table", Json{{"id", ""}, {"name", "empty-id"}});
	ASSERT_EQ(createResult["status"].toInt(), 200);
	EXPECT_EQ(createResult["id"].toString().size(), static_cast<size_t>(8));

	// Sort without explicit direction
	Json sortResult = idb.select("empty_table", Json{{"sort", "name"}});
	EXPECT_EQ(sortResult["status"].toInt(), 200);

	// Row order after a delete: swap-pop (last row moves into the hole)
	reset();
	ASSERT_EQ(idb.remove(kTableName, Json{{"id", "a1b2c3d4"}})["status"].toInt(), 200);
	Json all = idb.select(kTableName, Json(JsonType::Object));
	ASSERT_EQ(all["status"].toInt(), 200);
	ASSERT_EQ(all["data"].size(), 5);
	EXPECT_EQ(all["data"][0]["id"].toString(), "a6b7c8d9");
	EXPECT_EQ(all["data"][1]["id"].toString(), "a2b3c4d5");
	EXPECT_EQ(all["data"][4]["id"].toString(), "a5b6c7d8");
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
// DbBase integration: dbType "jsonfile" routes to the file backend
// ─────────────────────────────────────────────────────────────────────────────

TEST(DbBaseTest, JsonFileRouting) {
	const std::string dbPath = scratchPath("dbbase");
	removeDatabaseFiles(dbPath);
	{
		Json options;
		options.add("connString", dbPath);
		options.add("DbLogClose", true);
		Idb* db = new DbBase("jsonfile", options);
		ASSERT_EQ(db->execSql(kCreateTableSql)["status"].toInt(), 200);
		ASSERT_EQ(db->create(kTableName, Json{{"id", "bb001"}, {"name", "via-dbbase"}, {"age", 7}, {"score", 0.5}})["status"].toInt(), 200);
		Json result = db->select(kTableName, Json{{"id", "bb001"}});
		ASSERT_EQ(result["status"].toInt(), 200);
		EXPECT_EQ(result["data"][0]["name"].toString(), "via-dbbase");

		// insertBatch + query + update through the same surface
		Json rows(JsonType::Array);
		rows.add(Json{{"id", "bb002"}, {"name", "batch"}, {"age", 8}, {"score", 1.5}});
		ASSERT_EQ(db->insertBatch(kTableName, rows)["status"].toInt(), 200);
		result = db->select(kTableName, Json{{"age", ">,7"}});
		ASSERT_EQ(result["status"].toInt(), 200);
		EXPECT_EQ(result["data"].size(), 1);

		ASSERT_EQ(db->update(kTableName, Json{{"id", "bb001"}, {"age", 9}})["status"].toInt(), 200);
		result = db->select(kTableName, Json{{"id", "bb001"}});
		EXPECT_EQ(result["data"][0]["age"].toInt(), 9);

		ASSERT_EQ(db->remove(kTableName, Json{{"id", "bb001"}})["status"].toInt(), 200);

		// Unknown dbType is still rejected (DbBase throws a const char*)
		EXPECT_THROW(DbBase("no_such_db", options), const char*);
		delete db;
	}
	removeDatabaseFiles(dbPath);
}

TEST(DefaultPathTest, UsesExecutableDirectory) {
	const std::string expected = joinPath(gExecutableDirectory, "data.json");
	EXPECT_EQ(JsonFileDb::defaultStoragePath(), jfd::genericUtf8(jfd::fsPath(expected).lexically_normal()));
}

}  // namespace

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	gExecutableDirectory = executableDirectoryFromArgv(argc > 0 ? argv[0] : nullptr);
	return RUN_ALL_TESTS();
}
