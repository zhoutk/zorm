#include "DbBase.h"

#include "DbUtils.h"  // MakeJsonObject / status codes for the facade checks

// Factory: the only translation unit that needs the concrete backend
// headers. Everything downstream of Idb stays private to the library.
#include "Sqlit3Db.h"
#include "MysqlDb.h"
#include "PostgresDb.h"
#include "Dm8Db.h"
#include "JsonFileDb.h"

#include <algorithm>

namespace ZORM
{

	DbBase::DbBase(string dbType, Json options) {
		transform(dbType.begin(), dbType.end(), dbType.begin(), ::tolower);
		bool DbLogClose = options["DbLogClose"].toBool();
		if (dbType.compare("sqlite3") == 0)
			db = new Sqlit3::Sqlit3Db(options["connString"].toString(), DbLogClose, options["parameterized"].toBool());
		else if (dbType.compare("jsonfile") == 0) {
			// connString: path of the JSON data file (empty -> <exe dir>/data.json)
			db = new JsonFile::JsonFileDb(options["connString"].toString(), DbLogClose);
		}
		else if (dbType.compare("mysql") == 0) {
			string dbhost = options.take("db_host").toString();
			string dbuser = options.take("db_user").toString();
			string dbpwd = options.take("db_pass").toString();
			string dbname = options.take("db_name").toString();
			int dbport = options.take("db_port").toInt();

			db = new Mysql::MysqlDb(dbhost, dbuser, dbpwd, dbname, dbport, options);
		}
		else if (dbType.compare("postgres") == 0) {
			string dbhost = options.take("db_host").toString();
			string dbuser = options.take("db_user").toString();
			string dbpwd = options.take("db_pass").toString();
			string dbname = options.take("db_name").toString();
			int dbport = options.take("db_port").toInt();

			db = new Postgres::PostgresDb(dbhost, dbuser, dbpwd, dbname, dbport, options);
		}
		else if (dbType.compare("dm8") == 0) {
			string dbhost = options.take("db_host").toString();
			string dbuser = options.take("db_user").toString();
			string dbpwd = options.take("db_pass").toString();

			db = new Dm8::Dm8Db(dbhost, dbuser, dbpwd, options);
		}
		else {
			throw "Db Type error or not be supported. ";
		}
	}

	DbBase::~DbBase() {
		if (db) {
			delete db;
		}
	}

	Json DbBase::select(const string& tablename, const Json& params, vector<string> fields, Json values) {
		return values.isArray() ? db->select(tablename, params, fields) : DbUtils::MakeJsonObject(STPARAMERR);
	}

	Json DbBase::create(const string& tablename, const Json& params) {
		return db->create(tablename, params);
	}

	Json DbBase::update(const string& tablename, const Json& params) {
		return db->update(tablename, params);
	}

	Json DbBase::remove(const string& tablename, const Json& params) {
		return db->remove(tablename, params);
	}

	Json DbBase::querySql(const string& sql, Json params, Json values, vector<string> fields) {
		return params.isObject() && values.isArray() ? db->querySql(sql, params, values, fields) : DbUtils::MakeJsonObject(STPARAMERR);
	}

	Json DbBase::execSql(const string& sql, Json params, Json values) {
		return params.isObject() && values.isArray() ? db->execSql(sql, params, values) : DbUtils::MakeJsonObject(STPARAMERR);
	}

	Json DbBase::insertBatch(const string& tablename, const Json& elements, string constraint) {
		return elements.isArray() ? db->insertBatch(tablename, elements, constraint) : DbUtils::MakeJsonObject(STPARAMERR);
	}

	Json DbBase::transGo(const Json& sqls, bool isAsync) {
		return sqls.isArray() ? db->transGo(sqls) : DbUtils::MakeJsonObject(STPARAMERR);
	}

}
