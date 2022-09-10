﻿#pragma once
#include "Idb.h"
#include "Sqlit3Db.h"
#include <algorithm>

class ZORM_API DbBase : Idb
{
public:
	DbBase(string connStr, string dbType = "sqlit3") : connStr(connStr) {
		transform(dbType.begin(), dbType.end(), dbType.begin(), ::tolower);
		if (dbType.compare("sqlit3") == 0)
			db = new Sqlit3::Sqlit3Db(connStr);
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
		return db->select(tablename, params, fields);
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
		return db->querySql(sql, params, values, fields);
	}

	Json execSql(string sql, Json values = Json(JsonType::Array)) {
		return db->execSql(sql, values);
	}

	Json insertBatch(string tablename, Json& elements, string constraint = "id") {
		return db->insertBatch(tablename, elements, constraint);
	}

	Json transGo(Json& sqls, bool isAsync = false) {
		return db->transGo(sqls);
	}

private:
	string connStr;
	Idb * db;
};

