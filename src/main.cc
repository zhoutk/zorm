#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

int main()
{
	DbBase* db = new DbBase("./db.db");
	Json qObj;
	qObj.addSubitem("id", 1);
	//qObj.addSubitem("age", 28);
	Json rs = db->select("users", qObj/*, Utils::MakeVector("username")*/);
	std::cout << rs.toString();

	std::cout << "this is the last line.";

    return 0;
}