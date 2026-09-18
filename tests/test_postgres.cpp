// postgres backend: shared contract suite + postgres-specific behaviour.
// Requires a live server; see the connection options below.

#include <gtest/gtest.h>

#include "ContractSuite.h"

using namespace ZORM;
using namespace ZJSON;

namespace {

class PostgresEnv final : public contract::Env {
public:
	Idb* connect() override {
		Json options;
		options.add("db_host", "10.0.0.7");
		options.add("db_port", 5432);
		options.add("db_name", "dbtest");
		options.add("db_user", "root");
		options.add("db_pass", "123456");
		options.add("db_char", "utf8mb4");
		options.add("db_conn", 5);
		options.add("DbLogClose", false);
		options.add("parameterized", true);
		return new DbBase("postgres", options);
	}

	vector<string> schemaSqls() const override {
		return {
			"DROP TABLE IF EXISTS \"public\".\"table_for_test\";",
			"CREATE TABLE \"public\".\"table_for_test\" (\
				\"id\" char(64) NOT NULL,\
				\"name\" varchar(128) DEFAULT ''::character varying,\
				\"age\" int4 DEFAULT 0,\
				\"score\" numeric DEFAULT 0.0)",
			"ALTER TABLE \"public\".\"table_for_test\" ADD CONSTRAINT \"table_for_test_pkey\" PRIMARY KEY (\"id\");",
		};
	}
};

}  // namespace

ZORM_CONTRACT_TESTS()

// ─────────────────────────────────────────────────────────────────────────────
// postgres-specific behaviour
// ─────────────────────────────────────────────────────────────────────────────

TEST(PostgresExtra, MetadataCatalog) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();

	// A missing view yields 202.
	Json rs = db.querySql(
		"select viewname from pg_views where schemaname = 'public' and viewname = 'v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);
}

TEST(PostgresExtra, PlaceholderSql) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();
	ASSERT_TRUE(contract::env->reset());

	// postgres placeholders are $n (a raw `?` is a syntax error)
	Json values(JsonType::Array);
	values.add("placeholder-1");
	values.add("a1b2c3d4");
	Json rs = db.execSql("update table_for_test set name = $1 where id = $2", Json(), values);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "placeholder-1");

	// transGo element with $n placeholders and values
	Json sqlArr(JsonType::Array);
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ($1,$2,$3,$4)\",\"values\":[\"ph001\",\"ph-name\",9,9.9]}"));
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('ph002','ph-literal',10,10.1)\"}"));
	rs = db.transGo(sqlArr);
	ASSERT_EQ(rs["status"].toInt(), 200);
	rs = db.select("table_for_test", Json{{"id", "ph001"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "ph-name");
}

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	static PostgresEnv env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
