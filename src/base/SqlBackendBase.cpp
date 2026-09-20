#include "SqlBackendBase.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ZORM {

// ─────────────────────────────────────────────────────────────────────────
// Idb.h method skeletons (identical for every SQL backend)
// ─────────────────────────────────────────────────────────────────────────

Json SqlBackendBase::create(const string& tablename, const Json& params) {
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
	// Upsert parity: a provided id overwrites the row (gels/orm/jsonfile
	// semantics). The dialect supplies the clause.
	const Json providedId = params["id"];
	if (!providedId.isError() && !DbUtils::Trim(providedId.toString()).empty()) {
		vector<string> keys = DbUtils::GetVectorFromJson(params.getAllKeys());
		const string upsert = upsertClause("id", keys);
		if (!upsert.empty())
			sql += upsert;
	}
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	Json rs = conn->execNone(sql, values);
	if (rs["status"].toInt() != STSUCCESS)
		return rs;
	const string id = generatedId.empty() ? params["id"].toString() : generatedId;
	if (!id.empty()) {
		rs.add("id", id);
		rs.add("insertId", id);
	}
	rs.add("affectedRows", 1);
	return rs;
}

Json SqlBackendBase::update(const string& tablename, const Json& params) {
	if (params.isError())
		return DbUtils::MakeJsonObject(STPARAMERR);
	string sql;
	Json values(JsonType::Array);
	if (!buildUpdateSql(tablename, params, sql, values))
		return DbUtils::MakeJsonObject(STPARAMERR);
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	Json rs = conn->execNone(sql, values);
	if (rs["status"].toInt() != STSUCCESS)
		return rs;
	if (!rs["affected"].isError())
		rs.add("affectedRows", rs["affected"].toInt());
	else
		rs.add("affectedRows", 1);
	return rs;
}

Json SqlBackendBase::remove(const string& tablename, const Json& params) {
	if (params.isError())
		return DbUtils::MakeJsonObject(STPARAMERR);
	string sql;
	Json values(JsonType::Array);
	if (!buildDeleteSql(tablename, params, sql, values))
		return DbUtils::MakeJsonObject(STPARAMERR);
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	Json rs = conn->execNone(sql, values);
	if (rs["status"].toInt() != STSUCCESS)
		return rs;
	if (!rs["affected"].isError())
		rs.add("affectedRows", rs["affected"].toInt());
	else
		rs.add("affectedRows", 1);
	return rs;
}

Json SqlBackendBase::select(const string& tbname, const Json& params,
							vector<string> fields, Json values) {
	string sql = tbname;  // genSql mutates it into the finished statement
	string countSql;
	string err;
	Json rs = genSql(sql, values, params, fields, 1, queryByParameter, &countSql);
	if (rs["status"].toInt() != 200)
		return rs;
	// One lease for the main query AND the records count (O-3): both run
	// on the same connection so they observe the same snapshot.
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	Json result = conn->execQuery(sql, fields, values);
	if (result["status"].toInt() == 200)
		attachRecordsPages(conn, result, params, countSql, values);
	return result;
}

Json SqlBackendBase::querySql(const string& sqlstr, Json params,
							  Json values, vector<string> fields) {
	string sql(sqlstr);
	const bool parameterized = detectParameterized(sql);
	Json rs = genSql(sql, values, params, fields, 2, parameterized);
	if (rs["status"].toInt() != 200)
		return rs;
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	return conn->execQuery(sql, fields, values);
}

Json SqlBackendBase::execSql(const string& sqlstr, Json params, Json values) {
	string sql(sqlstr);
	const bool parameterized = detectParameterized(sql);
	Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
	if (rs["status"].toInt() != 200)
		return rs;
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	return conn->execNone(sql, values);
}

Json SqlBackendBase::insertBatch(const string& tablename, const Json& elements, string constraint) {
	if (!elements.isArray() || elements.size() < 1)
		return DbUtils::MakeJsonObject(STPARAMERR);
	const vector<string> keys0 = DbUtils::GetVectorFromJson(elements[0].getAllKeys());
	const vector<string> restrain = DbUtils::MakeVector(constraint);
	string sql = "insert into " + qualifiedTable(tablename);
	string keyStr = " ( ";
	string updateStr;
	keyStr.append(columnList(keys0)).append(" ) values ");
	resetPlaceholders();
	Json values(JsonType::Array);
	for (int i = 0; i < elements.size(); i++) {
		vector<string> keys = DbUtils::GetVectorFromJson(elements[i].getAllKeys());
		string valueStr = " ( ";
		for (size_t j = 0; j < keys.size(); j++) {
			if (i == 0) {
				auto iter = std::find(restrain.begin(), restrain.end(), keys[j]);
				if (iter == restrain.end())
					updateStr.append(keys[j]).append(" = ").append(excludedRef(keys[j])).append(",");
			}
			bool vIsString = elements[i][keys[j]].isString() || elements[i][keys[j]].isArray() || elements[i][keys[j]].isObject();
			string v = elements[i][keys[j]].toString();
			!queryByParameter && vIsString && escapeString(v);
			if (queryByParameter) {
				valueStr.append(placeholder(nextPlaceholder()));
				values.add(v);
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
		if (i < elements.size() - 1)
			valueStr.append(",");
		keyStr.append(valueStr);
	}
	// Upsert parity (O-6): duplicate ids update the row on every backend.
	const string upsert = upsertClause(constraint, keys0);
	if (updateStr.length() == 0 || upsert.empty())
		sql.append(keyStr);
	else {
		updateStr = updateStr.substr(0, updateStr.length() - 1);
		sql.append(keyStr).append(upsert);
	}
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	Json rs = conn->execNone(sql, values);
	if (rs["status"].toInt() == STSUCCESS)
		rs.add("affectedRows", elements.size());
	return rs;
}

Json SqlBackendBase::transGo(const Json& sqls, bool isAsync) {
	(void)isAsync;
	if (!sqls.isArray() || sqls.size() == 0)
		return DbUtils::MakeJsonObject(STPARAMERR);
	string err;
	Lease lease = acquireConnection(err);
	IDbConnection* conn = lease.get();
	if (conn == nullptr)
		return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
	if (!conn->beginTx()) {
		conn->rollbackTx();
		return DbUtils::MakeJsonObject(STDBOPERATEERR, "begin transaction failed");
	}
	for (int i = 0; i < sqls.size(); i++) {
		const Json element = sqls[i];
		const bool hasText = ZJSON::hasChild(element, "text");
		const bool hasSql = !hasText && ZJSON::hasChild(element, "sql");
		Json values = element["values"].isError() ? Json(JsonType::Array) : element["values"];
		string sql;
		if (hasText || hasSql) {
			sql = (hasText ? element["text"] : element["sql"]).toString();
		} else {
			const string table = element["table"].toString();
			const string method = element["method"].toString();
			const Json params = element["params"];
			const Json idValue = element["id"];
			if (!buildStructuredSql(table, method, params, !idValue.isError(), idValue, sql, values)) {
				conn->rollbackTx();
				return DbUtils::MakeJsonObject(STDBOPERATEERR, "transaction element is wrong.");
			}
		}
		resetPlaceholders();
		if (!conn->execTx(sql, values, &err)) {
			conn->rollbackTx();
			return DbUtils::MakeJsonObject(STDBOPERATEERR, "Running transaction error: " + err);
		}
	}
	if (!conn->commitTx()) {
		conn->rollbackTx();
		return DbUtils::MakeJsonObject(STDBOPERATEERR, "commit failed");
	}
	if (!DbLogClose)
		std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
	return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success.");
}

// ─────────────────────────────────────────────────────────────────────────
// Dialect hook defaults
// ─────────────────────────────────────────────────────────────────────────

std::string SqlBackendBase::placeholder(int) {
	return "?";
}
bool SqlBackendBase::numberedPlaceholders() {
	return false;
}
std::string SqlBackendBase::quoteIdent(const std::string& name) {
	return name;
}
std::string SqlBackendBase::qualifiedTable(const std::string& name) {
	return name;
}
std::string SqlBackendBase::likeColumn(const std::string& name) {
	return name;
}
std::string SqlBackendBase::orderClause(const std::string& sort) {
	return sort;
}
std::string SqlBackendBase::limitClause(int offset, int size) {
	return " limit " + DbUtils::IntTransToString(offset) + "," + DbUtils::IntTransToString(size);
}
std::string SqlBackendBase::aggColumn(const std::string& src) {
	return src;
}
std::string SqlBackendBase::aggAlias(const std::string& alias) {
	return alias;
}
std::string SqlBackendBase::countAliasSql() {
	return countAlias_;
}
std::string SqlBackendBase::columnList(const vector<string>& keys) {
	return DbUtils::GetVectorJoinStr(keys);
}
std::string SqlBackendBase::fieldsProjection(const vector<string>& fields) {
	return DbUtils::GetVectorJoinStr(fields);
}
std::string SqlBackendBase::excludedRefImpl(const std::string& column) {
	return "excluded." + column;
}
std::string SqlBackendBase::upsertClause(const std::string& constraint,
										 const vector<string>& keys) {
	std::string clause;
	for (const std::string& k : keys) {
		if (k == constraint)
			continue;
		if (!clause.empty())
			clause += ",";
		clause += k + " = " + excludedRefImpl(k);
	}
	return clause.empty() ? "" : " on conflict (" + constraint + ") do update set " + clause;
}
bool SqlBackendBase::detectParameterized(const std::string& sql) {
	return sql.find("?") != std::string::npos;
}
bool SqlBackendBase::escapeString(string& pStr) {
	std::string escaped;
	escaped.reserve(pStr.size() + 8);
	for (const char ch : pStr) {
		if (ch == '\'')
			escaped += "''";
		else
			escaped += ch;
	}
	pStr = escaped;
	return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Shared state
// ─────────────────────────────────────────────────────────────────────────

int SqlBackendBase::nextPlaceholder() {
	return placeholderIndex_++;
}
void SqlBackendBase::resetPlaceholders() {
	placeholderIndex_ = 1;
}

// ─────────────────────────────────────────────────────────────────────────
// Statement builders (identical for every backend except placeholders)
// ─────────────────────────────────────────────────────────────────────────

bool SqlBackendBase::buildInsertSql(const string& tablename, const Json& params,
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
	sql = "insert into " + qualifiedTable(tablename) + " (";
	resetPlaceholders();
	string vs;
	for (size_t i = 0; i < allKeys.size(); i++) {
		const string k = allKeys[i];
		sql.append(quoteIdent(k));
		const bool vIsString = row[k].isString() || row[k].isArray() || row[k].isObject();
		string v = row[k].toString();
		!queryByParameter && vIsString && escapeString(v);
		if (queryByParameter) {
			vs.append(placeholder(nextPlaceholder()));
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

bool SqlBackendBase::buildUpdateSql(const string& tablename, const Json& params,
									string& sql, Json& values) {
	if (!params.isObject())
		return false;
	vector<string> allKeys = DbUtils::GetVectorFromJson(params.getAllKeys());
	vector<string>::iterator iter = find(allKeys.begin(), allKeys.end(), "id");
	if (iter == allKeys.end())
		return false;
	// O-5 parity: an update carrying only the id (no columns) is rejected.
	if (allKeys.size() < 2)
		return false;
	sql = "update " + qualifiedTable(tablename) + " set ";
	string where = " where " + quoteIdent("id") + " = ";
	Json idJson;
	values = Json(JsonType::Array);
	resetPlaceholders();
	bool first = true;
	for (size_t i = 0; i < allKeys.size(); i++) {
		const string k = allKeys[i];
		if (k.compare("id") == 0) {
			idJson = params[k];
			continue;
		}
		const bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
		string v = params[k].toString();
		!queryByParameter && vIsString && escapeString(v);
		if (!first)
			sql.append(",");
		first = false;
		sql.append(quoteIdent(k)).append(" = ");
		if (queryByParameter) {
			sql.append(placeholder(nextPlaceholder()));
			vIsString ? values.add(v) : values.add(params[k].toDouble());
		} else {
			if (vIsString)
				sql.append("'").append(v).append("'");
			else
				sql.append(v);
		}
	}
	if (queryByParameter) {
		where.append(placeholder(nextPlaceholder()));
		values.concat(idJson);
	} else {
		const bool vIsString = idJson.isString() || idJson.isArray() || idJson.isObject();
		if (vIsString)
			where.append("'").append(idJson.toString()).append("'");
		else
			where.append(idJson.toString());
	}
	sql.append(where);
	return true;
}

bool SqlBackendBase::buildDeleteSql(const string& tablename, const Json& params,
									string& sql, Json& values) {
	if (!params.isObject())
		return false;
	const Json id = params["id"];
	if (id.isError())
		return false;
	sql = "delete from " + qualifiedTable(tablename) + " where " + quoteIdent("id") + " = ";
	values = Json(JsonType::Array);
	const bool vIsString = id.isString() || id.isArray() || id.isObject();
	resetPlaceholders();
	if (queryByParameter) {
		sql.append(placeholder(nextPlaceholder()));
		vIsString ? values.add(id.toString()) : values.add(id.toDouble());
	} else {
		if (vIsString)
			sql.append("'").append(id.toString()).append("'");
		else
			sql.append(id.toString());
	}
	return true;
}

bool SqlBackendBase::buildStructuredSql(const string& table, const string& method,
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
		resetPlaceholders();
		const vector<string> keys0 = DbUtils::GetVectorFromJson(params[0].getAllKeys());
		string keyStr = " ( ";
		string updateStr;
		keyStr.append(columnList(keys0)).append(" ) values ");
		for (int i = 0; i < params.size(); i++) {
			vector<string> keys = DbUtils::GetVectorFromJson(params[i].getAllKeys());
			string valueStr = " ( ";
			for (size_t j = 0; j < keys.size(); j++) {
				if (i == 0) {
					vector<string>::iterator iter = find(restrain_.begin(), restrain_.end(), keys[j]);
					if (iter == restrain_.end())
						updateStr.append(keys[j]).append(" = ").append(excludedRef(keys[j])).append(",");
				}
				bool vIsString = params[i][keys[j]].isString() || params[i][keys[j]].isArray() || params[i][keys[j]].isObject();
				string v = params[i][keys[j]].toString();
				!queryByParameter && vIsString && escapeString(v);
				if (queryByParameter) {
					valueStr.append(placeholder(nextPlaceholder()));
					values.add(v);
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
		const string upsert = upsertClause("id", keys0);
		if (updateStr.length() == 0 || upsert.empty())
			sql = "insert into " + qualifiedTable(table) + keyStr;
		else
			sql = "insert into " + qualifiedTable(table) + keyStr + upsert;
		return true;
	}
	return false;
}

// ─────────────────────────────────────────────────────────────────────────
// Smart query (genSql): WHERE assembly + aggregates + pagination
// ─────────────────────────────────────────────────────────────────────────

Json SqlBackendBase::genSql(string& querySql, Json& values, const Json& ps,
							vector<string> fields, int queryType, bool parameterized,
							string* countSql) {
	if (ps.isError())
		return DbUtils::MakeJsonObject(STPARAMERR);
	Json params(ps);
	const string rawname = querySql;
	querySql = "";
	string where;
	const string AndJoinStr = " and ";
	string fieldsJoinStr = "*";

	if (!fields.empty()) {
		fieldsJoinStr = fieldsProjection(fields);
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
	resetPlaceholders();
	if (numberedPlaceholders() && queryType != 1) {
		// A user-supplied statement may already carry $n placeholders -
		// numbering continues after them.
		placeholderIndex_ = static_cast<int>(
			std::count(rawname.begin(), rawname.end(), '$')) + 1;
	}
	for (size_t i = 0; i < len; i++) {
		string k = allKeys[i];
		bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
		string v = params[k].toString();
		!parameterized && vIsString && escapeString(v);
		if (where.length() > 0)
			where.append(" and ");

		if (DbUtils::FindStringFromVector(QUERY_EXTRA_KEYS, k)) {  // reserved key
			string whereExtra;
			vector<string> ele = DbUtils::MakeVector(params[k].toString());
			if (ele.size() < 2 || ((k.compare("ors") == 0 || k.compare("lks") == 0) && ele.size() % 2 == 1)) {
				return DbUtils::MakeJsonObject(STPARAMERR, k + " is wrong.");
			}
			if (k.compare("ins") == 0) {
				string c = ele.at(0);
				vector<string>(ele.begin() + 1, ele.end()).swap(ele);
				if (parameterized) {
					whereExtra.append(quoteIdent(c)).append(" in (");
					const int eleLen = ele.size();
					for (int e = 0; e < eleLen; e++) {
						whereExtra.append(placeholder(nextPlaceholder()));
						if (e < eleLen - 1)
							whereExtra.append(",");
						values.add(ele[e]);
					}
					whereExtra.append(")");
				} else {
					// non-parameterized: literal IN list
					whereExtra.append(quoteIdent(c)).append(" in ( ")
						.append(DbUtils::GetVectorJoinStr(ele)).append(" )");
				}
			} else {  // lks / ors
				whereExtra.append(" ( ");
				for (size_t j = 0; j < ele.size(); j += 2) {
					if (j > 0)
						whereExtra.append(" or ");
					whereExtra.append(likeColumn(ele.at(j))).append(" ");
					const string ph = placeholder(nextPlaceholder());
					string eqStr = parameterized
						? (k.compare("lks") == 0 ? " like " + ph : " = " + ph)
						: (k.compare("lks") == 0 ? " like '" : " = '");
					string vsStr = ele.at(j + 1);
					if (k.compare("lks") == 0) {
						vsStr.insert(0, "%");
						vsStr.append("%");
					}
					whereExtra.append(eqStr);
					if (parameterized)
						values.add(vsStr);
					else {
						vsStr.append("'");
						whereExtra.append(vsStr);
					}
				}
				whereExtra.append(" ) ");
			}
			where.append(whereExtra);
		} else {  // value condition
			if (DbUtils::FindStartsStringFromVector(QUERY_UNEQ_OPERS, v)) {
				vector<string> vls = DbUtils::MakeVector(v);
				if (vls.size() == 2) {
					if (parameterized) {
						where.append(quoteIdent(k)).append(vls.at(0)).append(placeholder(nextPlaceholder())).append(" ");
						values.add(vls.at(1));
					} else {
						where.append(quoteIdent(k)).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
					}
				} else if (vls.size() == 4) {
					if (parameterized) {
						where.append(quoteIdent(k)).append(vls.at(0)).append(placeholder(nextPlaceholder())).append(" and ");
						where.append(quoteIdent(k)).append(vls.at(2)).append(placeholder(nextPlaceholder())).append(" ");
						values.add(vls.at(1));
						values.add(vls.at(3));
					} else {
						where.append(quoteIdent(k)).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
						where.append(quoteIdent(k)).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
					}
				} else {
					return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
				}
			} else if (fuzzy == "1") {
				if (parameterized) {
					where.append(likeColumn(k)).append(" like ").append(placeholder(nextPlaceholder())).append(" ");
					values.add(v.insert(0, "%").append("%"));
				} else {
					where.append(likeColumn(k)).append(" like '%").append(v).append("%'");
				}
			} else {
				if (parameterized) {
					where.append(quoteIdent(k)).append(" = ").append(placeholder(nextPlaceholder())).append(" ");
					vIsString ? values.add(v) : values.add(params[k].toDouble());
				} else {
					if (vIsString)
						where.append(quoteIdent(k)).append(" = '").append(v).append("'");
					else
						where.append(quoteIdent(k)).append(" = ").append(v);
				}
			}
		}
	}

	string extra;
	if (!sum.empty()) {
		vector<string> ele = DbUtils::MakeVector(sum);
		if (ele.empty() || ele.size() % 2 == 1)
			return DbUtils::MakeJsonObject(STPARAMERR, "sum is wrong.");
		for (size_t i = 0; i < ele.size(); i += 2)
			extra.append("sum(").append(aggColumn(ele.at(i))).append(") as ").append(aggAlias(ele.at(i + 1))).append(" ");
	}
	if (!count.empty()) {
		vector<string> ele = DbUtils::MakeVector(count);
		if (ele.empty() || ele.size() % 2 == 1)
			return DbUtils::MakeJsonObject(STPARAMERR, "count is wrong.");
		for (size_t i = 0; i < ele.size(); i += 2)
			extra.append("count(").append(aggColumn(ele.at(i))).append(") as ").append(aggAlias(ele.at(i + 1))).append(" ");
	}

	if (queryType == 1) {
		const bool hasAgg = !extra.empty();
		if (hasAgg) {
			// Aggregate queries must produce engine-portable SQL: project
			// the group columns when grouping ("select age,count(*) ...
			// group by age", dm8 parity) or the aggregates alone.
			// Requested `fields` are ignored here - bare columns alongside
			// aggregates are invalid on strict engines (pg / MySQL
			// ONLY_FULL_GROUP_BY).
			if (!group.empty()) {
				vector<string> gs = DbUtils::MakeVector(group);
				for (size_t g = 0; g < gs.size(); g++)
					gs[g] = quoteIdent(gs[g]);
				fieldsJoinStr = DbUtils::GetVectorJoinStr(gs);
			} else {
				fieldsJoinStr = "";
			}
		} else if (!group.empty() && fields.empty()) {
			// Grouping without fields: project the group columns
			// (valid under ONLY_FULL_GROUP_BY, dm8 parity).
			vector<string> gs = DbUtils::MakeVector(group);
			for (size_t g = 0; g < gs.size(); g++)
				gs[g] = quoteIdent(gs[g]);
			fieldsJoinStr = DbUtils::GetVectorJoinStr(gs);
		}
		querySql.append("select ").append(fieldsJoinStr);
		if (hasAgg)
			querySql.append(fieldsJoinStr.empty() ? "" : ",").append(extra);
		querySql.append(" from ").append(qualifiedTable(rawname));
		if (where.length() > 0)
			querySql.append(" where ").append(where);
	} else {
		querySql.append(rawname);
		if (queryType == 2 && !fields.empty()) {
			size_t starIndex = querySql.find('*');
			if (starIndex < 10)
				querySql.replace(starIndex, 1, fieldsJoinStr.c_str());
		}
		if (where.length() > 0) {
			size_t whereIndex = querySql.find("where");
			if (whereIndex == querySql.npos)
				querySql.append(" where ").append(where);
			else
				querySql.append(" and ").append(where);
		}
	}

	if (!group.empty()) {
		vector<string> gs = DbUtils::MakeVector(group);
		for (size_t g = 0; g < gs.size(); g++)
			gs[g] = quoteIdent(gs[g]);
		querySql.append(" group by ").append(DbUtils::GetVectorJoinStr(gs));
	}

	if (countSql != nullptr && queryType == 1 && page > 0) {
		// Built from the known parts (O-4) instead of re-parsing the
		// finished statement; grouped queries count the groups via a
		// wrapped subquery so records == number of groups.
		const string wherePart = where.length() > 0 ? " where " + where : "";
		string groupPart;
		if (!group.empty()) {
			vector<string> gs = DbUtils::MakeVector(group);
			for (size_t g = 0; g < gs.size(); g++)
				gs[g] = quoteIdent(gs[g]);
			groupPart = " group by " + DbUtils::GetVectorJoinStr(gs);
		}
		if (group.empty())
			*countSql = "select count(1) as " + countAliasSql() + " from " + qualifiedTable(rawname) + wherePart;
		else
			*countSql = "select count(1) as " + countAliasSql() + " from (select * from " + qualifiedTable(rawname) + wherePart + groupPart + ") zorm_cnt";
	}

	if (!sort.empty())
		querySql.append(" order by ").append(orderClause(sort));

	if (page > 0) {
		page--;
		querySql.append(limitClause(page * size, size));
	}
	return DbUtils::MakeJsonObject(STSUCCESS);
}

// ─────────────────────────────────────────────────────────────────────────
// Pagination counters (run on the caller's leased connection - O-3)
// ─────────────────────────────────────────────────────────────────────────

void SqlBackendBase::attachRecordsPages(IDbConnection* conn, Json& result, const Json& params,
										const string& countSql, Json& values) {
	long long records = -1;
	const int page = atoi(params["page"].toString().c_str());
	const int size = atoi(params["size"].toString().c_str());
	if (page > 0 && size > 0 && !countSql.empty())
		records = runCountQueryOn(conn, countSql, values);
	if (records < 0)
		records = result["data"].size();
	result.add("records", records);
	result.add("pages", (page > 0 && size > 0)
		? (records == 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(records) / size)))
		: (records > 0 ? 1 : 0));
}

long long SqlBackendBase::runCountQueryOn(IDbConnection* conn, const string& countSql, Json& values) {
	if (countSql.empty())
		return -1;
	Json rs = conn->execQuery(countSql, vector<string>(), values);
	if (rs["status"].toInt() != 200 || rs["data"].size() == 0)
		return -1;
	return static_cast<long long>(rs["data"][0][countAlias_].toDouble());
}

}  // namespace ZORM
