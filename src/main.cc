#pragma once

#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

int main()
{
	DbBase* db = new DbBase("C:\\temp\\db.db");
	Json qObj;
	qObj.addSubitem("username", "张三");

	//cout << qObj.GetJsonString();

	Json rs = db->select("users", qObj/*, Utils::MakeVectorInitFromString("username")*/);
	std::cout << rs.toString();

    return 0;
}