#pragma once

// Postgres backend - dialect deltas + driver. Overrides against the
// SqlBackendBase defaults:
//   * placeholders: $1..$n (numbered across the whole statement);
//   * LIKE/fuzzy comparisons CAST the column to TEXT;
//   * limit syntax: "limit N OFFSET o";
//   * detectParameterized sniffs '$' instead of '?'.
// Upsert (ON CONFLICT ... excluded) and quote-doubling escaping use the
// shared defaults - postgres belongs to the "on conflict" group together
// with sqlite.

#include "SqlBackendBase.h"

#include <string>

namespace ZORM {

	namespace Postgres {

		class ZORM_API PostgresDb : public SqlBackendBase {

		public:
			PostgresDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 5432, Json options = Json());
			~PostgresDb() override;

			Lease acquireConnection(string& err) override;

		protected:
			std::string placeholder(int index) override;
			bool numberedPlaceholders() override;
			std::string likeColumn(const std::string& name) override;
			std::string limitClause(int offset, int size) override;
			bool detectParameterized(const std::string& sql) override;

		private:
			class Connection;

			IDbConnection* newConnection(string& err);

			DbPool::HandlePool<IDbConnection*> pool;
			int maxConn = 2;
			string dbhost;
			string dbuser;
			string dbpwd;
			string dbname;
			int dbport;
			string connString;
		};

	}

}
