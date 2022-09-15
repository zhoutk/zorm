#pragma once

#include "Idb.h"
#include "Utils.h"
#include "GlobalConstants.h"
#include "mysql.h"
#include <algorithm>
#include <cstring>

namespace ZORM {

	using namespace std;

	namespace Mysql {
		#define MAX_RES_FIELDS 50
		#define MAX_FIELD_DATA_SIZE 255

		vector<string> QUERY_EXTRA_KEYS;
		vector<string> QUERY_UNEQ_OPERS;

		class ZORM_API MysqlDb : public Idb {

		private:
			MYSQL* GetConnection() {
				//srand((unsigned int)(time(nullptr)));
				size_t index = (rand() % maxConn) + 1;
				if (index > pool.size()) {
					MYSQL* pmysql;
					pmysql = mysql_init((MYSQL*)nullptr);
					if (pmysql != nullptr)
					{
					 	!charsetName.empty() && mysql_options(pmysql, MYSQL_SET_CHARSET_NAME, charsetName.c_str());
						if (mysql_real_connect(pmysql, dbhost.c_str(), dbuser.c_str(), dbpwd.c_str(), dbname.c_str(), dbport, nullptr, 0))
						{
							pool.push_back(pmysql);
							return pmysql;
						}else{
							std::cout << "Error message : " << mysql_error(pmysql);
						}

					}
					return nullptr;
				}
				else {
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
			}

		public:

			MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 3306, Json options = Json()) :
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), queryByParameter(false)
			{
				init();

				if(!options["db_conn"].isError())
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
				if(!options["query_parameter"].isError())
					queryByParameter = options["query_parameter"].toBool();
			}

			Json create(string tablename, Json& params) override
			{
				if (!params.isError()) {
					string execSql = "insert into ";
					execSql.append(tablename).append(" ");

					vector<string> allKeys = params.getAllKeys();
					size_t len = allKeys.size();
					string fields = "", vs = "";
					for (size_t i = 0; i < len; i++) {
						string key = allKeys[i];
						fields.append(key);
						Json v = params[key];
						if (v.isString())
							vs.append("'").append(v.toString()).append("'");
						else
							vs.append(v.toString());
						if (i < len - 1) {
							fields.append(",");
							vs.append(",");
						}
					}
					execSql.append("(").append(fields).append(") values (").append(vs).append(")");
					return ExecNoneQuerySql(execSql);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}


			Json update(string tablename, Json& params) override
			{
			// 	if (params.IsObject()) {
			// 		string execSql = "update ";
			// 		execSql.append(tablename).append(" set ");

			// 		vector<string> allKeys = params.GetAllKeys();

			// 		vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
			// 		if (iter == allKeys.end()) {
			// 			return DbUtils::MakeJsonObject(STPARAMERR);
			// 		}
			// 		else {
			// 			size_t len = allKeys.size();
			// 			size_t conditionLen = len - 2;
			// 			string fields = "", where = " where id = ";
			// 			for (size_t i = 0; i < len; i++) {
			// 				string key = allKeys[i];
			// 				string v;
			// 				int vType;
			// 				params.GetValueAndTypeByKey(key, &v, &vType);
			// 				if (key.compare("id") == 0) {
			// 					conditionLen++;
			// 					if (vType == 6)
			// 						where.append(v);
			// 					else
			// 						where.append("'").append(v).append("'");
			// 				}
			// 				else {
			// 					fields.append(key).append(" = ");
			// 					if (vType == 6)
			// 						fields.append(v);
			// 					else
			// 						fields.append("'").append(v).append("'");
			// 					if (i < conditionLen) {
			// 						fields.append(",");
			// 					}
			// 				}
			// 			}
			// 			execSql.append(fields).append(where);
			// 			return ExecNoneQuerySql(execSql);
			// 		}
			// 	}
			// 	else {
					return DbUtils::MakeJsonObject(STPARAMERR);
			// 	}
			}


			Json remove(string tablename, Json& params) override
			{
			// 	if (params.IsObject()) {
			// 		string execSql = "delete from ";
			// 		execSql.append(tablename).append(" where id = ");

			// 		string v;
			// 		int vType;
			// 		params.GetValueAndTypeByKey("id", &v, &vType);

			// 		if (vType == 6)
			// 			execSql.append(v);
			// 		else
			// 			execSql.append("'").append(v).append("'");

			// 		return ExecNoneQuerySql(execSql);
			// 	}
			// 	else {
					return DbUtils::MakeJsonObject(STPARAMERR);
			// 	}
			}


			Json select(string tablename, Json &params, vector<string> fields = vector<string>(), Json values = Json(JsonType::Array), int queryType = 1) override
			{
				if (!params.isError()) {
					string querySql = "";
					string querySqlByParameter = "";
					string where = "";
					string whereByParameter = "";
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
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						Json v = params[k];
						if (where.length() > 0) {
							where.append(AndJoinStr);
						}

						if (DbUtils::FindStringFromVector(QUERY_EXTRA_KEYS, k)) {   // process key
							string whereExtra = "";
							string whereExtraByParameter = "";
							vector<string> ele = DbUtils::MakeVector(params[k].toString());
							if (ele.size() < 2 || ((k.compare("ors") == 0 || k.compare("lks") == 0) && ele.size() % 2 == 1)) {
								return DbUtils::MakeJsonObject(STPARAMERR, k + " is wrong.");
							}
							else {
								if (k.compare("ins") == 0) {
									string c = ele.at(0);
									vector<string>(ele.begin() + 1, ele.end()).swap(ele);
									whereExtra.append(c).append(" in ( ").append(DbUtils::GetVectorJoinStr(ele)).append(" )");

									whereExtraByParameter.append(c).append(" in (");
									int eleLen = ele.size();
									for(int i = 0; i < eleLen; i++){
										string el = ele[i];
										whereExtraByParameter.append("?");
										if(i < eleLen - 1)
											whereExtraByParameter.append(",");
										values.addSubitem(el);
									}
									whereExtraByParameter.append(")");
								}
								else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
									whereExtra.append(" ( ");
									for (size_t j = 0; j < ele.size(); j += 2) {
										if (j > 0) {
											whereExtra.append(" or ");
											whereExtraByParameter.append(" or ");
										}
										whereExtra.append(ele.at(j)).append(" ");
										whereExtraByParameter.append(ele.at(j)).append(" ");
										string eqStr = k.compare("lks") == 0 ? " like '" : " = '";
										string eqStrByParameter = k.compare("lks") == 0 ? " like ?" : " = ?";
										string vsStr = ele.at(j + 1);
										if (k.compare("lks") == 0) {
											vsStr.insert(0, "%");
											vsStr.append("%");
										}
										vsStr.append("'");
										whereExtra.append(eqStr).append(vsStr);
										whereExtraByParameter.append(eqStrByParameter);
										values.addSubitem(vsStr);
									}
									whereExtra.append(" ) ");
									whereExtraByParameter.append(" ) ");
								}
							}
							where.append(whereExtra);
							whereByParameter.append(whereExtraByParameter);
						}
						else {				// process value
							if (DbUtils::FindStringFromVector(QUERY_UNEQ_OPERS, v.toString())) {
								vector<string> vls = DbUtils::MakeVector(v.toString());
								if (vls.size() == 2) {
									where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
									whereByParameter.append(k).append(vls.at(0)).append(" ? ");
									values.addSubitem(vls.at(1));
								}
								else if (vls.size() == 4) {
									where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
									where.append(k).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
									whereByParameter.append(k).append(vls.at(0)).append(" ? ").append("and ");
									whereByParameter.append(k).append(vls.at(2)).append("? ");
									values.addSubitem(vls.at(1));
									values.addSubitem(vls.at(3));
								}
								else {
									return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
								}
							}
							else if (fuzzy == "1") {
								where.append(k).append(" like '%").append(v.toString()).append("%'");
								whereByParameter.append(k).append(" like ? ");
								values.addSubitem(v.toString().insert(0, "%").append("%"));
							}
							else {
								if(v.isString())
									where.append(k).append(" = '").append(v.toString()).append("'");
								else
									where.append(k).append(" = ").append(v.toString());

								whereByParameter.append(k).append(" = ? ");
								values.addSubitem(v);
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
						querySqlByParameter = querySql;
						if (where.length() > 0){
							querySql.append(" where ").append(where);
							querySqlByParameter.append(" where ").append(whereByParameter);
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
						querySqlByParameter = querySql;
						if (where.length() > 0) {
							size_t whereIndex = querySql.find("where");
							if (whereIndex == querySql.npos) {
								querySql.append(" where ").append(where);
								querySqlByParameter.append(" where ").append(whereByParameter);
							}
							else {
								querySql.append(" and ").append(where);
								querySqlByParameter.append(" and ").append(whereByParameter);
							}
						}
					}

					if (!group.empty()) {
						querySql.append(" group by ").append(group);
						querySqlByParameter.append(" group by ").append(group);
					}

					if (!sort.empty()) {
						querySql.append(" order by ").append(sort);
						querySqlByParameter.append(" order by ").append(sort);
					}

					if (page > 0) {
						page--;
						querySql.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
						querySqlByParameter.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
					}
					if(queryByParameter)
						return queryType == 3 ? ExecNoneQuerySql(querySqlByParameter, values) : ExecQuerySql(querySqlByParameter, fields, values);
					else
						return queryType == 3 ? ExecNoneQuerySql(querySql) : ExecQuerySql(querySql, fields);
				}
				else {
					return DbUtils::MakeJsonObject(STPARAMERR);
				}
			}

			Json querySql(string sql, Json params = Json(), Json values = Json(JsonType::Array), vector<string> filelds = vector<string>()) override
			{
				return select(sql, params, filelds, values, 2);
			}


			Json execSql(string sql, Json params = Json(), Json values = Json(JsonType::Array)) override
			{
				return select(sql, params, vector<string>(), values, 3);
			}


			Json insertBatch(string tablename, Json& elements, string constraint) override
			{
			// 	string sql = "insert into ";
			// 	if (elements.empty()) {
					return DbUtils::MakeJsonObject(STPARAMERR);
			// 	}
			// 	else {
			// 		string keyStr = " ( ";
			// 		string updateStr = "";
			// 		keyStr.append(DbUtils::GetVectorJoinStr(elements[0].GetAllKeys())).append(" ) values ");
			// 		for (size_t i = 0; i < elements.size(); i++) {
			// 			vector<string> keys = elements[i].GetAllKeys();
			// 			string valueStr = " ( ";
			// 			for (size_t j = 0; j < keys.size(); j++) {
			// 				if(i == 0)
			// 					updateStr.append(keys[j]).append(" = values(").append(keys[j]).append(")");
			// 				valueStr.append("'").append(elements[i][keys[j]]).append("'");
			// 				if (j < keys.size() - 1) {
			// 					valueStr.append(",");
			// 					if (i == 0)
			// 						updateStr.append(",");
			// 				}
			// 			}
			// 			valueStr.append(" )");
			// 			if (i < elements.size() - 1) {
			// 				valueStr.append(",");
			// 			}
			// 			keyStr.append(valueStr);
			// 		}
			// 		sql.append(tablename).append(keyStr).append(" on duplicate key update ").append(updateStr);
			// 	}
			// 	return ExecNoneQuerySql(sql);
			}


			Json transGo(Json& sqls, bool isAsync = false) override
			{
			// 	if (sqls.empty()) {
					return DbUtils::MakeJsonObject(STPARAMERR);
			// 	}
			// 	else {
			// 		bool isExecSuccess = true;
			// 		string errmsg = "Running transaction error: ";
			// 		MYSQL* mysql = GetConnection();
			// 		if (mysql == nullptr)
			// 			return DbUtils::MakeJsonObject(STDBCONNECTERR);

			// 		mysql_query(mysql, "begin;");
			// 		for (size_t i = 0; i < sqls.size(); i++) {
			// 			string u8Query = DbUtils::UnicodeToU8(sqls[i]);
			// 			if (mysql_query(mysql, u8Query.c_str()))
			// 			{
			// 				isExecSuccess = false;
			// 				errmsg.append(DbUtils::U8ToUnicode((char*)mysql_error(mysql))).append(". error code: ");
			// 				errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
			// 				cout << errmsg << endl;
			// 				break;
			// 			}
			// 		}
			// 		if (isExecSuccess)
			// 		{
			// 			mysql_query(mysql, "commit;");
			// 			cout << "Transaction Success: run " << sqls.size() << " sqls." << endl;
			// 			return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success.");
			// 		}
			// 		else
			// 		{
			// 			mysql_query(mysql, "rollback;");
			// 			return DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg);
			// 		}
			// 	}
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
			Json ExecQuerySql(string aQuery, vector<string> fields)
			{
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				MYSQL *mysql = GetConnection();
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR);
				if (mysql_query(mysql, aQuery.c_str()))
				{
					string errmsg = "";
					errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
				}
				else
				{
					MYSQL_RES *result = mysql_use_result(mysql);
					if (result != NULL)
					{
						MYSQL_ROW row;
						int num_fields = mysql_num_fields(result);
						MYSQL_FIELD *fields = mysql_fetch_fields(result);
						vector<Json> arr;
						while ((row = mysql_fetch_row(result)) && row != NULL)
						{
							Json al;
							for (int i = 0; i < num_fields; ++i)
							{
								al.addSubitem(fields[i].name, row[i]);
							}
							arr.push_back(al);
						}
						if (arr.empty())
							rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
						rs.addSubitem("data", arr);
					}
					mysql_free_result(result);
				}
				cout << "SQL: " << aQuery << endl;
				return rs;
			}

			Json ExecQuerySql(string aQuery, vector<string> fields, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				MYSQL* mysql = GetConnection();
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR);
				MYSQL_STMT* stmt = mysql_stmt_init(mysql);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
				}
				else
				{
					int vLen = values.size();
					MYSQL_BIND bind[vLen];
					std::memset(bind, 0, sizeof(bind));
					if (vLen > 0)
					{
						for (int i = 0; i < vLen; i++)
						{
							char* ele = (char*)values[i].toString().c_str();
							bind[i].buffer_type = MYSQL_TYPE_STRING;
							bind[i].buffer = (char *)ele;
							bind[i].buffer_length = std::strlen(ele);
							
						}
						if (mysql_stmt_bind_param(stmt, bind))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
						}
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
							return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
						}
						ret = mysql_stmt_store_result(stmt);
						MYSQL_ROW row;
						 int num_fields = mysql_num_fields(prepare_meta_result);
						fields = mysql_fetch_fields(prepare_meta_result);
						MYSQL_BIND ps[num_fields];
						std::memset(ps, 0, sizeof(ps));
						std::vector<char *> data(num_fields);
						char is_null[num_fields];
						memset(is_null, 0, sizeof(is_null));
						for (int i = 0; i < num_fields; ++i)
						{
							auto p = allocate_buffer_for_field(fields[i]);
							data[i] = new char[p.size];
							memset(data[i], 0, p.size);
							ps[i].buffer_type = p.type;
							ps[i].buffer = (void *)data[i];
							ps[i].buffer_length = p.size;
							ps[i].is_null = &is_null[i];
						}
						ret = mysql_stmt_bind_result(stmt, ps);
						vector<Json> arr;
						while (mysql_stmt_fetch(stmt) != MYSQL_NO_DATA)
						{
							Json al;
							for (int i = 0; i < num_fields; ++i)
							{
								if (is_null[i])
									al.addSubitem(fields[i].name, nullptr);
								else if (fields[i].type == MYSQL_TYPE_LONG)
									al.addSubitem(fields[i].name, (long)*((int *)data[i]));
								else if (fields[i].type == MYSQL_TYPE_DOUBLE)
									al.addSubitem(fields[i].name, *((double *)data[i]));
								else
									al.addSubitem(fields[i].name, data[i]);
							}
							arr.push_back(al);
						}
						rs.addSubitem("data", arr);
						for(auto el : data)
							delete [] el;
					}
				}
				mysql_stmt_close(stmt);
				cout << "SQL: " << aQuery << endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				MYSQL* mysql = GetConnection();
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR);

				if (mysql_query(mysql, aQuery.c_str()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
				}
				else {
					int affected = (int)mysql_affected_rows(mysql);
					int newId = (int)mysql_insert_id(mysql);
					rs.addSubitem("affected", affected);
					rs.addSubitem("newId", newId);
				}
				cout << "SQL: " << aQuery << endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery, Json values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				MYSQL* mysql = GetConnection();
				if (mysql == nullptr)
					return DbUtils::MakeJsonObject(STDBCONNECTERR);

				if (mysql_query(mysql, aQuery.c_str()))
				{
					string errmsg = "";
					errmsg.append((char*)mysql_error(mysql)).append(". error code: ");
					errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
					return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
				}
				else {
					int affected = (int)mysql_affected_rows(mysql);
					int newId = (int)mysql_insert_id(mysql);
					rs.addSubitem("affected", affected);
					rs.addSubitem("newId", newId);
				}
				cout << "SQL: " << aQuery << endl;
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

			vector<Json> my_process_stmt_result(MYSQL_STMT *stmt)
			{
				vector<Json> arr;

				int field_count;
				int row_count = 0;
				MYSQL_BIND buffer[MAX_RES_FIELDS];
				MYSQL_FIELD *field;
				MYSQL_RES *result;
				char data[MAX_RES_FIELDS][MAX_FIELD_DATA_SIZE];
				ulong length[MAX_RES_FIELDS];
				my_bool is_null[MAX_RES_FIELDS];
				int rc, i;

				if (!(result = mysql_stmt_result_metadata(stmt))) /* No meta info */
				{
					while (!mysql_stmt_fetch(stmt))
						row_count++;
					return arr;
				}
				rc = 1;
				mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, (void *)&rc);
				mysql_stmt_execute(stmt);
				rc = mysql_stmt_store_result(stmt);
				field = mysql_fetch_fields(result);

				field_count = std::min((int)mysql_num_fields(result), MAX_RES_FIELDS);

				memset(buffer, 0, sizeof(buffer));
				memset(length, 0, sizeof(length));
				memset(is_null, 0, sizeof(is_null));

				for (i = 0; i < field_count; i++)
				{
					buffer[i].buffer_type = MYSQL_TYPE_STRING;
					buffer[i].buffer_length = MAX_FIELD_DATA_SIZE;
					buffer[i].length = &length[i];
					buffer[i].buffer = (void *)data[i];
					buffer[i].is_null = &is_null[i];
				}

				rc = mysql_stmt_bind_result(stmt, buffer);
				mysql_field_seek(result, 0);
				while ((rc = mysql_stmt_fetch(stmt)) == 0)
				{
					mysql_field_seek(result, 0);
					Json al;
					for (i = 0; i < field_count; i++)
					{
						field = mysql_fetch_field(result);
						if (is_null[i])
							al.addSubitem(field->name, nullptr);
						else if (length[i] == 0)
						{
							data[i][0] = '\0'; /* unmodified buffer */
							fprintf(stdout, " %*s |", (int)field->max_length, data[i]);
						}
						else if (IS_NUM(field->type))
							al.addSubitem(field->name, atof(data[i]));
							//fprintf(stdout, " %*s |", (int)field->max_length, data[i]);
						else
							al.addSubitem(field->name, data[i]);
							//fprintf(stdout, " %-*s |", (int)field->max_length, data[i]);
					}
					arr.push_back(al);
				}
				mysql_free_result(result);
				return arr;
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
			bool queryByParameter;
		};

	}

}