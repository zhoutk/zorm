#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZJSON;

int main()
{

	Json options;
	options.add("db_host", "192.168.0.243");
	options.add("db_port", 5236);
	options.add("db_name", "dbtest");
	options.add("db_user", "SYSDBA");
	options.add("db_pass", "SYSDBA001");
	options.add("db_char", "utf8mb4");
	options.add("db_conn", 1);
	options.add("DbLogClose", false);
	options.add("parameterized", false);
	ZORM::DbBase* db = new ZORM::DbBase("dm8", options);
	// Json rs = db->execSql("DROP TABLE IF EXISTS \"public\".\"table_for_test\";");
	// rs = db->execSql("CREATE TABLE \"public\".\"table_for_test\" (\
	// 	\"id\" char(64) NOT NULL,\
	// 	\"name\" varchar(128) DEFAULT \'\'::character varying,\
	// 	\"age\" int4 DEFAULT 0,\
	// 	\"score\" numeric DEFAULT 0.0)");
	// rs = db->execSql("ALTER TABLE \"public\".\"table_for_test\" ADD CONSTRAINT \"table_for_test_pkey\" PRIMARY KEY (\"id\");");
	//Json cObj{
	//	{"id", "a1b2c3d4"},
	//	{"name", "kevin中国"},
	//	{"age", "20"}
	//};
	//Json cObj2{
	//	{"id", "a1b2c3d5"},
	//	{"name", "john美国"},
	//	{"age", "22"}
	//};
	//Json arr(JsonType::Array);
	//arr.add({cObj, cObj2});
	//Json rs = db->insertBatch("table_for_test", arr);

	Json sqlArr(JsonType::Array);
	Json j1;
	j1.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values ('a4b5c6d7','test003',21,78.48)");
	Json j2;
	j2.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values ('a6b7c8d9','test005',23,43.93)");
	Json j3;
	j3.add("text", "insert into \"dbtest\".\"table_for_test\" (\"id\",\"name\",\"age\",\"score\") values (?,?,?,?)");
	Json arr(JsonType::Array);
	arr.add({ "a5b6c7d8","test004",22,23.27});
	j3.add("values", arr);
	sqlArr.add({ j1, j2, j3 });
	Json rs = db->transGo(sqlArr);

	/*Json p;
	Json rs = db->select("table_for_test", p);*/

	std::cout << rs.toString() << "\n";

	Json cObj{
	{"id", "a8b2c3d4"},
	{"name", "Kevin 凯文"},
	{"age", 18}
	};
	rs = db->create("table_for_test", cObj);
	/*
	Json p;
	rs = db->select("table_for_test", p);
	std::string dd = rs.toString();
	std::cout << rs.toString() << "\n";

	Json obj("{\"id\":\"a1b2c3d4\"}");

	db->remove("table_for_test", obj);

	std::cout << "this is the last line.";*/

	/*Json uobj("{\"id\":\"a1b2c3d4\",\"name\":\"你的名字\"}");
	rs = db->update("table_for_test", uobj);

	Json obj("{\"id\":\"a1b2c3d4\"}");

	db->remove("table_for_test", obj);*/

    return 0;
}