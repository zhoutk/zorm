﻿#pragma once

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
				if (!params.isError()) {
					Json values(JsonType::Array);
					string execSql = "insert into ";
					execSql.append(tablename).append(" ");

					vector<string> allKeys = params.getAllKeys();
					size_t len = allKeys.size();
					string fields = "", vs = "";
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						fields.append(k);
						bool vIsString = params[k].isString();
						string v = params[k].toString();
						!queryByParameter && vIsString &&escapeString(v);
						if(queryByParameter){
							vs.append("$").append(DbUtils::IntTransToString(i+1));
							vIsString ? values.addSubitem(v) : values.addSubitem(params[k].toDouble());
						}else{
							if (vIsString)
								vs.append("'").append(v).append("'");
							else
								vs.append(v);
						}
						
						if (i < len - 1) {
							fields.append(",");
							vs.append(",");
						}
					}
					execSql.append("(").append(fields).append(") values (").append(vs).append(")");
					return ExecNoneQuerySql(execSql, values);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
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
				string sql = "insert into ";
				vector<string> restrain = DbUtils::MakeVector(constraint);
				if (elements.size() < 2) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					Json values = Json(JsonType::Array);
					string keyStr = " ( ";
					string updateStr = "";
					keyStr.append(DbUtils::GetVectorJoinStr(elements[0].getAllKeys())).append(" ) values ");
					int index = 1;
					for (size_t i = 0; i < elements.size(); i++) {
						vector<string> keys = elements[i].getAllKeys();
						string valueStr = " ( ";
						for (size_t j = 0; j < keys.size(); j++) {
							if (i == 0) {
								vector<string>::iterator iter = find(restrain.begin(), restrain.end(), keys[j]);
								if (iter == restrain.end())
									updateStr.append(keys[j]).append(" = excluded.").append(keys[j]).append(",");
							}
							bool vIsString = elements[i][keys[j]].isString();
							string v = elements[i][keys[j]].toString();
							!queryByParameter && vIsString && escapeString(v);
							if(queryByParameter){
								valueStr.append("$").append(DbUtils::IntTransToString(index++));
								values.addSubitem(v);
							}else{
								if(vIsString)
									valueStr.append("'").append(v).append("'");
								else
									valueStr.append(v);
							}
							if (j < keys.size() - 1) {
								valueStr.append(",");
							}
						}
						valueStr.append(" )");
						if (i < elements.size() - 1) {
							valueStr.append(",");
						}
						keyStr.append(valueStr);
					}
					if (updateStr.length() == 0) {
						sql.append(tablename).append(keyStr);
					}
					else
					{
						updateStr = updateStr.substr(0, updateStr.length() - 1);
						sql.append(tablename).append(keyStr).append(" on conflict (").append(constraint).append(") do update set ").append(updateStr);
					}
					return ExecNoneQuerySql(sql, values);
				}
			}

			Json transGo(Json& sqls, bool isAsync = false) override
			{
				if (sqls.size() < 2) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					bool isExecSuccess = true;
					string errmsg = "Running transaction error: ";
					char* err = nullptr;
					pqxx::connection* pq = GetConnection(err);
					if (pq == nullptr)
						return DbUtils::MakeJsonObject(STDBCONNECTERR, err);

					try {
						pqxx::work T(*pq);
						for (size_t i = 0; i < sqls.size(); i++) {
							string sql = sqls[i]["text"].toString();
							Json values = sqls[i]["values"].isError() ? Json(JsonType::Array) : sqls[i]["values"];
							pqxx::params ps;
							int len = values.size();
							for (int i = 0; i < len; i++)
								ps.append(values[i].toString());
							T.exec_params(sql, ps);
						}
						T.commit();
						std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
						return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success.");
					}
					catch (std::exception& e) {
						std::cout << "Transaction Error: " << e.what() << std::endl;
						return DbUtils::MakeJsonObject(STDBOPERATEERR, (char*)e.what());
					}
				}
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

			Json ExecNoneQuerySql(string aQuery, Json values = Json(JsonType::Array)) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				char* err;
				pqxx::connection *pq = GetConnection(err);
				if (pq == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				pqxx::params ps;
				int len = values.size();
				for(int i = 0; i < len; i++){
					ps.append(values[i].toString());
				}
				try
				{
					pqxx::nontransaction N(*pq);
					pqxx::result R(N.exec_params(aQuery, ps));

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

			bool ExecSqlForTransGo(string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
				return true;
			}

			bool escapeString(string& pStr)
			{
				pStr = GetConnection()->esc(pStr);
				return true;
			}

			std::string getEscapeString(string& pStr)
			{
				return GetConnection()->esc(pStr);
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