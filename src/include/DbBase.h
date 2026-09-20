#pragma once
#include "Idb.h"

namespace ZORM
{

	// Public factory / facade: pick a backend by name and use the unified
	// Idb interface. The concrete backends are linked privately - this header
	// does NOT pull in any driver headers (mysql.h, DPI.h, ...).
	class ZORM_API DbBase : public Idb
	{
	public:
		// dbType: "sqlite3" | "jsonfile" | "mysql" | "postgres" | "dm8"
		// (case-insensitive). Throws const char* for an unsupported type.
		DbBase(string dbType, Json options = Json());
		~DbBase() override;

		Json select(const string& tablename, const Json& params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override;
		Json create(const string& tablename, const Json& params) override;
		Json update(const string& tablename, const Json& params) override;
		Json remove(const string& tablename, const Json& params) override;
		Json querySql(const string& sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override;
		Json execSql(const string& sql, Json params = Json(), Json values = Json(JsonType::Array)) override;
		Json insertBatch(const string& tablename, const Json& elements, string constraint = "id") override;
		Json transGo(const Json& sqls, bool isAsync = false) override;

	private:
		Idb* db;
	};

}
