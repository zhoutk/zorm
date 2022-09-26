#pragma once

#include "Idb.h"
#include "Utils.h"
#include "GlobalConstants.h"
 #include <pqxx/pqxx>
#include <algorithm>
#include <cstring>

namespace ZORM {

	using std::string;

	namespace Postgres {
		vector<string> QUERY_EXTRA_KEYS;
		vector<string> QUERY_UNEQ_OPERS;

		class ZORM_API PostgresDb : public Idb
		{
		private:
			pqxx::connection* GetConnection(char* err = nullptr)
			{
				size_t index = (rand() % maxConn) + 1;
				if (index > pool.size())
				{
					try
					{
						pqxx::connection *pqsql = new pqxx::connection(connString);
						if (pqsql->is_open())
						{
							pool.push_back(pqsql);
							return pqsql;
						}
						else
							return nullptr;
					}
					catch (const std::exception &e)
					{
						err = (char*)e.what();
						std::cout << "Error message : " << err;
						return nullptr;
					}
				}
				else
				{
					return pool.at(index - 1);
				}
			}

			void init(){
				QUERY_EXTRA_KEYS = DbUtils::MakeVector("ins,lks,ors");

				QUERY_UNEQ_OPERS.push_back(">,");
				QUERY_UNEQ_OPERS.push_back(">=,");
				QUERY_UNEQ_OPERS.push_back("<,");
				QUERY_UNEQ_OPERS.push_back("<=,");
				QUERY_UNEQ_OPERS.push_back("<>,");
				QUERY_UNEQ_OPERS.push_back("=,");

				connString = "dbname=" + dbname + " user=" + dbuser + " password=" + dbpwd + " hostaddr=" + dbhost + " port=" + DbUtils::IntTransToString(dbport);
			}

		public:

			PostgresDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 5432, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), maxConn(2), DbLogClose(false), queryByParameter(false)
			{
				init();

				if(!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
				if (!options["DbLogClose"].isError())
					DbLogClose = options["DbLogClose"].toBool();
				if(!options["parameterized"].isError())
					queryByParameter = options["parameterized"].toBool();
			}

			Json create(string tablename, Json& params) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json update(string tablename, Json& params) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}


			Json remove(string tablename, Json& params) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}


			Json select(string tablename, Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}


			Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				return ExecNoneQuerySql(sql);
			}


			Json insertBatch(string tablename, Json& elements, string constraint) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json transGo(Json& sqls, bool isAsync = false) override
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			~PostgresDb()
			{
				while (pool.size())
				{
					(pool.back())->close();
					pool.pop_back();
				}
			}

		private:
			Json genSql(string& querySql, Json& values, Json& params, vector<string> fields = vector<string>(), int queryType = 1, bool parameterized = false)
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json ExecQuerySql(string aQuery, vector<string> fields)
			{
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			Json ExecNoneQuerySql(string aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				char* err;
				pqxx::connection *pq = GetConnection(err);
				if (pq == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				try
				{
					pqxx::nontransaction N(*pq);
					pqxx::result R(N.exec(aQuery));

					rs.addSubitem("affected", R.affected_rows());
					rs.addSubitem("newId", R.inserted_oid());
				}
				catch (const std::exception &e)
				{
					std::cout << e.what();
					return DbUtils::MakeJsonObject(STDBOPERATEERR, e.what());
				}
				std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery, Json values) {
				return DbUtils::MakeJsonObject(STPARAMERR);
			}

			bool ExecSqlForTransGo(string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
				return true;
			}

			bool escapeString(string& pStr)
			{
				return true;
			}

		private:
			std::vector<pqxx::connection*> pool;
			int maxConn;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string charsetName;
			bool DbLogClose;
			bool queryByParameter;
			string connString;
		};
	}

}