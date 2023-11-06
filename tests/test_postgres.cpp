#include "gtest/gtest.h"
#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZORM;

TEST(TestTest, test_postgres) {
	Json options;
	options.add("db_host", "192.168.5.12");
	options.add("db_port", 5432);
	options.add("db_name", "dbtest");
	options.add("db_user", "root");
	options.add("db_pass", "123456");
	options.add("db_char", "utf8mb4");
	options.add("db_conn", 5);
	options.add("DbLogClose", false);
	options.add("parameterized", true);
	DbBase* db = new DbBase("postgres", options);
	Json rs = db->execSql("DROP TABLE IF EXISTS \"public\".\"table_for_test\";");
	EXPECT_EQ(rs["status"].toInt(), 200);
	rs = db->execSql("CREATE TABLE \"public\".\"table_for_test\" (\
		\"id\" char(64) NOT NULL,\
		\"name\" varchar(128) DEFAULT \'\'::character varying,\
		\"age\" int4 DEFAULT 0,\
		\"score\" numeric DEFAULT 0.0)");
	EXPECT_EQ(rs["status"].toInt(), 200);
	rs = db->execSql("ALTER TABLE \"public\".\"table_for_test\" ADD CONSTRAINT \"table_for_test_pkey\" PRIMARY KEY (\"id\");");
	EXPECT_EQ(rs["status"].toInt(), 200);
	Json cObj{
		{"id", "a1b2c3d4"},
		{"name", "Kevin 凯文"},
		{"age", 18},
		{"score", 99.99}
	};
	rs = db->create("table_for_test", cObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	rs = db->querySql("select viewname from pg_views where schemaname = 'public' and viewname = 'v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);

	Json cObjs(JsonType::Array);
	cObjs.add(Json("{\"id\":\"a2b3c4d5\",\"name\":\"test001\",\"age\":19,\"score\":69.15}"));
	cObjs.add(Json("{\"id\":\"a3b4c5d6\",\"name\":\"test002\",\"age\":20,\"score\":56.87}"));
	rs = db->insertBatch("table_for_test", cObjs);
	EXPECT_EQ(rs["status"].toInt(), 200);

	Json sqlArr(JsonType::Array);
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('a4b5c6d7','test003',21,78.48)\"}"));
	//sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('a5b6c7d8','test004',22,23.27)\"}"));
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ($1,$2,$3,$4)\",\"values\":[\"a5b6c7d8\",\"test004\",22,23.27]}"));
	sqlArr.add(Json("{\"text\":\"insert into table_for_test (id,name,age,score) values ('a6b7c8d9','test005',23,43.93)\"}"));
	rs = db->transGo(sqlArr);
	EXPECT_EQ(rs["status"].toInt(), 200);

	Json qObj;
	rs = db->select("table_name_not_exist_in_db", qObj);
	EXPECT_EQ(rs["status"].toInt(), 701);

	qObj.add("id", "a1b2c3d4");
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "Kevin 凯文");
	EXPECT_EQ(rs["data"][0]["age"].toInt(), 18);
	EXPECT_EQ(rs["data"][0]["score"].toDouble(), 99.99);

	qObj.add("age", 18);
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["score"].toDouble(), 99.99);

	qObj = Json{{"name", "Kevin"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 202);

	qObj = Json{{"name", "Kevin 凯文"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	qObj = Json{{"name", "Kevin"}, {"fuzzy", 1}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "Kevin 凯文");

	qObj = Json{{"id", "a1b2c3d4"}};
	qObj.add("score", 6.6);
	rs = db->update("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	qObj = Json{{"id", "a1b2c3d4"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["score"].toDouble(), 6.6);

	rs = db->remove("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["status"].toInt(), 202);

	qObj = Json{{"name", "test"}, {"fuzzy", 1}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"].size(), 5);

	string sql = "select * from table_for_test";
	qObj = Json{{"id", "a2b3c4d5"}};
	rs = db->querySql(sql, qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "select * from table_for_test where name = $1 ";
	Json arrObj(JsonType::Array);
	arrObj.add("test001");
	rs = db->querySql(sql, Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "select * from table_for_test where name = $1 ";
	qObj.clear();
	qObj.add("age", 19);
	rs = db->querySql(sql, qObj, arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "update table_for_test set name = $1 where id = $2 ";
	arrObj = Json(JsonType::Array);
	arrObj.add({"test999", "a5b6c7d8"});
	rs = db->execSql(sql, Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "update table_for_test set name = $1 where id = $2 ";
	qObj.clear();
	qObj.add("score", 23.27);
	qObj.add("age", 22);
	arrObj = Json(JsonType::Array);
	arrObj.add({"test888", "a5b6c7d8"});
	rs = db->execSql(sql, qObj, arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	qObj = Json{{"id", "a5b6c7d8"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "test888");

	qObj = Json{{"ins", "age,20,21,23"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"].size(), 3);

	qObj = Json{{"lks", "name,001,age,23"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"].size(), 2);

	qObj = Json{{"ors", "age,19,age,23"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"].size(), 2);

	qObj = Json{{"page", 1}, {"size", 3}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"].size(), 3);

	qObj = Json{{"count", "1,total"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["total"].toInt(), 5);

	qObj = Json{{"sum", "age,ageSum"}, {"age", "<=,20"}};
	rs = db->select("table_for_test", qObj);
	EXPECT_EQ(rs["data"][0]["agesum"].toInt(), 39);

	 qObj = Json{{"id", "a4b5c6d7"}};
	 qObj.add("age", 22);
	 rs = db->update("table_for_test", qObj);

	 qObj = Json{{"group", "age"}, {"count", "*,total"}, {"sort", "total desc"}};
	 std::vector<string> vs{ "id", "age"};
	 rs = db->select("table_for_test", qObj, vs);
	 EXPECT_EQ(rs["data"][0]["total"].toInt(), 2);

}