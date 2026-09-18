// sqlite3 backend: shared contract suite + sqlite3-specific behaviour.

#include <gtest/gtest.h>

#include "ContractSuite.h"

using namespace ZORM;
using namespace ZJSON;

namespace {

class Sqlite3Env final : public contract::Env {
public:
	Idb* connect() override {
		Json options;
		options.add("connString", ":memory:");
		options.add("DbLogClose", false);
		options.add("parameterized", true);
		return new DbBase("sqlite3", options);
	}

	vector<string> schemaSqls() const override {
		return {
			"DROP TABLE IF EXISTS \"table_for_test\";",
			"CREATE TABLE \"table_for_test\" (\
				\"id\" char(64) NOT NULL,\
				\"name\" TEXT DEFAULT '',\
				\"age\" integer DEFAULT 0,\
				\"score\" real DEFAULT 0.0,\
				PRIMARY KEY (\"id\"));",
		};
	}
};

}  // namespace

ZORM_CONTRACT_TESTS()

// ─────────────────────────────────────────────────────────────────────────────
// sqlite3-specific behaviour
// ─────────────────────────────────────────────────────────────────────────────

TEST(Sqlite3Extra, MetadataCatalog) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();

	// Views are catalogued in sqlite_master; a missing view yields 202.
	Json rs = db.querySql(
		"SELECT name FROM sqlite_master WHERE type = 'view' AND name = 'v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);
}

TEST(Sqlite3Extra, QuerySqlWithParamsAndValues) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();
	ASSERT_TRUE(contract::env->reset());

	// SQL text with ? placeholders bound from values
	Json arrObj(JsonType::Array);
	arrObj.add("test001");
	Json rs = db.querySql("select * from table_for_test where name = ? ", Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	// params extend the SQL's own conditions
	Json qObj;
	qObj.add("age", 19);
	rs = db.querySql("select * from table_for_test where name = ? ", qObj, arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);
}

TEST(Sqlite3Extra, ParameterizedToggle) {
	// The parameterized switch changes how the same select is executed.
	Json options;
	options.add("connString", ":memory:");
	options.add("DbLogClose", false);
	options.add("parameterized", false);
	std::unique_ptr<Idb> db(new DbBase("sqlite3", options));

	ASSERT_EQ(db->execSql("DROP TABLE IF EXISTS \"table_for_test\";")["status"].toInt(), 200);
	ASSERT_EQ(db->execSql("CREATE TABLE \"table_for_test\" (\
		\"id\" char(64) NOT NULL,\
		\"name\" TEXT DEFAULT '',\
		\"age\" integer DEFAULT 0,\
		\"score\" real DEFAULT 0.0,\
		PRIMARY KEY (\"id\"));")["status"].toInt(), 200);

	Json row{{"id", "p001"}, {"name", "O'Brien"}, {"age", 1}, {"score", 0.5}};
	EXPECT_EQ(db->create("table_for_test", row)["status"].toInt(), 200);

	// The quote in O'Brien must not break the generated SQL.
	Json rs = db->select("table_for_test", Json{{"id", "p001"}});
	EXPECT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "O'Brien");
}

TEST(Sqlite3Extra, PlaceholderSql) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();
	ASSERT_TRUE(contract::env->reset());

	// execSql with ? placeholders
	Json values(JsonType::Array);
	values.add("placeholder-1");
	values.add("a1b2c3d4");
	Json rs = db.execSql("update table_for_test set name = ? where id = ?", Json(), values);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "placeholder-1");

	// transGo element with ? placeholders and values
	Json sqlArr(JsonType::Array);
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values (?,?,?,?)\",\"values\":[\"ph001\",\"ph-name\",9,9.9]}"));
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('ph002','ph-literal',10,10.1)\"}"));
	rs = db.transGo(sqlArr);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "ph001"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "ph-name");

	// querySql with ? placeholders
	Json arrObj(JsonType::Array);
	arrObj.add("ph-name");
	rs = db.querySql("select * from table_for_test where name = ?", Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);
}

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	static Sqlite3Env env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
