#pragma once

// IDbConnection - the per-connection execution surface the shared SQL
// algorithm layer (SqlBackendBase) drives.
//
// Each SQL backend implements a small Connection class wrapping its native
// handle (sqlite3*, MYSQL*, PGconn*, Dm8Con*) and pools instances of it
// through DbPool::HandlePool<IDbConnection*>. Because every pooled object
// IS-A IDbConnection, the backend base class is free of templates: the
// algorithms in SqlBackendBase.cpp are compiled once and dispatch the
// driver calls through this interface (one virtual call per statement -
// nothing against actual database round-trip cost).
//
// A connection object carries copies of the backend flags it needs
// (parameterized / log) taken at creation time; they do not change while
// the backend is alive.

#include "zjson.hpp"

#include <string>
#include <vector>

namespace ZORM {

	using std::string;
	using std::vector;

	class IDbConnection {
	public:
		virtual ~IDbConnection() = default;

		// Executes a statement that returns rows. `fields` is the projection
		// list baked into the SQL (backends may ignore it - column names come
		// from the driver metadata).
		virtual Json execQuery(const string& sql, const vector<string>& fields, Json& values) = 0;

		// Executes a statement without a result set; implementations add
		// "affected" on success.
		virtual Json execNone(const string& sql, Json& values) = 0;

		// Transaction step: execute one statement inside the caller's open
		// transaction; on failure append the driver error to *err (when err
		// is non-null) and return false.
		virtual bool execTx(const string& sql, Json& values, string* err) = 0;

		virtual bool beginTx() = 0;
		virtual bool commitTx() = 0;
		virtual void rollbackTx() = 0;

		// Escapes a literal for this backend's parameterized=false path using
		// the live connection (charset-aware where the driver supports it).
		// Default: double the single quotes (the SQL standard rule).
		virtual void escapeLiteral(std::string& text) {
			std::string escaped;
			escaped.reserve(text.size() + 8);
			for (const char ch : text) {
				if (ch == '\'')
					escaped += "''";
				else
					escaped += ch;
			}
			text = escaped;
		}
	};

}
