// tests/TestConfig.cpp
// ----------------------------------------------------------------------------
// Loads the backend config from tests/dbconfig.json (gels configs.ts parity).
// See TestConfig.h for the dialect resolution order.
// ----------------------------------------------------------------------------
#include "TestConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ZORM {
namespace contract {

namespace fs = std::filesystem;

// Reads a small text file. Returns false on failure.
static bool readWholeFile(const fs::path& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

static std::string trim(const std::string& text) {
	std::string::size_type begin = 0;
	std::string::size_type end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

static bool readConfigFile(Json& out) {
	// 1. Current working directory (source-tree runs).
	// 2. The tests/ source directory (when running from the repo root).
	const fs::path candidates[] = {
		fs::current_path() / "dbconfig.json",
		fs::current_path() / "tests" / "dbconfig.json",
	};
	for (const fs::path& candidate : candidates) {
		std::string raw;
		if (!readWholeFile(candidate, raw)) {
			continue;
		}
		std::string err;
		Json parsed = Json::ParseJson(raw, err);
		if (parsed.isError()) {
			continue;
		}
		out = parsed;
		return true;
	}
	return false;
}

// Searches for dbconfig.json next to the executable first (the test binary
// gets a copy at build time), then in the current directory / source tree.
// This is how CTest's build-directory working dir still finds the config.
bool readConfigFileNearExecutable(const char* argv0, Json& out) {
	if (argv0 != nullptr && *argv0 != '\0') {
		std::error_code error;
		const fs::path absolute = fs::absolute(fs::u8path(argv0), error);
		if (!error) {
			const fs::path exeDir = absolute.lexically_normal().parent_path();
			std::string raw;
			if (readWholeFile(exeDir / "dbconfig.json", raw)) {
				std::string err;
				Json parsed = Json::ParseJson(raw, err);
				if (!parsed.isError()) {
					out = parsed;
					return true;
				}
			}
		}
	}
	return readConfigFile(out);
}

static void applyBackendConfig(const Json& backend, BackendConfig& cfg) {
	cfg.name = backend["name"].toString();
	cfg.type = backend["type"].toString();
	cfg.options = backend["options"];
	cfg.rawTable = backend["rawTable"].toString();
	cfg.quoteColumn = backend["quoteColumn"].toString();
	cfg.catalogSql = backend["catalogSql"].toString();
	cfg.nullRendering = backend["nullRendering"].toString();
	if (cfg.nullRendering.empty())
		cfg.nullRendering = "empty";
	cfg.placeholder = backend["placeholder"].toString();
	if (!backend["supportsWherePlaceholders"].isError())
		cfg.supportsWherePlaceholders = backend["supportsWherePlaceholders"].toBool();
	if (!backend["autoCreateTables"].isError())
		cfg.autoCreateTables = backend["autoCreateTables"].toBool();
	const Json schema = backend["schema"];
	if (schema.isArray()) {
		for (int i = 0; i < schema.size(); ++i) {
			cfg.schema.push_back(schema[i].toString());
		}
	}
}

// Global config, filled by main() before RUN_ALL_TESTS().
BackendConfig g_config;
const char* g_dialect = "sqlite3-mem";

BackendConfig loadConfig(const std::string& dialect) {
	BackendConfig cfg;
	cfg.name = dialect;
	cfg.type = dialect;
	cfg.rawTable = "table_for_test";
	cfg.placeholder = "?";
	cfg.nullRendering = "empty";

	Json file;
	if (readConfigFile(file)) {
		const Json backends = file["backends"];
		if (backends.isObject()) {
			Json backend = backends[dialect];
			if (!backend.isError()) {
				applyBackendConfig(backend, cfg);
				return cfg;
			}
		}
	}

	throw std::runtime_error("unknown dialect '" + dialect +
							 "': not found in dbconfig.json backends");
}

static std::string resolveDialectFrom(Json& file) {
	const Json dialect = file["db_dialect"];
	if (!dialect.isError()) {
		std::string value = trim(dialect.toString());
		if (!value.empty()) {
			return value;
		}
	}
	return std::string();
}

namespace {
// Shared resolution steps; the two entry points differ only in how the
// config file is located (cwd / source tree vs. next to the executable).
std::string dialectFromArgv(int argc, char* argv[]) {
	for (int i = 1; i < argc - 1; ++i) {
		const std::string arg = argv[i];
		if (arg == "--dialect" || arg == "-d") {
			return argv[i + 1];
		}
	}
	if (const char* env = std::getenv("ZORM_DB_DIALECT")) {
		std::string value = trim(env);
		if (!value.empty()) {
			return value;
		}
	}
	return std::string();
}
}  // namespace

std::string resolveDialect(int argc, char* argv[]) {
	// 1. --dialect <name>  2. ZORM_DB_DIALECT
	std::string value = dialectFromArgv(argc, argv);
	if (!value.empty()) {
		return value;
	}
	// 3. config file db_dialect
	Json file;
	if (readConfigFile(file)) {
		std::string value = resolveDialectFrom(file);
		if (!value.empty()) {
			return value;
		}
	}
	// 4. default
	return "sqlite3-mem";
}

std::string resolveDialectWithArgv(int argc, char* argv[]) {
	// 1. --dialect <name>  2. ZORM_DB_DIALECT
	std::string value = dialectFromArgv(argc, argv);
	if (!value.empty()) {
		return value;
	}
	// 3. config file db_dialect (executable dir first)
	Json file;
	if (readConfigFileNearExecutable(argc > 0 ? argv[0] : nullptr, file)) {
		std::string value = resolveDialectFrom(file);
		if (!value.empty()) {
			return value;
		}
	}
	// 4. default
	return "sqlite3-mem";
}

}  // namespace contract
}  // namespace ZORM
