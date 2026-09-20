#pragma once

// Mysql backend - dialect deltas + driver. Placeholders, limit syntax,
// quoting and column projection all match the SqlBackendBase defaults;
// the overrides are:
//   * upsert: ON DUPLICATE KEY UPDATE col = values(col) (O-6 parity);
//   * escaping: mysql_real_escape_string against a pooled connection
//     (charset-aware), with a plain literal fallback when no connection
//     can be acquired.
//
// TLS/SSL (delivery to third parties): the client prefers TLS whenever the
// server offers it (ssl-mode=PREFERRED). Optional options:
//   db_ssl_ca / db_ssl_capath / db_ssl_cert / db_ssl_key / db_ssl_cipher
//   db_ssl_verify   (bool, default false - verify the server certificate)
//   db_ssl_required (bool, default false - fail when TLS is unavailable)

#include "SqlBackendBase.h"

#include <string>

namespace ZORM {

	namespace Mysql {

		class ZORM_API MysqlDb : public SqlBackendBase {

		public:
			MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport = 3306, Json options = Json());
			~MysqlDb() override;

			Lease acquireConnection(string& err) override;

		protected:
			// Upsert parity (O-6): duplicate ids update the row.
			std::string upsertClause(const std::string& constraint, const std::vector<std::string>& keys) override;
			std::string excludedRefImpl(const std::string& column) override;
			bool escapeString(string& pStr) override;

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
			string charsetName;
			string sslKey, sslCert, sslCa, sslCapath, sslCipher;
			bool sslVerify = false;
			bool sslRequired = false;
		};

	}

}
