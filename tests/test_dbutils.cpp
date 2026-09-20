// tests/test_dbutils.cpp
// ----------------------------------------------------------------------------
// Backend-independent unit tests for the pieces of the library that do not
// need a database connection: DbUtils helpers and the DbBase facade's
// parameter validation. These are the paths the per-backend contract suite
// cannot cover (they run before any SQL is generated).
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <string>

#include "DbBase.h"
#include "DbUtils.h"

using namespace ZORM;
using namespace ZJSON;

// ─────────────────────────────────────────────────────────────────────────────
// DbUtils
// ─────────────────────────────────────────────────────────────────────────────

TEST(DbUtils, GenerateIdIs8HexChars) {
	for (int i = 0; i < 100; i++) {
		const std::string id = DbUtils::GenerateId();
		EXPECT_EQ(id.size(), 8u) << id;
		for (char c : id)
			EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << id;
	}
}

TEST(DbUtils, GenerateIdIsNotConstant) {
	std::string first = DbUtils::GenerateId();
	bool differs = false;
	for (int i = 0; i < 50 && !differs; i++)
		differs = DbUtils::GenerateId() != first;
	EXPECT_TRUE(differs);
}

TEST(DbUtils, Trim) {
	EXPECT_EQ(DbUtils::Trim(""), "");
	EXPECT_EQ(DbUtils::Trim("  "), "");
	EXPECT_EQ(DbUtils::Trim(" abc "), "abc");
	EXPECT_EQ(DbUtils::Trim("\t abc \n"), "abc");
	EXPECT_EQ(DbUtils::Trim("abc"), "abc");
	EXPECT_EQ(DbUtils::Trim("a b"), "a b");
}

TEST(DbUtils, MakeVector) {
	auto v = DbUtils::MakeVector("a,b,c");
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b");
	EXPECT_EQ(v[2], "c");

	auto single = DbUtils::MakeVector("only");
	ASSERT_EQ(single.size(), 1u);
	EXPECT_EQ(single[0], "only");

	auto semi = DbUtils::MakeVector("a;b", ';');
	ASSERT_EQ(semi.size(), 2u);
	EXPECT_EQ(semi[1], "b");
}

TEST(DbUtils, FindStringFromVector) {
	EXPECT_TRUE(DbUtils::FindStringFromVector({"ins", "lks", "ors"}, "lks"));
	EXPECT_FALSE(DbUtils::FindStringFromVector({"ins", "lks", "ors"}, "fuzzy"));
	EXPECT_FALSE(DbUtils::FindStringFromVector({}, "lks"));
}

TEST(DbUtils, FindStartsStringFromVector) {
	EXPECT_TRUE(DbUtils::FindStartsStringFromVector({">,", ">=,"}, ">=,21"));
	EXPECT_TRUE(DbUtils::FindStartsStringFromVector({">,", ">=,"}, ">,21"));
	EXPECT_FALSE(DbUtils::FindStartsStringFromVector({">,", ">=,"}, "21"));
	// value equal in length to the key (no suffix) must not match
	EXPECT_FALSE(DbUtils::FindStartsStringFromVector({">,"}, ">,"));
}

TEST(DbUtils, GetVectorJoinStr) {
	EXPECT_EQ(DbUtils::GetVectorJoinStr({"a", "b", "c"}), "a,b,c");
	EXPECT_EQ(DbUtils::GetVectorJoinStr({}), "");
	EXPECT_EQ(DbUtils::GetVectorJoinStr({"x"}), "x");
}

TEST(DbUtils, GetVectorJoinStrArroundQuots) {
	EXPECT_EQ(DbUtils::GetVectorJoinStrArroundQuots({"a", "b"}), "\"a\",\"b\"");
	EXPECT_EQ(DbUtils::GetVectorJoinStrArroundQuots({}), "\"\"");
	EXPECT_EQ(DbUtils::GetVectorJoinStrArroundQuots({"x"}), "\"x\"");
}

TEST(DbUtils, GetVectorFromJson) {
	Json arr(JsonType::Array);
	arr.add("a");
	arr.add("b");
	auto v = DbUtils::GetVectorFromJson(arr);
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b");
	// non-array input yields an empty vector
	EXPECT_TRUE(DbUtils::GetVectorFromJson(Json("{\"k\":1}")).empty());
}

TEST(DbUtils, MakeJsonObjectStatusAndMessage) {
	Json ok = DbUtils::MakeJsonObject(STSUCCESS);
	EXPECT_EQ(ok["status"].toInt(), 200);
	EXPECT_EQ(ok["message"].toString(), "Operation succeeded. ");

	Json err = DbUtils::MakeJsonObject(STPARAMERR, "fuzzy is wrong.");
	EXPECT_EQ(err["status"].toInt(), 301);
	// message = code message + " details, " + info
	EXPECT_NE(err["message"].toString().find("details, fuzzy is wrong."), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DbBase facade: parameter validation happens before any backend call and
// must behave identically regardless of dbType. sqlite3-mem never connects,
// so these run fully offline.
// ─────────────────────────────────────────────────────────────────────────────

class DbBaseFacade : public ::testing::Test {
protected:
	void SetUp() override {
		Json options;
		options.add("connString", ":memory:");
		db.reset(new DbBase("sqlite3", options));
	}
	std::unique_ptr<DbBase> db;
};

TEST_F(DbBaseFacade, UnknownDbTypeThrows) {
	EXPECT_ANY_THROW(new DbBase("no-such-db"));
}

TEST_F(DbBaseFacade, SelectRejectsNonArrayValues) {
	Json rs = db->select("t", Json(), vector<string>(), Json("{\"a\":1}"));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, QuerySqlRejectsNonObjectParams) {
	Json rs = db->querySql("select 1", Json("[1]"), Json(JsonType::Array));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, QuerySqlRejectsNonArrayValues) {
	Json rs = db->querySql("select 1", Json(), Json("{\"a\":1}"));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, ExecSqlRejectsNonObjectParams) {
	Json rs = db->execSql("create table t(a int)", Json("[1]"), Json(JsonType::Array));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, InsertBatchRejectsNonArray) {
	Json rs = db->insertBatch("t", Json("{\"a\":1}"));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, TransGoRejectsNonArray) {
	Json rs = db->transGo(Json("{\"text\":\"select 1\"}"));
	EXPECT_EQ(rs["status"].toInt(), 301);
}

TEST_F(DbBaseFacade, SelectWithArrayValuesPassesThrough) {
	// valid shape: the statement itself is wrong (no such table) -> a db
	// error (701), NOT the facade 301 - proving the facade let it through.
	Json values(JsonType::Array);
	Json rs = db->select("no_such_table", Json(), vector<string>(), values);
	EXPECT_EQ(rs["status"].toInt(), 701);
}
