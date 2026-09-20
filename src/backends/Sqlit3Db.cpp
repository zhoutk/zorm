#include "Sqlit3Db.h"

#include <assert.h>
#include <iostream>

namespace ZORM {

	namespace Sqlit3 {

		// ─────────────────────────────────────────────────────────────────
		// Connection: IDbConnection around a raw sqlite3*
		// ─────────────────────────────────────────────────────────────────

		class Sqlit3Db::Connection final : public IDbConnection {
		public:
			Connection(sqlite3* handle, bool logClose) : h_(handle), logClose_(logClose) {}
			~Connection() override {
				sqlite3_close(h_);
			}

			Json execQuery(const string& aQuery, const vector<string>& fields, Json& values) override {
				(void)fields;
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(h_, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					string errmsg = sqlite3_errmsg(h_);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					sqlite3_finalize(stmt);
					if (!logClose_)
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
				sqlite3_get_table(h_, aQueryLimit0.c_str(), &pRes, &nRow, &nCol, &pErr);
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
							// Json::str: text that looks like JSON ([1,2], {...})
							// is data here, not a document to parse.
							al.add(k, Json::str((char*)sqlite3_column_text(stmt, j)));
						else  // BLOB / SQLITE_NULL -> empty (this backend's NULL rendering)
							al.add(k, "");
					}
					arr.push_back(al);
				}
				if (arr.size() == 0)
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				sqlite3_finalize(stmt);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json execNone(const string& aQuery, Json& values) override {
				int stepRet = SQLITE_OK;
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(h_, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					string errmsg = sqlite3_errmsg(h_);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					sqlite3_finalize(stmt);
					if (!logClose_)
						std::cout << "SQL: " << aQuery << std::endl;
					return rs;
				}
				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				stepRet = sqlite3_step(stmt);
				if (stepRet == SQLITE_DONE)
					rs.add("affected", static_cast<long long>(sqlite3_changes64(h_)));
				sqlite3_finalize(stmt);
				if (stepRet != SQLITE_DONE)
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, "Error Code : " + DbUtils::IntTransToString(stepRet)));
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			bool execTx(const string& aQuery, Json& values, string* out) override {
				int stepRet = SQLITE_OK;
				sqlite3_stmt* stmt = NULL;
				const int ret = sqlite3_prepare_v2(h_, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					if (out)
						*out += sqlite3_errmsg(h_);
					sqlite3_finalize(stmt);
					return false;
				}
				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				stepRet = sqlite3_step(stmt);
				sqlite3_finalize(stmt);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return stepRet == SQLITE_DONE;
			}

			bool beginTx() override {
				return sqlite3_exec(h_, "begin;", 0, 0, 0) == SQLITE_OK;
			}
			bool commitTx() override {
				return sqlite3_exec(h_, "commit;", 0, 0, 0) == SQLITE_OK;
			}
			void rollbackTx() override {
				sqlite3_exec(h_, "rollback;", 0, 0, 0);
			}

		private:
			sqlite3* h_;
			bool logClose_;
		};

		// ─────────────────────────────────────────────────────────────────
		// Sqlit3Db
		// ─────────────────────────────────────────────────────────────────

		Sqlit3Db::Sqlit3Db(const char* apFilename, bool logFlag, bool parameterized,
			const int aFlags, const int aBusyTimeoutMs, const char* apVfs)
			: pool(
				[this](string& err) -> IDbConnection* {
					sqlite3* handle = nullptr;
					const int ret = sqlite3_open_v2(mFilename.c_str(), &handle, mFlags,
													mVfs.empty() ? nullptr : mVfs.c_str());
					if (SQLITE_OK != ret) {
						(void)err;
						if (handle != nullptr)
							sqlite3_close(handle);
						return nullptr;
					}
					return new Connection(handle, DbLogClose);
				},
				[](IDbConnection* c) { delete c; },
				1),
			mFilename(apFilename),
			mFlags(aFlags),
			mVfs(apVfs == nullptr ? "" : apVfs) {
			(void)aBusyTimeoutMs;
			DbLogClose = logFlag;
			queryByParameter = parameterized;
		}

		Sqlit3Db::Sqlit3Db(const std::string& aFilename, bool logFlag, bool parameterized,
			const int aFlags, const int aBusyTimeoutMs, const std::string& aVfs)
			: Sqlit3Db(aFilename.c_str(), logFlag, parameterized, aFlags, aBusyTimeoutMs,
					   aVfs.empty() ? nullptr : aVfs.c_str()) {}

		Sqlit3Db::~Sqlit3Db() = default;

		SqlBackendBase::Lease Sqlit3Db::acquireConnection(string& err) {
			return pool.acquire(err);
		}

	}

}
