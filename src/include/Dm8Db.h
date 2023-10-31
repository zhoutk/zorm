#pragma once

#include "Idb.h"
#include "Utils.h"
#include "GlobalConstants.h"
#include <algorithm>
#include <iostream>
#include "DPI.h"
#include "DPIext.h"
#include "DPItypes.h"


namespace ZORM {

	using std::string;

	namespace Dm8 {

		struct Dm8Con {
			dhenv henv; /* 环境句柄 */
			dhcon hcon; /* 连接句柄 */
			dhstmt hstmt; /* 语句句柄 */
			Dm8Con() { henv = nullptr; hcon = nullptr; hstmt = nullptr; }
		};

		class ZORM_API Dm8Db : public Idb {

		public:
			const vector<string> QUERY_EXTRA_KEYS { "ins", "lks", "ors"};
			const vector<string> QUERY_UNEQ_OPERS { ">,", ">=,", "<,", "<=,", "<>,", "=,"};
			 
		private:
			void dpi_err_msg_print(sdint2 hndl_type, dhandle hndl, string& errOut)
			{
				sdint4 err_code;
				sdint2 msg_len;
				sdbyte err_msg[SDBYTE_MAX];
				char err[SDBYTE_MAX];

				/* 获取错误信息集合 */
				dpi_get_diag_rec(hndl_type, hndl, 1, &err_code, err_msg, sizeof(err_msg), &msg_len);
				printf("err_msg = %s, err_code = %d\n", err_msg, err_code);
				sprintf(err, "err_msg = %s, err_code = %d\n", err_msg, err_code);
				errOut = string(err);
			}

			Dm8Con* GetConnection(string& err) {
				size_t index = (rand() % maxConn) + 1;
				if (index > pool.size()) {
					Dm8Con* dmCon = new Dm8Con;
					DPIRETURN rt; 
					rt = dpi_alloc_env(&dmCon->henv);
					rt = dpi_alloc_con(dmCon->henv, &dmCon->hcon);
					dbhost.append(":").append(DbUtils::IntTransToString(dbport));
					rt = dpi_login(dmCon->hcon, (sdbyte*)dbhost.c_str(), (sdbyte*)dbuser.c_str(), (sdbyte*)dbpwd.c_str());
					if (!DSQL_SUCCEEDED(rt))
					{
						dpi_err_msg_print(DSQL_HANDLE_DBC, dmCon->hcon, err);
						return nullptr;
					}
					dpi_alloc_stmt(dmCon->hcon, &dmCon->hstmt);
					pool.push_back(dmCon);
					return dmCon;
				}
				else {
					return pool.at(index - 1);
				}
			}

		public:

			Dm8Db(string dbhost, string dbuser, string dbpwd, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(""), dbport(5236), maxConn(1), DbLogClose(false), queryByParameter(false)
			{
				if(!options["db_conn"].isError())
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
				if (!options["db_name"].isError())
					dbname = options["db_name"].toString();
				if (!options["db_port"].isError())
					dbport = options["db_port"].toInt();
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
					execSql.append("\"").append(dbname).append("\"").append(".").append("\"").append(tablename).append("\"").append(" ");

					vector<string> allKeys = params.getAllKeys();
					size_t len = allKeys.size();
					string fields = "", vs = "";
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						fields.append("\"").append(k).append("\"");
						bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
						string v = params[k].toString();
						!queryByParameter && vIsString && escapeString(v);
						if(queryByParameter){
							vs.append("?");
							vIsString ? values.add(v) : values.add(params[k].toDouble());
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
					return queryByParameter ? ExecNoneQuerySql(execSql, values) : ExecNoneQuerySql(execSql);
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
					execSql.append("\"").append(dbname).append("\"").append(".").append("\"").append(tablename).append("\"").append(" set ");

					vector<string> allKeys = params.getAllKeys();

					vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
					if (iter == allKeys.end()) {
						return DbUtils::MakeJsonObject(STPARAMERR);
					}
					else {
						size_t len = allKeys.size();
						size_t conditionLen = len - 2;
						string fields = "", where = " where \"id\" = ";
						Json idJson;
						for (size_t i = 0; i < len; i++) {
							string k = allKeys[i];
							bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
							string v = params[k].toString();
							!queryByParameter && vIsString&& escapeString(v);
							if (k.compare("id") == 0) {
								conditionLen++;
								if (queryByParameter) {
									where.append(" ? ");
									idJson = params[k];
								}
								else {
									if (vIsString)
										where.append("'").append(v).append("'");
									else
										where.append(v);
								}
							}
							else {
								fields.append("\"").append(k).append("\"").append(" = ");
								if (queryByParameter)
								{
									fields.append(" ? ");
									vIsString ? values.add(v) : values.add(params[k].toDouble());
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
						values.concat(idJson);
						execSql.append(fields).append(where);
						return queryByParameter ? ExecNoneQuerySql(execSql, values) : ExecNoneQuerySql(execSql);
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
					execSql.append("\"").append(dbname).append("\"").append(".").append("\"").append(tablename).append("\"").append(" where \"id\" = ");

					string k = "id";
					bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
					string v = params[k].toString();
					!queryByParameter && vIsString&& escapeString(v);
					if (queryByParameter) {
						execSql.append(" ? ");
						vIsString ? values.add(v) : values.add(params[k].toDouble());
					}
					else {
						if (vIsString)
							execSql.append("'").append(v).append("'");
						else
							execSql.append(v);
					}
					return queryByParameter ? ExecNoneQuerySql(execSql, values) : ExecNoneQuerySql(execSql);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}

			Json select(string tablename, Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array)) override
			{
				return Json(); /*= genSql(tablename, values, params, fields, 1, queryByParameter);
				if(rs["status"].toInt() == 200)
					return queryByParameter ? ExecQuerySql(tablename, fields, values) : ExecQuerySql(tablename, fields);
				else
					return rs;*/
			}

			Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> fields = vector<string>()) override
			{
				return Json();
				/*bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, fields, 2, parameterized);
				if(rs["status"].toInt() == 200)
					return parameterized ? ExecQuerySql(sql, fields, values) : ExecQuerySql(sql, fields);
				else
					return rs;*/
			}


			Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				return Json();
				/*bool parameterized = sql.find("?") != sql.npos;
				Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
				if(rs["status"].toInt() == 200)
					return parameterized ? ExecNoneQuerySql(sql, values) : ExecNoneQuerySql(sql);
				else
					return rs;*/
			}


			Json insertBatch(string tablename, Json& elements, string constraint) override
			{
				return Json();
				/*string sql = "insert into ";
				if (elements.size() < 2) {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
				else {
					Json values = Json(JsonType::Array);
					string keyStr = " ( ";
					string updateStr = "";
					keyStr.append(DbUtils::GetVectorJoinStr(elements[0].getAllKeys())).append(" ) values ");
					for (int i = 0; i < elements.size(); i++) {
						vector<string> keys = elements[i].getAllKeys();
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
					return queryByParameter ? ExecNoneQuerySql(sql,values) : ExecNoneQuerySql(sql);
				}*/
			}

			Json transGo(Json& sqls, bool isAsync = false) override
			{
				return Json();
				/*if (sqls.size() < 2) {
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
						string sql = sqls[i]["text"].toString();
						Json values = sqls[i]["values"].isError() ? Json(JsonType::Array) : sqls[i]["values"];
						isExecSuccess = ExecSqlForTransGo(sql, values, &errmsg);
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
				}*/
			}

			~Dm8Db()
			{
				while (pool.size())
				{
					Dm8Con* con = pool.back();
					dpi_logout(con->hcon);
					dpi_free_con(con->hcon);
					dpi_free_env(con->henv);
					pool.pop_back();
				}
			}

//		private:
//			Json genSql(string& querySql, Json& values, Json& params, vector<string> fields = vector<string>(), int queryType = 1, bool parameterized = false)
//			{
//				if (!params.isError()) {
//					string tablename = querySql;
//					querySql = "";
//					string where = "";
//					const string AndJoinStr = " and ";
//					string fieldsJoinStr = "*";
//
//					if (!fields.empty()) {
//						fieldsJoinStr = DbUtils::GetVectorJoinStr(fields);
//					}
//
//					string fuzzy = params.take("fuzzy").toString();
//					string sort = params.take("sort").toString();
//					int page = atoi(params.take("page").toString().c_str());
//					int size = atoi(params.take("size").toString().c_str());
//					string sum = params.take("sum").toString();
//					string count = params.take("count").toString();
//					string group = params.take("group").toString();
//
//					vector<string> allKeys = params.getAllKeys();
//					size_t len = allKeys.size();
//					for (size_t i = 0; i < len; i++) {
//						string k = allKeys[i];
//						bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
//						string v = params[k].toString();
//						!parameterized && vIsString && escapeString(v);
//						if (where.length() > 0) {
//							where.append(AndJoinStr);
//						}
//
//						if (DbUtils::FindStringFromVector(QUERY_EXTRA_KEYS, k)) {   // process key
//							string whereExtra = "";
//							vector<string> ele = DbUtils::MakeVector(params[k].toString());
//							if (ele.size() < 2 || ((k.compare("ors") == 0 || k.compare("lks") == 0) && ele.size() % 2 == 1)) {
//								return DbUtils::MakeJsonObject(STPARAMERR, k + " is wrong.");
//							}
//							else {
//								if (k.compare("ins") == 0) {
//									string c = ele.at(0);
//									vector<string>(ele.begin() + 1, ele.end()).swap(ele);
//									if(parameterized){
//										whereExtra.append(c).append(" in (");
//										int eleLen = ele.size();
//										for (int i = 0; i < eleLen; i++)
//										{
//											string el = ele[i];
//											whereExtra.append("?");
//											if (i < eleLen - 1)
//												whereExtra.append(",");
//											values.add(el);
//										}
//										whereExtra.append(")");
//									}else
//										whereExtra.append(c).append(" in ( ").append(DbUtils::GetVectorJoinStr(ele)).append(" )");
//								}
//								else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
//									whereExtra.append(" ( ");
//									for (size_t j = 0; j < ele.size(); j += 2) {
//										if (j > 0) {
//											whereExtra.append(" or ");
//										}
//										whereExtra.append(ele.at(j)).append(" ");
//										string eqStr = parameterized ? (k.compare("lks") == 0 ? " like ?" : " = ?") : (k.compare("lks") == 0 ? " like '" : " = '");
//										string vsStr = ele.at(j + 1);
//										if (k.compare("lks") == 0) {
//											vsStr.insert(0, "%");
//											vsStr.append("%");
//										}
//										whereExtra.append(eqStr);
//										if(parameterized)
//											values.add(vsStr);
//										else{
//											vsStr.append("'");
//											whereExtra.append(vsStr);
//										}
//									}
//									whereExtra.append(" ) ");
//								}
//							}
//							where.append(whereExtra);
//						}
//						else {				// process value
//							if (DbUtils::FindStartsStringFromVector(QUERY_UNEQ_OPERS, v)) {
//								vector<string> vls = DbUtils::MakeVector(v);
//								if (vls.size() == 2) {
//									if(parameterized){
//										where.append(k).append(vls.at(0)).append(" ? ");
//										values.add(vls.at(1));
//									}else
//										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
//								}
//								else if (vls.size() == 4) {
//									if(parameterized){
//										where.append(k).append(vls.at(0)).append(" ? ").append("and ");
//										where.append(k).append(vls.at(2)).append("? ");
//										values.add(vls.at(1));
//										values.add(vls.at(3));
//									}else{
//										where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
//										where.append(k).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
//									}
//								}
//								else {
//									return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
//								}
//							}
//							else if (fuzzy == "1") {
//								if(parameterized){
//									where.append(k).append(" like ? ");
//									values.add(v.insert(0, "%").append("%"));
//								}
//								else
//									where.append(k).append(" like '%").append(v).append("%'");
//								
//							}
//							else {
//								if(parameterized){
//									where.append(k).append(" = ? ");
//									vIsString ? values.add(v) : values.add(params[k].toDouble());
//								}else{
//									if (vIsString)
//										where.append(k).append(" = '").append(v).append("'");
//									else
//										where.append(k).append(" = ").append(v);
//								}
//							}
//						}
//					}
//
//					string extra = "";
//					if (!sum.empty()) {
//						vector<string> ele = DbUtils::MakeVector(sum);
//						if (ele.empty() || ele.size() % 2 == 1)
//							return DbUtils::MakeJsonObject(STPARAMERR, "sum is wrong.");
//						else {
//							for (size_t i = 0; i < ele.size(); i += 2) {
//								extra.append(",cast(sum(").append(ele.at(i)).append(") as double) as ").append(ele.at(i + 1)).append(" ");
//							}
//						}
//					}
//					if (!count.empty()) {
//						vector<string> ele = DbUtils::MakeVector(count);
//						if (ele.empty() || ele.size() % 2 == 1)
//							return DbUtils::MakeJsonObject(STPARAMERR, "count is wrong.");
//						else {
//							for (size_t i = 0; i < ele.size(); i += 2) {
//								extra.append(",count(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
//							}
//						}
//					}
//
//					if (queryType == 1) {
//						querySql.append("select ").append(fieldsJoinStr).append(extra).append(" from ").append(tablename);
//						if (where.length() > 0){
//							querySql.append(" where ").append(where);
//						}
//					}
//					else {
//						querySql.append(tablename);
//						if (queryType == 2 && !fields.empty()) {
//							size_t starIndex = querySql.find('*');
//							if (starIndex < 10) {
//								querySql.replace(starIndex, 1, fieldsJoinStr.c_str());
//							}
//						}
//						if (where.length() > 0) {
//							size_t whereIndex = querySql.find("where");
//							if (whereIndex == querySql.npos) {
//								querySql.append(" where ").append(where);
//							}
//							else {
//								querySql.append(" and ").append(where);
//							}
//						}
//					}
//
//					if (!group.empty()) {
//						querySql.append(" group by ").append(group);
//					}
//
//					if (!sort.empty()) {
//						querySql.append(" order by ").append(sort);
//					}
//
//					if (page > 0) {
//						page--;
//						querySql.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
//					}
//					return DbUtils::MakeJsonObject(STSUCCESS);
//				}
//				else {
//					return DbUtils::MakeJsonObject(STPARAMERR);
//				}
//			}
//
//			Json ExecQuerySql(string aQuery, vector<string> fields)
//			{
//				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
//				string err = "";
//				MYSQL *mysql = GetConnection(err);
//				if (mysql == nullptr)
//					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
//				if (mysql_query(mysql, aQuery.c_str()))
//				{
//					string errmsg = "";
//					errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
//					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
//					return rs;
//				}
//				else
//				{
//					MYSQL_RES *result = mysql_use_result(mysql);
//					if (result != NULL)
//					{
//						MYSQL_ROW row;
//						int num_fields = mysql_num_fields(result);
//						MYSQL_FIELD *fields = mysql_fetch_fields(result);
//						vector<Json> arr;
//						while ((row = mysql_fetch_row(result)) && row != NULL)
//						{
//							Json al;
//							for (int i = 0; i < num_fields; ++i)
//							{
//								if(IS_NUM(fields[i].type))
//									al.add(fields[i].name, atof(row[i]));
//								else
//									al.add(fields[i].name, row[i]);
//							}
//							arr.push_back(al);
//						}
//						if (arr.empty())
//							rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
//						rs.add("data", arr);
//					}
//					mysql_free_result(result);
//				}
//				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
//				return rs;
//			}
//
//			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
//				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
//				string err = "";
//				MYSQL* mysql = GetConnection(err);
//				if (mysql == nullptr)
//					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
//				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
//				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
//				{
//					string errmsg = "";
//					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
//					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
//					return rs;
//				}
//				else
//				{
//					const int vLen = values.size();
//					std::vector<char *> dataInputs;
//					if (vLen > 0)
//					{
//						MYSQL_BIND *bind = new MYSQL_BIND[vLen];
//						std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
//						dataInputs.resize(vLen);
//						for (int i = 0; i < vLen; i++)
//						{
//							string ele = values[i].toString();
//							int eleLen = ele.length() + 1;
//							dataInputs[i] = new char[eleLen];
//							memset(dataInputs[i], 0, eleLen);
//							memcpy(dataInputs[i], ele.c_str(), eleLen);
//							bind[i].buffer_type = MYSQL_TYPE_STRING;
//							bind[i].buffer = (void *)dataInputs[i];
//							bind[i].buffer_length = eleLen - 1;
//						}
//						if (mysql_stmt_bind_param(stmt, bind))
//						{
//							string errmsg = "";
//							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
//							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//							rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
//							return rs;
//						}
//						delete [] bind;
//					}
//
//					MYSQL_RES* prepare_meta_result = mysql_stmt_result_metadata(stmt);
//					MYSQL_FIELD* fields;
//					if (prepare_meta_result != nullptr)
//					{
//						int ret = 1;
//						mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, (void *)&ret);
//						if (mysql_stmt_execute(stmt))
//						{
//							string errmsg = "";
//							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
//							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//							rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
//							return rs;
//						}
//						ret = mysql_stmt_store_result(stmt);
//						int num_fields = mysql_num_fields(prepare_meta_result);
//						fields = mysql_fetch_fields(prepare_meta_result);
//						MYSQL_BIND *ps = new MYSQL_BIND[num_fields];
//						std::memset(ps, 0, sizeof(MYSQL_BIND) * num_fields);
//						std::vector<char *> dataOuts(num_fields);
//						char* is_null = new char[num_fields];
//						memset(is_null, 0, sizeof(char) * num_fields);
//						for (int i = 0; i < num_fields; ++i)
//						{
//							auto p = allocate_buffer_for_field(fields[i]);
//							dataOuts[i] = new char[p.size];
//							memset(dataOuts[i], 0, p.size);
//							ps[i].buffer_type = p.type;
//							ps[i].buffer = (void *)dataOuts[i];
//							ps[i].buffer_length = p.size;
//							ps[i].is_null = &is_null[i];
//						}
//						ret = mysql_stmt_bind_result(stmt, ps);
//						Json arr(JsonType::Array);
//						while (mysql_stmt_fetch(stmt) != MYSQL_NO_DATA)
//						{
//							Json al;
//							for (int i = 0; i < num_fields; ++i)
//							{
//								if (is_null[i])
//									al.add(fields[i].name, nullptr);
//								else if (fields[i].type == MYSQL_TYPE_LONG || fields[i].type == MYSQL_TYPE_LONGLONG)  //count
//									al.add(fields[i].name, (long)*((int *)dataOuts[i]));
//								else if (fields[i].type == MYSQL_TYPE_DOUBLE || fields[i].type == MYSQL_TYPE_NEWDECIMAL) //sum
//									al.add(fields[i].name, *((double *)dataOuts[i]));
//								else
//									al.add(fields[i].name, dataOuts[i]);
//							}
//							arr.push_back(al);
//						}
//						if (arr.size() == 0)
//							rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
//						rs.add("data", arr);
//						delete [] ps;
//						delete [] is_null;
//						for(auto el : dataOuts)
//							delete [] el;
//					}
//					for (auto el : dataInputs)
//						delete[] el;
//				}
//				mysql_stmt_close(stmt);
//				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
//				return rs;
//			}

			Json ExecNoneQuerySql(string aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				Dm8Con* con = GetConnection(err);
				if (con == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				DPIRETURN rt = dpi_exec_direct(con->hstmt, (sdbyte*)aQuery.c_str());
				if (!DSQL_SUCCEEDED(rt))
				{
					dpi_err_msg_print(DSQL_HANDLE_STMT, con->hstmt, err);
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);;
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery, Json values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err = "";
				Dm8Con* con = GetConnection(err);
				if (con == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
				DPIRETURN rt = dpi_prepare(con->hstmt, (sdbyte*)aQuery.c_str());
				if (!DSQL_SUCCEEDED(rt))
				{
					dpi_err_msg_print(DSQL_HANDLE_STMT, con->hstmt, err);
					return DbUtils::MakeJsonObject(STDBCONNECTERR, err);;
				}
				int vLen = values.size();
				if (vLen > 0)
				{
					std::vector<char*> dataInputs;
					dataInputs.resize(vLen); 
					std::vector<slength> in_ptrs;
					in_ptrs.resize(vLen);
					std::vector<double> in_dbs;
					in_dbs.resize(vLen);
					std::vector<int> in_ints;
					in_ints.resize(vLen);
					for (int i = 0; i < vLen; i++)
					{
						if (values[i].isString()) {
							string ele = values[i].toString();
							int eleLen = ele.length() + 1;
							dataInputs[i] = new char[eleLen];
							memset(dataInputs[i], 0, eleLen);
							memcpy(dataInputs[i], ele.c_str(), eleLen);
							in_ptrs[i] = eleLen - 1;
							rt = dpi_bind_param(con->hstmt, i + 1, 
								DSQL_PARAM_INPUT, DSQL_C_NCHAR, DSQL_VARCHAR, 
								in_ptrs[i], 0, (void*)dataInputs[i], in_ptrs[i], &in_ptrs[i]);
						}
						else {
							if (getDecimalCount(values[i].toDouble()) > 0) {
								in_dbs[i] = values[i].toDouble();
								in_ptrs[i] = sizeof(in_dbs[i]);
								rt = dpi_bind_param(con->hstmt, i + 1,
									DSQL_PARAM_INPUT, DSQL_C_DOUBLE, DSQL_DOUBLE,
									in_ptrs[i], 0, &in_dbs[i], in_ptrs[i], &in_ptrs[i]);
							}
							else {
								in_ints[i] = values[i].toInt();
								in_ptrs[i] = sizeof(in_ints[i]);
								rt = dpi_bind_param(con->hstmt, i + 1,
									DSQL_PARAM_INPUT, DSQL_C_SLONG, DSQL_INT,
									in_ptrs[i], 0, &in_ints[i], in_ptrs[i], &in_ptrs[i]);
							}
							
						}
					}
					rt = dpi_exec(con->hstmt);
					if (!DSQL_SUCCEEDED(rt))
					{
						dpi_err_msg_print(DSQL_HANDLE_STMT, con->hstmt, err);
						return DbUtils::MakeJsonObject(STDBCONNECTERR, err);;
					}
				}
				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

//			struct st_buffer_size_type
//			{
//				size_t size;
//				enum_field_types type;
//				st_buffer_size_type(size_t s, enum_field_types t) : size(s + 1), type(t) {}
//			};
//
//			
//			st_buffer_size_type allocate_buffer_for_field(const MYSQL_FIELD field)
//			{
//				switch (field.type)
//				{
//				case MYSQL_TYPE_NULL:
//					return st_buffer_size_type(0, field.type);
//				case MYSQL_TYPE_TINY:
//					return st_buffer_size_type(1, field.type);
//				case MYSQL_TYPE_SHORT:
//					return st_buffer_size_type(2, field.type);
//				case MYSQL_TYPE_INT24:
//				case MYSQL_TYPE_LONG:
//				case MYSQL_TYPE_FLOAT:
//					return st_buffer_size_type(4, field.type);
//				case MYSQL_TYPE_DOUBLE:
//				case MYSQL_TYPE_LONGLONG:
//					return st_buffer_size_type(8, field.type);
//				case MYSQL_TYPE_YEAR:
//					return st_buffer_size_type(2, MYSQL_TYPE_SHORT);
//				case MYSQL_TYPE_TIMESTAMP:
//				case MYSQL_TYPE_DATE:
//				case MYSQL_TYPE_TIME:
//				case MYSQL_TYPE_DATETIME:
//					return st_buffer_size_type(sizeof(MYSQL_TIME), field.type);
//
//				case MYSQL_TYPE_TINY_BLOB:
//				case MYSQL_TYPE_MEDIUM_BLOB:
//				case MYSQL_TYPE_LONG_BLOB:
//				case MYSQL_TYPE_BLOB:
//				case MYSQL_TYPE_STRING:
//				case MYSQL_TYPE_VAR_STRING:
//				case MYSQL_TYPE_JSON:{
//					return st_buffer_size_type(field.max_length, field.type);
//				}
//				case MYSQL_TYPE_DECIMAL:
//				case MYSQL_TYPE_NEWDECIMAL:
//					return st_buffer_size_type(64, field.type);
//#if A1
//				case MYSQL_TYPE_TIMESTAMP:
//				case MYSQL_TYPE_YEAR:
//					return st_buffer_size_type(10, field.type);
//#endif
//#if A0
//				case MYSQL_TYPE_ENUM:
//				case MYSQL_TYPE_SET:
//#endif
//				case MYSQL_TYPE_BIT:
//					return st_buffer_size_type(8, MYSQL_TYPE_BIT);
//				case MYSQL_TYPE_GEOMETRY:
//					return st_buffer_size_type(field.max_length, MYSQL_TYPE_BIT);
//				default:
//					return st_buffer_size_type(field.max_length, field.type);
//				}
//			};
//
//			bool ExecSqlForTransGo(string aQuery, Json values = Json(JsonType::Array), string* out = nullptr) {
//				string err = "";
//				MYSQL* mysql = GetConnection(err);
//				if (mysql == nullptr){
//					if(out)
//						*out += "can not connect the database.";
//					return false;
//				}
//
//				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
//				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
//				{
//					string errmsg = "";
//					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
//					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//					if(out)
//						*out += errmsg;
//					return false;
//				}
//				else {
//					int vLen = values.size();
//					std::vector<char *> dataInputs;
//					if (vLen > 0)
//					{
//						MYSQL_BIND *bind = new MYSQL_BIND[vLen];
//						std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
//						dataInputs.resize(vLen);
//						for (int i = 0; i < vLen; i++)
//						{
//							string ele = values[i].toString();
//							int eleLen = ele.length() + 1;
//							dataInputs[i] = new char[eleLen];
//							memset(dataInputs[i], 0, eleLen);
//							memcpy(dataInputs[i], ele.c_str(), eleLen);
//							bind[i].buffer_type = MYSQL_TYPE_STRING;
//							bind[i].buffer = (void *)dataInputs[i];
//							bind[i].buffer_length = eleLen - 1;
//						}
//						if (mysql_stmt_bind_param(stmt, bind))
//						{
//							string errmsg = "";
//							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
//							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//							if (out)
//								*out += errmsg;
//							return false;
//						}
//						delete [] bind;
//					}
//					if (mysql_stmt_execute(stmt))
//					{
//						string errmsg = "";
//						errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
//						errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
//						if (out)
//							*out += errmsg;
//						return false;
//					}
//					for (auto el : dataInputs)
//						delete[] el;
//				}
//				!DbLogClose && std::cout << "SQL: " << aQuery << std::endl;
//				return true;
//			}

			int getDecimalCount(double data) {
				data = std::abs(data);
				data -= (int)data;
				int ct = 0;
				double minValue = 0.0000000001;
				while (!(std::abs(data - 1) < minValue || std::abs(data) < minValue) && ct < 11) {
					data *= 10;
					data -= (int)data;
					ct++;
					minValue *= 10;
				}
				return ct;
			}

			bool escapeString(string& dest)
			{
				string sql = dest;
				dest = "";
				char escape;
				for (auto character : sql) {
					switch (character) {
					case 0: /* Must be escaped for 'mysql' */
						escape = '0';
						break;
					case '\n': /* Must be escaped for logs */
						escape = 'n';
						break;
					case '\r':
						escape = 'r';
						break;
					case '\\':
						escape = '\\';
						break;
					case '\'':
						escape = '\'';
						break;
					case '"': /* Better safe than sorry */
						escape = '"';
						break;
					case '\032': /* This gives problems on Win32 */
						escape = 'Z';
						break;
					default:
						escape = 0;
					}
					if (escape != 0) {
						dest += '\\';
						dest += escape;
					}
					else {
						dest += character;
					}
				}
				return true;
			}

		private:
			vector<Dm8Con*> pool;
			int maxConn;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string charsetName;
			bool DbLogClose;
			bool queryByParameter;
		};

	}

}