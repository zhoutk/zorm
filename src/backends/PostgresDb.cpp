#include "PostgresDb.h"

#include "pg_type_d.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libpq-fe.h>

namespace ZORM {

	namespace Postgres {

		namespace {

			// libpq's execParams wants one NUL-terminated C string per
			// parameter; shared by every PQexecParams call site.
			std::vector<char*> valuesToCStrings(Json& values) {
				const int vLen = values.size();
				std::vector<char*> dataInputs(vLen);
				for (int i = 0; i < vLen; i++) {
					string ele = values[i].toString();
					const int eleLen = static_cast<int>(ele.length()) + 1;
					dataInputs[i] = new char[eleLen];
					std::memset(dataInputs[i], 0, eleLen);
					std::memcpy(dataInputs[i], ele.c_str(), eleLen);
				}
				return dataInputs;
			}

			void freeCStrings(std::vector<char*>& dataInputs) {
				for (auto el : dataInputs)
					delete[] el;
			}

		}  // namespace

		// ─────────────────────────────────────────────────────────────────
		// Connection: IDbConnection around a raw PGconn*
		// ─────────────────────────────────────────────────────────────────

		class PostgresDb::Connection final : public IDbConnection {
		public:
			Connection(PGconn* handle, bool logClose) : h_(handle), logClose_(logClose) {}
			~Connection() override {
				PQfinish(h_);
			}

			Json execQuery(const string& aQuery, const vector<string>& fields, Json& values) override {
				(void)fields;
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				std::vector<char*> dataInputs = valuesToCStrings(values);
				PGresult* res = PQexecParams(h_, aQuery.c_str(), static_cast<int>(dataInputs.size()), nullptr,
											 dataInputs.empty() ? nullptr : dataInputs.data(), nullptr, nullptr, 0);
				freeCStrings(dataInputs);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_TUPLES_OK) {
					std::cout << PQerrorMessage(h_) << std::endl;
					rs = DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(h_));
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
							// Json::str: JSON-looking text stays text.
							al.add(PQfname(res, j), Json::str(PQgetvalue(res, i, j)));
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

			Json execNone(const string& aQuery, Json& values) override {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				std::vector<char*> dataInputs = valuesToCStrings(values);
				PGresult* res = PQexecParams(h_, aQuery.c_str(), static_cast<int>(dataInputs.size()), nullptr,
											 dataInputs.empty() ? nullptr : dataInputs.data(), nullptr, nullptr, 0);
				freeCStrings(dataInputs);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					std::cout << PQerrorMessage(h_) << std::endl;
					rs = DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(h_));
				} else {
					const char* tuples = PQcmdTuples(res);
					if (tuples != nullptr && *tuples != '\0')
						rs.add("affected", static_cast<long long>(std::atoll(tuples)));
				}
				PQclear(res);
				return rs;
			}

			bool execTx(const string& aQuery, Json& values, string* out) override {
				std::vector<char*> dataInputs = valuesToCStrings(values);
				PGresult* res = PQexecParams(h_, aQuery.c_str(), static_cast<int>(dataInputs.size()), nullptr,
											 dataInputs.empty() ? nullptr : dataInputs.data(), nullptr, nullptr, 0);
				freeCStrings(dataInputs);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					const string err = PQerrorMessage(h_);
					std::cout << err << std::endl;
					if (out)
						*out = err;
					PQclear(res);
					return false;
				}
				PQclear(res);
				return true;
			}

			bool beginTx() override {
				Json empty(JsonType::Array);
				return execTx("BEGIN TRANSACTION;", empty, nullptr);
			}
			bool commitTx() override {
				Json empty(JsonType::Array);
				return execTx("COMMIT;", empty, nullptr);
			}
			void rollbackTx() override {
				Json empty(JsonType::Array);
				execTx("ROLLBACK;", empty, nullptr);
			}

		private:
			PGconn* h_;
			bool logClose_;
		};

		// ─────────────────────────────────────────────────────────────────
		// PostgresDb
		// ─────────────────────────────────────────────────────────────────

		PostgresDb::PostgresDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport, Json options) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport),
			pool(
				[this](string& err) -> IDbConnection* { return this->newConnection(err); },
				[](IDbConnection* c) { delete c; },
				2) {
			connString = "dbname=" + dbname + " user=" + dbuser + " password=" + dbpwd +
						 " hostaddr=" + dbhost + " port=" + DbUtils::IntTransToString(dbport);
			if (!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
				maxConn = options["db_conn"].toInt();
			if (!options["DbLogClose"].isError())
				DbLogClose = options["DbLogClose"].toBool();
			if (!options["parameterized"].isError())
				queryByParameter = options["parameterized"].toBool();
			pool.setMaxConn(maxConn);
		}

		PostgresDb::~PostgresDb() = default;

		SqlBackendBase::Lease PostgresDb::acquireConnection(string& err) {
			return pool.acquire(err);
		}

		std::string PostgresDb::placeholder(int index) {
			return "$" + DbUtils::IntTransToString(index);
		}
		bool PostgresDb::numberedPlaceholders() {
			return true;
		}
		// postgres compares text contextually; CAST keeps LIKE / = working
		// on non-text columns for lks / ors / fuzzy.
		std::string PostgresDb::likeColumn(const std::string& name) {
			return "CAST(" + name + " as TEXT)";
		}
		std::string PostgresDb::limitClause(int offset, int size) {
			return " limit " + DbUtils::IntTransToString(size) + " OFFSET " + DbUtils::IntTransToString(offset);
		}
		bool PostgresDb::detectParameterized(const std::string& sql) {
			return sql.find("$") != std::string::npos;
		}

		IDbConnection* PostgresDb::newConnection(string& err) {
			PGconn* pqsql = PQconnectdb(connString.c_str());
			if (PQstatus(pqsql) == CONNECTION_OK)
				return new Connection(pqsql, DbLogClose);
			err = string(PQerrorMessage(pqsql));
			std::cout << "Error message : " << err;
			PQfinish(pqsql);
			return nullptr;
		}

	}

}
