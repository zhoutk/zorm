﻿#pragma once
#include <assert.h>
#include "Idb.h"
#include "sqlite3.h"
#include "Utils.h"
#include "GlobalConstants.h"
#include <algorithm>

using std::string;
using std::vector;

namespace Sqlit3 {

#define SQLITECPP_ASSERT(expression, message)   assert(expression && message)

	const int   OPEN_READONLY = SQLITE_OPEN_READONLY;
	const int   OPEN_READWRITE = SQLITE_OPEN_READWRITE;
	const int   OPEN_CREATE = SQLITE_OPEN_CREATE;
	const int   OPEN_URI = SQLITE_OPEN_URI;

	const int   OK = SQLITE_OK;

	class ZORM_API Sqlit3Db : public Idb
	{
	public:
		struct Deleter
		{
			void operator()(sqlite3* apSQLite) {
				const int ret = sqlite3_close(apSQLite);
				(void)ret;
				SQLITECPP_ASSERT(SQLITE_OK == ret, "database is locked");
			};
		};

	private:
		std::unique_ptr<sqlite3, Deleter> mSQLitePtr;
		std::string mFilename;
		vector<string> QUERY_EXTRA_KEYS;
		vector<string> QUERY_UNEQ_OPERS;

	public:
		Sqlit3Db(const char* apFilename, bool logFlag = false,
			const int   aFlags = OPEN_READWRITE | OPEN_CREATE,
			const int   aBusyTimeoutMs = 0,
			const char* apVfs = nullptr) : mFilename(apFilename)
		{
			QUERY_EXTRA_KEYS = DbUtils::MakeVector("ins,lks,ors");

			QUERY_UNEQ_OPERS.push_back(">,");
			QUERY_UNEQ_OPERS.push_back(">=,");
			QUERY_UNEQ_OPERS.push_back("<,");
			QUERY_UNEQ_OPERS.push_back("<=,");
			QUERY_UNEQ_OPERS.push_back("<>,");
			QUERY_UNEQ_OPERS.push_back("=,");

			sqlite3* handle;
			const int ret = sqlite3_open_v2(apFilename, &handle, aFlags, apVfs);
			mSQLitePtr.reset(handle);
			if (SQLITE_OK != ret)
			{
				string errmsg = "DB Error, code: ";
				errmsg.append(DbUtils::IntTransToString(ret)).append("; message: ");
				errmsg.append(sqlite3_errmsg(getHandle()));
				throw errmsg;
			}
			if (aBusyTimeoutMs > 0)
			{
				const int ret = sqlite3_busy_timeout(getHandle(), aBusyTimeoutMs);
				if (OK != ret)
				{
					string errmsg = "DB Error, code: ";
					errmsg.append(DbUtils::IntTransToString(ret)).append("; message: ");
					errmsg.append(sqlite3_errmsg(getHandle()));
					throw errmsg;
				}
			}
			DbLogClose = logFlag;
		};

		Sqlit3Db(const std::string& aFilename, bool logFlag = false,
			const int          aFlags = OPEN_READWRITE | OPEN_CREATE,
			const int          aBusyTimeoutMs = 0,
			const std::string& aVfs = "") {
			new (this)Sqlit3Db(aFilename.c_str(), logFlag, aFlags, aBusyTimeoutMs, aVfs.empty() ? nullptr : aVfs.c_str());
		};

		Json remove(string tablename, Json& params)
		{
			if (!params.isError()) {
				string execSql = "delete from ";
				execSql.append(tablename).append(" where id = ");

				Json v = params["id"];

				if (v.isString())
					execSql.append("'").append(v.toString()).append("'");
				else
					execSql.append(v.toString());

				return ExecNoneQuerySql(execSql);
			}
			else {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
		}

		Json update(string tablename, Json& params)
		{
			if (!params.isError()) {
				string execSql = "update ";
				execSql.append(tablename).append(" set ");

				vector<string> allKeys = params.getAllKeys();

				vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
				if (iter == allKeys.end()) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					size_t len = allKeys.size();
					size_t conditionLen = len - 2;
					string fields = "", where = " where id = ";
					for (size_t i = 0; i < len; i++) {
						string key = allKeys[i];
						Json v = params[key];
						if (key.compare("id") == 0) {
							conditionLen++;
							if (v.isString())
								where.append("'").append(v.toString()).append("'");
							else
								where.append(v.toString());
						}
						else {
							fields.append(key).append(" = ");
							if (v.isString())
								fields.append("'").append(v.toString()).append("'");
							else
								fields.append(v.toString());
							if (i < conditionLen) {
								fields.append(",");
							}
						}
					}
					execSql.append(fields).append(where);
					return ExecNoneQuerySql(execSql);
				}
			}
			else {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
		}

		Json create(string tablename, Json& params)
		{
			if (!params.isError()) {
				string execSql = "insert into ";
				execSql.append(tablename).append(" ");

				vector<string> allKeys = params.getAllKeys();
				size_t len = allKeys.size();
				string fields = "", vs = "";
				for (size_t i = 0; i < len; i++) {
					string key = allKeys[i];
					fields.append(key);
					Json v = params[key];
					if (v.isString())
						vs.append("'").append(v.toString()).append("'");
					else
						vs.append(v.toString());
					if (i < len - 1) {
						fields.append(",");
						vs.append(",");
					}
				}
				execSql.append("(").append(fields).append(") values (").append(vs).append(")");
				return ExecNoneQuerySql(execSql);
			}
			else {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
		}

		Json execSql(string sql, Json values = Json(JsonType::Array)) {
			return ExecNoneQuerySql(sql, values);
		}

		Json querySql(string sql, Json& values, Json& params, vector<string> fields = vector<string>()) {
			return select(sql, params, fields, values, 2);
		}

		Json insertBatch(string tablename, Json& elements, string constraint) {
			string sql = "insert into ";
			if (elements.size() < 1) {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
			else {
				string keyStr = " (";
				keyStr.append(DbUtils::GetVectorJoinStr(elements[0].getAllKeys())).append(" ) ");
				for (size_t i = 0; i < elements.size(); i++) {
					vector<string> keys = elements[i].getAllKeys();
					string valueStr = " select ";
					for (size_t j = 0; j < keys.size(); j++) {
						valueStr.append("'").append(elements[i][keys[j]].toString()).append("'");
						if (j < keys.size() - 1) {
							valueStr.append(",");
						}
					}
					if (i < elements.size() - 1) {
						valueStr.append(" union all ");
					}
					keyStr.append(valueStr);
				}
				sql.append(tablename).append(keyStr);
			}
			return ExecNoneQuerySql(sql);
		}

		Json transGo(Json& sqls, bool isAsync = false) {
			if (sqls.size() < 1) {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
			else {
				char* zErrMsg = 0;
				bool isExecSuccess = true;
				sqlite3_exec(getHandle(), "begin;", 0, 0, &zErrMsg);
				for (size_t i = 0; i < sqls.size(); i++) {
					string sql = sqls[i].toString();
					int rc = sqlite3_exec(getHandle(), sql.c_str(), 0, 0, &zErrMsg);
					if (rc != SQLITE_OK)
					{
						isExecSuccess = false;
						std::cout << "Transaction Fail, sql " << i + 1 << " is wrong. Error: " << zErrMsg << std::endl;
						sqlite3_free(zErrMsg);
						break;
					}
				}
				if (isExecSuccess)
				{
					sqlite3_exec(getHandle(), "commit;", 0, 0, 0);
					sqlite3_close(getHandle());
					!DbLogClose && std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
					return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success, run " + DbUtils::IntTransToString(sqls.size()) + " sqls.");
				}
				else
				{
					sqlite3_exec(getHandle(), "rollback;", 0, 0, 0);
					sqlite3_close(getHandle());
					return DbUtils::MakeJsonObject(STDBOPERATEERR, zErrMsg);
				}
			}
		}

		Json select(string tableOrSql, Json& params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array), int queryType = 1) {
			if (!params.isError()) {
				string querySql = "";
				string where = "";
				const string AndJoinStr = " and ";
				string fieldsJoinStr = "*";

				if (!fields.empty()) {
					fieldsJoinStr = DbUtils::GetVectorJoinStr(fields);
				}

				string fuzzy = params.getAndRemove("fuzzy").toString();
				string sort = params.getAndRemove("sort").toString();
				int page = params.getAndRemove("page").toInt();
				int size = params.getAndRemove("size").toInt();
				string sum = params.getAndRemove("sum").toString();
				string count = params.getAndRemove("count").toString();
				string group = params.getAndRemove("group").toString();

				vector<string> allKeys = params.getAllKeys();
				size_t len = allKeys.size();
				for (size_t i = 0; i < len; i++) {
					string k = allKeys[i];
					string v = params[k].toString();
					if (where.length() > 0) {
						where.append(AndJoinStr);
					}

					if (DbUtils::FindStringFromVector(QUERY_EXTRA_KEYS, k)) {   // process key
						string whereExtra = "";
						vector<string> ele = DbUtils::MakeVector(params[k].toString());
						if (ele.size() < 2 || ((k.compare("ors") == 0 || k.compare("lks") == 0) && ele.size() % 2 == 1)) {
							return DbUtils::MakeJsonObject(STPARAMERR, k + " is wrong.");
						}
						else {
							if (k.compare("ins") == 0) {
								string c = ele.at(0);
								vector<string>(ele.begin() + 1, ele.end()).swap(ele);
								whereExtra.append(c).append(" in ( ? )");
								values.addSubitem(DbUtils::GetVectorJoinStr(ele));
							}
							else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
								whereExtra.append(" ( ");
								for (size_t j = 0; j < ele.size(); j += 2) {
									if (j > 0) {
										whereExtra.append(" or ");
									}
									whereExtra.append(ele.at(j)).append(" ");
									string eqStr = k.compare("lks") == 0 ? " like ?" : " = ?";
									string vsStr = ele.at(j + 1);
									if (k.compare("lks") == 0) {
										vsStr.insert(0, "%");
										vsStr.append("%");
									}
									whereExtra.append(eqStr);
									values.addSubitem(vsStr);
								}
								whereExtra.append(" ) ");
							}
						}
						where.append(whereExtra);
					}
					else {				// process value
						if (DbUtils::FindStartsStringFromVector(QUERY_UNEQ_OPERS, v)) {
							vector<string> vls = DbUtils::MakeVector(v);
							if (vls.size() == 2) {
								where.append(k).append(vls.at(0)).append(" ? ");
								values.addSubitem(vls.at(1));
							}
							else if (vls.size() == 4) {
								where.append(k).append(vls.at(0)).append(" ? ").append("and ");
								where.append(k).append(vls.at(2)).append("? ");
								values.addSubitem(vls.at(1));
								values.addSubitem(vls.at(3));
							}
							else {
								return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
							}
						}
						else if (fuzzy == "1") {
							where.append(k).append(" like ? ");
							values.addSubitem(v.insert(0, "%").append("%"));
						}
						else {
								where.append(k).append(" = ? ");
								values.addSubitem(v);
						}
					}
				}

				string extra = "";
				if (!sum.empty()) {
					vector<string> ele = DbUtils::MakeVector(sum);
					if (ele.empty() || ele.size() % 2 == 1)
						return DbUtils::MakeJsonObject(STPARAMERR, "sum is wrong.");
					else {
						for (size_t i = 0; i < ele.size(); i += 2) {
							extra.append(",sum(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
						}
					}
				}
				if (!count.empty()) {
					vector<string> ele = DbUtils::MakeVector(count);
					if (ele.empty() || ele.size() % 2 == 1)
						return DbUtils::MakeJsonObject(STPARAMERR, "count is wrong.");
					else {
						for (size_t i = 0; i < ele.size(); i += 2) {
							extra.append(",count(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
						}
					}
				}

				if (queryType == 1) {
					querySql.append("select ").append(fieldsJoinStr).append(extra).append(" from ").append(tableOrSql);
					if (where.length() > 0)
						querySql.append(" where ").append(where);
				}
				else {
					querySql.append(tableOrSql);
					if (!fields.empty()) {
						size_t starIndex = querySql.find('*');
						if (starIndex < 10) {
							querySql.replace(starIndex, 1, fieldsJoinStr.c_str());
						}
					}
					if (where.length() > 0) {
						size_t whereIndex = querySql.find("where");
						if (whereIndex == querySql.npos) {
							querySql.append(" where ").append(where);
						}
						else {
							querySql.append(" and ").append(where);
						}
					}
				}

				if (!group.empty()) {
					querySql.append(" group by ").append(group);
				}

				if (!sort.empty()) {
					querySql.append(" order by ").append(sort);
				}

				if (page > 0) {
					page--;
					querySql.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
				}
				return ExecQuerySql(querySql, fields, values);
			}
			else {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}
		};

		sqlite3* getHandle()
		{
			return mSQLitePtr.get();
		}

	private:
		Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
			Json rs = DbUtils::MakeJsonObject(STSUCCESS);
			sqlite3_stmt* stmt = NULL;
			sqlite3* handle = getHandle();
			const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
			if (SQLITE_OK != ret)
			{
				string errmsg = sqlite3_errmsg(getHandle());
				rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
			}
			else {
				int insertPot = aQuery.find("where");
				if (insertPot == aQuery.npos) {
					insertPot = aQuery.find("limit");
					if (insertPot == aQuery.npos) {
						insertPot = aQuery.length();
					}
				}
				string aQueryLimit0 = aQuery.substr(0, insertPot).append(" limit 1");
				char** pRes = NULL;
				int nRow = 0, nCol = 0;
				char* pErr = NULL;
				sqlite3_get_table(handle, aQueryLimit0.c_str(), &pRes, &nRow, &nCol, &pErr);
				for (int j = 0; j < nCol; j++)
				{
					string fs = *(pRes + j);
					if (find(fields.begin(), fields.end(), fs) == fields.end()) {
						fields.push_back(fs);
					}
				}
				if (pErr != NULL)
				{
					sqlite3_free(pErr);
				}
				sqlite3_free_table(pRes);

				for(int i = 0; i < values.size(); i++){
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}

				vector<Json> arr;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					Json al;
					for (int j = 0; j < nCol; j++)
					{
						string k = fields.at(j);
						int nType = sqlite3_column_type(stmt, j);
						if (nType == 1) {					//SQLITE_INTEGER
							al.addSubitem(k, sqlite3_column_int(stmt, j));
						}
						else if (nType == 2) {				//SQLITE_FLOAT
							al.addSubitem(k, sqlite3_column_double(stmt, j));
						}
						else if (nType == 3) {				//SQLITE_TEXT
							al.addSubitem(k, (char*)sqlite3_column_text(stmt, j));
						}
						//else if (nType == 4) {				//SQLITE_BLOB

						//}
						//else if (nType == 5) {				//SQLITE_NULL

						//}
						else{
							al.addSubitem(k, "");
						}
					}
					arr.push_back(al);
				}
				if (arr.empty())
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.addSubitem("data", arr);
			}
			sqlite3_finalize(stmt);
			//if(!DbLogClose)
			!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
			return rs;
		}

		Json ExecNoneQuerySql(string aQuery, Json values = Json(JsonType::Array)) {
			int stepRet = SQLITE_OK;
			std::string errStr = "Error Code : ";
			Json rs = DbUtils::MakeJsonObject(STSUCCESS);
			sqlite3_stmt* stmt = NULL;
			sqlite3* handle = getHandle();
			const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
			if (SQLITE_OK != ret)
			{
				string errmsg = sqlite3_errmsg(getHandle());
				rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
			}
			else {
				for(int i = 0; i < values.size(); i++){
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				stepRet = sqlite3_step(stmt);
			}
			sqlite3_finalize(stmt);
			!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
			return stepRet == SQLITE_DONE ? rs : DbUtils::MakeJsonObject(STDBOPERATEERR, errStr.append(DbUtils::IntTransToString(stepRet)));
		}

		bool DbLogClose;

	};
}
