﻿#pragma once

#include "Idb.h"
#include "Utils.h"
#include "GlobalConstants.h"
#include <pqxx/pqxx>
#include <algorithm>
#include <cstring>
#include "pg_type_d.h"

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
				if (!params.isError()) {
					Json values(JsonType::Array);
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
						int index = 1;
						Json idJson;
						for (size_t i = 0; i < len; i++) {
							string k = allKeys[i];
							bool vIsString = params[k].isString();
							string v = params[k].toString();
							!queryByParameter && vIsString &&escapeString(v);
							if (k.compare("id") == 0) {
								conditionLen++;
								if(queryByParameter){
									//where.append(" ? ");
									idJson = params[k];
								}else{
									if (vIsString)
										where.append("'").append(v).append("'");
									else
										where.append(v);
								}
							}
							else {
								fields.append(k).append(" = ");
								if (queryByParameter)
								{
									fields.append(" $").append(DbUtils::IntTransToString(index++));
									vIsString ? values.addSubitem(v) : values.addSubitem(params[k].toDouble());
								}
								else
								{
									if (vIsString)
										fields.append("'").append(v).append("'");
									else
										fields.append(v);
								}
								if (i < conditionLen) {
									fields.append(",");
								}
							}
						}
						if(queryByParameter){
							where.append(" $").append(DbUtils::IntTransToString(index++));
							values.concat(idJson);
						}
						execSql.append(fields).append(where);
						return ExecNoneQuerySql(execSql, values);
					}
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}


			Json remove(string tablename, Json& params) override
			{
				if (!params.isError()) {
					Json values(JsonType::Array);
					string execSql = "delete from ";
					execSql.append(tablename).append(" where id = ");

					string k = "id";
					bool vIsString = params[k].isString();
					string v = params[k].toString();
					!queryByParameter && vIsString &&escapeString(v);
					if(queryByParameter){
						execSql.append(" $1 ");
						vIsString ? values.addSubitem(v) : values.addSubitem(params[k].toDouble());
					}else{
						if (vIsString)
							execSql.append("'").append(v).append("'");
						else
							execSql.append(v);
					}
					return ExecNoneQuerySql(execSql, values);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}


			Json select(string tablename, Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override
			{
				Json rs = genSql(tablename, values, params, fields, 1, queryByParameter);
				if(rs["status"].toInt() == 200)
					return ExecQuerySql(tablename, fields, values);
				else
					return rs;
			}

			Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, fields, 2, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecQuerySql(sql, fields, values);
				else
					return rs;
			}

			Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecNoneQuerySql(sql, values);
				else
					return rs;
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
				if (!params.isError()) {
					string tablename = querySql;
					querySql = "";
					string where = "";
					const string AndJoinStr = " and ";
					string fieldsJoinStr = "*";

					if (!fields.empty()) {
						fieldsJoinStr = DbUtils::GetVectorJoinStr(fields);
					}

					string fuzzy = params.getAndRemove("fuzzy").toString();
					string sort = params.getAndRemove("sort").toString();
					int page = atoi(params.getAndRemove("page").toString().c_str());
					int size = atoi(params.getAndRemove("size").toString().c_str());
					string sum = params.getAndRemove("sum").toString();
					string count = params.getAndRemove("count").toString();
					string group = params.getAndRemove("group").toString();

					vector<string> allKeys = params.getAllKeys();
					size_t len = allKeys.size();
					int index = 1;
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						bool vIsString = params[k].isString();
						string v = params[k].toString();
						!parameterized && vIsString && escapeString(v);
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
									if(parameterized){
										whereExtra.append(c).append(" in (");
										int eleLen = ele.size();
										for (int i = 0; i < eleLen; i++)
										{
											string el = ele[i];
											whereExtra.append("$").append(DbUtils::IntTransToString(index++));
											if (i < eleLen - 1)
												whereExtra.append(",");
											values.addSubitem(el);
										}
										whereExtra.append(")");
									}else
										whereExtra.append(c).append(" in ( ").append(DbUtils::GetVectorJoinStr(ele)).append(" )");
								}
								else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
									whereExtra.append(" ( ");
									for (size_t j = 0; j < ele.size(); j += 2) {
										if (j > 0) {
											whereExtra.append(" or ");
										}
										whereExtra.append(ele.at(j)).append(" ");
										string curIndexStr = string("$").append(DbUtils::IntTransToString(index++));
										string eqStr = parameterized ? (k.compare("lks") == 0 ? string(" like ").append(curIndexStr) : string(" = ").append(curIndexStr)) : (k.compare("lks") == 0 ? " like '" : " = '");
										string vsStr = ele.at(j + 1);
										if (k.compare("lks") == 0) {
											vsStr.insert(0, "%");
											vsStr.append("%");
										}
										whereExtra.append(eqStr);
										if(parameterized)
											values.addSubitem(vsStr);
										else{
											vsStr.append("'");
											whereExtra.append(vsStr);
										}
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
									if(parameterized){
										where.append(k).append(vls.at(0)).append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
										values.addSubitem(vls.at(1));
									}else
										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
								}
								else if (vls.size() == 4) {
									if(parameterized){
										where.append(k).append(vls.at(0)).append(" $").append(DbUtils::IntTransToString(index++)).append(" ").append("and ");
										where.append(k).append(vls.at(2)).append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
										values.addSubitem(vls.at(1));
										values.addSubitem(vls.at(3));
									}else{
										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
										where.append(k).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
									}
								}
								else {
									return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
								}
							}
							else if (fuzzy == "1") {
								if(parameterized){
									where.append(k).append(" like ").append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
									values.addSubitem(v.insert(0, "%").append("%"));
								}
								else
									where.append(k).append(" like '%").append(v).append("%'");
								
							}
							else {
								if(parameterized){
									where.append(k).append(" =").append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
									vIsString ? values.addSubitem(v) : values.addSubitem(params[k].toDouble());
								}else{
									if (vIsString)
										where.append(k).append(" = '").append(v).append("'");
									else
										where.append(k).append(" = ").append(v);
								}
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
								extra.append(",cast(sum(").append(ele.at(i)).append(") as double) as ").append(ele.at(i + 1)).append(" ");
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
						querySql.append("select ").append(fieldsJoinStr).append(extra).append(" from ").append(tablename);
						if (where.length() > 0){
							querySql.append(" where ").append(where);
						}
					}
					else {
						querySql.append(tablename);
						if (queryType == 2 && !fields.empty()) {
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
					return DbUtils::MakeJsonObject(STSUCCESS);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}

			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				char* err = nullptr;
				pqxx::connection* pq = GetConnection(err);
				if (pq == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				try {
					pqxx::params ps;
					int len = values.size();
					for(int i = 0; i < len; i++){
						ps.append(values[i].toString());
					}
					pqxx::nontransaction N(*pq);
					pqxx::result R(N.exec_params(aQuery, ps));

					size_t coLen = R.columns();
					vector<Json> arr;
					for (pqxx::result::const_iterator c = R.begin(); c != R.end(); ++c) {
						Json al;
						for (int i = 0; i < coLen; ++i)
						{
							auto rsType = R.column_type(i);
							switch (rsType)
							{
							case INT2OID:
							case INT4OID:
							case INT8OID:
							case NUMERICOID:
								al.addSubitem(R.column_name(i), atof(c[i].c_str()));
								break;

							default:
								al.addSubitem(R.column_name(i), (char*)c[i].c_str());
								break;
							}
						}
						arr.push_back(al);
					}
					if (arr.empty())
						rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
					rs.addSubitem("data", arr);
					R.clear();

					std::cout << "SQL: " << aQuery << std::endl;
					return rs;
				}
				catch (const std::exception& e) {
					return DbUtils::MakeJsonObject(STDBOPERATEERR, (char*)e.what());
				}
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
					R.clear();
				}
				catch (const std::exception &e)
				{
					std::cout << e.what();
					return DbUtils::MakeJsonObject(STDBOPERATEERR, e.what());
				}
				std::cout << "SQL: " << aQuery << std::endl;
				return rs;
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