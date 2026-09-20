#pragma once

// Sqlite3 backend - dialect + driver only. Shared algorithm layer:
// SqlBackendBase.h; connection: DbPool::HandlePool (a single exclusive lease,
// which also serializes concurrent access to the one file handle).

#include <assert.h>
#include "Idb.h"
#include "sqlite3.h"
#include "DbUtils.h"
#include "GlobalConstants.h"
#include "SqlBackendBase.h"
#include "DbPool.h"
#include <regex>
#include <algorithm>
#include <cmath>

namespace ZORM {

	using std::string;
	using std::vector;

	namespace Sqlit3 {

#define SQLITECPP_ASSERT(expression, message)   assert(expression && message)

		const int   OPEN_READONLY = SQLITE_OPEN_READONLY;
		const int   OPEN_READWRITE = SQLITE_OPEN_READWRITE;
		const int   OPEN_CREATE = SQLITE_OPEN_CREATE;
		const int   OPEN_URI = SQLITE_OPEN_URI;

		const int   OK = SQLITE_OK;

		class ZORM_API Sqlit3Db : public SqlBackendBase<Sqlit3Db, sqlite3*> {

		public:
			Sqlit3Db(const char* apFilename, bool logFlag = false, bool parameterized = false,
				const int   aFlags = OPEN_READWRITE | OPEN_CREATE,
				const int   aBusyTimeoutMs = 0,
				const char* apVfs = nullptr)
				: pool(
					[this, aFlags, apVfs](string& err) -> sqlite3* { return this->connect(err, aFlags, apVfs); },
					[](sqlite3* h) { sqlite3_close(h); },
					1),
				mFilename(apFilename) {
				(void)aBusyTimeoutMs;
				DbLogClose = logFlag;
				queryByParameter = parameterized;
			}

			Sqlit3Db(const std::string& aFilename, bool logFlag = false, bool parameterized = false,
				const int          aFlags = OPEN_READWRITE | OPEN_CREATE,
				const int          aBusyTimeoutMs = 0,
				const std::string& aVfs = "")
				: Sqlit3Db(aFilename.c_str(), logFlag, parameterized, aFlags, aBusyTimeoutMs,
						   aVfs.empty() ? nullptr : aVfs.c_str()) {}

			// ─────────────────────────────────────────────────────────────────
			// CRTP hooks: dialect
			// ─────────────────────────────────────────────────────────────────

			using Handle = sqlite3*;
			using Lease = DbPool::HandlePool<Handle>::Lease;

			Lease acquireHandle(string& err) {
				return pool.acquire(err);
			}

			std::string placeholder(int) {
				return "?";
			}
			bool numberedPlaceholders() {
				return false;
			}
			std::string quoteIdent(const std::string& name) {
				return name;
			}
			std::string qualifiedTable(const std::string& name) {
				return name;
			}
			std::string likeColumn(const std::string& name) {
				return name;
			}
			std::string orderClause(const std::string& sort) {
				return sort;
			}
			std::string limitClause(int offset, int size) {
				return " limit " + DbUtils::IntTransToString(offset) + "," + DbUtils::IntTransToString(size);
			}
			std::string aggColumn(const std::string& src) {
				return src;
			}
			std::string aggAlias(const std::string& alias) {
				return alias;
			}
			std::string countAliasSql() {
				return countAlias_;
			}
			std::string columnList(const std::vector<std::string>& keys) {
				return DbUtils::GetVectorJoinStr(keys);
			}
			std::string fieldsProjection(const vector<string>& fields) {
				return DbUtils::GetVectorJoinStr(fields);
			}
			std::string excludedRefImpl(const std::string& column) {
				return "excluded." + column;
			}
			bool detectParameterized(const std::string& sql) {
				return sql.find("?") != std::string::npos;
			}

			// Upsert parity (O-6): duplicate ids update the row.
			std::string upsertClause(const std::string& constraint, const std::vector<std::string>& keys) {
				std::string clause;
				for (const std::string& k : keys) {
					if (k == constraint)
						continue;
					if (!clause.empty())
						clause += ",";
					clause += k + " = excluded." + k;
				}
				return clause.empty() ? "" : " on conflict (" + constraint + ") do update set " + clause;
			}

			// ─────────────────────────────────────────────────────────────────
			// CRTP hooks: driver
			// ─────────────────────────────────────────────────────────────────

			bool escapeString(string& pStr) {
				pStr = std::regex_replace(pStr, std::regex("'"), "''");
				return true;
			}

			Json execQueryOn(Handle handle, const string& aQuery, const vector<string>& fields, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					string errmsg = sqlite3_errmsg(handle);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					sqlite3_finalize(stmt);
					if (!DbLogClose)
						std::cout << "SQL: " << aQuery << std::endl;
					return rs;
				}
				// Column-discovery probe: run the statement without the final
				// limit/where tail once so the column names are known even for
				// empty result sets.
				int insertPot = aQuery.find("where");
				if (insertPot == aQuery.npos) {
					insertPot = aQuery.find("limit");
					if (insertPot == aQuery.npos)
						insertPot = static_cast<int>(aQuery.length());
				}
				string aQueryLimit0 = aQuery.substr(0, insertPot).append(" limit 1");
				char** pRes = NULL;
				int nRow = 0, nCol = 0;
				char* pErr = NULL;
				sqlite3_get_table(handle, aQueryLimit0.c_str(), &pRes, &nRow, &nCol, &pErr);
				// The probe's column names ARE the result columns (fields are
				// already baked into the SQL by genSql).
				vector<string> cols;
				for (int j = 0; j < nCol; j++)
					cols.push_back(*(pRes + j));
				if (pErr != NULL)
					sqlite3_free(pErr);
				sqlite3_free_table(pRes);

				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}

				Json arr(JsonType::Array);
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					Json al;
					for (int j = 0; j < nCol; j++) {
						string k = cols.at(j);
						const int nType = sqlite3_column_type(stmt, j);
						if (nType == 1)  // SQLITE_INTEGER
							al.add(k, sqlite3_column_int(stmt, j));
						else if (nType == 2)  // SQLITE_FLOAT
							al.add(k, sqlite3_column_double(stmt, j));
						else if (nType == 3)  // SQLITE_TEXT
							al.add(k, (char*)sqlite3_column_text(stmt, j));
						else  // BLOB / SQLITE_NULL -> empty (this backend's NULL rendering)
							al.add(k, "");
					}
					arr.push_back(al);
				}
				if (arr.size() == 0)
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				sqlite3_finalize(stmt);
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json execNoneOn(Handle handle, const string& aQuery, Json& values) {
				int stepRet = SQLITE_OK;
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					string errmsg = sqlite3_errmsg(handle);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					sqlite3_finalize(stmt);
					if (!DbLogClose)
						std::cout << "SQL: " << aQuery << std::endl;
					return rs;
				}
				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				stepRet = sqlite3_step(stmt);
				if (stepRet == SQLITE_DONE)
					rs.add("affected", static_cast<long long>(sqlite3_changes64(handle)));
				sqlite3_finalize(stmt);
				if (stepRet != SQLITE_DONE)
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, "Error Code : " + DbUtils::IntTransToString(stepRet)));
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			bool execTxOn(Handle handle, const string& aQuery, Json& values, string* out) {
				int stepRet = SQLITE_OK;
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					if (out)
						*out += sqlite3_errmsg(handle);
					sqlite3_finalize(stmt);
					return false;
				}
				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				stepRet = sqlite3_step(stmt);
				sqlite3_finalize(stmt);
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				return stepRet == SQLITE_DONE;
			}

			bool beginTx(Handle handle) {
				return sqlite3_exec(handle, "begin;", 0, 0, 0) == SQLITE_OK;
			}
			bool commitTx(Handle handle) {
				return sqlite3_exec(handle, "commit;", 0, 0, 0) == SQLITE_OK;
			}
			void rollbackTx(Handle handle) {
				sqlite3_exec(handle, "rollback;", 0, 0, 0);
			}

		private:
			sqlite3* connect(string& err, const int aFlags, const char* apVfs) {
				(void)err;
				sqlite3* handle = nullptr;
				const int ret = sqlite3_open_v2(mFilename.c_str(), &handle, aFlags, apVfs);
				if (SQLITE_OK != ret) {
					if (handle != nullptr)
						sqlite3_close(handle);
					return nullptr;
				}
				return handle;
			}

		private:
			DbPool::HandlePool<sqlite3*> pool;
			std::string mFilename;
		};

	}

}
