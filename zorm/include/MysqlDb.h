#pragma once

#include "Idb.h"
#include "Utils.h"
#include "GlobalConstants.h"
#include "mysql.h"
#include <algorithm>

using namespace std;

namespace Mysql {

	const int MAXCONN = 2;
	char* QUERY_EXTRA_KEYS[] = { "ins", "lks", "ors" };
	char* QUERY_UNEQ_OPERS[] = { ">,", ">=,", "<,", "<=,", "<>,", "=," };

	class ZORM_API MysqlDb : public Idb {

	private:
		MYSQL* GetConnection() {
			//srand((unsigned int)(time(NULL)));
			size_t index = (rand() % maxConn) + 1;
			if (index > pool.size()) {
				MYSQL* pmysql;
				pmysql = mysql_init((MYSQL*)NULL);
				if (pmysql != NULL)
				{
					if (mysql_real_connect(pmysql, dbhost.c_str(), dbuser.c_str(), dbpwd.c_str(), dbname.c_str(), dbport, NULL, 0))
					{
						pool.push_back(pmysql);
						return pmysql;
					}
				}
				return nullptr;
			}
			else {
				return pool.at(index - 1);
			}
		}

	public:

		MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(3306), maxConn(MAXCONN)
		{

		}

		MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport, int maxConn) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), maxConn(maxConn)
		{

		}

		MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport), maxConn(MAXCONN)
		{

		}

		// Json create(string tablename, Json& params) override
		// {
		// 	if (params.IsObject()) {
		// 		string execSql = "insert into ";
		// 		execSql.append(tablename).append(" ");

		// 		vector<string> allKeys = params.GetAllKeys();
		// 		size_t len = allKeys.size();
		// 		string fields = "", vs = "";
		// 		for (size_t i = 0; i < len; i++) {
		// 			string key = allKeys[i];
		// 			fields.append(key);
		// 			string v;
		// 			int vType;
		// 			params.GetValueAndTypeByKey(key, &v, &vType);
		// 			if (vType == 6)
		// 				vs.append(v);
		// 			else
		// 				vs.append("'").append(v).append("'");
		// 			if (i < len - 1) {
		// 				fields.append(",");
		// 				vs.append(",");
		// 			}
		// 		}
		// 		execSql.append("(").append(fields).append(") values (").append(vs).append(")");
		// 		return ExecNoneQuerySql(execSql);
		// 	}
		// 	else {
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
		// 	}
		// }


		// Json update(string tablename, Json& params) override
		// {
		// 	if (params.IsObject()) {
		// 		string execSql = "update ";
		// 		execSql.append(tablename).append(" set ");

		// 		vector<string> allKeys = params.GetAllKeys();

		// 		vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
		// 		if (iter == allKeys.end()) {
		// 			return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
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
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
		// 	}
		// }


		// Json remove(string tablename, Json& params) override
		// {
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
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
		// 	}
		// }


		Json select(string tablename, Json& params, vector<string> fields = vector<string>(), int queryType = 1) override
		{
			if (!params.isError()) {
				string querySql = "";
				string where = "";
				const string AndJoinStr = " and ";
				string fieldsJoinStr = "*";

				if (!fields.empty()) {
					fieldsJoinStr = DbUtils::GetVectorJoinStr(fields);
				}

				// string fuzzy = params.GetStringValueAndRemove("fuzzy");
				// string sort = params.GetStringValueAndRemove("sort");
				// int page = atoi(params.GetStringValueAndRemove("page").c_str());
				// int size = atoi(params.GetStringValueAndRemove("size").c_str());
				// string sum = params.GetStringValueAndRemove("sum");
				// string count = params.GetStringValueAndRemove("count");
				// string group = params.GetStringValueAndRemove("group");

				// vector<string> allKeys = params.GetAllKeys();
				// size_t len = allKeys.size();
				// for (size_t i = 0; i < len; i++) {
				// 	string k = allKeys[i];
				// 	string v;
				// 	int vType;
				// 	params.GetValueAndTypeByKey(k, &v, &vType);
				// 	if (where.length() > 0) {
				// 		where.append(AndJoinStr);
				// 	}

				// 	if (DbUtils::FindCharArray(QUERY_EXTRA_KEYS, (char*)k.c_str())) {   // process key
				// 		string whereExtra = "";
				// 		vector<string> ele = DbUtils::MakeVectorInitFromString(params[k]);
				// 		if (ele.size() < 2 || ((k.compare("ors") == 0 || k.compare("lks") == 0) && ele.size() % 2 == 1)) {
				// 			return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR, k + " is wrong.");
				// 		}
				// 		else {
				// 			if (k.compare("ins") == 0) {
				// 				string c = ele.at(0);
				// 				vector<string>(ele.begin() + 1, ele.end()).swap(ele);
				// 				whereExtra.append(c).append(" in ( ").append(DbUtils::GetVectorJoinStr(ele)).append(" )");
				// 			}
				// 			else if (k.compare("lks") == 0 || k.compare("ors") == 0) {
				// 				whereExtra.append(" ( ");
				// 				for (size_t j = 0; j < ele.size(); j += 2) {
				// 					if (j > 0) {
				// 						whereExtra.append(" or ");
				// 					}
				// 					whereExtra.append(ele.at(j)).append(" ");
				// 					string eqStr = k.compare("lks") == 0 ? " like '" : " = '";
				// 					string vsStr = ele.at(j + 1);
				// 					if (k.compare("lks") == 0) {
				// 						vsStr.insert(0, "%");
				// 						vsStr.append("%");
				// 					}
				// 					vsStr.append("'");
				// 					whereExtra.append(eqStr).append(vsStr);
				// 				}
				// 				whereExtra.append(" ) ");
				// 			}
				// 		}
				// 		where.append(whereExtra);
				// 	}
				// 	else {				// process value
				// 		if (DbUtils::FindStartsCharArray(QUERY_UNEQ_OPERS, (char*)v.c_str())) {
				// 			vector<string> vls = DbUtils::MakeVectorInitFromString(v);
				// 			if (vls.size() == 2) {
				// 				where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
				// 			}
				// 			else if (vls.size() == 4) {
				// 				where.append(k).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
				// 				where.append(k).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
				// 			}
				// 			else {
				// 				return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR, "not equal value is wrong.");
				// 			}
				// 		}
				// 		else if (!fuzzy.empty() && vType == QJsonValue::String) {
				// 			where.append(k).append(" like '%").append(v).append("%'");
				// 		}
				// 		else {
				// 			if (vType == QJsonValue::Double)
				// 				where.append(k).append(" = ").append(v);
				// 			else
				// 				where.append(k).append(" = '").append(v).append("'");
				// 		}
				// 	}
				// }

				// string extra = "";
				// if (!sum.empty()) {
				// 	vector<string> ele = DbUtils::MakeVectorInitFromString(sum);
				// 	if (ele.empty() || ele.size() % 2 == 1)
				// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR, "sum is wrong.");
				// 	else {
				// 		for (size_t i = 0; i < ele.size(); i += 2) {
				// 			extra.append(",sum(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
				// 		}
				// 	}
				// }
				// if (!count.empty()) {
				// 	vector<string> ele = DbUtils::MakeVectorInitFromString(count);
				// 	if (ele.empty() || ele.size() % 2 == 1)
				// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR, "count is wrong.");
				// 	else {
				// 		for (size_t i = 0; i < ele.size(); i += 2) {
				// 			extra.append(",count(").append(ele.at(i)).append(") as ").append(ele.at(i + 1)).append(" ");
				// 		}
				// 	}
				// }

				// if (queryType == 1) {
				// 	querySql.append("select ").append(fieldsJoinStr).append(extra).append(" from ").append(tablename);
				// 	if (where.length() > 0)
				// 		querySql.append(" where ").append(where);
				// }
				// else {
				// 	querySql.append(tablename);
				// 	if (!fields.empty()) {
				// 		size_t starIndex = querySql.find('*');
				// 		if (starIndex < 10) {
				// 			querySql.replace(starIndex, 1, fieldsJoinStr.c_str());
				// 		}
				// 	}
				// 	if (where.length() > 0) {
				// 		size_t whereIndex = querySql.find("where");
				// 		if (whereIndex == querySql.npos) {
				// 			querySql.append(" where ").append(where);
				// 		}
				// 		else {
				// 			querySql.append(" and ").append(where);
				// 		}
				// 	}
				// }

				// if (!group.empty()) {
				// 	querySql.append(" group by ").append(group);
				// }

				// if (!sort.empty()) {
				// 	querySql.append(" order by ").append(sort);
				// }

				// if (page > 0) {
				// 	page--;
				// 	querySql.append(" limit ").append(DbUtils::IntTransToString(page * size)).append(",").append(DbUtils::IntTransToString(size));
				// }

				querySql = "select * from tasks";

				return ExecQuerySql(querySql, fields);
			}
			else {
				return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
			}
		}

		// Json querySql(string sql, Json& params = Json(), vector<string> filelds = vector<string>()) override
		// {
		// 	return select(sql, params, filelds, 2);
		// }


		// Json execSql(string sql) override
		// {
		// 	return ExecNoneQuerySql(sql);
		// }


		// Json insertBatch(string tablename, vector<Json> elements, string constraint) override
		// {
		// 	string sql = "insert into ";
		// 	if (elements.empty()) {
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
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
		// }


		// Json transGo(vector<string> sqls, bool isAsync = false) override
		// {
		// 	if (sqls.empty()) {
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STPARAMERR);
		// 	}
		// 	else {
		// 		bool isExecSuccess = true;
		// 		string errmsg = "Running transaction error: ";
		// 		MYSQL* mysql = GetConnection();
		// 		if (mysql == nullptr)
		// 			return DbUtils::MakeJsonObjectForFuncReturn(STDBCONNECTERR);

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
		// 			return DbUtils::MakeJsonObjectForFuncReturn(STSUCCESS, "Transaction success.");
		// 		}
		// 		else
		// 		{
		// 			mysql_query(mysql, "rollback;");
		// 			return DbUtils::MakeJsonObjectForFuncReturn(STDBOPERATEERR, errmsg);
		// 		}
		// 	}
		// }

		~MysqlDb()
		{
			while (pool.size())
			{
				mysql_close(pool.back());
				pool.pop_back();
			}
		}

	private:

		Json ExecQuerySql(string aQuery, vector<string> fields) {
			Json rs = DbUtils::MakeJsonObjectForFuncReturn(STSUCCESS);
			string u8Query = DbUtils::UnicodeToU8(aQuery);
			MYSQL* mysql = GetConnection();
			if (mysql == nullptr)
				return DbUtils::MakeJsonObjectForFuncReturn(STDBCONNECTERR);
			if (mysql_query(mysql, u8Query.c_str()))
			{
				string errmsg = "";
				errmsg.append(DbUtils::U8ToUnicode((char*)mysql_error(mysql))).append(". error code: ");
				errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
				return rs.extend(DbUtils::MakeJsonObjectForFuncReturn(STDBOPERATEERR, errmsg));
			}
			else
			{
				MYSQL_RES* result = mysql_use_result(mysql);
				if (result != NULL)
				{
					MYSQL_ROW row;
					int num_fields = mysql_num_fields(result);
					MYSQL_FIELD* fields = mysql_fetch_fields(result);
					vector<Json> arr;
					while ((row = mysql_fetch_row(result)) && row != NULL)
					{
						Json al;
						for (int i = 0; i < num_fields; ++i)
						{
							al.addSubitem(fields[i].name, DbUtils::U8ToUnicode(row[i]));
						}
						arr.push_back(al);
					}
					if (arr.empty())
						rs.extend(DbUtils::MakeJsonObjectForFuncReturn(STQUERYEMPTY));
					rs.addSubitem("data", arr);
				}
				mysql_free_result(result);
			}
			cout << "SQL: " << aQuery << endl;
			return rs;
		}

		// Json ExecNoneQuerySql(string aQuery) {
		// 	Json rs = DbUtils::MakeJsonObjectForFuncReturn(STSUCCESS);
		// 	string u8Query = DbUtils::UnicodeToU8(aQuery);
		// 	MYSQL* mysql = GetConnection();
		// 	if (mysql == nullptr)
		// 		return DbUtils::MakeJsonObjectForFuncReturn(STDBCONNECTERR);

		// 	if (mysql_query(mysql, u8Query.c_str()))
		// 	{
		// 		string errmsg = "";
		// 		errmsg.append(DbUtils::U8ToUnicode((char*)mysql_error(mysql))).append(". error code: ");
		// 		errmsg.append(DbUtils::IntTransToString(mysql_errno(mysql)));
		// 		return rs.ExtendObject(DbUtils::MakeJsonObjectForFuncReturn(STDBOPERATEERR, errmsg));
		// 	}
		// 	else {
		// 		int affected = (int)mysql_affected_rows(mysql);
		// 		int newId = (int)mysql_insert_id(mysql);
		// 		rs.AddValueInt("affected", affected);
		// 		rs.AddValueInt("newId", newId);
		// 	}
		// 	cout << "SQL: " << aQuery << endl;
		// 	return rs;
		// }

	private:
		vector<MYSQL*> pool;
		int maxConn;
		string dbhost;
		string dbuser;
		string dbpwd;
		string dbname;
		int dbport;
	};

}