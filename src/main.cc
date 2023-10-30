#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZJSON;

int main()
{

	Json options;
	options.add("db_host", "192.168.6.6");
	options.add("db_port", 5432);
	options.add("db_name", "dbtest");
	options.add("db_user", "root");
	options.add("db_pass", "123456");
	options.add("db_char", "utf8mb4");
	options.add("db_conn", 5);
	options.add("DbLogClose", false);
	options.add("parameterized", true);
	ZORM::DbBase* db = new ZORM::DbBase("postgres", options);
	// Json rs = db->execSql("DROP TABLE IF EXISTS \"public\".\"table_for_test\";");
	// rs = db->execSql("CREATE TABLE \"public\".\"table_for_test\" (\
	// 	\"id\" char(64) NOT NULL,\
	// 	\"name\" varchar(128) DEFAULT \'\'::character varying,\
	// 	\"age\" int4 DEFAULT 0,\
	// 	\"score\" numeric DEFAULT 0.0)");
	// rs = db->execSql("ALTER TABLE \"public\".\"table_for_test\" ADD CONSTRAINT \"table_for_test_pkey\" PRIMARY KEY (\"id\");");
	Json cObj{
		{"id", "a1b2c3d4"},
		{"name", "Kevin 凯文"},
		{"age", 18},
		{"score", 99.99}
	};
	Json rs = db->create("table_for_test", cObj);

	Json obj("   \r\n{\"id\":\"a2b3c4d5\",\"name\":\"test001\",\"age\":19,\"score\":69.15}");
	std::cout << "type : " << obj.getValueType() << " ; content : " << obj.toString();

	std::cout << "this is the last line.";

    return 0;
}