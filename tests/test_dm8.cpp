#include "gtest/gtest.h"
#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZORM;

TEST(TestTest, test_mysql) {
	Json options;
	options.add("db_host", "192.168.5.12");
	options.add("db_port", 5236);
	options.add("db_name", "dbtest");
	options.add("db_user", "SYSDBA");
	options.add("db_pass", "SYSDBA001");
	options.add("db_conn", 1);
	options.add("DbLogClose", false);
	options.add("parameterized", true);
	DbBase* db = new DbBase("dm8", options);
	Json rs = db->execSql("DROP TABLE IF EXISTS \"dbtest\".\"table_for_test\"");
	EXPECT_EQ(rs["status"].toInt(), 200);
	rs = db->execSql("CREATE TABLE \"dbtest\".\"table_for_test\" (\
		\"id\" CHAR(64) NOT NULL,\
		\"name\" VARCHAR(128),\
		\"age\" INTEGER,\
		\"score\" DOUBLE,\
		NOT CLUSTER PRIMARY KEY(\"id\")) STORAGE(ON \"MAIN\", CLUSTERBTR) ;");
	EXPECT_EQ(rs["status"].toInt(), 200);
	Json cObj{
		{"id", "a1b2c3d4"},
		{"name", "Kevin 凯文"},
		{"age", 18},
		{"score", 99.99}
	};
	rs = db->create("table_for_test", cObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	rs = db->querySql("select * from user_objects WHERE OBJECT_TYPE = 'VIEW' AND OBJECT_NAME ='v_table_name_not_exist_in_db'");
	EXPECT_EQ(rs["status"].toInt(), 202);

	Json cObjs(JsonType::Array);
	cObjs.add(Json("{\"id\":\"a2b3c4d5\",\"name\":\"test001\",\"age\":19,\"score\":69.15}"));
	cObjs.add(Json("{\"id\":\"a3b4c5d6\",\"name\":\"test002\",\"age\":20,\"score\":56.87}"));
	rs = db->insertBatch("table_for_test", cObjs);
	EXPECT_EQ(rs["status"].toInt(), 200);

	Json sqlArr(JsonType::Array);
	Json j1;
	j1.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values ('a4b5c6d7','test003',21,78.48)");
	Json j2;
	j2.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values (?,?,?,?)");
	Json arr(JsonType::Array);
	arr.add({ "a5b6c7d8","test004",22,23.27 });
	j2.add("values", arr);
	std::cout << j2.toString() << "\n";
	std::cout << j2.toString() << "\n";
	Json j3;
	j3.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values ('a6b7c8d9','test005',23,43.93)");
	sqlArr.add({ j1, j2, j3 });
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

	string sql = "select * from \"dbtest\".\"table_for_test\"";
	qObj = Json{{"id", "a2b3c4d5"}};
	rs = db->querySql(sql, qObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "select * from \"dbtest\".\"table_for_test\" where \"name\" = ? ";
	Json arrObj(JsonType::Array);
	arrObj.add("test001");
	rs = db->querySql(sql, Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "select * from \"dbtest\".\"table_for_test\" where \"name\" = ? ";
	qObj.clear();
	qObj.add("age", 19);
	rs = db->querySql(sql, qObj, arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "update \"dbtest\".\"table_for_test\" set \"name\" = ? where \"id\" = ? ";
	arrObj = Json(JsonType::Array);
	arrObj.add({"test999", "a5b6c7d8"});
	rs = db->execSql(sql, Json(), arrObj);
	EXPECT_EQ(rs["status"].toInt(), 200);

	sql = "update \"dbtest\".\"table_for_test\" set \"name\" = ? where \"id\" = ? ";
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
	std::vector<string> vs{"id"};
	rs = db->select("table_for_test", qObj, vs);
	EXPECT_EQ(rs["data"][0]["TOTAL"].toInt(), 5);

	qObj = Json{{"sum", "age,ageSum"}, {"age", "<=,20"}};
	rs = db->select("table_for_test", qObj, vs);
	EXPECT_EQ(rs["data"][0]["AGESUM"].toInt(), 39);

	qObj = Json{{"id", "a4b5c6d7"}};
	qObj.add("age", 22);
	rs = db->update("table_for_test", qObj);

	qObj = Json{{"group", "age"}, {"count", "*,total"}, {"sort", "total desc"}};
	vs.clear();
	vs.push_back("age");
	rs = db->select("table_for_test", qObj, vs);
	EXPECT_EQ(rs["data"][0]["TOTAL"].toInt(), 2);

}