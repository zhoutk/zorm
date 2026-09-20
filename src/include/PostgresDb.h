#pragma once

// Postgres backend - dialect + driver only. Shared algorithm layer:
// SqlBackendBase.h; connections: DbPool::HandlePool with exclusive RAII
// leases. Placeholders are $n (numbered across the whole statement).

#include "Idb.h"
#include "DbUtils.h"
#include "GlobalConstants.h"
#include "SqlBackendBase.h"
#include "DbPool.h"
#include "pg_type_d.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libpq-fe.h>

namespace ZORM {

	using std::string;

	namespace Postgres {

		class ZORM_API PostgresDb : public SqlBackendBase<PostgresDb, PGconn*> {

		public:
			PostgresDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 5432, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport),
				pool(
					[this](string& err) -> PGconn* { return this->connect(err); },
					[](PGconn* c) { PQfinish(c); },
					2) {
				init();
				if (!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
					maxConn = options["db_conn"].toInt();
				if (!options["DbLogClose"].isError())
					DbLogClose = options["DbLogClose"].toBool();
				if (!options["parameterized"].isError())
					queryByParameter = options["parameterized"].toBool();
				pool.setMaxConn(maxConn);
			}

			// ---------------- CRTP hooks: dialect ----------------

			using Handle = PGconn*;
			using Lease = DbPool::HandlePool<Handle>::Lease;

			Lease acquireHandle(string& err) {
				return pool.acquire(err);
			}

			std::string placeholder(int index) {
				return "$" + DbUtils::IntTransToString(index);
			}
			bool numberedPlaceholders() {
				return true;
			}
			std::string quoteIdent(const std::string& name) {
				return name;
			}
			std::string qualifiedTable(const std::string& name) {
				return name;
			}
			// postgres compares text contextually; CAST keeps LIKE / = working
			// on non-text columns for lks / ors / fuzzy.
			std::string likeColumn(const std::string& name) {
				return "CAST(" + name + " as TEXT)";
			}
			std::string orderClause(const std::string& sort) {
				return sort;
			}
			std::string limitClause(int offset, int size) {
				return " limit " + DbUtils::IntTransToString(size) + " OFFSET " + DbUtils::IntTransToString(offset);
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
				return sql.find("$") != std::string::npos;
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

			// ---------------- CRTP hooks: driver ----------------

			bool escapeString(string& pStr) {
				(void)pStr;
				return true;  // values ride through PQexecParams binding
			}

			Json execQueryOn(Handle pq, const string& aQuery, const vector<string>& fields, Json& values) {
				(void)fields;
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				const int vLen = values.size();
				std::vector<char*> dataInputs(vLen);
				for (int i = 0; i < vLen; i++) {
					string ele = values[i].toString();
					const int eleLen = static_cast<int>(ele.length()) + 1;
					dataInputs[i] = new char[eleLen];
					std::memset(dataInputs[i], 0, eleLen);
					std::memcpy(dataInputs[i], ele.c_str(), eleLen);
				}
				PGresult* res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr,
											 vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr, 0);
				for (auto el : dataInputs)
					delete[] el;
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_TUPLES_OK) {
					std::cout << PQerrorMessage(pq) << std::endl;
					rs = DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(pq));
					PQclear(res);
					return rs;
				}
				const int coLen = PQnfields(res);
				Json arr(JsonType::Array);
				for (int i = 0; i < PQntuples(res); i++) {
					Json al;
					for (int j = 0; j < coLen; j++) {
						// NULL-aware decoding: SQL NULL becomes a real JSON null
						// (parity with the jsonfile backend); numeric types
						// become numbers, the rest strings.
						if (PQgetisnull(res, i, j)) {
							al.add(PQfname(res, j), nullptr);
							continue;
						}
						auto rsType = PQftype(res, j);
						switch (rsType) {
						case INT2OID:
						case INT4OID:
						case INT8OID:
						case NUMERICOID:
							al.add(PQfname(res, j), atof(PQgetvalue(res, i, j)));
							break;
						default:
							al.add(PQfname(res, j), PQgetvalue(res, i, j));
							break;
						}
					}
					arr.push_back(al);
				}
				if (arr.size() == 0)
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				PQclear(res);
				return rs;
			}

			Json execNoneOn(Handle pq, const string& aQuery, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				const int vLen = values.size();
				std::vector<char*> dataInputs(vLen);
				for (int i = 0; i < vLen; i++) {
					string ele = values[i].toString();
					const int eleLen = static_cast<int>(ele.length()) + 1;
					dataInputs[i] = new char[eleLen];
					std::memset(dataInputs[i], 0, eleLen);
					std::memcpy(dataInputs[i], ele.c_str(), eleLen);
				}
				PGresult* res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr,
											 vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr, 0);
				for (auto el : dataInputs)
					delete[] el;
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					std::cout << PQerrorMessage(pq) << std::endl;
					rs = DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(pq));
				} else {
					const char* tuples = PQcmdTuples(res);
					if (tuples != nullptr && *tuples != '\0')
						rs.add("affected", static_cast<long long>(std::atoll(tuples)));
				}
				PQclear(res);
				return rs;
			}

			bool execTxOn(Handle pq, const string& aQuery, Json& values, string* out) {
				const int vLen = values.size();
				std::vector<char*> dataInputs(vLen);
				for (int i = 0; i < vLen; i++) {
					string ele = values[i].toString();
					const int eleLen = static_cast<int>(ele.length()) + 1;
					dataInputs[i] = new char[eleLen];
					std::memset(dataInputs[i], 0, eleLen);
					std::memcpy(dataInputs[i], ele.c_str(), eleLen);
				}
				PGresult* res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr,
											 vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr, 0);
				for (auto el : dataInputs)
					delete[] el;
				if (!DbLogClose)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					const string err = PQerrorMessage(pq);
					std::cout << err << std::endl;
					if (out)
						*out = err;
					PQclear(res);
					return false;
				}
				PQclear(res);
				return true;
			}

			bool beginTx(Handle pq) {
				Json empty(JsonType::Array);
				return execTxOn(pq, "BEGIN TRANSACTION;", empty, nullptr);
			}
			bool commitTx(Handle pq) {
				Json empty(JsonType::Array);
				return execTxOn(pq, "COMMIT;", empty, nullptr);
			}
			void rollbackTx(Handle pq) {
				Json empty(JsonType::Array);
				execTxOn(pq, "ROLLBACK;", empty, nullptr);
			}

		private:
			PGconn* connect(string& err) {
				PGconn* pqsql = PQconnectdb(connString.c_str());
				if (PQstatus(pqsql) == CONNECTION_OK)
					return pqsql;
				err = string(PQerrorMessage(pqsql));
				std::cout << "Error message : " << err;
				PQfinish(pqsql);
				return nullptr;
			}

			void init() {
				connString = "dbname=" + dbname + " user=" + dbuser + " password=" + dbpwd +
							 " hostaddr=" + dbhost + " port=" + DbUtils::IntTransToString(dbport);
			}

		private:
			DbPool::HandlePool<PGconn*> pool;
			int maxConn = 2;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string connString;
		};

	}

}
