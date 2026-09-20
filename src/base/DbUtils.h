#pragma once

// Internal helpers shared by the SQL backends and the DbBase facade.
// Not part of the public API.

#include "zjson.hpp"
#include "GlobalConstants.h"

#include <cstdint>
#include <string>
#include <vector>

// NOTE: zjson defines its own ZJSON::string/vector; keep std:: explicit here.

namespace ZORM {

	using ZJSON::Json;

	class DbUtils {
	public:
		// 8 lowercase hex characters - the id shape the ORM/gels contract
		// expects for auto-generated ids (shared by jsonfile and the SQL
		// backends so create() with a missing id behaves identically).
		static std::string GenerateId();

		static std::string Trim(const std::string& text);

		static bool FindStartsStringFromVector(const std::vector<std::string>& strs, const std::string& value);

		static bool FindStringFromVector(const std::vector<std::string>& strs, const std::string& value);

		static std::vector<std::string> MakeVector(const std::string& str, char flag = ',');

		static std::string IntTransToString(int val);

		static std::string GetVectorJoinStrArroundQuots(const std::vector<std::string>& v);

		static std::vector<std::string> GetVectorFromJson(const Json& js);

		static std::string GetVectorJoinStr(const std::vector<std::string>& v);

		static Json MakeJsonObject(StatusCodes code, const std::string& info = "");
	};

}
