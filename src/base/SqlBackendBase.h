#pragma once

// SqlBackendBase - shared algorithm layer for the SQL backends.
//
// A gels-inspired baseDao/sqlDialect split. Since the pooled objects are
// IDbConnection (see DbConnection.h), this class needs no CRTP template:
// the 8 Idb method skeletons, statement builders, smart-query (genSql)
// assembly, pagination counters and the transaction loop are implemented
// ONCE in SqlBackendBase.cpp and drive the driver through IDbConnection;
// the dialect differences go through the virtual hooks below.
//
// Backend = a subclass that supplies:
//   * a Connection class implementing IDbConnection around its native
//     handle, created by the connect function handed to its pool;
//   * overrides of only the dialect hooks that DIFFER from the defaults;
//   * acquireConnection(), handing out exclusive leases from its pool.
//
// Similar backends end up sharing the defaults naturally - the
// "ON CONFLICT ... excluded" upsert group (sqlite/postgres), the
// `limit o,n` group (sqlite/mysql/dm8), the `?` placeholder group - and
// only true one-offs (dm8's quoted identifiers, pg's $n placeholders)
// need their own overrides.
//
// Hooks a backend MUST provide (driver):
//   Lease acquireConnection(std::string& err);
//
// Dialect hooks (defaults in SqlBackendBase.cpp; override what differs):
//   std::string placeholder(int index);                  // "?"
//   bool numberedPlaceholders();                         // false (pg: true)
//   std::string quoteIdent(const std::string&);          // identity
//   std::string qualifiedTable(const std::string&);      // identity
//   std::string likeColumn(const std::string&);          // identity (pg CAST)
//   std::string orderClause(const std::string& sort);    // identity (dm8 quotes)
//   std::string limitClause(int offset, int size);       // " limit o,n"
//   std::string aggColumn(const std::string&);           // identity
//   std::string aggAlias(const std::string&);            // identity
//   std::string countAliasSql();                         // countAlias_
//   std::string columnList(const vector<string>&);       // comma join
//   std::string fieldsProjection(const vector<string>&); // comma join
//   std::string excludedRefImpl(const std::string&);     // "excluded." + col
//   std::string upsertClause(constraint, keys);          // ON CONFLICT excluded
//   bool detectParameterized(const std::string& sql);    // contains '?'
//   void escapeString(std::string&);                     // double the quotes

#include "Idb.h"
#include "DbUtils.h"
#include "DbPool.h"
#include "DbConnection.h"
#include "GlobalConstants.h"

#include <string>
#include <vector>

namespace ZORM {

class SqlBackendBase : public Idb {
public:
	using Pool = DbPool::HandlePool<IDbConnection*>;
	using Lease = Pool::Lease;

	// ─────────────────────────────────────────────────────────────────────
	// Idb.h method skeletons (identical for every SQL backend)
	// ─────────────────────────────────────────────────────────────────────

	Json create(const string& tablename, const Json& params) override;
	Json update(const string& tablename, const Json& params) override;
	Json remove(const string& tablename, const Json& params) override;
	Json select(const string& tbname, const Json& params,
				vector<string> fields = vector<string>(),
				Json values = Json(JsonType::Array)) override;
	Json querySql(const string& sqlstr, Json params = Json(),
				  Json values = Json(JsonType::Array),
				  vector<string> fields = vector<string>()) override;
	Json execSql(const string& sqlstr, Json params = Json(),
				 Json values = Json(JsonType::Array)) override;
	Json insertBatch(const string& tablename, const Json& elements, string constraint) override;
	Json transGo(const Json& sqls, bool isAsync = false) override;

protected:
	SqlBackendBase() = default;
	~SqlBackendBase() override = default;

	// ─────────────────────────────────────────────────────────────────────
	// Driver hook: every backend hands out exclusive connections here.
	// ─────────────────────────────────────────────────────────────────────
	virtual Lease acquireConnection(string& err) = 0;

	// ─────────────────────────────────────────────────────────────────────
	// Dialect hooks - defaults cover the plain-SQL majority; override
	// only the lines your dialect writes differently.
	// ─────────────────────────────────────────────────────────────────────
	virtual std::string placeholder(int index);
	virtual bool numberedPlaceholders();
	virtual std::string quoteIdent(const std::string& name);
	virtual std::string qualifiedTable(const std::string& name);
	virtual std::string likeColumn(const std::string& name);
	virtual std::string orderClause(const std::string& sort);
	virtual std::string limitClause(int offset, int size);
	virtual std::string aggColumn(const std::string& src);
	virtual std::string aggAlias(const std::string& alias);
	virtual std::string countAliasSql();
	virtual std::string columnList(const vector<string>& keys);
	virtual std::string fieldsProjection(const vector<string>& fields);
	virtual std::string excludedRefImpl(const std::string& column);
	// Upsert parity (O-6): default is the ANSI "ON CONFLICT (...) DO UPDATE"
	// with excluded.* refs (sqlite/postgres). MySQL overrides with
	// ON DUPLICATE KEY; dm8 returns "" (read-then-write upsert instead).
	virtual std::string upsertClause(const std::string& constraint,
									 const vector<string>& keys);
	virtual bool detectParameterized(const std::string& sql);
	// Literal escaping for the parameterized=false path. Default doubles the
	// single quotes (the SQL standard rule sqlite/postgres/dm8 follow).
	virtual bool escapeString(string& pStr);

	// ─────────────────────────────────────────────────────────────────────
	// Shared state
	// ─────────────────────────────────────────────────────────────────────
	const vector<string> QUERY_EXTRA_KEYS{"ins", "lks", "ors"};
	const vector<string> QUERY_UNEQ_OPERS{">,", ">=,", "<,", "<=,", "<>,", "=,"};
	bool DbLogClose = false;
	bool queryByParameter = false;
	std::string countAlias_ = "_zorm_total";
	vector<string> restrain_{"id"};

	int nextPlaceholder();
	void resetPlaceholders();

	// ─────────────────────────────────────────────────────────────────────
	// Statement builders (identical for every backend except placeholders)
	// ─────────────────────────────────────────────────────────────────────
	bool buildInsertSql(const string& tablename, const Json& params,
						string& sql, Json& values, string& generatedId);
	bool buildUpdateSql(const string& tablename, const Json& params,
						string& sql, Json& values);
	bool buildDeleteSql(const string& tablename, const Json& params,
						string& sql, Json& values);
	bool buildStructuredSql(const string& table, const string& method,
							const Json& params, bool hasId, const Json& idValue,
							string& sql, Json& values);

	// ─────────────────────────────────────────────────────────────────────
	// Smart query (genSql): WHERE assembly + aggregates + pagination
	// ─────────────────────────────────────────────────────────────────────
	Json genSql(string& querySql, Json& values, const Json& ps,
				vector<string> fields = vector<string>(),
				int queryType = 1, bool parameterized = false,
				string* countSql = nullptr);

	void attachRecordsPages(IDbConnection* conn, Json& result, const Json& params,
							const string& countSql, Json& values);
	long long runCountQueryOn(IDbConnection* conn, const string& countSql, Json& values);

private:
	int placeholderIndex_ = 1;

	const std::string excludedRef(const std::string& column) {
		return excludedRefImpl(column);
	}
};

}  // namespace ZORM
