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
				dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport)
			{
				init();

				if(!options["db_conn"].isError())
					maxConn = options["db_conn"].toInt();
				if(!options["db_char"].isError())
					charsetName = options["db_char"].toString();
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
					for (size_t i = 0; i < len; i++) {
						string k = allKeys[i];
						string v = params[k].toString();
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
									whereExtra.append(c).append(" in ( ").append(DbUtils::GetVectorJoinStr(ele)).append(" )");
								}
								else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
									whereExtra.append(" ( ");
									for (size_t j = 0; j < ele.size(); j += 2) {
										if (j > 0) {
											whereExtra.append(" or ");
										}
										whereExtra.append(ele.at(j)).append(" ");
										string eqStr = k.compare("lks") == 0 ? " like '" : " = '";
										string vsStr = ele.at(j + 1);
										if (k.compare("lks") == 0) {
											vsStr.insert(0, "%");
											vsStr.append("%");
										}
										vsStr.append("'");
										whereExtra.append(eqStr).append(vsStr);
									}
									whereExtra.append(" ) ");
								}
							}
							where.append(whereExtra);
						}
						else {				// process value
							if (DbUtils::FindStringFromVector(QUERY_UNEQ_OPERS, v)) {
								vector<string> vls = DbUtils::MakeVector(v);
								if (vls.size() == 2) {
									where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
								}
								else if (vls.size() == 4) {
									where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
									where.append(k).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
								}
								else {
									return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
								}
							}
							else if (fuzzy == "1") {
								where.append(k).append(" like '%").append(v).append("%'");
							}
							else {
								where.append(k).append(" = '").append(v).append("'");
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

					if (page > 0) {
						page--;
						querySql.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
					}
					return queryType == 3 ? ExecNoneQuerySql(querySql, values) : ExecQuerySql(querySql, fields, values);
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
					if (vLen > 0)
					{
						for (int i = 0; i < vLen; i++)
						{
							string ele = values[i].toString();
							bind[i].buffer_type = MYSQL_TYPE_STRING;
							bind[i].buffer = (char *)ele.c_str();
							bind[i].buffer_length = ele.length();
							
						}
						if (mysql_stmt_bind_param(stmt, bind))
						{
							string errmsg = "";
							errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
							errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
							return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
						}
					}
					if (mysql_stmt_execute(stmt))
					{
						string errmsg = "";
						errmsg.append((char *)mysql_error(mysql)).append(". error code: ");
						errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
						return rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errmsg));
					}
					MYSQL_RES* prepare_meta_result = mysql_stmt_result_metadata(stmt);
					int num_fields;
					MYSQL_FIELD* fields;
					if (prepare_meta_result != nullptr)
					{
						MYSQL_ROW row;
						num_fields = mysql_num_fields(prepare_meta_result);
						fields = mysql_fetch_fields(prepare_meta_result);
						MYSQL_BIND ps[num_fields];
						std::memset(ps, 0, sizeof(ps));
						for (int i = 0; i < num_fields; ++i)
						{
							if (IS_NUM(fields[i].type)){
								ps[i].buffer_type = MYSQL_TYPE_LONG;
								ps[i].length = 0;
							}
							else{
								ps[i].buffer_type = MYSQL_TYPE_STRING;
								ps[i].length = 0;
							}
						}
						int ret = mysql_stmt_bind_result(stmt, ps);
						std::cout << mysql_stmt_error(stmt);
						int d = ret;
					}
					int result = mysql_stmt_store_result(stmt);
					while (!mysql_stmt_fetch(stmt)){
						int dd = 1;
					}
					// if (result != nullptr)
					// {
					// 	MYSQL_ROW row;
					// 	int num_fields = mysql_num_fields(result);
					// 	MYSQL_FIELD* fields = mysql_fetch_fields(result);
					// 	vector<Json> arr;
					// 	while ((row = mysql_fetch_row(result)) && row != nullptr)
					// 	{
					// 		Json al;
					// 		for (int i = 0; i < num_fields; ++i)
					// 		{
					// 			if(IS_NUM(fields[i].type))
					// 				al.addSubitem(fields[i].name, atof(row[i]));
					// 			else 
					// 				al.addSubitem(fields[i].name, row[i]);
					// 		}
					// 		arr.push_back(al);
					// 	}
					// 	if (arr.empty())
					// 		rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
					// 	rs.addSubitem("data", arr);
					// }
					// mysql_free_result(result);
				}
				mysql_stmt_close(stmt);
				cout << "SQL: " << aQuery << endl;
				return rs;
			}

			Json ExecNoneQuerySql(string aQuery, Json values = Json(JsonType::Array)) {
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

		private:
			vector<MYSQL*> pool;
			int maxConn;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string charsetName;
		};

	}

}