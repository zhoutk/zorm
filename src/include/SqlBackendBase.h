#pragma once

// SqlBackendBase - shared algorithm layer for the SQL backends.
//
// A gels-inspired baseDao/sqlDialect split, expressed with C++17 idioms that
// fit zorm's single-header-per-backend style:
//   * the statement builders, smart-query (genSql) assembly, pagination
//     counters, transaction loop and Idb.h method skeletons live ONCE here;
//   * each backend is still a complete single header that inherits this base
//     and supplies its dialect + driver through CRTP hooks (zero virtual
//     dispatch for the internals, one vtable for Idb);
//   * connections come from DbPool::HandlePool with exclusive RAII leases
//     (O-2), so a select() runs its main query and the records count on the
//     same connection (O-3) and a transaction owns its session end-to-end.
//
// Hooks the derived backend must provide (CRTP, no virtuals):
//   using Handle;                                       // driver handle type
//   using Lease  = DbPool::HandlePool<Handle>::Lease;
//   Lease acquireHandle(std::string& err);              // exclusive lease
//   Json execQueryOn(Handle, const string&, const vector<string>&, Json&);
//   Json execNoneOn(Handle, const string&, Json&);
//   bool execTxOn(Handle, const string&, Json&, std::string* err);
//   bool beginTx(Handle); bool commitTx(Handle); void rollbackTx(Handle);
//   std::string placeholder(int index);                 // "?" or "$index"
//   bool numberedPlaceholders();                        // postgres: $1..$n
//   std::string quoteIdent(const std::string&);         // dm8 quotes ids
//   std::string qualifiedTable(const std::string&);
//   std::string likeColumn(const std::string&);         // pg CAST(x as TEXT)
//   std::string orderClause(const std::string& sort);   // dm8 quotes tokens
//   std::string limitClause(int offset, int size);
//   std::string upsertClause(const std::string& constraint,
//                            const std::vector<std::string>& keys);
//   std::string aggColumn(const std::string& src);      // aggregate argument
//   std::string aggAlias(const std::string& alias);     // aggregate alias
//   std::string countAliasSql();                        // count-alias SQL text
//   std::string fieldsProjection(const vector<string>& fields);
//   bool detectParameterized(const std::string& sql);
//   bool escapeString(std::string&);

#include "Idb.h"
#include "DbUtils.h"
#include "DbPool.h"
#include "GlobalConstants.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace ZORM {

template <typename Derived, typename Handle>
class SqlBackendBase : public Idb {
public:
	using Lease = typename DbPool::HandlePool<Handle>::Lease;

	// ─────────────────────────────────────────────────────────────────────
	// Idb.h method skeletons (identical for every SQL backend)
	// ─────────────────────────────────────────────────────────────────────

	Json create(const string& tablename, const Json& params) override {
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
			const string upsert = self()->upsertClause("id", keys);
			if (!upsert.empty())
				sql += upsert;
		}
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		Json rs = self()->execNoneOn(handle, sql, values);
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

	Json update(const string& tablename, const Json& params) override {
		if (params.isError())
			return DbUtils::MakeJsonObject(STPARAMERR);
		string sql;
		Json values(JsonType::Array);
		if (!buildUpdateSql(tablename, params, sql, values))
			return DbUtils::MakeJsonObject(STPARAMERR);
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		Json rs = self()->execNoneOn(handle, sql, values);
		if (rs["status"].toInt() != STSUCCESS)
			return rs;
		if (!rs["affected"].isError())
			rs.add("affectedRows", rs["affected"].toInt());
		else
			rs.add("affectedRows", 1);
		return rs;
	}

	Json remove(const string& tablename, const Json& params) override {
		if (params.isError())
			return DbUtils::MakeJsonObject(STPARAMERR);
		string sql;
		Json values(JsonType::Array);
		if (!buildDeleteSql(tablename, params, sql, values))
			return DbUtils::MakeJsonObject(STPARAMERR);
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		Json rs = self()->execNoneOn(handle, sql, values);
		if (rs["status"].toInt() != STSUCCESS)
			return rs;
		if (!rs["affected"].isError())
			rs.add("affectedRows", rs["affected"].toInt());
		else
			rs.add("affectedRows", 1);
		return rs;
	}

	Json select(const string& tbname, const Json& params,
				vector<string> fields = vector<string>(),
				Json values = Json(JsonType::Array)) override {
		string sql = tbname;  // genSql mutates it into the finished statement
		string countSql;
		string err;
		Json rs = genSql(sql, values, params, fields, 1, queryByParameter, &countSql);
		if (rs["status"].toInt() != 200)
			return rs;
		// One lease for the main query AND the records count (O-3): both run
		// on the same connection so they observe the same snapshot.
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		Json result = self()->execQueryOn(handle, sql, fields, values);
		if (result["status"].toInt() == 200)
			attachRecordsPages(handle, result, params, countSql, values);
		return result;
	}

	Json querySql(const string& sqlstr, Json params = Json(),
				  Json values = Json(JsonType::Array),
				  vector<string> fields = vector<string>()) override {
		string sql(sqlstr);
		const bool parameterized = self()->detectParameterized(sql);
		Json rs = genSql(sql, values, params, fields, 2, parameterized);
		if (rs["status"].toInt() != 200)
			return rs;
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		return self()->execQueryOn(handle, sql, fields, values);
	}

	Json execSql(const string& sqlstr, Json params = Json(),
				 Json values = Json(JsonType::Array)) override {
		string sql(sqlstr);
		const bool parameterized = self()->detectParameterized(sql);
		Json rs = genSql(sql, values, params, std::vector<string>(), 3, parameterized);
		if (rs["status"].toInt() != 200)
			return rs;
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		return self()->execNoneOn(handle, sql, values);
	}

	Json insertBatch(const string& tablename, const Json& elements, string constraint) override {
		if (!elements.isArray() || elements.size() < 1)
			return DbUtils::MakeJsonObject(STPARAMERR);
		const vector<string> keys0 = DbUtils::GetVectorFromJson(elements[0].getAllKeys());
		const vector<string> restrain = DbUtils::MakeVector(constraint);
		string sql = "insert into " + self()->qualifiedTable(tablename);
		string keyStr = " ( ";
		string updateStr;
		keyStr.append(self()->columnList(keys0)).append(" ) values ");
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
				!queryByParameter && vIsString && self()->escapeString(v);
				if (queryByParameter) {
					valueStr.append(self()->placeholder(nextPlaceholder()));
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
		const string upsert = self()->upsertClause(constraint, keys0);
		if (updateStr.length() == 0 || upsert.empty())
			sql.append(keyStr);
		else {
			updateStr = updateStr.substr(0, updateStr.length() - 1);
			sql.append(keyStr).append(upsert);
		}
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		Json rs = self()->execNoneOn(handle, sql, values);
		if (rs["status"].toInt() == STSUCCESS)
			rs.add("affectedRows", elements.size());
		return rs;
	}

	Json transGo(const Json& sqls, bool isAsync = false) override {
		(void)isAsync;
		if (!sqls.isArray() || sqls.size() == 0)
			return DbUtils::MakeJsonObject(STPARAMERR);
		string err;
		Lease lease = self()->acquireHandle(err);
		Handle handle = lease.get();
		if (handle == nullptr)
			return DbUtils::MakeJsonObject(STDBCONNECTERR, err);
		if (!self()->beginTx(handle)) {
			self()->rollbackTx(handle);
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
					self()->rollbackTx(handle);
					return DbUtils::MakeJsonObject(STDBOPERATEERR, "transaction element is wrong.");
				}
			}
			resetPlaceholders();
			if (!self()->execTxOn(handle, sql, values, &err)) {
				self()->rollbackTx(handle);
				return DbUtils::MakeJsonObject(STDBOPERATEERR, "Running transaction error: " + err);
			}
		}
		if (!self()->commitTx(handle)) {
			self()->rollbackTx(handle);
			return DbUtils::MakeJsonObject(STDBOPERATEERR, "commit failed");
		}
		if (!DbLogClose)
			std::cout << "Transaction Success: run " << sqls.size() << " sqls." << std::endl;
		return DbUtils::MakeJsonObject(STSUCCESS, "Transaction success.");
	}

protected:
	// ─────────────────────────────────────────────────────────────────────
	// Shared state
	// ─────────────────────────────────────────────────────────────────────
	const vector<string> QUERY_EXTRA_KEYS{"ins", "lks", "ors"};
	const vector<string> QUERY_UNEQ_OPERS{">,", ">=,", "<,", "<=,", "<>,", "=,"};
	bool DbLogClose = false;
	bool queryByParameter = false;
	std::string countAlias_ = "_zorm_total";
	vector<string> restrain_{"id"};

	int nextPlaceholder() {
		return placeholderIndex_++;
	}
	void resetPlaceholders() {
		placeholderIndex_ = 1;
	}

	// ─────────────────────────────────────────────────────────────────────
	// Statement builders (identical for every backend except placeholders)
	// ─────────────────────────────────────────────────────────────────────

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
		sql = "insert into " + self()->qualifiedTable(tablename) + " (";
		resetPlaceholders();
		string vs;
		for (size_t i = 0; i < allKeys.size(); i++) {
			const string k = allKeys[i];
			sql.append(self()->quoteIdent(k));
			const bool vIsString = row[k].isString() || row[k].isArray() || row[k].isObject();
			string v = row[k].toString();
			!queryByParameter && vIsString && self()->escapeString(v);
			if (queryByParameter) {
				vs.append(self()->placeholder(nextPlaceholder()));
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
		// O-5 parity: an update carrying only the id (no columns) is rejected.
		if (allKeys.size() < 2)
			return false;
		sql = "update " + self()->qualifiedTable(tablename) + " set ";
		string where = " where " + self()->quoteIdent("id") + " = ";
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
			!queryByParameter && vIsString && self()->escapeString(v);
			if (!first)
				sql.append(",");
			first = false;
			sql.append(self()->quoteIdent(k)).append(" = ");
			if (queryByParameter) {
				sql.append(self()->placeholder(nextPlaceholder()));
				vIsString ? values.add(v) : values.add(params[k].toDouble());
			} else {
				if (vIsString)
					sql.append("'").append(v).append("'");
				else
					sql.append(v);
			}
		}
		if (queryByParameter) {
			where.append(self()->placeholder(nextPlaceholder()));
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

	bool buildDeleteSql(const string& tablename, const Json& params,
						string& sql, Json& values) {
		if (!params.isObject())
			return false;
		const Json id = params["id"];
		if (id.isError())
			return false;
		sql = "delete from " + self()->qualifiedTable(tablename) + " where " + self()->quoteIdent("id") + " = ";
		values = Json(JsonType::Array);
		const bool vIsString = id.isString() || id.isArray() || id.isObject();
		resetPlaceholders();
		if (queryByParameter) {
			sql.append(self()->placeholder(nextPlaceholder()));
			vIsString ? values.add(id.toString()) : values.add(id.toDouble());
		} else {
			if (vIsString)
				sql.append("'").append(id.toString()).append("'");
			else
				sql.append(id.toString());
		}
		return true;
	}

	bool buildStructuredSql(const string& table, const string& method,
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
			keyStr.append(self()->columnList(keys0)).append(" ) values ");
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
					!queryByParameter && vIsString && self()->escapeString(v);
					if (queryByParameter) {
						valueStr.append(self()->placeholder(nextPlaceholder()));
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
			const string upsert = self()->upsertClause("id", keys0);
			if (updateStr.length() == 0 || upsert.empty())
				sql = "insert into " + self()->qualifiedTable(table) + keyStr;
			else
				sql = "insert into " + self()->qualifiedTable(table) + keyStr + upsert;
			return true;
		}
		return false;
	}

	// ─────────────────────────────────────────────────────────────────────
	// Smart query (genSql): WHERE assembly + aggregates + pagination
	// ─────────────────────────────────────────────────────────────────────

	Json genSql(string& querySql, Json& values, const Json& ps, vector<string> fields = vector<string>(),
				int queryType = 1, bool parameterized = false, string* countSql = nullptr) {
		if (ps.isError())
			return DbUtils::MakeJsonObject(STPARAMERR);
		Json params(ps);
		const string rawname = querySql;
		querySql = "";
		string where;
		const string AndJoinStr = " and ";
		string fieldsJoinStr = "*";

		if (!fields.empty()) {
			fieldsJoinStr = self()->fieldsProjection(fields);
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
		if (self()->numberedPlaceholders() && queryType != 1) {
			// A user-supplied statement may already carry $n placeholders -
			// numbering continues after them.
			placeholderIndex_ = static_cast<int>(
				std::count(rawname.begin(), rawname.end(), '$')) + 1;
		}
		for (size_t i = 0; i < len; i++) {
			string k = allKeys[i];
			bool vIsString = params[k].isString() || params[k].isArray() || params[k].isObject();
			string v = params[k].toString();
			!parameterized && vIsString && self()->escapeString(v);
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
					whereExtra.append(self()->quoteIdent(c)).append(" in (");
					const int eleLen = ele.size();
					for (int e = 0; e < eleLen; e++) {
						whereExtra.append(self()->placeholder(nextPlaceholder()));
						if (e < eleLen - 1)
							whereExtra.append(",");
						values.add(ele[e]);
					}
					whereExtra.append(")");
				} else {  // lks / ors
					whereExtra.append(" ( ");
					for (size_t j = 0; j < ele.size(); j += 2) {
						if (j > 0)
							whereExtra.append(" or ");
						whereExtra.append(self()->likeColumn(ele.at(j))).append(" ");
						const string ph = self()->placeholder(nextPlaceholder());
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
							where.append(self()->quoteIdent(k)).append(vls.at(0)).append(self()->placeholder(nextPlaceholder())).append(" ");
							values.add(vls.at(1));
						} else {
							where.append(self()->quoteIdent(k)).append(vls.at(0)).append("'").append(vls.at(1)).append("'");
						}
					} else if (vls.size() == 4) {
						if (parameterized) {
							where.append(self()->quoteIdent(k)).append(vls.at(0)).append(self()->placeholder(nextPlaceholder())).append(" and ");
							where.append(self()->quoteIdent(k)).append(vls.at(2)).append(self()->placeholder(nextPlaceholder())).append(" ");
							values.add(vls.at(1));
							values.add(vls.at(3));
						} else {
							where.append(self()->quoteIdent(k)).append(vls.at(0)).append("'").append(vls.at(1)).append("' and ");
							where.append(self()->quoteIdent(k)).append(vls.at(2)).append("'").append(vls.at(3)).append("'");
						}
					} else {
						return DbUtils::MakeJsonObject(STPARAMERR, "not equal value is wrong.");
					}
				} else if (fuzzy == "1") {
					if (parameterized) {
						where.append(self()->likeColumn(k)).append(" like ").append(self()->placeholder(nextPlaceholder())).append(" ");
						values.add(v.insert(0, "%").append("%"));
					} else {
						where.append(self()->likeColumn(k)).append(" like '%").append(v).append("%'");
					}
				} else {
					if (parameterized) {
						where.append(self()->quoteIdent(k)).append(" = ").append(self()->placeholder(nextPlaceholder())).append(" ");
						vIsString ? values.add(v) : values.add(params[k].toDouble());
					} else {
						if (vIsString)
							where.append(self()->quoteIdent(k)).append(" = '").append(v).append("'");
						else
							where.append(self()->quoteIdent(k)).append(" = ").append(v);
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
				extra.append("sum(").append(self()->aggColumn(ele.at(i))).append(") as ").append(self()->aggAlias(ele.at(i + 1))).append(" ");
		}
		if (!count.empty()) {
			vector<string> ele = DbUtils::MakeVector(count);
			if (ele.empty() || ele.size() % 2 == 1)
				return DbUtils::MakeJsonObject(STPARAMERR, "count is wrong.");
			for (size_t i = 0; i < ele.size(); i += 2)
				extra.append("count(").append(self()->aggColumn(ele.at(i))).append(") as ").append(self()->aggAlias(ele.at(i + 1))).append(" ");
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
						gs[g] = self()->quoteIdent(gs[g]);
					fieldsJoinStr = DbUtils::GetVectorJoinStr(gs);
				} else {
					fieldsJoinStr = "";
				}
			} else if (!group.empty() && fields.empty()) {
				// Grouping without fields: project the group columns
				// (valid under ONLY_FULL_GROUP_BY, dm8 parity).
				vector<string> gs = DbUtils::MakeVector(group);
				for (size_t g = 0; g < gs.size(); g++)
					gs[g] = self()->quoteIdent(gs[g]);
				fieldsJoinStr = DbUtils::GetVectorJoinStr(gs);
			}
			querySql.append("select ").append(fieldsJoinStr);
			if (hasAgg)
				querySql.append(fieldsJoinStr.empty() ? "" : ",").append(extra);
			querySql.append(" from ").append(self()->qualifiedTable(rawname));
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
				gs[g] = self()->quoteIdent(gs[g]);
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
					gs[g] = self()->quoteIdent(gs[g]);
				groupPart = " group by " + DbUtils::GetVectorJoinStr(gs);
			}
			if (group.empty())
				*countSql = "select count(1) as " + self()->countAliasSql() + " from " + self()->qualifiedTable(rawname) + wherePart;
			else
				*countSql = "select count(1) as " + self()->countAliasSql() + " from (select * from " + self()->qualifiedTable(rawname) + wherePart + groupPart + ") zorm_cnt";
		}

		if (!sort.empty())
			querySql.append(" order by ").append(self()->orderClause(sort));

		if (page > 0) {
			page--;
			querySql.append(self()->limitClause(page * size, size));
		}
		return DbUtils::MakeJsonObject(STSUCCESS);
	}

	// ─────────────────────────────────────────────────────────────────────
	// Pagination counters (run on the caller's leased connection - O-3)
	// ─────────────────────────────────────────────────────────────────────

	void attachRecordsPages(Handle handle, Json& result, const Json& params,
							const string& countSql, Json& values) {
		long long records = -1;
		const int page = atoi(params["page"].toString().c_str());
		const int size = atoi(params["size"].toString().c_str());
		if (page > 0 && size > 0 && !countSql.empty())
			records = runCountQueryOn(handle, countSql, values);
		if (records < 0)
			records = result["data"].size();
		result.add("records", records);
		result.add("pages", (page > 0 && size > 0)
			? (records == 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(records) / size)))
			: (records > 0 ? 1 : 0));
	}

	long long runCountQueryOn(Handle handle, const string& countSql, Json& values) {
		if (countSql.empty())
			return -1;
		Json rs = self()->execQueryOn(handle, countSql, vector<string>(), values);
		if (rs["status"].toInt() != 200 || rs["data"].size() == 0)
			return -1;
		return static_cast<long long>(rs["data"][0][countAlias_].toDouble());
	}

private:
	int placeholderIndex_ = 1;

	Derived* self() {
		return static_cast<Derived*>(this);
	}

	const std::string excludedRef(const std::string& column) {
		return self()->excludedRefImpl(column);
	}
};

}  // namespace ZORM
