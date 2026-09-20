// tests/test_contract.cpp
// ----------------------------------------------------------------------------
// Config-driven contract test runner (gels-style): ONE binary, SIX backends.
//
// The backend under test is selected by (in order):
//   --dialect <name> | ZORM_DB_DIALECT | tests/dbconfig.json "db_dialect"
//   e.g. sqlite3-mem | sqlite3 | jsonfile | mysql | postgres | dm8
//
// The same suite runs against every backend - switching the database under
// test is switching one config value, exactly like gels' db_dialect.
// Per-backend binaries (test_sqlite3.cpp, test_mysql.cpp, ...) are gone.
// ----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "ContractSuite.h"
#include "DbBase.h"
#include "TestConfig.h"

using namespace ZORM;
using namespace ZJSON;

namespace {

// Config-driven Env: connection options + DDL both come from dbconfig.json.
class ConfigEnv final : public contract::Env {
public:
	Idb* connect() override {
		return new DbBase(contract::g_config.type, contract::g_config.options);
	}

	vector<string> schemaSqls() const override {
		return contract::g_config.schema;
	}
};

}  // namespace

// Instantiate the shared contract suite (runs against the configured backend).
ZORM_CONTRACT_TESTS()

// Backend-agnostic sanity checks that exercise DbBase directly.
TEST(Contract, DbBaseRouting) {
	// Unknown db type is rejected.
	Json options;
	options.add("connString", ":memory:");
	EXPECT_THROW(DbBase("no_such_db", options), const char*);

	// The configured backend connects through DbBase.
	contract::env->connectOnce();
	Idb& db = contract::env->db();
	EXPECT_TRUE(contract::env->reset());
	Json rs = db.select("table_for_test", Json{{"id", "a1b2c3d4"}});
	ASSERT_EQ(rs["status"].toInt(), 200);
	EXPECT_EQ(rs["data"][0]["name"].toString(), "Kevin 凯文");
}

int main(int argc, char* argv[]) {
	::testing::InitGoogleTest(&argc, argv);

	// Resolve the backend under test (config-driven). argv[0]-aware lookup so
	// CTest's build-directory working dir still finds dbconfig.json (copied
	// next to the executable at build time).
	const std::string dialect = contract::resolveDialectWithArgv(argc, argv);
	contract::g_dialect = dialect.c_str();
	contract::g_config = contract::loadConfig(dialect);

	// --no-param runs the same dialect with parameterized=false, which
	// exercises the plain-SQL paths (literal escaping, non-parameterized
	// decoding) that the parameterized suite never touches.
	bool noParam = false;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--no-param")
			noParam = true;
	}
	if (noParam)
		contract::g_config.options.add("parameterized", false);

	std::cout << "[==========] backend under test: " << dialect
			  << (noParam ? " (parameterized=false)" : "") << std::endl;

	static ConfigEnv env;
	contract::env = &env;
	return RUN_ALL_TESTS();
}
