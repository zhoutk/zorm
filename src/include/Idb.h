#pragma once
#include "dll_global.h"
#include "zjson.hpp"
#include <vector>
#include <iostream>

namespace ZORM
{

	using std::string;
	using std::vector;
	using namespace ZJSON;

	class ZORM_API Idb
	{
	public:
		// Polymorphic base: without this, `delete` through an Idb* (as DbBase
		// does) is undefined behaviour and derived backends' members (e.g.
		// Sqlit3Db's sqlite handle) are never destroyed. Adding it keeps every
		// existing signature and call site unchanged.
		virtual ~Idb() = default;
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
