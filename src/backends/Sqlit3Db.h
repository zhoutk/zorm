#pragma once

// Sqlite3 backend - driver only. Every dialect detail (placeholders, limit,
// quoting, ON CONFLICT upsert, quote-doubling escaping) matches the
// SqlBackendBase defaults, so this class carries NO dialect overrides:
// it only opens the database and serves exclusive IDbConnection leases.
// Shared algorithm layer: SqlBackendBase.cpp; pool: DbPool::HandlePool (a
// single exclusive lease, which also serializes concurrent access to the
// one file handle).

#include "SqlBackendBase.h"
#include "sqlite3.h"

#include <string>

namespace ZORM {

	namespace Sqlit3 {

#define SQLITECPP_ASSERT(expression, message)   assert(expression && message)

		const int   OPEN_READONLY = SQLITE_OPEN_READONLY;
		const int   OPEN_READWRITE = SQLITE_OPEN_READWRITE;
		const int   OPEN_CREATE = SQLITE_OPEN_CREATE;
		const int   OPEN_URI = SQLITE_OPEN_URI;

		const int   OK = SQLITE_OK;

		class ZORM_API Sqlit3Db : public SqlBackendBase {

		public:
			Sqlit3Db(const char* apFilename, bool logFlag = false, bool parameterized = false,
				const int   aFlags = OPEN_READWRITE | OPEN_CREATE,
				const int   aBusyTimeoutMs = 0,
				const char* apVfs = nullptr);
			Sqlit3Db(const std::string& aFilename, bool logFlag = false, bool parameterized = false,
				const int          aFlags = OPEN_READWRITE | OPEN_CREATE,
				const int          aBusyTimeoutMs = 0,
				const std::string& aVfs = "");
			~Sqlit3Db() override;

			Lease acquireConnection(string& err) override;

		private:
			class Connection;

			DbPool::HandlePool<IDbConnection*> pool;
			std::string mFilename;
			int mFlags;
			std::string mVfs;
		};

	}

}
