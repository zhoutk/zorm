// dm8 backend: shared contract suite + dm8-specific behaviour.
// Requires a live server; see the connection options below.

#include <gtest/gtest.h>

#include "ContractSuite.h"

using namespace ZORM;
using namespace ZJSON;

namespace {

class Dm8Env final : public contract::Env {
public:
	string rawSelectAll() const override {
		return "select * from \"table_for_test\"";
	}
	string rawInsert(const string& valuesList) const override {
		return "insert into \"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values (" + valuesList + ")";
	}
	string rawUpdate(const string& column, const string& sqlLiteral, const string& id) const override {
		return "update \"table_for_test\" set \"" + column + "\" = " + sqlLiteral + " where \"id\" = '" + id + "'";
	}

	Idb* connect() override {
		Json options;
		options.add("db_host", "10.0.0.7");
		options.add("db_port", 5236);
		options.add("db_name", "dbtest");
		options.add("db_user", "SYSDBA");
		options.add("db_pass", "123456");
		options.add("db_conn", 1);
		options.add("DbLogClose", false);
		options.add("parameterized", true);
		return new DbBase("dm8", options);
	}

	vector<string> schemaSqls() const override {
		return {
			// Raw SQL in querySql/transGo does not carry the schema prefix, so
			// point the session's default schema at the test database.
			"SET SCHEMA \"dbtest\";",
			"DROP TABLE IF EXISTS \"dbtest\".\"table_for_test\"",
			"CREATE TABLE \"dbtest\".\"table_for_test\" (\
				\"id\" CHAR(64) NOT NULL,\
				\"name\" VARCHAR(128),\
				\"age\" INTEGER,\
				\"score\" DOUBLE,\
				NOT CLUSTER PRIMARY KEY(\"id\")) STORAGE(ON \"MAIN\", CLUSTERBTR) ;",
		};
	}
};

}  // namespace

ZORM_CONTRACT_TESTS()

// ─────────────────────────────────────────────────────────────────────────────
// dm8-specific behaviour
// ─────────────────────────────────────────────────────────────────────────────

TEST(Dm8Extra, MetadataCatalog) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();

	// A missing view yields 202.
	Json rs = db.querySql(
		"select * from user_objects WHERE OBJECT_TYPE = 'VIEW' AND OBJECT_NAME ='v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);
}

TEST(Dm8Extra, PlaceholderSql) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();
	ASSERT_TRUE(contract::env->reset());

	// execSql with ? placeholders (dm8 needs quoted identifiers in raw SQL)
	Json values(JsonType::Array);
	values.add("placeholder-1");
	values.add("a1b2c3d4");
	Json rs = db.execSql("update \"table_for_test\" set \"name\" = ? where \"id\" = ?", Json(), values);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "placeholder-1");

	// transGo element with ? placeholders and values
	Json sqlArr(JsonType::Array);
	sqlArr.add(Json("{\"text\":\"insert into \\\"table_for_test\\\" (\\\"id\\\",\\\"name\\\",\\\"age\\\",\\\"score\\\") values (?,?,?,?)\",\"values\":[\"ph001\",\"ph-name\",9,9.9]}"));
	sqlArr.add(Json("{\"text\":\"insert into \\\"table_for_test\\\" (\\\"id\\\",\\\"name\\\",\\\"age\\\",\\\"score\\\") values ('ph002','ph-literal',10,10.1)\"}"));
	rs = db.transGo(sqlArr);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "ph001"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "ph-name");
}

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	static Dm8Env env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
