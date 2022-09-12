#pragma once
#include "Idb.h"
#include "Sqlit3Db.h"
#include <algorithm>

namespace ZORM
{

	class ZORM_API DbBase : public Idb
	{
	public:
		DbBase(string dbType, Json options = Json()) {
			transform(dbType.begin(), dbType.end(), dbType.begin(), ::tolower);
			bool DbLogClose = options["DbLogClose"].toBool();
			if (dbType.compare("sqlite3") == 0)
				db = new Sqlit3::Sqlit3Db(options["connString"].toString(), DbLogClose);
			else {
				throw "Db Type error or not be supported. ";
			}
		};
		~DbBase() {
			if (db) {
				delete db;
			}
		};

		Json select(string tablename, Json& params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array), int queryType = 1) {
			return values.isArray() ? db->select(tablename, params, fields) : DbUtils::MakeJsonObject(STPARAMERR);
		};

		Json create(string tablename, Json& params) {
			return db->create(tablename, params);
		};

		Json update(string tablename, Json& params) {
			return db->update(tablename, params);
		};

		Json remove(string tablename, Json& params) {
			return db->remove(tablename, params);
		};

		Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) {
			return params.isObject() && values.isArray() ? db->querySql(sql, params, values, fields) : DbUtils::MakeJsonObject(STPARAMERR);
		}

		Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) {
			return params.isObject() && values.isArray() ? db->execSql(sql, params, values) : DbUtils::MakeJsonObject(STPARAMERR);
		}

		Json insertBatch(string tablename, Json& elements, string constraint = "id") {
			return elements.isArray() ? db->insertBatch(tablename, elements, constraint) : DbUtils::MakeJsonObject(STPARAMERR);
		}

		Json transGo(Json& sqls, bool isAsync = false) {
			return sqls.isArray() ? db->transGo(sqls) : DbUtils::MakeJsonObject(STPARAMERR);
		}

	private:
		Idb * db;
	};

}
