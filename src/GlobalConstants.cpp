#include "GlobalConstants.h"

namespace {

const std::pair<int, std::string> kStatusPairs[] = {
	std::make_pair(200, "Operation succeeded. "),
	std::make_pair(202, "Query result is empty. "),
	std::make_pair(301, "Error: Param is wrong. "),
	std::make_pair(404, "Error: Request resource is not found. "),
	std::make_pair(411, "Error: Upload file fail. "),
	std::make_pair(421, "Error: Json web token authorize fail. "),
	std::make_pair(422, "Error: Password is wrong. "),
	std::make_pair(423, "Error: Username is wrong. "),
	std::make_pair(431, "Error: Authorization is less. "),
	std::make_pair(432, "Error: User is not found. "),
	std::make_pair(500, "Error: Exception is thrown. "),
	std::make_pair(700, "Error: Database connection is wrong. "),
	std::make_pair(701, "Error: Database operation is wrong. "),
	std::make_pair(702, "Error: Database table must have id field. "),
	std::make_pair(703, "Error: Database modify & serve need resart. "),
	std::make_pair(801, "Error: Parent record is not found. "),
};

}  // namespace

const std::map<int, std::string>& StatusMessages() {
	static const std::map<int, std::string> messages(
		kStatusPairs, kStatusPairs + sizeof(kStatusPairs) / sizeof(kStatusPairs[0]));
	return messages;
}
