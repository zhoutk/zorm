#pragma once
#include <assert.h>
#include "Idb.h"
#include "sqlite3.h"
#include "DbUtils.h"
#include "GlobalConstants.h"
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
			// Alias used by the records/pages count query. Kept lowercase and
			// unambiguous; SQLite is case-insensitive on column names, other
			// backends quote it explicitly.
			std::string countAlias_ = "_zorm_total";

		public:
			Sqlit3Db(const char* apFilename, bool logFlag = false, bool parameterized = false,
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
				queryByParameter = parameterized;
			};

			Sqlit3Db(const std::string& aFilename, bool logFlag = false, bool parameterized = false,
				const int          aFlags = OPEN_READWRITE | OPEN_CREATE,
				const int          aBusyTimeoutMs = 0,
				const std::string& aVfs = "") {
				new (this)Sqlit3Db(aFilename.c_str(), logFlag, parameterized, aFlags, aBusyTimeoutMs, aVfs.empty() ? nullptr : aVfs.c_str());
			};

			Json create(const string& tablename, const Json& params)
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				// Array payload: single element -> single row, many -> batch
				// (gels/orm parity; jsonfile backend does the same).
				if (params.isArray()) {
					if (params.size() == 0)
						return DbUtils::MakeJsonObject(STPARAMERR);
					if (params.size() > 1)
						return insertBatch(tablename, params, "id");
					return create(tablename, params[0]);
				}
				string sql;
				string generatedId;
				Json values(JsonType::Array);
				if (!buildInsertSql(tablename, params, sql, values, generatedId))
					return DbUtils::MakeJsonObject(STPARAMERR);
				// Upsert parity: create() with a provided id overwrites the
				// row (gels/orm/jsonfile semantics) instead of failing with a
				// primary-key violation. Uses INSERT ... ON CONFLICT (id) DO
				// UPDATE SET <non-id cols> = excluded.<col> (SQLite >= 3.24).
				const Json providedId = params["id"];
				if (queryByParameter && !providedId.isError() && !DbUtils::Trim(providedId.toString()).empty()) {
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
				// affectedRows parity (jsonfile reports it; gels reports it)
				if (rs["affected"].isError())
					rs.add("affectedRows", 1);
				return rs;
			}

			Json update(const string& tablename, const Json& params)
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				string sql;
				Json values(JsonType::Array);
				if (!buildUpdateSql(tablename, params, sql, values))
					return DbUtils::MakeJsonObject(STPARAMERR);
				Json rs = ExecNoneQuerySql(sql, values);
				if (rs["status"].toInt() == STSUCCESS && !rs["affected"].isError())
					rs.add("affectedRows", rs["affected"].toInt());
				return rs;
			}

			Json remove(const string& tablename, const Json& params)
			{
				if (params.isError())
					return DbUtils::MakeJsonObject(STPARAMERR);
				string sql;
				Json values(JsonType::Array);
				if (!buildDeleteSql(tablename, params, sql, values))
					return DbUtils::MakeJsonObject(STPARAMERR);
				Json rs = ExecNoneQuerySql(sql, values);
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
				Json result = ExecQuerySql(tablename, fields, values);
				if (result["status"].toInt() == 200)
					attachRecordsPages(result, params, countSql, values);
				return result;
			}

			Json execSql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecNoneQuerySql(sql, values);
				else
					return rs;
			}

			Json querySql(const string& sqlstr, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				string sql(sqlstr);
				bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, fields, 2, parameterized);
				if(rs["status"].toInt() == 200)
					return ExecQuerySql(sql, fields, values);
				else
					return rs;
			}

			Json insertBatch(const string& tablename, const Json& elements, string constraint) {
				string sql = "insert into ";
				if (elements.size() < 1 || !elements.isArray()) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					Json values = Json(JsonType::Array);
					string keyStr = " (";
					keyStr.append(DbUtils::GetVectorJoinStr(DbUtils::GetVectorFromJson(elements[0].getAllKeys()))).append(" ) ");
					for (int i = 0; i < elements.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(elements[i].getAllKeys());
						string valueStr = " select ";
						for (int j = 0; j < keys.size(); j++) {
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
							}
						}
						if (i < elements.size() - 1) {
							valueStr.append(" union all ");
						}
						keyStr.append(valueStr);
					}
					sql.append(tablename).append(keyStr);
					Json rs = ExecNoneQuerySql(sql, values);
					if (rs["status"].toInt() == STSUCCESS)
						rs.add("affectedRows", elements.size());
					return rs;
				}
			}

			// Structured element -> SQL. Returns "" when the element is a raw
			// SQL text element (caller uses element["text"]/["values"]).
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
					if (!params.isArray())
						return false;
					// Reuse insertBatch's SQL shape for a single batch element.
					if (params.size() == 0)
						return false;
					Json batchValues(JsonType::Array);
					string keyStr = " (";
					keyStr.append(DbUtils::GetVectorJoinStr(DbUtils::GetVectorFromJson(params[0].getAllKeys()))).append(" ) ");
					for (int i = 0; i < params.size(); i++) {
						vector<string> keys = DbUtils::GetVectorFromJson(params[i].getAllKeys());
						string valueStr = " select ";
						for (int j = 0; j < keys.size(); j++) {
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
							if (j < keys.size() - 1)
								valueStr.append(",");
						}
						if (i < params.size() - 1)
							valueStr.append(" union all ");
						keyStr.append(valueStr);
					}
					sql = "insert into " + table + keyStr;
					values = batchValues;
					return true;
				}
				return false;
			}

			Json transGo(const Json& sqls, bool isAsync = false) {
				if (!sqls.isArray() || sqls.size() == 0) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					char* zErrMsg = 0;
					string errmsg = "Running transaction error: ";
					bool isExecSuccess = true;
					sqlite3_exec(getHandle(),"PRAGMA synchronous = OFF; ",0,0,0);
					sqlite3_exec(getHandle(), "begin;", 0, 0, &zErrMsg);
					for (size_t i = 0; i < sqls.size(); i++) {
						const Json element = sqls[i];
						// Raw SQL text element: {"text"/"sql": ..., "values": [...]}
						// Only direct children count; a structured element may
						// legitimately carry a param literally named "text".
						const bool hasText = ZJSON::hasChild(element, "text");
						const bool hasSql = !hasText && ZJSON::hasChild(element, "sql");
						Json values = element["values"].isError() ? Json(JsonType::Array) : element["values"];
						string sql;
						if (hasText || hasSql) {
							const Json text = hasText ? element["text"] : element["sql"];
							sql = text.toString();
						} else {
							// Structured element: {table, method, params, id}
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
						isExecSuccess = ExecSqlForTransGo(sql, values, &errmsg);
						if (!isExecSuccess)
							break;
					}
					if (isExecSuccess)
					{
						sqlite3_exec(getHandle(), "commit;", 0, 0, 0);
						!DbLogClose && std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
						return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success, run " + DbUtils::IntTransToString(sqls.size()) + " sqls.");
					}
					else
					{
						sqlite3_exec(getHandle(), "rollback;", 0, 0, 0);
						return DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg);
					}
				}
			}

			sqlite3* getHandle()
			{
				return mSQLitePtr.get();
			}

		private:
			// Builds "insert into <t> (<cols>) values (<ph>)" with bound values.
			// When the payload has no (or an empty) id, a generated 8-hex id is
			// added (gels/orm auto-id parity). Returns false for empty payloads.
			bool buildInsertSql(const string& tablename, const Json& params,
								string& sql, Json& values, string& generatedId) {
				if (!params.isObject())
					return false;
				// Empty payload: no columns, nothing to insert. Reject before
				// the id auto-generation kicks in (gels parity: an empty
				// create() is a param error).
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

			// Builds "update <t> set <col>=<ph>,... where id = <ph>".
			// Rejects payloads without an id (STPARAMERR at the call site).
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
				size_t len = allKeys.size();
				bool first = true;
				for (size_t i = 0; i < len; i++) {
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

			// Builds "delete from <t> where id = <ph>". Rejects missing ids.
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

			Json genSql(string& querySql, Json& values, const Json& ps, vector<string> fields = vector<string>(), int queryType = 1, bool parameterized = false, string* countSql = nullptr) {
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
						querySql.append("select ").append(fieldsJoinStr).append(extra).append(" from ").append(tablename);
						if (where.length() > 0)
							querySql.append(" where ").append(where);
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

					// Pagination needs a count over the same filter to report
					// records/pages. It must be computed BEFORE the LIMIT is
					// appended, so capture it here and let the caller run it.
					if (countSql != nullptr && queryType == 1 && page > 0) {
						*countSql = DbUtils::CountSqlFromSelect(querySql, countAlias_);
						// count(col) with a group returns per-group rows; the
						// records count for a grouped page is the row count of
						// the (unlimited) grouped result, which we approximate
						// with count(1) over the same group. SQL backends count
						// the group rows via a subselect in gels; here we use
						// the simple count and accept pages computed from the
						// filtered row count when grouped.
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
			};

			// Runs a count query that returns exactly one row with one column
			// named countAlias_ and returns its integer value; -1 on error.
			long long runCountQuery(const string& countSql, Json& values) {
				if (countSql.empty())
					return -1;
				sqlite3_stmt* stmt = NULL;
				sqlite3* handle = getHandle();
				const int ret = sqlite3_prepare_v2(handle, countSql.c_str(), static_cast<int>(countSql.size()), &stmt, NULL);
				if (SQLITE_OK != ret) {
					!DbLogClose && std::cout << "SQL: " << countSql << " -> " << sqlite3_errmsg(handle) << std::endl;
					return -1;
				}
				for (int i = 0; i < values.size(); i++) {
					string ele = values[i].toString();
					sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
				}
				long long count = -1;
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					count = sqlite3_column_int64(stmt, 0);
				}
				sqlite3_finalize(stmt);
				return count;
			}

			// Adds records/pages to a select result, gels-style.
			void attachRecordsPages(Json& result, const Json& params, const string& countSql, Json& values) {
				long long records = -1;
				const string pageText = params["page"].toString();
				const string sizeText = params["size"].toString();
				const int page = atoi(pageText.c_str());
				const int size = atoi(sizeText.c_str());
				if (page > 0 && size > 0 && !countSql.empty()) {
					records = runCountQuery(countSql, values);
				}
				if (records < 0) {
					records = result["data"].size();
				}
				result.add("records", records);
				result.add("pages", (page > 0 && size > 0)
					? (records == 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(records) / size)))
					: (records > 0 ? 1 : 0));
			}

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

					Json arr(JsonType::Array);
					while (sqlite3_step(stmt) == SQLITE_ROW) {
						Json al;
						for (int j = 0; j < nCol; j++)
						{
							string k = fields.at(j);
							int nType = sqlite3_column_type(stmt, j);
							if (nType == 1) {					//SQLITE_INTEGER
								al.add(k, sqlite3_column_int(stmt, j));
							}
							else if (nType == 2) {				//SQLITE_FLOAT
								al.add(k, sqlite3_column_double(stmt, j));
							}
							else if (nType == 3) {				//SQLITE_TEXT
								al.add(k, (char*)sqlite3_column_text(stmt, j));
							}
							//else if (nType == 4) {				//SQLITE_BLOB

							//}
							//else if (nType == 5) {				//SQLITE_NULL

							//}
							else{
								al.add(k, "");
							}
						}
						arr.push_back(al);
					}
					if (arr.size() == 0)
						rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
					rs.add("data", arr);
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
					if (stepRet == SQLITE_DONE)
						rs.add("affected", sqlite3_changes(handle));
				}
				sqlite3_finalize(stmt);
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return stepRet == SQLITE_DONE ? rs : DbUtils::MakeJsonObject(STDBOPERATEERR, errStr.append(DbUtils::IntTransToString(stepRet)));
			}

			bool ExecSqlForTransGo(string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
				int stepRet = SQLITE_OK;
				sqlite3_stmt* stmt = NULL;
				sqlite3* handle = getHandle();
				const int ret = sqlite3_prepare_v2(handle, aQuery.c_str(), static_cast<int>(aQuery.size()), &stmt, NULL);
				if (SQLITE_OK == ret) {
					for(int i = 0; i < values.size(); i++){
						string ele = values[i].toString();
						sqlite3_bind_text(stmt, i + 1, ele.c_str(), ele.length(), SQLITE_TRANSIENT);
					}
					stepRet = sqlite3_step(stmt);
				}else{
					if(out)
						*out += sqlite3_errmsg(getHandle());
				}
				sqlite3_finalize(stmt);
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return SQLITE_OK == ret && stepRet == SQLITE_DONE ? true : false;
			}

			bool escapeString(string& pStr)
			{
				pStr = std::regex_replace(pStr, std::regex("'"), "''");
				return true;
			}

			bool DbLogClose;
			bool queryByParameter;
		};
	}

}