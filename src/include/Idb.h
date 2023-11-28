#pragma once
#include "dll_global.h"
#ifdef __QJSON
#include "qjson.h"
#define JSONCHOICE QJSON
#else
#include "zjson.hpp"
#define JSONCHOICE ZJSON
#endif // __QJSON
#include <vector>
#include <iostream>

namespace ZORM
{

	using std::string;
	using std::vector;
	using namespace JSONCHOICE;

	class ZORM_API Idb
	{
	public:
		virtual Json select(const string& tablename, const Json &params,
							vector<string> fields = vector<string>(),
							Json values = Json(JsonType::Array)) = 0;
		virtual Json create(const string& tablename, const Json &params) = 0;
		virtual Json update(const string& tablename, const Json &params) = 0;
		virtual Json remove(const string& tablename, const Json &params) = 0;
		virtual Json querySql(const string& sql, Json params = Json(),
							  Json values = Json(JsonType::Array),
							  vector<string> fields = vector<string>()) = 0;
		virtual Json execSql(const string& sql, Json params = Json(),
							 Json values = Json(JsonType::Array)) = 0;
		virtual Json insertBatch(const string& tablename, const Json &elements, string constraint = "id") = 0;
		virtual Json transGo(const Json &sqls, bool isAsync = false) = 0;
	};

}
