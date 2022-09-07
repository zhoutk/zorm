﻿#pragma once
#include "dll_global.h"
#include "zjson.hpp"
#include <vector>
#include <iostream>

using std::string;
using std::vector;
using namespace ZJSON;

class ZORM_API Idb
{
public:
	virtual Json select(string tablename, Json& params, vector<string> fields = vector<string>(), Json values = Json(), int queryType = 1) = 0;
	virtual Json create(string tablename, Json& params) = 0;
	virtual Json update(string tablename, Json& params) = 0;
	virtual Json remove(string tablename, Json& params) = 0;
	virtual Json querySql(string sql, Json& values, Json& params, vector<string> fields = vector<string>()) = 0;
	virtual Json execSql(string sql) = 0;
	virtual Json insertBatch(string tablename, Json& elements, string constraint = "id") = 0;
	virtual Json transGo(Json& sqls, bool isAsync = false) = 0;
};

