// mysql backend: shared contract suite + mysql-specific behaviour.
// Requires a live server; see the connection options below.

#include <gtest/gtest.h>

#include "ContractSuite.h"

using namespace ZORM;
using namespace ZJSON;

namespace {

class MysqlEnv final : public contract::Env {
public:
	Idb* connect() override {
		Json options;
		options.add("db_host", "10.0.0.7");
		options.add("db_port", 3306);
		options.add("db_name", "dbtest");
		options.add("db_user", "root");
		options.add("db_pass", "123456");
		options.add("db_char", "utf8mb4");
		options.add("db_conn", 5);
		options.add("DbLogClose", false);
		options.add("parameterized", true);
		return new DbBase("mysql", options);
	}

	vector<string> schemaSqls() const override {
		return {
			"DROP TABLE IF EXISTS `table_for_test`;",
			"CREATE TABLE `table_for_test` (\
				`id` char(64) CHARACTER SET utf8mb4 NOT NULL,\
				`name` varchar(128) CHARACTER SET utf8mb4 NULL DEFAULT '',\
				`age` int(0) NULL DEFAULT 0,\
				`score` double NULL DEFAULT 0,\
				PRIMARY KEY (`id`) USING BTREE\
				) ENGINE = InnoDB CHARACTER SET = utf8mb4 ROW_FORMAT = Dynamic;",
		};
	}
};

}  // namespace

ZORM_CONTRACT_TESTS()

// ─────────────────────────────────────────────────────────────────────────────
// mysql-specific behaviour
// ─────────────────────────────────────────────────────────────────────────────

TEST(MysqlExtra, MetadataCatalog) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();

	// A missing view yields 202.
	Json rs = db.querySql(
		"SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS WHERE TABLE_SCHEMA='dbtest' AND TABLE_NAME='v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);
}

TEST(MysqlExtra, PlaceholderSql) {
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
}

TEST(MysqlExtra, ConnectionInfo) {
	contract::env->connectOnce();
	Idb& db = contract::env->db();

	// Informational: client identity and TLS state of this connection. The
	// client prefers TLS (ssl-mode=PREFERRED); against a skip_ssl server the
	// connection falls back to plain text (empty Ssl_version).
	std::cout << "client library: " << mysql_get_client_info() << std::endl;
	const char* statusNames[] = {"Ssl_version", "Ssl_cipher"};
	for (const char* name : statusNames) {
		Json rs = db.querySql(std::string("SHOW STATUS LIKE '") + name + "'");
		if (rs["status"].toInt() == 200 && rs["data"].size() > 0) {
			std::cout << name << ": '" << rs["data"][0]["Value"].toString() << "'" << std::endl;
		}
	}
	SUCCEED();
}

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);
	static MysqlEnv env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
