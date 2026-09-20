#include "DbUtils.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <sstream>
#include <time.h>

namespace ZORM {

using std::string;
using std::vector;

	std::string DbUtils::GenerateId() {
		static thread_local std::mt19937_64 engine([]() {
			std::random_device device;
			std::seed_seq seed{ device(), device(), device(), device() };
			return std::mt19937_64(seed);
		}());
		std::uniform_int_distribution<std::uint32_t> distribution(0u, 0xFFFFFFFFu);
		char buffer[16] = { 0 };
		std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned int>(distribution(engine)));
		return std::string(buffer);
	}

	std::string DbUtils::Trim(const std::string& text) {
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

	bool DbUtils::FindStartsStringFromVector(const vector<string>& strs, const string& value) {
		for (const string& key : strs) {
			const size_t vlen = value.size();
			const size_t klen = key.size();
			if (vlen > klen && value.substr(0, klen).compare(key) == 0)
				return true;
		}
		return false;
	}

	bool DbUtils::FindStringFromVector(const vector<string>& strs, const string& value) {
		return std::find(strs.begin(), strs.end(), value) != strs.end();
	}

	vector<string> DbUtils::MakeVector(const string& str, char flag) {
		vector<string> rs;
		std::istringstream iss(str);
		string temp;

		while (std::getline(iss, temp, flag)) {
			rs.push_back(temp);
		}
		return rs;
	}

	string DbUtils::IntTransToString(int val) {
		std::stringstream ss;
		ss << val;
		return ss.str();
	}

	string DbUtils::GetVectorJoinStrArroundQuots(const vector<string>& v) {
		std::stringstream ss;
		ss << "\"";
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (i != 0)
				ss << "\",\"";
			ss << v[i];
		}
		ss << "\"";
		return ss.str();
	}

	vector<string> DbUtils::GetVectorFromJson(const Json& js) {
		vector<string> rs;
		if (js.isArray()) {
			std::vector<Json> items = js.toVector();
			for (auto& item : items) {
				rs.push_back(item.toString());
			}
		}
		return rs;
	}

	string DbUtils::GetVectorJoinStr(const vector<string>& v) {
		std::stringstream ss;
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (i != 0)
				ss << ",";
			ss << v[i];
		}
		return ss.str();
	}

	Json DbUtils::MakeJsonObject(StatusCodes code, const string& info) {
		Json rs;
		rs.add("status", (int)code);
		string text(info);
		if (!text.empty()) {
			auto index = text.find_first_of("\n");
			if (index != text.npos)
				text = text.substr(0, index);
			text.insert(0, " details, ");
		}
		text.insert(0, StatusMessages().at((int)code));
		rs.add("message", text);
		return rs;
	}

}
