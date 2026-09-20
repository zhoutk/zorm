#pragma once

// DM8 backend - dialect deltas + driver. The largest override set, because
// DM8 folds unquoted identifiers to upper case (CASE_SENSITIVE=Y servers)
// while zorm's DDL uses quoted lower-case names:
//   * identifiers are quoted lower-case everywhere (columns, aliases,
//     order-by tokens, projections);
//   * create() implements upsert as read-then-update/insert (DM8 has no
//     INSERT ... ON CONFLICT and its DPI cannot bind placeholders inside a
//     MERGE ... USING (SELECT ? FROM DUAL)), so upsertClause returns "";
//   * insertBatch routes rows through create() for the same reason;
//   * transactions toggle AUTOCOMMIT and restore it afterwards.

#include "SqlBackendBase.h"

#include <string>

namespace ZORM {

	namespace Dm8 {

		// Raw DPI connection triple (env/con/stmt) lives in Dm8Db.cpp; the
		// pool stores connection objects opaquely behind IDbConnection*.

		class ZORM_API Dm8Db : public SqlBackendBase {

		public:
			Dm8Db(string dbhost, string dbuser, string dbpwd, Json options = Json());
			~Dm8Db() override;

			Lease acquireConnection(string& err) override;

			// Read-then-write upsert (O-6): both are overridden because DM8
			// has no INSERT ... ON CONFLICT.
			Json create(const string& tablename, const Json& params) override;
			Json insertBatch(const string& tablename, const Json& elements, string constraint) override;

		protected:
			// ── dialect: quoted-lowercase identifier group ──
			std::string quoteIdent(const std::string& name) override;
			std::string qualifiedTable(const std::string& name) override;
			std::string likeColumn(const std::string& name) override;
			std::string orderClause(const std::string& sort) override;
			std::string aggColumn(const std::string& src) override;
			std::string aggAlias(const std::string& alias) override;
			std::string countAliasSql() override;
			std::string columnList(const std::vector<std::string>& keys) override;
			std::string fieldsProjection(const vector<string>& fields) override;
			std::string upsertClause(const std::string& constraint, const std::vector<std::string>& keys) override;

		private:
			class Connection;

			IDbConnection* newConnection(string& err);

			DbPool::HandlePool<IDbConnection*> pool;
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
