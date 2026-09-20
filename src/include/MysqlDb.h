#pragma once

#include "Idb.h"
#include "DbUtils.h"
#include "GlobalConstants.h"
#include "mysql.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>

namespace ZORM {

	using std::string;

	namespace Mysql {

		class ZORM_API MysqlDb : public Idb {

		public:
			const vector<string> QUERY_EXTRA_KEYS { "ins", "lks", "ors"};
			const vector<string> QUERY_UNEQ_OPERS { ">,", ">=,", "<,", "<=,", "<>,", "=,"};
			 
		private:
			MYSQL* GetConnection(string& err) {
				// Round-robin cursor instead of rand(): rand() carries shared
				// state (not guaranteed thread-safe by POSIX), while
				// GetConnection runs on every statement in threaded servers.
				size_t index = (s_poolCursor.fetch_add(1) % static_cast<unsigned int>(maxConn)) + 1;
				if (index > pool.size()) {
					MYSQL* pmysql;
					pmysql = mysql_init((MYSQL*)nullptr);
					if (pmysql != nullptr)
					{
					 	!charsetName.empty() && mysql_options(pmysql, MYSQL_SET_CHARSET_NAME, charsetName.c_str());
						// TLS/SSL, default ssl-mode=PREFERRED: mysql_ssl_set()
						// switches the client into TLS negotiation - WITHOUT it
						// MariaDB Connector/C connects in plain text even to
						// TLS-capable servers. NULL arguments keep client-side
						// certificates optional; the db_ssl_ca/cert/key/...
						// options apply real files when set.
						//   db_ssl_verify=true   verify the server certificate
						//                        (needs db_ssl_ca for self-signed)
						//   db_ssl_required=true fail the connection when the
						//                        server cannot do TLS (strict
						//                        mode for third-party deploys)
						mysql_ssl_set(pmysql,
									  sslKey.empty() ? nullptr : sslKey.c_str(),
									  sslCert.empty() ? nullptr : sslCert.c_str(),
									  sslCa.empty() ? nullptr : sslCa.c_str(),
									  sslCapath.empty() ? nullptr : sslCapath.c_str(),
									  sslCipher.empty() ? nullptr : sslCipher.c_str());
						char verifyFlag = sslVerify ? 1 : 0;
						mysql_options(pmysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verifyFlag);
						// NOTE: only set ENFORCE when actually required - setting
						// it to 0 after mysql_ssl_set() clears the TLS flag again
						// and the connection degrades to plain text.
						if (sslRequired)
						{
							char enforceFlag = 1;
							mysql_options(pmysql, MYSQL_OPT_SSL_ENFORCE, &enforceFlag);
						}
						if (mysql_real_connect(pmysql, dbhost.c_str(), dbuser.c_str(), dbpwd.c_str(), dbname.c_str(), dbport, nullptr, 0))
						{
							pool.push_back(pmysql);
							return pmysql;
						}else{
							err = (char*)mysql_error(pmysql);
							std::cout << "Error message : " << mysql_error(pmysql);
						}

					}
					return nullptr;
				}
				else {
					return pool.at(index - 1);
				}
			}

		public:

			MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 3306, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), maxConn(2), DbLogClose(false), queryByParameter(false)
			{
				if(!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
				if (!options["DbLogClose"].isError())
					DbLogClose = options["DbLogClose"].toBool();
				if(!options["parameterized"].isError())
					queryByParameter = options["parameterized"].toBool();
				if (!options["db_ssl_key"].isError())
					sslKey = options["db_ssl_key"].toString();
				if (!options["db_ssl_cert"].isError())
					sslCert = options["db_ssl_cert"].toString();
				if (!options["db_ssl_ca"].isError())
					sslCa = options["db_ssl_ca"].toString();
				if (!options["db_ssl_capath"].isError())
					sslCapath = options["db_ssl_capath"].toString();
				if (!options["db_ssl_cipher"].isError())
					sslCipher = options["db_ssl_cipher"].toString();
				if (!options["db_ssl_verify"].isError())
					sslVerify = options["db_ssl_verify"].toBool();
				if (!options["db_ssl_required"].isError())
					sslRequired = options["db_ssl_required"].toBool();
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
				// Upsert parity: a provided id overwrites the row (gels/orm/
				// jsonfile semantics) via ON DUPLICATE KEY UPDATE.
				const Json providedId = params["id"];
				if (!providedId.isError() && !DbUtils::Trim(providedId.toString()).empty()) {
					vector<string> keys = DbUtils::GetVectorFromJson(params.getAllKeys());
					string updateClause;
					for (const string& k : keys) {
						if (k == "id")
							continue;
						if (!updateClause.empty())
							updateClause += ",";
						updateClause += k + " = values(" + k + ")";
					}
					if (!updateClause.empty()) {
						sql += " on duplicate key update " + updateClause;
					}
				}
				Json rs = queryByParameter ? ExecNoneQuerySql(sql, values) : ExecNoneQuerySql(sql);
				if (rs["status"].toInt() != STSUCCESS)
					return rs;
				const string id = generatedId.empty()
					? params["id"].toString()
					: generatedId;
				if (!id.empty()) {
					rs.add("id", id);
					rs.add("insertId", id);
				}
				if (!rs["affected"].isError())
					rs.add("affectedRows", rs["affected"].toInt());
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
				Json rs = queryByParameter ? ExecNoneQuerySql(sql, values) : ExecNoneQuerySql(sql);
				if (rs["status"].toInt() == STSUCCESS && !rs["affected"].isError())
					rs.add("affectedRows", rs["affected"].toInt());
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
				Json rs = queryByParameter ? ExecNoneQuerySql(sql, values) : ExecNoneQuerySql(sql);
				if (rs["status"].toInt() == STSUCCESS && !rs["affected"].isError())
					rs.add("affectedRows", rs["affected"].toInt());
				return rs;
			}


			Json select(const string& tbname, const Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override
			{
				string tablename = tbname;
				string countSql;
				Json rs = genSql(tablename, values, params, fields, 1, queryByParameter, &countSql);
				if (rs["status"].toInt() != 200)
					return rs;
				Json result = queryByParameter ? ExecQuerySql(tablename, fields, values) : ExecQuerySql(tablename, fields);
				if (result["status"].toInt() == 200)
					attachRecordsPages(result, params, countSql, values);
				return result;
			}

			Json querySql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, fields, 2, parameterized);
				if(rs["status"].toInt() == 200)
					return parameterized ? ExecQuerySql(sql, fields, values) : ExecQuerySql(sql, fields);
				else
					return rs;
			}


			Json execSql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
				if(rs["status"].toInt() == 200)
					return parameterized ? ExecNoneQuerySql(sql, values) : ExecNoneQuerySql(sql);
				else
					return rs;
			}


			Json insertBatch(const string& tablename, const Json& elements, string constraint) override
			{
				string sql = "insert into ";
				if (!elements.isArray() || elements.size() < 1) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					Json values = Json(JsonType::Array);
					string keyStr = " ( ";
					string updateStr = "";
					keyStr.append(DbUtils::GetVectorJoinStr(DbUtils::GetVectorFromJson(elements[0].getAllKeys()))).append(" ) values ");
					for (int i = 0; i < elements.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(elements[i].getAllKeys());
						string valueStr = " ( ";
						for (int j = 0; j < keys.size(); j++) {
							if(i == 0)
								updateStr.append(keys[j]).append(" = values(").append(keys[j]).append(")");
							bool vIsString = elements[i][keys[j]].isString() || elements[i][keys[j]].isArray() || elements[i][keys[j]].isObject();
							string v = elements[i][keys[j]].toString();
							!queryByParameter && vIsString && escapeString(v);
							if(queryByParameter){
								valueStr.append("?");
								values.add(v);
							}else{
								if(vIsString)
									valueStr.append("'").append(v).append("'");
								else
									valueStr.append(v);
							}
							if (j < keys.size() - 1) {
								valueStr.append(",");
								if (i == 0)
									updateStr.append(",");
							}
						}
						valueStr.append(" )");
						if (i < elements.size() - 1) {
							valueStr.append(",");
						}
						keyStr.append(valueStr);
					}
					sql.append(tablename).append(keyStr).append(" on duplicate key update ").append(updateStr);
					Json rs = queryByParameter ? ExecNoneQuerySql(sql,values) : ExecNoneQuerySql(sql);
					if (rs["status"].toInt() == STSUCCESS)
						rs.add("affectedRows", elements.size());
					return rs;
				}
			}

			// Structured element -> SQL text + values.
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
					for (int i = 0; i < params.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(params[i].getAllKeys());
						string valueStr = " ( ";
						for (int j = 0; j < keys.size(); j++) {
							if (i == 0)
								updateStr.append(keys[j]).append(" = values(").append(keys[j]).append(")");
							bool vIsString = params[i][keys[j]].isString() || params[i][keys[j]].isArray() || params[i][keys[j]].isObject();
							string v = params[i][keys[j]].toString();
							!queryByParameter && vIsString && escapeString(v);
							if (queryByParameter) {
								valueStr.append("?");
								batchValues.add(v);
							} else {
								if (vIsString)
									valueStr.append("'").append(v).append("'");
								else
									valueStr.append(v);
							}
							if (j < keys.size() - 1) {
								valueStr.append(",");
								if (i == 0)
									updateStr.append(",");
							}
						}
						valueStr.append(" )");
						if (i < params.size() - 1)
							valueStr.append(",");
						keyStr.append(valueStr);
					}
					sql = "insert into " + table + keyStr + " on duplicate key update " + updateStr;
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
					MYSQL* mysql = GetConnection(err);
					if (mysql == nullptr)
						return DbUtils::MakeJsonObject(STDBCONNECTERR, err);

					mysql_query(mysql, "begin;");
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
								errmsg += "transaction element is wrong.";
								isExecSuccess = false;
								break;
							}
						}
						isExecSuccess = ExecSqlForTransGo(mysql, sql, values, &errmsg);
						if (!isExecSuccess)
							break;
					}
					if (isExecSuccess)
					{
						mysql_query(mysql, "commit;");
						!DbLogClose && std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
						return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success.");
					}
					else
					{
						mysql_query(mysql, "rollback;");
						return DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg);
					}
				}
			}

			~MysqlDb()
			{
				while (pool.size())
				{
					mysql_close(pool.back());
					pool.pop_back();
				}
			}

		private:
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
											whereExtra.append("?");
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
										whereExtra.append(ele.at(j)).append(" ");
										string eqStr = parameterized ? (k.compare("lks") == 0 ? " like ?" : " = ?") : (k.compare("lks") == 0 ? " like '" : " = '");
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
										where.append(k).append(vls.at(0)).append(" ? ");
										values.add(vls.at(1));
									}else
										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
								}
								else if (vls.size() == 4) {
									if(parameterized){
										where.append(k).append(vls.at(0)).append(" ? ").append("and ");
										where.append(k).append(vls.at(2)).append("? ");
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
									where.append(k).append(" like ? ");
									values.add(v.insert(0, "%").append("%"));
								}
								else
									where.append(k).append(" like '%").append(v).append("%'");
								
							}
							else {
								if(parameterized){
									where.append(k).append(" = ? ");
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

					if (countSql != nullptr && queryType == 1 && page > 0) {
						*countSql = DbUtils::CountSqlFromSelect(querySql, countAlias_);
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

			// Runs the records/pages count query; returns -1 on failure.
			long long runCountQuery(const string& countSql, Json& values) {
				if (countSql.empty())
					return -1;
				Json rs = queryByParameter ? ExecQuerySql(countSql, vector<string>(), values) : ExecQuerySql(countSql, vector<string>());
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

			Json ExecQuerySql(string aQuery, vector<string> fields)
			{
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				MYSQL *mysql = GetConnection(err);
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				if (mysql_query(mysql, aQuery.c_str()))
				{
					string errmsg = "";
					errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					return rs;
				}
				else
				{
					MYSQL_RES *result = mysql_use_result(mysql);
					if (result != NULL)
					{
						MYSQL_ROW row;
						int num_fields = mysql_num_fields(result);
						MYSQL_FIELD *fields = mysql_fetch_fields(result);
						Json arr(JsonType::Array);
						while ((row = mysql_fetch_row(result)) && row != NULL)
						{
							Json al;
							for (int i = 0; i < num_fields; ++i)
							{
								// SQL NULL arrives as a NULL row[i]: reading it
								// through atof() would crash. Decode to a real
								// JSON null (parity with the parameterized path).
								if (row[i] == nullptr)
									al.add(fields[i].name, nullptr);
								else if (IS_NUM(fields[i].type))
									al.add(fields[i].name, atof(row[i]));
								else
									al.add(fields[i].name, row[i]);
							}
							arr.push_back(al);
						}
						if (arr.isEmpty())
							rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
						rs.add("data", arr);
					}
					mysql_free_result(result);
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				MYSQL* mysql = GetConnection(err);
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					mysql_stmt_close(stmt);             // was leaked on this path
					return rs;
				}
				else
				{
					const int vLen = values.size();
					std::vector<char *> dataInputs;
					if (vLen > 0)
					{
						MYSQL_BIND *bind = new MYSQL_BIND[vLen];
						std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
						dataInputs.resize(vLen);
						for (int i = 0; i < vLen; i++)
						{
							string ele = values[i].toString();
							int eleLen = ele.length() + 1;
							dataInputs[i] = new char[eleLen];
							memset(dataInputs[i], 0, eleLen);
							memcpy(dataInputs[i], ele.c_str(), eleLen);
							bind[i].buffer_type = MYSQL_TYPE_STRING;
							bind[i].buffer = (void *)dataInputs[i];
							bind[i].buffer_length = eleLen - 1;
						}
						if (mysql_stmt_bind_param(stmt, bind))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
							delete [] bind;                 // was leaked on this path
							mysql_stmt_close(stmt);         // was leaked on this path
							return rs;
						}
						delete [] bind;
					}

					MYSQL_RES* prepare_meta_result = mysql_stmt_result_metadata(stmt);
					MYSQL_FIELD* fields;
					if (prepare_meta_result != nullptr)
					{
						int ret = 1;
						mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, (void *)&ret);
						if (mysql_stmt_execute(stmt))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
							mysql_free_result(prepare_meta_result);  // was leaked
							mysql_stmt_close(stmt);                  // was leaked
							return rs;
						}
						ret = mysql_stmt_store_result(stmt);
						int num_fields = mysql_num_fields(prepare_meta_result);
						fields = mysql_fetch_fields(prepare_meta_result);
						MYSQL_BIND *ps = new MYSQL_BIND[num_fields];
						std::memset(ps, 0, sizeof(MYSQL_BIND) * num_fields);
						std::vector<char *> dataOuts(num_fields);
						char* is_null = new char[num_fields];
						memset(is_null, 0, sizeof(char) * num_fields);
						for (int i = 0; i < num_fields; ++i)
						{
							auto p = allocate_buffer_for_field(fields[i]);
							dataOuts[i] = new char[p.size];
							memset(dataOuts[i], 0, p.size);
							ps[i].buffer_type = p.type;
							ps[i].buffer = (void *)dataOuts[i];
							ps[i].buffer_length = p.size;
							ps[i].is_null = &is_null[i];
						}
						ret = mysql_stmt_bind_result(stmt, ps);
						Json arr(JsonType::Array);
						while (mysql_stmt_fetch(stmt) != MYSQL_NO_DATA)
						{
							Json al;
							for (int i = 0; i < num_fields; ++i)
							{
								if (is_null[i])
									al.add(fields[i].name, nullptr);
								// Decoding must match the declared buffer width:
								// the previous (long)*(int*) read truncated
								// LONGLONG (counts > 2^31) and reinterpreted
								// TINY/SHORT/FLOAT buffers as raw strings.
								else if (fields[i].type == MYSQL_TYPE_TINY)
									al.add(fields[i].name, (long long)*(signed char *)dataOuts[i]);
								else if (fields[i].type == MYSQL_TYPE_SHORT || fields[i].type == MYSQL_TYPE_YEAR)
									al.add(fields[i].name, (long long)*(short *)dataOuts[i]);
								else if (fields[i].type == MYSQL_TYPE_LONG || fields[i].type == MYSQL_TYPE_INT24)
									al.add(fields[i].name, (long long)*(int *)dataOuts[i]);
								else if (fields[i].type == MYSQL_TYPE_LONGLONG)  //count
									al.add(fields[i].name, *(long long *)dataOuts[i]);
								else if (fields[i].type == MYSQL_TYPE_FLOAT)
								{
									float f = 0.0f;
									std::memcpy(&f, dataOuts[i], sizeof(float));
									al.add(fields[i].name, (double)f);
								}
								else if (fields[i].type == MYSQL_TYPE_DOUBLE || fields[i].type == MYSQL_TYPE_DECIMAL || fields[i].type == MYSQL_TYPE_NEWDECIMAL) //sum
									al.add(fields[i].name, *((double *)dataOuts[i]));
								else
									al.add(fields[i].name, dataOuts[i]);
							}
							arr.push_back(al);
						}
						if (arr.isEmpty())
							rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
						rs.add("data", arr);
						delete [] ps;
						delete [] is_null;
						for(auto el : dataOuts)
							delete [] el;
						mysql_free_result(prepare_meta_result);  // was leaked per query
					}
					for (auto el : dataInputs)
						delete[] el;
				}
				mysql_stmt_close(stmt);
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				MYSQL* mysql = GetConnection(err);
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);

				if (mysql_query(mysql, aQuery.c_str()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					return rs;
				}
				else {
					int affected = (int)mysql_affected_rows(mysql);
					rs.add("affected", affected);
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery, Json values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				MYSQL* mysql = GetConnection(err);
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);

				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					mysql_stmt_close(stmt);             // was leaked on this path
					return rs;
				}
				else {
					int vLen = values.size();
					std::vector<char *> dataInputs;
					if (vLen > 0)
					{
						MYSQL_BIND *bind = new MYSQL_BIND[vLen];
						std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
						dataInputs.resize(vLen);
						for (int i = 0; i < vLen; i++)
						{
							string ele = values[i].toString();
							int eleLen = ele.length() + 1;
							dataInputs[i] = new char[eleLen];
							memset(dataInputs[i], 0, eleLen);
							memcpy(dataInputs[i], ele.c_str(), eleLen);
							bind[i].buffer_type = MYSQL_TYPE_STRING;
							bind[i].buffer = (void *)dataInputs[i];
							bind[i].buffer_length = eleLen - 1;
						}
						if (mysql_stmt_bind_param(stmt, bind))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
							delete [] bind;                 // was leaked on this path
							mysql_stmt_close(stmt);         // was leaked on this path
							return rs;
						}
						delete [] bind;
					}
					if (mysql_stmt_execute(stmt))
					{
						string errmsg = "";
						errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
						errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
						mysql_stmt_close(stmt);             // was leaked on this path
						return rs;
					}
					int affected = (int)mysql_affected_rows(mysql);
					rs.add("affected", affected);
					for (auto el : dataInputs)
						delete[] el;
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			struct st_buffer_size_type
			{
				size_t size;
				enum_field_types type;
				st_buffer_size_type(size_t s, enum_field_types t) : size(s + 1), type(t) {}
			};

			
			st_buffer_size_type allocate_buffer_for_field(const MYSQL_FIELD field)
			{
				switch (field.type)
				{
				case MYSQL_TYPE_NULL:
					return st_buffer_size_type(0, field.type);
				case MYSQL_TYPE_TINY:
					return st_buffer_size_type(1, field.type);
				case MYSQL_TYPE_SHORT:
					return st_buffer_size_type(2, field.type);
				case MYSQL_TYPE_INT24:
				case MYSQL_TYPE_LONG:
				case MYSQL_TYPE_FLOAT:
					return st_buffer_size_type(4, field.type);
				case MYSQL_TYPE_DOUBLE:
				case MYSQL_TYPE_LONGLONG:
					return st_buffer_size_type(8, field.type);
				case MYSQL_TYPE_YEAR:
					return st_buffer_size_type(2, MYSQL_TYPE_SHORT);
				case MYSQL_TYPE_TIMESTAMP:
				case MYSQL_TYPE_DATE:
				case MYSQL_TYPE_TIME:
				case MYSQL_TYPE_DATETIME:
					return st_buffer_size_type(sizeof(MYSQL_TIME), field.type);

				case MYSQL_TYPE_TINY_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_LONG_BLOB:
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_STRING:
				case MYSQL_TYPE_VAR_STRING:
				case MYSQL_TYPE_JSON:{
					return st_buffer_size_type(field.max_length, field.type);
				}
				case MYSQL_TYPE_DECIMAL:
				case MYSQL_TYPE_NEWDECIMAL:
					return st_buffer_size_type(64, field.type);
#if A1
				case MYSQL_TYPE_TIMESTAMP:
				case MYSQL_TYPE_YEAR:
					return st_buffer_size_type(10, field.type);
#endif
#if A0
				case MYSQL_TYPE_ENUM:
				case MYSQL_TYPE_SET:
#endif
				case MYSQL_TYPE_BIT:
					return st_buffer_size_type(8, MYSQL_TYPE_BIT);
				case MYSQL_TYPE_GEOMETRY:
					return st_buffer_size_type(field.max_length, MYSQL_TYPE_BIT);
				default:
					return st_buffer_size_type(field.max_length, field.type);
				}
			};

			bool ExecSqlForTransGo(MYSQL* mysql, string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
				{
					string errmsg = "";
					errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					if(out)
						*out += errmsg;
					mysql_stmt_close(stmt);             // was leaked on this path
					return false;
				}
				else {
					int vLen = values.size();
					std::vector<char *> dataInputs;
					if (vLen > 0)
					{
						MYSQL_BIND *bind = new MYSQL_BIND[vLen];
						std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
						dataInputs.resize(vLen);
						for (int i = 0; i < vLen; i++)
						{
							string ele = values[i].toString();
							int eleLen = ele.length() + 1;
							dataInputs[i] = new char[eleLen];
							memset(dataInputs[i], 0, eleLen);
							memcpy(dataInputs[i], ele.c_str(), eleLen);
							bind[i].buffer_type = MYSQL_TYPE_STRING;
							bind[i].buffer = (void *)dataInputs[i];
							bind[i].buffer_length = eleLen - 1;
						}
						if (mysql_stmt_bind_param(stmt, bind))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							if (out)
								*out += errmsg;
							delete [] bind;                 // was leaked on this path
							mysql_stmt_close(stmt);         // was leaked on this path
							return false;
						}
						delete [] bind;
					}
					if (mysql_stmt_execute(stmt))
					{
						string errmsg = "";
						errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
						errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
						if (out)
							*out += errmsg;
						mysql_stmt_close(stmt);             // was leaked on this path
						return false;
					}
					for (auto el : dataInputs)
						delete[] el;
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return true;
			}

			bool escapeString(string& pStr)
			{
				string err = "";
				MYSQL* mysql = GetConnection(err);
				if (mysql == nullptr)
				{
					// No connection to resolve the charset against: fall back
					// to plain MySQL-compatible literal escaping instead of
					// dereferencing a NULL handle.
					string escaped;
					escaped.reserve(pStr.length() * 2 + 1);
					for (char ch : pStr)
					{
						switch (ch)
						{
						case 0: escaped += "\0"; break;
						case '\n': escaped += "\n"; break;
						case '\r': escaped += "\r"; break;
						case '\\': escaped += "\\"; break;
						case '\'': escaped += "\'"; break;
						case '"': escaped += "\""; break;
						case 26: escaped += "\Z"; break;
						default: escaped += ch; break;
						}
					}
					pStr = escaped;
					return true;
				}
				char *tStr = new char[pStr.length() * 2 + 1];
				mysql_real_escape_string(mysql, tStr, pStr.c_str(), pStr.length());
				pStr = std::string(tStr);
				delete[] tStr;
				return true;
			}

		private:
			vector<MYSQL*> pool;
			int maxConn;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string charsetName;
			bool DbLogClose;
			bool queryByParameter;
			// TLS/SSL configuration (delivery to third parties): when any of
			// the file options is set the client uses TLS; sslVerify controls
			// server-certificate verification (off by default so the
			// self-signed certificates common on intranet servers work; turn
			// on together with db_ssl_ca pointing at a trusted CA).
			string sslKey, sslCert, sslCa, sslCapath, sslCipher;
			bool sslVerify = false;
			bool sslRequired = false;
			// Alias used by the records/pages count query.
			std::string countAlias_ = "_zorm_total";

			// Thread-safe round-robin cursor shared by the connection pool.
			inline static std::atomic<unsigned int> s_poolCursor{0};

			// SQL builders (shared by create/update/remove/transGo).
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
						vs.append("?");
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
				sql = "update " + tablename + " set ";
				string where = " where id = ";
				Json idJson;
				values = Json(JsonType::Array);
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
						sql.append(" ? ");
						vIsString ? values.add(v) : values.add(params[k].toDouble());
					} else {
						if (vIsString)
							sql.append("'").append(v).append("'");
						else
							sql.append(v);
					}
				}
				if (queryByParameter) {
					where.append(" ? ");
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
					sql.append(" ? ");
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