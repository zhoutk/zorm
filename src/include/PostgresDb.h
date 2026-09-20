#pragma once

#include "Idb.h"
#include "DbUtils.h"
#include "GlobalConstants.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <libpq-fe.h>
#include "pg_type_d.h"

namespace ZORM {

	using std::string;

	namespace Postgres {

		class ZORM_API PostgresDb : public Idb {
			
		public:
			const vector<string> QUERY_EXTRA_KEYS { "ins", "lks", "ors"};
			const vector<string> QUERY_UNEQ_OPERS { ">,", ">=,", "<,", "<=,", "<>,", "=,"};

		private:
			PGconn * GetConnection(string& err)
			{
				size_t index = (rand() % maxConn) + 1;
				if (index > pool.size())
				{
					PGconn *pqsql = PQconnectdb(connString.c_str());
					if (PQstatus(pqsql) == CONNECTION_OK)
					{
						pool.push_back(pqsql);
						return pqsql;
					}
					else
					{
						err = string(PQerrorMessage(pqsql));
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
				connString = "dbname=" + dbname + " user=" + dbuser + " password=" + dbpwd + " hostaddr=" + dbhost + " port=" + DbUtils::IntTransToString(dbport);
			}

		public:

			PostgresDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 5432, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), maxConn(2), DbLogClose(false), queryByParameter(false)
			{
				init();
				restrain_ = DbUtils::MakeVector("id");

				if(!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
				if (!options["DbLogClose"].isError())
					DbLogClose = options["DbLogClose"].toBool();
				if(!options["parameterized"].isError())
					queryByParameter = options["parameterized"].toBool();
			}

			Json create(const string& tablename, const Json& params) override
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				if (params.isArray()) {
					if (params.size() == 0)
						return DbUtils::MakeJsonObject(STPARAMERR);
					if (params.size() > 1)
						return insertBatch(tablename, params, "id");
					return create(tablename, params[0]);
				}
				string sql;
				Json values(JsonType::Array);
				string generatedId;
				if (!buildInsertSql(tablename, params, sql, values, generatedId))
					return DbUtils::MakeJsonObject(STPARAMERR);
				// Upsert parity: a provided id overwrites the row via
				// ON CONFLICT (id) DO UPDATE.
				const Json providedId = params["id"];
				if (!providedId.isError() && !DbUtils::Trim(providedId.toString()).empty()) {
					vector<string> keys = DbUtils::GetVectorFromJson(params.getAllKeys());
					string updateClause;
					for (const string& k : keys) {
						if (k == "id")
							continue;
						if (!updateClause.empty())
							updateClause += ",";
						updateClause += k + " = excluded." + k;
					}
					if (!updateClause.empty()) {
						sql += " on conflict (id) do update set " + updateClause;
					}
				}
				Json rs = ExecNoneQuerySql(sql, values);
				if (rs["status"].toInt() != STSUCCESS)
					return rs;
				const string id = generatedId.empty()
					? params["id"].toString()
					: generatedId;
				if (!id.empty()) {
					rs.add("id", id);
					rs.add("insertId", id);
				}
				rs.add("affectedRows", 1);
				return rs;
			}

			Json update(const string& tablename, const Json& params) override
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				string sql;
				Json values(JsonType::Array);
				if (!buildUpdateSql(tablename, params, sql, values))
					return DbUtils::MakeJsonObject(STPARAMERR);
				Json rs = ExecNoneQuerySql(sql, values);
				if (rs["status"].toInt() == STSUCCESS)
					rs.add("affectedRows", 1);
				return rs;
			}

			Json remove(const string& tablename, const Json& params) override
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				string sql;
				Json values(JsonType::Array);
				if (!buildDeleteSql(tablename, params, sql, values))
					return DbUtils::MakeJsonObject(STPARAMERR);
				Json rs = ExecNoneQuerySql(sql, values);
				if (rs["status"].toInt() == STSUCCESS)
					rs.add("affectedRows", 1);
				return rs;
			}

			Json select(const string& tbname, const Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override
			{
				string tablename = tbname;
				string countSql;
				Json rs = genSql(tablename, values, params, fields, 1, queryByParameter, &countSql);
				if (rs["status"].toInt() != 200)
					return rs;
				Json result = ExecQuerySql(tablename, fields, values);
				if (result["status"].toInt() == 200)
					attachRecordsPages(result, params, countSql, values);
				return result;
			}

			Json querySql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("$") != sql.npos;
				Json rs = genSql(sql, values, params, fields, 2, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecQuerySql(sql, fields, values);
				else
					return rs;
			}

			Json execSql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("$") != sql.npos;
				Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecNoneQuerySql(sql, values);
				else
					return rs;
			}

			Json insertBatch(const string& tablename, const Json& elements, string constraint) override
			{
				string sql = "insert into ";
				vector<string> restrain = DbUtils::MakeVector(constraint);
				if (!elements.isArray() || elements.size() < 1) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					Json values = Json(JsonType::Array);
					string keyStr = " ( ";
					string updateStr = "";
					keyStr.append(DbUtils::GetVectorJoinStr(DbUtils::GetVectorFromJson(elements[0].getAllKeys()))).append(" ) values ");
					int index = 1;
					for (size_t i = 0; i < elements.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(elements[i].getAllKeys());
						string valueStr = " ( ";
						for (size_t j = 0; j < keys.size(); j++) {
							if (i == 0) {
								vector<string>::iterator iter = find(restrain.begin(), restrain.end(), keys[j]);
								if (iter == restrain.end())
									updateStr.append(keys[j]).append(" = excluded.").append(keys[j]).append(",");
							}
							bool vIsString = elements[i][keys[j]].isString() || elements[i][keys[j]].isArray() || elements[i][keys[j]].isObject();
							string v = elements[i][keys[j]].toString();
							!queryByParameter && vIsString && escapeString(v);
							if(queryByParameter){
								valueStr.append("$").append(DbUtils::IntTransToString(index++));
								values.add(v);
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
					Json rs = ExecNoneQuerySql(sql, values);
					if (rs["status"].toInt() == STSUCCESS)
						rs.add("affectedRows", elements.size());
					return rs;
				}
			}

			// Structured element -> SQL text + values ($n numbering).
			bool buildStructuredSql(const Json& element, const string& table, const string& method,
									const Json& params, bool hasId, const Json& idValue,
									string& sql, Json& values) {
				if (method == "Insert") {
					if (!params.isObject())
						return false;
					string generatedId;
					return buildInsertSql(table, params, sql, values, generatedId);
				}
				if (method == "Update") {
					if (!params.isObject() || !hasId)
						return false;
					Json merged(params);
					ZJSON::setChild(merged, "id", idValue);
					return buildUpdateSql(table, merged, sql, values);
				}
				if (method == "Delete") {
					if (!hasId)
						return false;
					return buildDeleteSql(table, Json{{"id", idValue}}, sql, values);
				}
				if (method == "Batch") {
					if (!params.isArray() || params.size() == 0)
						return false;
					Json batchValues(JsonType::Array);
					string keyStr = " ( ";
					string updateStr = "";
					keyStr.append(DbUtils::GetVectorJoinStr(DbUtils::GetVectorFromJson(params[0].getAllKeys()))).append(" ) values ");
					int index = 1;
					for (size_t i = 0; i < params.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(params[i].getAllKeys());
						string valueStr = " ( ";
						for (size_t j = 0; j < keys.size(); j++) {
							if (i == 0) {
								vector<string>::iterator iter = find(restrain_.begin(), restrain_.end(), keys[j]);
								if (iter == restrain_.end())
									updateStr.append(keys[j]).append(" = excluded.").append(keys[j]).append(",");
							}
							bool vIsString = params[i][keys[j]].isString() || params[i][keys[j]].isArray() || params[i][keys[j]].isObject();
							string v = params[i][keys[j]].toString();
							!queryByParameter && vIsString && escapeString(v);
							if (queryByParameter) {
								valueStr.append("$").append(DbUtils::IntTransToString(index++));
								batchValues.add(v);
							} else {
								if (vIsString)
									valueStr.append("'").append(v).append("'");
								else
									valueStr.append(v);
							}
							if (j < keys.size() - 1)
								valueStr.append(",");
						}
						valueStr.append(" )");
						if (i < params.size() - 1)
							valueStr.append(",");
						keyStr.append(valueStr);
					}
					if (updateStr.length() == 0) {
						sql = "insert into " + table + keyStr;
					} else {
						updateStr = updateStr.substr(0, updateStr.length() - 1);
						sql = "insert into " + table + keyStr + " on conflict (id) do update set " + updateStr;
					}
					values = batchValues;
					return true;
				}
				return false;
			}

			Json transGo(const Json& sqls, bool isAsync = false) override
			{
				if (!sqls.isArray() || sqls.size() == 0) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					bool isExecSuccess = true;
					string errmsg = "Running transaction error: ";
					string err = "";
					PGconn* pq = GetConnection(err);
					if (pq == nullptr)
						return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
					Json tsqls(JsonType::Array);
					tsqls.push_back(Json{{"text", "BEGIN TRANSACTION;"}});
					for (size_t i = 0; i < sqls.size(); i++) {
						const Json element = sqls[i];
						const bool hasText = ZJSON::hasChild(element, "text");
						const bool hasSql = !hasText && ZJSON::hasChild(element, "sql");
						Json values = element["values"].isError() ? Json(JsonType::Array) : element["values"];
						string sql;
						if (hasText || hasSql) {
							const Json text = hasText ? element["text"] : element["sql"];
							sql = text.toString();
						} else {
							const string table = element["table"].toString();
							const string method = element["method"].toString();
							const Json params = element["params"];
							const Json idValue = element["id"];
							if (!buildStructuredSql(element, table, method, params,
													!idValue.isError(), idValue, sql, values)) {
								err += "transaction element is wrong.";
								ExecSqlForTransGo(pq, "ROLLBACK;");
								return DbUtils::MakeJsonObject(STDBOPERATEERR, err);
							}
						}
						tsqls.push_back(Json{{"text", sql}, {"values", values}});
					}
					tsqls.push_back(Json{{"text", "END TRANSACTION;"}});
					for (size_t i = 0; i < tsqls.size(); i++) {
						string sql = tsqls[i]["text"].toString();
						Json values = tsqls[i]["values"].isError() ? Json(JsonType::Array) : tsqls[i]["values"];
						if(!ExecSqlForTransGo(pq, sql, values, &err)){
							ExecSqlForTransGo(pq, "ROLLBACK;");
							return DbUtils::MakeJsonObject(STDBOPERATEERR, err);
						}
					}
					!DbLogClose && std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
					return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success."); 
				}
			}

			~PostgresDb()
			{
				while (pool.size())
				{
					PQfinish(pool.back());
					pool.pop_back();
				}
			}

		private:
			int getParameterizedIndex(std::string_view sql){
				int index = 0, cur = 0;
				do{
					cur = sql.find("$", cur);
					++index;
				}while(cur++ != sql.npos);
				return index;
			}

			Json genSql(string& querySql, Json& values, const Json& ps, vector<string> fields = vector<string>(), int queryType = 1, bool parameterized = false, string* countSql = nullptr)
			{
				if (!ps.isError()) {
					Json params(ps);
					string tablename = querySql;
					querySql = "";
					string where = "";
					const string AndJoinStr = " and ";
					string fieldsJoinStr = "*";

					if (!fields.empty()) {
						fieldsJoinStr = DbUtils::GetVectorJoinStr(fields);
					}

					string fuzzy = params.take("fuzzy").toString();
					string sort = params.take("sort").toString();
					int page = atoi(params.take("page").toString().c_str());
					int size = atoi(params.take("size").toString().c_str());
					string sum = params.take("sum").toString();
					string count = params.take("count").toString();
					string group = params.take("group").toString();

					vector<string> allKeys = DbUtils::GetVectorFromJson(params.getAllKeys());
					size_t len = allKeys.size();
					int index = getParameterizedIndex(tablename);
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
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
											values.add(el);
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
										whereExtra.append("CAST(").append(ele.at(j)).append(" as TEXT) ");
										string curIndexStr = string("$").append(DbUtils::IntTransToString(index++));
										string eqStr = parameterized ? 
													   (k.compare("lks") == 0 ? string(" like ").append(curIndexStr) : string(" = ").append(curIndexStr)) : 
													   (k.compare("lks") == 0 ? " like '" : " = '");
										string vsStr = ele.at(j + 1);
										if (k.compare("lks") == 0) {
											vsStr.insert(0, "%");
											vsStr.append("%");
										}
										whereExtra.append(eqStr);
										if(parameterized)
											values.add(vsStr);
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
										values.add(vls.at(1));
									}else
										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
								}
								else if (vls.size() == 4) {
									if(parameterized){
										where.append(k).append(vls.at(0)).append(" $").append(DbUtils::IntTransToString(index++)).append(" ").append("and ");
										where.append(k).append(vls.at(2)).append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
										values.add(vls.at(1));
										values.add(vls.at(3));
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
									where.append("CAST(").append(k).append(" as TEXT) ").append(" like ").append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
									values.add(v.insert(0, "%").append("%"));
								}
								else
									where.append(k).append(" like '%").append(v).append("%'");
								
							}
							else {
								if(parameterized){
									where.append(k).append(" =").append(" $").append(DbUtils::IntTransToString(index++)).append(" ");
									vIsString ? values.add(v) : values.add(params[k].toDouble());
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
								extra.append("sum(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
							}
						}
					}
					if (!count.empty()) {
						vector<string> ele = DbUtils::MakeVector(count);
						if (ele.empty() || ele.size() % 2 == 1)
							return DbUtils::MakeJsonObject(STPARAMERR, "count is wrong.");
						else {
							for (size_t i = 0; i < ele.size(); i += 2) {
								extra.append("count(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
							}
						}
					}

					if (queryType == 1) {
						if(extra.find("count(") != extra.npos || extra.find("sum(") != extra.npos)
							fieldsJoinStr = "";
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

					if (countSql != nullptr && queryType == 1 && page > 0) {
						// Built from the known parts instead of re-parsing the finished
						// statement (O-4); grouped queries count the groups via a
						// wrapped subquery so records == number of groups.
						const string wherePart = where.length() > 0 ? " where " + where : "";
						if (group.empty())
						*countSql = "select count(1) as " + countAlias_ + " from " + tablename + wherePart;
					else
						*countSql = "select count(1) as " + countAlias_ + " from (select * from " + tablename + wherePart + " group by " + group + ") zorm_cnt";
					}

					if (page > 0) {
						page--;
						querySql.append(" limit ").append(DbUtils::IntTransToString(size)).append(" OFFSET ").append(DbUtils::IntTransToString(page * size));
					}
					return DbUtils::MakeJsonObject(STSUCCESS);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}

			// Runs the count query; returns -1 on failure.
			long long runCountQuery(const string& countSql, Json& values) {
				if (countSql.empty())
					return -1;
				Json rs = ExecQuerySql(countSql, vector<string>(), values);
				if (rs["status"].toInt() != 200 || rs["data"].size() == 0)
					return -1;
				return static_cast<long long>(rs["data"][0][countAlias_].toDouble());
			}

			void attachRecordsPages(Json& result, const Json& params, const string& countSql, Json& values) {
				long long records = -1;
				const string pageText = params["page"].toString();
				const string sizeText = params["size"].toString();
				const int page = atoi(pageText.c_str());
				const int size = atoi(sizeText.c_str());
				if (page > 0 && size > 0 && !countSql.empty())
					records = runCountQuery(countSql, values);
				if (records < 0)
					records = result["data"].size();
				result.add("records", records);
				result.add("pages", (page > 0 && size > 0)
					? (records == 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(records) / size)))
					: (records > 0 ? 1 : 0));
			}

			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				PGconn *pq = GetConnection(err);
				if (pq == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				int vLen = values.size();
				std::vector<char *> dataInputs;
				if(vLen > 0){
					dataInputs.resize(vLen);
					for (int i = 0; i < vLen; i++) {
						string ele = values[i].toString();
						int eleLen = ele.length() + 1;
						dataInputs[i] = new char[eleLen];
						memset(dataInputs[i], 0, eleLen);
						memcpy(dataInputs[i], ele.c_str(), eleLen);
					}
				}
				PGresult *res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr, vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr,0);
				for (auto el : dataInputs)
					delete[] el;
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) == PGRES_TUPLES_OK) {
					int coLen = PQnfields(res);
					Json arr(JsonType::Array);
					for (int i = 0; i < PQntuples(res); i++) {
						Json al;
						for (int j = 0; j < coLen; j++)
						{
							// NULL-aware decoding: SQL NULL becomes a real
							// JSON null (parity with the jsonfile backend);
							// numeric types become numbers, the rest strings.
							if (PQgetisnull(res, i, j)) {
								al.add(PQfname(res, j), nullptr);
								continue;
							}
							auto rsType = PQftype(res, j);
							switch (rsType)
							{
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
				}else{
					std::cout << PQerrorMessage(pq) << std::endl;
					PQclear(res);
					return DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(pq));
				}
			}

			Json ExecNoneQuerySql(string aQuery, Json values = Json(JsonType::Array)) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				PGconn *pq = GetConnection(err);
				if (pq == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				int vLen = values.size();
				std::vector<char *> dataInputs;
				if(vLen > 0){
					dataInputs.resize(vLen);
					for (int i = 0; i < vLen; i++) {
						string ele = values[i].toString();
						int eleLen = ele.length() + 1;
						dataInputs[i] = new char[eleLen];
						memset(dataInputs[i], 0, eleLen);
						memcpy(dataInputs[i], ele.c_str(), eleLen);
					}
				}
				PGresult *res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr, vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr,0);
				for (auto el : dataInputs)
					delete[] el;
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					std::cout << PQerrorMessage(pq) << std::endl;
					rs = DbUtils::MakeJsonObject(STDBOPERATEERR, PQerrorMessage(pq));
				}
				PQclear(res);
				return rs;
			}

			bool escapeString(string& pStr)
			{
				// string err = "";
				// pStr = GetConnection(err)->esc(pStr);
				return true;
			}

			std::string getEscapeString(string& pStr)
			{
				string err = "";
				return err;//GetConnection(err)->esc(pStr);
			}

			bool ExecSqlForTransGo(PGconn *pq, string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
				int vLen = values.size();
				std::vector<char *> dataInputs;
				if(vLen > 0){
					dataInputs.resize(vLen);
					for (int i = 0; i < vLen; i++) {
						string ele = values[i].toString();
						int eleLen = ele.length() + 1;
						dataInputs[i] = new char[eleLen];
						memset(dataInputs[i], 0, eleLen);
						memcpy(dataInputs[i], ele.c_str(), eleLen);
					}
				}
				PGresult *res = PQexecParams(pq, aQuery.c_str(), vLen, nullptr, vLen > 0 ? dataInputs.data() : nullptr, nullptr, nullptr,0);
				for (auto el : dataInputs)
					delete[] el;
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					string err = PQerrorMessage(pq);
					std::cout << err << std::endl;
					if(out)
						*out = err;
					return false;
				}
				PQclear(res);
				return true;
			}

		private:
			std::vector<PGconn*> pool;
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
			std::string countAlias_ = "_zorm_total";
			vector<string> restrain_;

			// SQL builders (shared by create/update/remove/transGo). $n placeholders.
			bool buildInsertSql(const string& tablename, const Json& params,
								string& sql, Json& values, string& generatedId) {
				if (!params.isObject())
					return false;
				if (ZJSON::memberCount(params) == 0)
					return false;
				Json row(params);
				const Json id = params["id"];
				if (id.isError() || DbUtils::Trim(id.toString()).empty()) {
					generatedId = DbUtils::GenerateId();
					ZJSON::setChild(row, "id", generatedId);
				}
				vector<string> allKeys = DbUtils::GetVectorFromJson(row.getAllKeys());
				if (allKeys.empty())
					return false;
				sql = "insert into " + tablename + " (";
				string vs = "";
				values = Json(JsonType::Array);
				for (size_t i = 0; i < allKeys.size(); i++) {
					string k = allKeys[i];
					sql.append(k);
					bool vIsString = row[k].isString() || row[k].isArray() || row[k].isObject();
					string v = row[k].toString();
					!queryByParameter && vIsString && escapeString(v);
					if (queryByParameter) {
						vs.append("$").append(DbUtils::IntTransToString(i + 1));
						vIsString ? values.add(v) : values.add(row[k].toDouble());
					} else {
						if (vIsString)
							vs.append("'").append(v).append("'");
						else
							vs.append(v);
					}
					if (i < allKeys.size() - 1) {
						sql.append(",");
						vs.append(",");
					}
				}
				sql.append(") values (").append(vs).append(")");
				return true;
			}

			bool buildUpdateSql(const string& tablename, const Json& params,
								string& sql, Json& values) {
				if (!params.isObject())
					return false;
				vector<string> allKeys = DbUtils::GetVectorFromJson(params.getAllKeys());
				vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
				if (iter == allKeys.end())
					return false;
				// O-5 parity: an update carrying only the id (no columns) is
				// rejected - it would otherwise build "update t set  where ...".
				if (allKeys.size() < 2)
					return false;
				sql = "update " + tablename + " set ";
				string where = " where id = ";
				Json idJson;
				values = Json(JsonType::Array);
				int index = 1;
				bool first = true;
				for (size_t i = 0; i < allKeys.size(); i++) {
					string k = allKeys[i];
					if (k.compare("id") == 0) {
						idJson = params[k];
						continue;
					}
					bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
					string v = params[k].toString();
					!queryByParameter && vIsString && escapeString(v);
					if (!first)
						sql.append(",");
					first = false;
					sql.append(k).append(" = ");
					if (queryByParameter) {
						sql.append(" $").append(DbUtils::IntTransToString(index++));
						vIsString ? values.add(v) : values.add(params[k].toDouble());
					} else {
						if (vIsString)
							sql.append("'").append(v).append("'");
						else
							sql.append(v);
					}
				}
				if (queryByParameter) {
					where.append(" $").append(DbUtils::IntTransToString(index++));
					values.concat(idJson);
				} else {
					bool vIsString = idJson.isString() || idJson.isArray() || idJson.isObject();
					if (vIsString)
						where.append("'").append(idJson.toString()).append("'");
					else
						where.append(idJson.toString());
				}
				sql.append(where);
				return true;
			}

			bool buildDeleteSql(const string& tablename, const Json& params,
								string& sql, Json& values) {
				if (!params.isObject())
					return false;
				const Json id = params["id"];
				if (id.isError())
					return false;
				sql = "delete from " + tablename + " where id = ";
				values = Json(JsonType::Array);
				bool vIsString = id.isString() || id.isArray() || id.isObject();
				if (queryByParameter) {
					sql.append(" $1 ");
					vIsString ? values.add(id.toString()) : values.add(id.toDouble());
				} else {
					if (vIsString)
						sql.append("'").append(id.toString()).append("'");
					else
						sql.append(id.toString());
				}
				return true;
			}
		};
	}

}