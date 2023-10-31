#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZJSON;

int main()
{

	Json options;
	options.add("db_host", "192.168.5.12");
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
	Json cObj{
		{"age", ">=,16,<=,20"}
	};
	//Json rs = db->create("table_for_test", cObj);

	Json rs = db->select("table_for_test", cObj);

	/*Json uobj("{\"id\":\"a1b2c3d4\",\"name\":\"ÄãµÄÃû×Ö\"}");
	rs = db->update("table_for_test", uobj);

	Json obj("{\"id\":\"a1b2c3d4\"}");

	db->remove("table_for_test", obj);*/

	std::cout << rs.toString() << "\n";
	std::cout << "this is the last line.";

    return 0;
}