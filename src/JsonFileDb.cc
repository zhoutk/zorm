#include "JsonFileDb.h"

#include "FileLock.h"
#include "GlobalConstants.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// JsonFileDb - JSON file backend for ZORM::Idb.
//
// Plain C++17 + ZJSON so the ORM layer carries no database client dependency
// at all: Idb.h / zjson.hpp can be hosted alongside the sqlite3 / mysql /
// postgres / dm8 backends. The hardening behaviour (cross-process lock,
// atomic write, corrupt file backup, single shared instance per path, id
// index) follows the proven design of our orm project's JsonFileDb, and the
// query semantics mirror the gels project's jsonFileDao (the cross-language
// reference contract).
//
// On top of the structured API (create/update/remove/insertBatch/transGo with
// {table, method, params} elements) the SQL shims also accept the SQL text
// style used by the other ZORM backends:
//   * execSql("UPDATE ?? SET score = ? WHERE id = ?", values = [table, v, id])
//   * execSql("insert into t (a,b) values (?,?)", values = [va, vb]) - both
//     `?` placeholders and SQL literals are understood, which is what the
//     {"text": ..., "values": [...]} elements of transGo carry.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

namespace fs = std::filesystem;
using namespace ZJSON;

constexpr char kDefaultDbName[] = "data.json";

// Primitives shared with the cross-process lock implementation (see
// FileLock.h): UTF-8 path conversion, trimming/splitting, clock/pid/host
// queries, whole-file reads and logging.
using ZORM::JsonFile::detail::currentProcessId;
using ZORM::JsonFile::detail::equalsIgnoreCaseAscii;
using ZORM::JsonFile::detail::fsPath;
using ZORM::JsonFile::detail::genericUtf8;
using ZORM::JsonFile::detail::isProcessAlive;
using ZORM::JsonFile::detail::logMessage;
using ZORM::JsonFile::detail::readWholeFile;
using ZORM::JsonFile::detail::split;
using ZORM::JsonFile::detail::toLowerAscii;
using ZORM::JsonFile::detail::trim;
using ZORM::JsonFile::FileLock;

// ─────────────────────────────────────────────────────────────────────────────
// Small string helpers
// ─────────────────────────────────────────────────────────────────────────────

bool startsWith(const std::string& text, const std::string& prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
	if (needle.empty()) {
		return true;
	}
	if (haystack.size() < needle.size()) {
		return false;
	}
	const std::string loweredHaystack = toLowerAscii(haystack);
	const std::string loweredNeedle = toLowerAscii(needle);
	return loweredHaystack.find(loweredNeedle) != std::string::npos;
}

// QString::split(QRegularExpression("\\s+"), SkipEmptyParts)
std::vector<std::string> splitWhitespace(const std::string& text) {
	std::vector<std::string> parts;
	std::string current;
	for (const char ch : text) {
		if (std::isspace(static_cast<unsigned char>(ch))) {
			if (!current.empty()) {
				parts.push_back(current);
				current.clear();
			}
		} else {
			current.push_back(ch);
		}
	}
	if (!current.empty()) {
		parts.push_back(current);
	}
	return parts;
}

// QString::number(value, 'g', 15)
std::string numberText(double value) {
	char buffer[64] = {0};
	std::snprintf(buffer, sizeof(buffer), "%.15g", value);
	return std::string(buffer);
}

// QString::toInt(): 0 when the text is not a (possibly signed) integer.
int parseInt(const std::string& text) {
	if (text.empty()) {
		return 0;
	}
	char* end = nullptr;
	const long value = std::strtol(text.c_str(), &end, 10);
	if (end == text.c_str()) {
		return 0;
	}
	while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
		++end;
	}
	if (end != nullptr && *end != '\0') {
		return 0;
	}
	return static_cast<int>(value);
}

// QString::toDouble(bool* ok): 0 / false when the text is not fully numeric.
double parseDouble(const std::string& text, bool& ok) {
	ok = false;
	if (text.empty()) {
		return 0.0;
	}
	char* end = nullptr;
	const double value = std::strtod(text.c_str(), &end);
	if (end == text.c_str()) {
		return 0.0;
	}
	while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
		++end;
	}
	if (end != nullptr && *end != '\0') {
		return 0.0;
	}
	ok = true;
	return value;
}

// QDateTime::currentDateTime().toString("yyyyMMddHHmmsszzz")
std::string compactTimestamp() {
	const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const long long millis =
		std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

	std::tm local = {};
#ifdef _WIN32
	::localtime_s(&local, &seconds);
#else
	::localtime_r(&seconds, &local);
#endif

	char buffer[32] = {0};
	std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d%02d%02d%02d%03lld",
				  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
				  local.tm_hour, local.tm_min, local.tm_sec, millis);
	return std::string(buffer);
}

std::uint64_t seedEngine() {
	std::random_device device;
	const std::uint64_t ticks = static_cast<std::uint64_t>(
		std::chrono::high_resolution_clock::now().time_since_epoch().count());
	return (static_cast<std::uint64_t>(device()) << 32) ^ ticks ^ static_cast<std::uint64_t>(currentProcessId());
}

// 8 lowercase hex characters, which is the id shape the ORM contract expects.
std::string generateId() {
	static thread_local std::mt19937_64 engine(seedEngine());
	std::uniform_int_distribution<std::uint32_t> distribution(0u, 0xFFFFFFFFu);
	char buffer[16] = {0};
	std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned int>(distribution(engine)));
	return std::string(buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Filesystem helpers (UTF-8 aware)
// ─────────────────────────────────────────────────────────────────────────────

std::string joinPath(const std::string& directory, const std::string& leaf) {
	std::string base = directory;
	while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
		base.pop_back();
	}
	if (base.empty()) {
		return leaf;
	}
	return base + "/" + leaf;
}

// Directory of the running executable.
std::string executableDirectory() {
#ifdef _WIN32
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length =
			::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			buffer.clear();
			break;
		}
		if (length < static_cast<DWORD>(buffer.size())) {
			buffer.resize(length);
			break;
		}
		buffer.resize(buffer.size() * 2);
	}
	if (!buffer.empty()) {
		return genericUtf8(fs::path(buffer).parent_path());
	}
#else
	std::error_code linkError;
	const fs::path executable = fs::read_symlink("/proc/self/exe", linkError);
	if (!linkError && !executable.empty()) {
		return genericUtf8(executable.parent_path());
	}
#endif
	std::error_code currentError;
	return genericUtf8(fs::current_path(currentError));
}

bool isRelativePath(const std::string& path) {
	return fsPath(path).is_relative();
}

std::string defaultDatabasePath() {
	return joinPath(executableDirectory(), kDefaultDbName);
}

std::string resolvedDatabasePath(const std::string& configuredPath) {
	std::string path = trim(configuredPath);
	if (path.empty()) {
		path = defaultDatabasePath();
	} else if (isRelativePath(path)) {
		std::error_code currentError;
		path = joinPath(genericUtf8(fs::current_path(currentError)), path);
	}

	const fs::path resolved = fsPath(path);
	std::error_code directoryError;
	fs::create_directories(resolved.parent_path(), directoryError);
	return genericUtf8(resolved.lexically_normal());
}

void flushToDisk(const fs::path& path) {
#ifdef _WIN32
	HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
								  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle != INVALID_HANDLE_VALUE) {
		::FlushFileBuffers(handle);
		::CloseHandle(handle);
	}
#else
	const int descriptor = ::open(path.c_str(), O_WRONLY);
	if (descriptor >= 0) {
		::fsync(descriptor);
		::close(descriptor);
	}
#endif
}

// Atomic replace. Mirrors QSaveFile: the payload goes to a sibling temp file and
// is then renamed over the destination, so readers only ever observe the old or
// the new complete content.
bool writeFileAtomic(const std::string& filePath, const std::string& payload) {
	const fs::path target = fsPath(filePath);
	const fs::path temp = fsPath(filePath + ".tmp." + std::to_string(currentProcessId()) + "." + generateId());

	{
		std::ofstream output(temp, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			return false;
		}
		if (!payload.empty()) {
			output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
		}
		output.flush();
		const bool written = output.good();
		output.close();
		if (!written || output.fail()) {
			std::error_code removeError;
			fs::remove(temp, removeError);
			return false;
		}
	}

	flushToDisk(temp);

#ifdef _WIN32
	if (::MoveFileExW(temp.c_str(), target.c_str(),
					  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
		std::error_code removeError;
		fs::remove(temp, removeError);
		return false;
	}
#else
	if (::rename(temp.c_str(), target.c_str()) != 0) {
		std::error_code removeError;
		fs::remove(temp, removeError);
		return false;
	}
#endif
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ZJSON helpers
// ─────────────────────────────────────────────────────────────────────────────

// The "no such value" node, matching QJsonValue::Undefined.
Json undefinedValue() {
	return Json(JsonType::Object)["__missing__"];
}

// Direct-child lookup only - QJsonObject::value()/contains() never searched
// nested objects, while ZJSON::Json::operator[] falls back to a deep search.
const Json* childOf(const Json& object, const std::string& key) {
	if (!object.isObject()) {
		return nullptr;
	}
	for (auto it = object.cbegin(); it != object.cend(); ++it) {
		if (it->key() == key) {
			return &it->value();
		}
	}
	return nullptr;
}

// Iteration yields non-materializing string_view keys, while Json::add/find/
// contains take std::string (string_view does not convert implicitly). Use this
// where an owning key is needed - i.e. when building a new object/array.
std::string ownedKey(std::string_view key) {
	return std::string(key);
}

Json childValue(const Json& object, const std::string& key) {
	const Json* found = childOf(object, key);
	return found != nullptr ? *found : undefinedValue();
}

// QJsonObject::size()
int keyCount(const Json& object) {
	if (!object.isObject()) {
		return 0;
	}
	int count = 0;
	for (auto it = object.cbegin(); it != object.cend(); ++it) {
		++count;
	}
	return count;
}

// Replaces an object member (QJsonObject::insert). Rebuilt instead of
// remove()+add() so that nested children that happen to share the key are kept.
void setObjectValue(Json& object, const std::string& key, const Json& value) {
	if (key.empty()) {
		return;
	}
	Json replacement(JsonType::Object);
	for (auto it = object.cbegin(); it != object.cend(); ++it) {
		if (it->key() != key) {
			replacement.add(ownedKey(it->key()), it->value());
		}
	}
	replacement.add(key, value);
	object = replacement;
}

// In-place array element assignment (QJsonArray::operator[] = value).
//
// NOTE: this must not assign through the child node itself.  ZJSON's
// Json::operator= does `this->brother = nullptr`, which is correct for a
// standalone Json but severs the sibling chain when the target is a linked
// element of an array.  Splice instead: insert() copies only the new value and
// re-links the neighbours (a rebuild would deep-copy every element).
bool setArrayElement(Json& array, int index, const Json& value) {
	if (!array.isArray() || index < 0 || index >= array.size()) {
		return false;
	}
	array.insert(index, value);
	array.remove(index + 1);
	return true;
}

Json asObject(const Json& value) {
	return value.isObject() ? value : Json(JsonType::Object);
}

// get + remove of a *direct* object member (QJsonObject::take). ZJSON::Json::take()
// would deep-search and recursively delete matching keys anywhere in the subtree,
// which is not what the query-parameter handling expects.
Json takeChild(Json& object, const std::string& key) {
	const Json* found = childOf(object, key);
	if (found == nullptr) {
		return undefinedValue();
	}
	const Json value = *found;

	Json replacement(JsonType::Object);
	for (auto it = object.cbegin(); it != object.cend(); ++it) {
		if (it->key() != key) {
			replacement.add(ownedKey(it->key()), it->value());
		}
	}
	object = replacement;
	return value;
}

// QJsonValue::Type ordering (Null=0, Bool=1, Double=2, String=3, Array=4,
// Object=5, Undefined=0x80) - used to build the group key.
int valueTypeCode(const Json& value) {
	if (value.isNull()) {
		return 0;
	}
	if (value.isTrue() || value.isFalse()) {
		return 1;
	}
	if (value.isNumber()) {
		return 2;
	}
	if (value.isString()) {
		return 3;
	}
	if (value.isArray()) {
		return 4;
	}
	if (value.isObject()) {
		return 5;
	}
	return 128;
}

bool isNil(const Json& value) {
	return value.isError() || value.isNull();
}

// QJsonValue::toVariant().toString()
std::string variantText(const Json& value) {
	if (value.isString()) {
		return value.toString();
	}
	if (value.isNumber()) {
		return numberText(value.toDouble());
	}
	if (value.isTrue()) {
		return std::string("true");
	}
	if (value.isFalse()) {
		return std::string("false");
	}
	return std::string();
}

std::string valueText(const Json& value) {
	if (value.isString()) {
		return value.toString();
	}
	if (value.isNumber()) {
		return numberText(value.toDouble());
	}
	if (value.isTrue()) {
		return std::string("true");
	}
	if (value.isFalse()) {
		return std::string("false");
	}
	if (value.isNull() || value.isError()) {
		return std::string();
	}
	// Non-scalars were stringified as a single-element array document
	// (QJsonDocument(QJsonArray{value}).toJson(Compact)) - preserved verbatim.
	return "[" + value.toString() + "]";
}

double numericValue(const Json& value, bool* ok = nullptr) {
	bool localOk = false;
	double result = 0.0;
	if (value.isNumber()) {
		result = value.toDouble();
		localOk = true;
	} else if (value.isString()) {
		result = parseDouble(trim(value.toString()), localOk);
	}
	if (ok != nullptr) {
		*ok = localOk;
	}
	return result;
}

// qFuzzyCompare
bool fuzzyCompare(double left, double right) {
	return std::abs(left - right) <= 0.000000000001 * std::min(std::abs(left), std::abs(right));
}

bool valuesEqual(const Json& left, const Json& right) {
	if (isNil(left) && isNil(right)) {
		return true;
	}
	if (isNil(left) || isNil(right)) {
		return false;
	}

	bool leftOk = false;
	bool rightOk = false;
	const double leftNumber = numericValue(left, &leftOk);
	const double rightNumber = numericValue(right, &rightOk);
	if (leftOk && rightOk) {
		return fuzzyCompare(leftNumber + 1.0, rightNumber + 1.0);
	}
	return valueText(left) == valueText(right);
}

bool containsValue(const Json& left, const Json& right) {
	if (isNil(left) || isNil(right)) {
		return false;
	}
	return valueText(left).find(valueText(right)) != std::string::npos;
}

bool compareByOperator(const Json& left, const std::string& op, const Json& right) {
	bool leftOk = false;
	bool rightOk = false;
	const double leftNumber = numericValue(left, &leftOk);
	const double rightNumber = numericValue(right, &rightOk);
	if (leftOk && rightOk) {
		if (op == ">,") return leftNumber > rightNumber;
		if (op == ">=,") return leftNumber >= rightNumber;
		if (op == "<,") return leftNumber < rightNumber;
		if (op == "<=,") return leftNumber <= rightNumber;
		if (op == "<>,") return !fuzzyCompare(leftNumber + 1.0, rightNumber + 1.0);
		if (op == "=,") return fuzzyCompare(leftNumber + 1.0, rightNumber + 1.0);
	}

	const std::string leftText = valueText(left);
	const std::string rightText = valueText(right);
	if (op == ">,") return leftText > rightText;
	if (op == ">=,") return leftText >= rightText;
	if (op == "<,") return leftText < rightText;
	if (op == "<=,") return leftText <= rightText;
	if (op == "<>,") return leftText != rightText;
	if (op == "=,") return leftText == rightText;
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Response helpers
// ─────────────────────────────────────────────────────────────────────────────

Json makeStatusResponse(StatusCodes code, const std::string& detail = std::string()) {
	std::string info = detail;
	Json response;
	response.add("status", static_cast<int>(code));
	if (!info.empty()) {
		const std::string::size_type index = info.find('\n');
		if (index != std::string::npos) {
			info = info.substr(0, index);
		}
		info.insert(0, " details, ");
	}
	info.insert(0, STCODEMESSAGES[static_cast<int>(code)]);
	response.add("message", info);
	return response;
}

Json makeResponse(StatusCodes code, const std::string& detail = std::string()) {
	return makeStatusResponse(code, detail);
}

Json makeRunResponse(int affectedRows, const Json& insertId = Json(0)) {
	Json response = makeResponse(STSUCCESS);
	response.add("affectedRows", affectedRows);
	response.add("insertId", insertId);
	return response;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store access (JSON array of table objects)
// ─────────────────────────────────────────────────────────────────────────────

Json readStore(const std::string& filePath, bool* ok = nullptr) {
	if (ok != nullptr) {
		*ok = true;  // optimistic; cleared below on actual read failure
	}

	const fs::path path = fsPath(filePath);
	std::error_code existsError;
	if (!fs::exists(path, existsError)) {
		return Json(JsonType::Array);
	}

	std::string raw;
	if (!readWholeFile(path, raw)) {
		logMessage("critical", "Cannot open database file: " + filePath);
		// Distinguish a read failure from a non-existent DB - the file is
		// present but unreadable, so a subsequent write must NOT replace it
		// with an empty store.
		if (ok != nullptr) {
			*ok = false;
		}
		return Json(JsonType::Array);
	}

	if (trim(raw).empty()) {
		logMessage("warning", "Database file is empty: " + filePath);
		return Json(JsonType::Array);
	}

	std::string parseError;
	// ParseJsonStrictUtf8 also rejects invalid UTF-8: such a file is reported
	// as corrupt (and backed up) instead of being loaded verbatim and written
	// back out later.
	const Json document = Json::ParseJsonStrictUtf8(raw, parseError);
	if (document.isError() || !document.isArray()) {
		// Backup corrupted file before returning an empty store, so the
		// existing data is not silently lost on the next write.
		const std::string backupPath = filePath + ".corrupt." + compactTimestamp();
		std::error_code copyError;
		if (fs::copy_file(path, fsPath(backupPath), fs::copy_options::overwrite_existing, copyError)) {
			logMessage("critical", "Database file is corrupted, backed up to: " + backupPath +
									   " parse error: " + parseError);
		} else {
			logMessage("critical", "Database file is corrupted, failed to back up to: " + backupPath +
									   " parse error: " + parseError);
		}
		// Corrupt file - refuse writes to avoid an empty-store overwrite
		// (the corrupt original has been backed up above).
		if (ok != nullptr) {
			*ok = false;
		}
		return Json(JsonType::Array);
	}
	return document;
}

// Atomic replace (temp file + commit/rename). Compact JSON keeps rewrite cost
// proportional to data size without pretty-print padding.
bool writeStore(const std::string& filePath, const Json& store) {
	return writeFileAtomic(filePath, store.toString());
}

int findTableIndex(const Json& store, const std::string& tableName) {
	for (int index = 0; index < store.size(); ++index) {
		if (childValue(store[index], "table").toString() == tableName) {
			return index;
		}
	}
	return -1;
}

Json ensureTable(Json& store, const std::string& tableName) {
	const int existingIndex = findTableIndex(store, tableName);
	if (existingIndex >= 0) {
		return store[existingIndex];
	}

	Json tableObject;
	setObjectValue(tableObject, "table", Json(tableName));
	setObjectValue(tableObject, "rows", Json(JsonType::Array));
	store.push_back(tableObject);
	return tableObject;
}

void replaceTable(Json& store, const Json& tableObject) {
	const std::string tableName = childValue(tableObject, "table").toString();
	const int index = findTableIndex(store, tableName);
	if (index >= 0) {
		setArrayElement(store, index, tableObject);
	} else {
		store.push_back(tableObject);
	}
}

std::vector<std::string> columnNames(const Json& tableObject) {
	std::vector<std::string> columns;
	const Json columnArray = childValue(tableObject, "columns");
	if (!columnArray.isArray()) {
		return columns;
	}
	for (int index = 0; index < columnArray.size(); ++index) {
		const std::string column = trim(columnArray[index].toString());
		if (!column.empty()) {
			columns.push_back(column);
		}
	}
	return columns;
}

Json applySchemaDefaults(const Json& tableObject, const Json& rowObject) {
	Json normalized = asObject(rowObject);
	const std::vector<std::string> columns = columnNames(tableObject);
	for (const std::string& column : columns) {
		if (!normalized.contains(column)) {
			normalized.add(column, Json(nullptr));
		}
	}
	return normalized;
}

// ─────────────────────────────────────────────────────────────────────────────
// Index-aware row operations
//
// These replace the O(n) linear scan in the row helpers with O(1) hash-map
// lookups. deleteRowIndexed uses swap-pop + index fixup.
// ─────────────────────────────────────────────────────────────────────────────

Json upsertRowIndexed(Json& tableObject, const Json& inputRow,
					  std::unordered_map<std::string, int>& idIndex) {
	Json rows = childValue(tableObject, "rows");
	if (!rows.isArray()) {
		rows = Json(JsonType::Array);
	}

	Json normalized = applySchemaDefaults(tableObject, inputRow);
	std::string id = trim(variantText(childValue(normalized, "id")));
	if (id.empty()) {
		id = generateId();
		setObjectValue(normalized, "id", Json(id));
	}

	auto it = idIndex.find(id);
	if (it != idIndex.end()) {
		setArrayElement(rows, it->second, normalized);
	} else {
		idIndex[id] = rows.size();
		rows.push_back(normalized);
	}
	setObjectValue(tableObject, "rows", rows);

	Json response = makeRunResponse(1, Json(id));
	response.add("id", id);
	return response;
}

Json updateRowIndexed(Json& tableObject, const Json& patch, const std::string& id,
					  const std::unordered_map<std::string, int>& idIndex) {
	auto it = idIndex.find(id);
	if (it == idIndex.end()) {
		return makeRunResponse(0);
	}

	Json rows = childValue(tableObject, "rows");
	if (!rows.isArray() || it->second < 0 || it->second >= rows.size()) {
		return makeRunResponse(0);
	}

	Json row = asObject(rows[it->second]);
	if (patch.isObject()) {
		for (auto p = patch.cbegin(); p != patch.cend(); ++p) {
			setObjectValue(row, ownedKey(p->key()), p->value());
		}
	}
	setObjectValue(row, "id", Json(id));
	row = applySchemaDefaults(tableObject, row);
	setArrayElement(rows, it->second, row);
	setObjectValue(tableObject, "rows", rows);
	return makeRunResponse(1);
}

Json deleteRowIndexed(Json& tableObject, const std::string& id,
					  std::unordered_map<std::string, int>& idIndex) {
	auto it = idIndex.find(id);
	if (it == idIndex.end()) {
		return makeRunResponse(0);
	}

	Json rows = childValue(tableObject, "rows");
	if (!rows.isArray()) {
		idIndex.erase(it);
		return makeRunResponse(0);
	}

	const int targetIndex = it->second;
	const int lastIndex = rows.size() - 1;
	if (targetIndex < 0 || targetIndex > lastIndex) {
		idIndex.erase(it);
		return makeRunResponse(0);
	}

	if (targetIndex != lastIndex) {
		// swap-pop: move the last row into the deleted slot
		const Json movedRow = rows[lastIndex];
		setArrayElement(rows, targetIndex, movedRow);
		// Update the index for the moved row
		const std::string movedId = trim(variantText(childValue(movedRow, "id")));
		if (!movedId.empty()) {
			idIndex[movedId] = targetIndex;
		}
	}
	rows.remove(lastIndex);
	idIndex.erase(it);
	setObjectValue(tableObject, "rows", rows);
	return makeRunResponse(1);
}

// Linear-scan fallbacks, used when a table has no id index yet.
Json updateRow(Json& tableObject, const Json& patch, const std::string& id) {
	Json rows = childValue(tableObject, "rows");
	if (!rows.isArray()) {
		return makeRunResponse(0);
	}
	for (int index = 0; index < rows.size(); ++index) {
		Json row = asObject(rows[index]);
		if (!valuesEqual(childValue(row, "id"), Json(id))) {
			continue;
		}
		if (patch.isObject()) {
			for (auto p = patch.cbegin(); p != patch.cend(); ++p) {
				setObjectValue(row, ownedKey(p->key()), p->value());
			}
		}
		setObjectValue(row, "id", Json(id));
		row = applySchemaDefaults(tableObject, row);
		setArrayElement(rows, index, row);
		setObjectValue(tableObject, "rows", rows);
		return makeRunResponse(1);
	}
	return makeRunResponse(0);
}

Json deleteRow(Json& tableObject, const std::string& id) {
	Json rows = childValue(tableObject, "rows");
	if (!rows.isArray()) {
		return makeRunResponse(0);
	}
	for (int index = 0; index < rows.size(); ++index) {
		if (!valuesEqual(childValue(asObject(rows[index]), "id"), Json(id))) {
			continue;
		}
		rows.remove(index);
		setObjectValue(tableObject, "rows", rows);
		return makeRunResponse(1);
	}
	return makeRunResponse(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query pipeline
// ─────────────────────────────────────────────────────────────────────────────

struct AggregateSpec {
	std::string source;
	std::string alias;
};

struct QuerySpec {
	std::vector<std::function<bool(const Json&)>> conditions;
	std::string groupField;
	std::vector<AggregateSpec> countSpecs;
	std::vector<AggregateSpec> sumSpecs;
	std::string sortText;
	int page = 0;
	int size = 20;
	bool hasAggregates = false;
};

Json parseRequestedFields(Json& params, std::vector<std::string>& fields) {
	if (!fields.empty()) {
		return makeResponse(STSUCCESS);
	}

	const Json fieldsValue = takeChild(params, "fields");
	if (fieldsValue.isError()) {
		return makeResponse(STSUCCESS);
	}
	if (!fieldsValue.isArray()) {
		return makeResponse(STPARAMERR, "fields is wrong.");
	}

	for (int index = 0; index < fieldsValue.size(); ++index) {
		if (!fieldsValue[index].isString()) {
			return makeResponse(STPARAMERR, "fields is wrong.");
		}
		fields.push_back(fieldsValue[index].toString());
	}
	return makeResponse(STSUCCESS);
}

std::vector<AggregateSpec> parseAggregateSpecs(const Json& value, bool* ok) {
	std::vector<AggregateSpec> specs;
	const std::vector<std::string> parts = split(value.toString(), ',', true);
	if (parts.size() < 2 || parts.size() % 2 == 1) {
		if (ok != nullptr) {
			*ok = false;
		}
		return specs;
	}

	for (std::string::size_type index = 0; index < parts.size(); index += 2) {
		specs.push_back({parts[index], parts[index + 1]});
	}
	if (ok != nullptr) {
		*ok = true;
	}
	return specs;
}

int countMatches(const Json& rows, const std::string& source) {
	if (source == "*" || source == "1") {
		return rows.size();
	}

	int count = 0;
	for (int index = 0; index < rows.size(); ++index) {
		if (!isNil(childValue(asObject(rows[index]), source))) {
			++count;
		}
	}
	return count;
}

Json sumMatches(const Json& rows, const std::string& source) {
	bool hasValue = false;
	double total = 0.0;
	for (int index = 0; index < rows.size(); ++index) {
		bool ok = false;
		const double numeric = numericValue(childValue(asObject(rows[index]), source), &ok);
		if (ok) {
			hasValue = true;
			total += numeric;
		}
	}
	return hasValue ? Json(total) : Json(nullptr);
}

Json buildAggregateRow(const Json& rows, const QuerySpec& spec) {
	Json row;
	for (const AggregateSpec& countSpec : spec.countSpecs) {
		setObjectValue(row, countSpec.alias, Json(countMatches(rows, countSpec.source)));
	}
	for (const AggregateSpec& sumSpec : spec.sumSpecs) {
		setObjectValue(row, sumSpec.alias, sumMatches(rows, sumSpec.source));
	}
	return row;
}

int compareSortValues(const Json& left, const Json& right) {
	if (isNil(left) && isNil(right)) return 0;
	if (isNil(left)) return 1;
	if (isNil(right)) return -1;

	bool leftOk = false;
	bool rightOk = false;
	const double leftNumber = numericValue(left, &leftOk);
	const double rightNumber = numericValue(right, &rightOk);
	if (leftOk && rightOk) {
		if (leftNumber < rightNumber) return -1;
		if (leftNumber > rightNumber) return 1;
		return 0;
	}
	const int comparison = valueText(left).compare(valueText(right));
	return comparison < 0 ? -1 : (comparison > 0 ? 1 : 0);
}

struct SortClause {
	std::string field;
	int direction = 1;
};

void sortRows(Json& rows, const std::string& sortText) {
	if (trim(sortText).empty() || !rows.isArray()) {
		return;
	}

	std::vector<SortClause> clauses;
	const std::vector<std::string> sortParts = split(sortText, ',', false);
	for (const std::string& part : sortParts) {
		const std::vector<std::string> tokens = splitWhitespace(trim(part));
		if (tokens.empty()) {
			continue;
		}
		const bool descending = tokens.size() > 1 && equalsIgnoreCaseAscii(tokens[1], "desc");
		clauses.push_back({tokens[0], descending ? -1 : 1});
	}

	std::vector<Json> rowList;
	rowList.reserve(static_cast<std::size_t>(rows.size() > 0 ? rows.size() : 0));
	for (int index = 0; index < rows.size(); ++index) {
		rowList.push_back(rows[index]);
	}

	std::sort(rowList.begin(), rowList.end(),
			  [&clauses](const Json& left, const Json& right) {
				  for (const SortClause& clause : clauses) {
					  const int comparison = compareSortValues(childValue(left, clause.field),
															   childValue(right, clause.field));
					  if (comparison != 0) {
						  return comparison * clause.direction < 0;
					  }
				  }
				  return false;
			  });

	Json sorted(JsonType::Array);
	for (const Json& row : rowList) {
		sorted.push_back(row);
	}
	rows = sorted;
}

Json projectFields(const Json& rows, const std::vector<std::string>& fields, const QuerySpec& spec) {
	if (fields.empty()) {
		return rows;
	}

	std::set<std::string> allowed(fields.begin(), fields.end());
	if (!spec.groupField.empty()) {
		allowed.insert(spec.groupField);
	}
	for (const AggregateSpec& countSpec : spec.countSpecs) {
		allowed.insert(countSpec.alias);
	}
	for (const AggregateSpec& sumSpec : spec.sumSpecs) {
		allowed.insert(sumSpec.alias);
	}

	Json projected(JsonType::Array);
	for (int index = 0; index < rows.size(); ++index) {
		const Json row = asObject(rows[index]);
		Json nextRow;
		for (auto it = row.cbegin(); it != row.cend(); ++it) {
			if (allowed.find(ownedKey(it->key())) != allowed.end()) {
				nextRow.add(ownedKey(it->key()), it->value());
			}
		}
		projected.push_back(nextRow);
	}
	return projected;
}

Json makeQueryResult(const Json& rows,
					 int totalRecords,
					 int page,
					 int size,
					 StatusCodes code) {
	Json response = makeResponse(code);
	response.add("data", rows);
	response.add("records", totalRecords);
	const int pages = (page > 0 && size > 0)
		? (totalRecords == 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(totalRecords) / size)))
		: (totalRecords > 0 ? 1 : 0);
	response.add("pages", pages);
	return response;
}

std::string extractColumnName(const std::string& part) {
	// Mirrors: ^[`"]?([^\s`"()]+)[`"]?\s+
	std::string::size_type index = 0;
	if (index < part.size() && (part[index] == '`' || part[index] == '"')) {
		++index;
	}
	const std::string::size_type nameStart = index;
	while (index < part.size()) {
		const char ch = part[index];
		if (std::isspace(static_cast<unsigned char>(ch)) || ch == '`' || ch == '"' ||
			ch == '(' || ch == ')') {
			break;
		}
		++index;
	}
	if (index == nameStart) {
		return std::string();
	}
	const std::string name = part.substr(nameStart, index - nameStart);
	if (index < part.size() && (part[index] == '`' || part[index] == '"')) {
		++index;
	}
	if (index >= part.size() || !std::isspace(static_cast<unsigned char>(part[index]))) {
		return std::string();
	}
	return name;
}

std::vector<std::string> parseCreateColumns(const std::string& sql) {
	const std::string::size_type openIndex = sql.find('(');
	const std::string::size_type closeIndex = sql.rfind(')');
	if (openIndex == std::string::npos || closeIndex == std::string::npos || closeIndex <= openIndex) {
		return {};
	}

	const std::string body = sql.substr(openIndex + 1, closeIndex - openIndex - 1);
	std::vector<std::string> parts;
	std::string current;
	int depth = 0;
	for (const char ch : body) {
		if (ch == '(') {
			++depth;
			current.push_back(ch);
			continue;
		}
		if (ch == ')') {
			depth = depth > 0 ? depth - 1 : 0;
			current.push_back(ch);
			continue;
		}
		if (ch == ',' && depth == 0) {
			parts.push_back(trim(current));
			current.clear();
			continue;
		}
		current.push_back(ch);
	}
	if (!trim(current).empty()) {
		parts.push_back(trim(current));
	}

	std::vector<std::string> columns;
	for (const std::string& part : parts) {
		const std::string trimmed = trim(part);
		const std::string lowered = toLowerAscii(trimmed);
		if (startsWith(lowered, "primary key") || startsWith(lowered, "unique") ||
			startsWith(lowered, "key ") || startsWith(lowered, "constraint")) {
			continue;
		}
		const std::string column = extractColumnName(trimmed);
		if (!column.empty()) {
			columns.push_back(column);
		}
	}
	return columns;
}

// Mirrors the pattern:
//   (?:from|into|update|table|drop\s+table\s+if\s+exists)\s+[`"]?([\w.-]+)[`"]?
// Scanned left to right; at each position the alternatives are tried in order.
std::string resolveTableNameFromSql(const std::string& sql) {
	static const std::vector<std::vector<std::string>> alternatives = {
		{"from"}, {"into"}, {"update"}, {"table"}, {"drop", "table", "if", "exists"}};

	const std::string lowered = toLowerAscii(sql);
	for (std::string::size_type i = 0; i < lowered.size(); ++i) {
		for (const std::vector<std::string>& words : alternatives) {
			std::string::size_type position = i;
			bool matched = true;
			for (std::string::size_type wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
				if (wordIndex > 0) {
					const std::string::size_type spaceStart = position;
					while (position < lowered.size() &&
						   std::isspace(static_cast<unsigned char>(lowered[position]))) {
						++position;
					}
					if (position == spaceStart) {  // \s+ needs at least one character
						matched = false;
						break;
					}
				}
				if (lowered.compare(position, words[wordIndex].size(), words[wordIndex]) != 0) {
					matched = false;
					break;
				}
				position += words[wordIndex].size();
			}
			if (!matched) {
				continue;
			}

			const std::string::size_type spaceStart = position;
			while (position < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[position]))) {
				++position;
			}
			if (position == spaceStart) {  // \s+
				continue;
			}
			if (position < lowered.size() && (lowered[position] == '`' || lowered[position] == '"')) {
				++position;  // [`"]?
			}
			const std::string::size_type nameStart = position;
			while (position < lowered.size()) {
				const char ch = lowered[position];
				const bool wordChar = std::isalnum(static_cast<unsigned char>(ch)) != 0;
				if (!wordChar && ch != '_' && ch != '.' && ch != '-') {
					break;  // [\w.-]+
				}
				++position;
			}
			if (position == nameStart) {
				continue;
			}
			return sql.substr(nameStart, position - nameStart);
		}
	}
	return std::string();
}

// ─────────────────────────────────────────────────────────────────────────────
// SQL text parsing (literals + `?` placeholders, `??` table placeholder)
//
// This is what lets the file backend execute the {"text": ..., "values": [...]}
// elements the other ZORM backends accept in transGo / execSql.
// ─────────────────────────────────────────────────────────────────────────────

// Case-insensitive whole-word search on an already-lowercased haystack; the
// returned index refers to the same offsets in the original text (same length).
std::string::size_type findWordLower(const std::string& lowered, const std::string& word,
									 std::string::size_type from = 0) {
	auto isWordChar = [](char ch) {
		return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
	};
	std::string::size_type pos = lowered.find(word, from);
	while (pos != std::string::npos) {
		const bool leftOk = pos == 0 || !isWordChar(lowered[pos - 1]);
		const std::string::size_type end = pos + word.size();
		const bool rightOk = end >= lowered.size() || !isWordChar(lowered[end]);
		if (leftOk && rightOk) {
			return pos;
		}
		pos = lowered.find(word, pos + 1);
	}
	return std::string::npos;
}

// Splits on `delimiter` at nesting depth 0 only; single quotes are respected.
std::vector<std::string> splitTopLevel(const std::string& text, char delimiter) {
	std::vector<std::string> parts;
	std::string current;
	int depth = 0;
	bool inString = false;
	for (const char ch : text) {
		if (inString) {
			current.push_back(ch);
			if (ch == '\'') {
				inString = false;
			}
			continue;
		}
		if (ch == '\'') {
			inString = true;
			current.push_back(ch);
			continue;
		}
		if (ch == '(') {
			++depth;
		} else if (ch == ')') {
			depth = depth > 0 ? depth - 1 : 0;
		}
		if (ch == delimiter && depth == 0) {
			parts.push_back(current);
			current.clear();
			continue;
		}
		current.push_back(ch);
	}
	parts.push_back(current);
	return parts;
}

// Parses a non-placeholder SQL value token: 'string' (with '' escaping),
// null / true / false, or a fully numeric literal.
bool parseSqlLiteral(const std::string& token, Json& out) {
	const std::string t = trim(token);
	if (t.empty()) {
		return false;
	}
	if (t.front() == '\'') {
		if (t.size() < 2 || t.back() != '\'') {
			return false;
		}
		std::string s;
		for (std::string::size_type i = 1; i + 1 < t.size(); ++i) {
			if (t[i] == '\'') {
				if (i + 2 < t.size() && t[i + 1] == '\'') {  // doubled quote
					s.push_back('\'');
					++i;
				} else {
					return false;  // premature closing quote
				}
			} else {
				s.push_back(t[i]);
			}
		}
		out = Json(s);
		return true;
	}

	const std::string lowered = toLowerAscii(t);
	if (lowered == "null") {
		out = Json(nullptr);
		return true;
	}
	if (lowered == "true") {
		out = Json(true);
		return true;
	}
	if (lowered == "false") {
		out = Json(false);
		return true;
	}

	char* end = nullptr;
	const double value = std::strtod(t.c_str(), &end);
	if (end == t.c_str()) {
		return false;
	}
	while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
		++end;
	}
	if (end != nullptr && *end != '\0') {
		return false;
	}
	out = Json(value);
	return true;
}

// Binds `?` placeholders against the values array in order.
struct ValueCursor {
	const Json& values;
	int index = 0;

	bool next(Json& out) {
		if (!values.isArray() || index >= values.size()) {
			return false;
		}
		out = values[index];
		++index;
		return true;
	}
};

// A single SQL value token: either a `?` placeholder or a literal.
struct SqlValue;
bool parseSqlValue(const std::string& token, SqlValue& out);

// Reads an identifier or the `??` placeholder starting at pos; returns the raw
// token and advances pos. `[\w.-]+` matches the table-name shape used by the
// other backends' generated SQL.
std::string readTableNameToken(const std::string& sql, std::string::size_type& pos, bool& placeholder) {
	while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
		++pos;
	}
	placeholder = false;
	if (pos < sql.size() && sql[pos] == '?') {
		placeholder = true;
		pos += 2;  // skip "??"
		return std::string();
	}
	if (pos < sql.size() && (sql[pos] == '`' || sql[pos] == '"')) {
		++pos;
	}
	const std::string::size_type start = pos;
	while (pos < sql.size()) {
		const char ch = sql[pos];
		const bool wordChar = std::isalnum(static_cast<unsigned char>(ch)) != 0;
		if (!wordChar && ch != '_' && ch != '.' && ch != '-') {
			break;
		}
		++pos;
	}
	return sql.substr(start, pos - start);
}

// Content between the first '(' at/after pos and its matching ')'.
bool readParenthesized(const std::string& sql, std::string::size_type& pos, std::string& body) {
	while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
		++pos;
	}
	if (pos >= sql.size() || sql[pos] != '(') {
		return false;
	}
	int depth = 0;
	bool inString = false;
	const std::string::size_type start = ++pos;
	while (pos < sql.size()) {
		const char ch = sql[pos];
		if (inString) {
			if (ch == '\'') {
				inString = false;
			}
		} else if (ch == '\'') {
			inString = true;
		} else if (ch == '(') {
			++depth;
		} else if (ch == ')') {
			if (depth == 0) {
				body = sql.substr(start, pos - start);
				++pos;
				return true;
			}
			--depth;
		}
		++pos;
	}
	return false;
}

// Values array without its first `skip` elements (used after stripping the
// `??` table-name binding).
Json valuesFrom(const Json& values, int skip) {
	Json rest(JsonType::Array);
	for (int i = skip; i < values.size(); ++i) {
		rest.push_back(values[i]);
	}
	return rest;
}

// A single SQL value token: either a `?` placeholder or a literal.
struct SqlValue {
	bool isPlaceholder = false;
	Json literal;
};

bool parseSqlValue(const std::string& token, SqlValue& out) {
	const std::string t = trim(token);
	if (t == "?") {
		out = SqlValue{true, Json()};
		return true;
	}
	out = SqlValue{false, Json()};
	return parseSqlLiteral(t, out.literal);
}

Json execInsertSql(Json& store,
				   std::unordered_map<std::string, std::unordered_map<std::string, int>>& tableIdxMap,
				   const std::string& sql, const Json& values) {
	const std::string lowered = toLowerAscii(sql);
	std::string::size_type pos = lowered.find("into");
	pos += 4;  // "into"

	bool tablePlaceholder = false;
	std::string::size_type tablePos = pos;
	const std::string tableName = readTableNameToken(sql, tablePos, tablePlaceholder);
	if (tablePlaceholder) {
		// `INSERT INTO ?? ...` - the table name is the first bound value.
		if (!values.isArray() || values.size() < 1) {
			return makeResponse(STDBOPERATEERR, "INSERT INTO ?? needs the table name as the first value");
		}
		return execInsertSql(store, tableIdxMap, "INSERT INTO " + values[0].toString() +
													 sql.substr(tablePos),
							 values.size() > 1 ? valuesFrom(values, 1) : Json(JsonType::Array));
	}
	if (tableName.empty()) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}

	Json tableObject = ensureTable(store, tableName);
	auto& idIndex = tableIdxMap[tableName];
	Json payload;

	// Payload style: `INSERT INTO t ?` / `INSERT INTO ?? ?` - the row object is
	// bound as the next value.
	std::string::size_type afterTable = tablePos;
	while (afterTable < sql.size() && std::isspace(static_cast<unsigned char>(sql[afterTable]))) {
		++afterTable;
	}
	ValueCursor cursor{values};
	if (afterTable < sql.size() && sql[afterTable] == '?') {
		Json bound;
		if (!cursor.next(bound) || !bound.isObject()) {
			return makeResponse(STDBOPERATEERR, "INSERT INTO t ? needs a row object value");
		}
		payload = bound;
		const Json result = upsertRowIndexed(tableObject, payload, idIndex);
		replaceTable(store, tableObject);
		return result;
	}

	// Column-list style: `INSERT INTO t (a,b,c) VALUES (?,?,?)`.
	std::string columnsBody;
	if (!readParenthesized(sql, afterTable, columnsBody)) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	std::string::size_type valuesPos = findWordLower(lowered, "values", afterTable);
	if (valuesPos == std::string::npos) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	valuesPos += 6;  // skip the word "values" itself
	std::string valuesBody;
	if (!readParenthesized(sql, valuesPos, valuesBody)) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}

	std::vector<std::string> columns;
	for (std::string& column : splitTopLevel(columnsBody, ',')) {
		std::string field = trim(column);
		field.erase(std::remove_if(field.begin(), field.end(),
								   [](char ch) { return ch == '`' || ch == '"'; }),
					field.end());
		if (field.empty()) {
			return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
		}
		columns.push_back(field);
	}

	std::vector<std::string> valueTokens = splitTopLevel(valuesBody, ',');
	if (columns.size() != valueTokens.size()) {
		return makeResponse(STDBOPERATEERR, "column count does not match values count");
	}

	Json row(JsonType::Object);
	for (std::size_t i = 0; i < valueTokens.size(); ++i) {
		SqlValue value;
		if (!parseSqlValue(valueTokens[i], value)) {
			return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
		}
		if (value.isPlaceholder) {
			Json bound;
			if (!cursor.next(bound)) {
				return makeResponse(STDBOPERATEERR, "not enough values to bind placeholders");
			}
			row.add(columns[i], bound);
		} else {
			row.add(columns[i], value.literal);
		}
	}

	const Json result = upsertRowIndexed(tableObject, row, idIndex);
	replaceTable(store, tableObject);
	return result;
}

Json execUpdateSql(Json& store,
				   std::unordered_map<std::string, std::unordered_map<std::string, int>>& tableIdxMap,
				   const std::string& sql, const Json& values) {
	const std::string lowered = toLowerAscii(sql);
	std::string::size_type pos = 6;  // "update"

	bool tablePlaceholder = false;
	std::string::size_type tablePos = pos;
	const std::string tableName = readTableNameToken(sql, tablePos, tablePlaceholder);
	if (tablePlaceholder) {
		if (!values.isArray() || values.size() < 1) {
			return makeResponse(STDBOPERATEERR, "UPDATE ?? needs the table name as the first value");
		}
		return execUpdateSql(store, tableIdxMap, "UPDATE " + values[0].toString() +
													 sql.substr(tablePos),
							 values.size() > 1 ? valuesFrom(values, 1) : Json(JsonType::Array));
	}
	if (tableName.empty()) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}

	const std::string::size_type setPos = findWordLower(lowered, "set", tablePos);
	const std::string::size_type wherePos = findWordLower(lowered, "where", tablePos);
	if (setPos == std::string::npos || wherePos == std::string::npos || wherePos < setPos) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	const std::string setClause = sql.substr(setPos + 3, wherePos - setPos - 3);

	// `... where id = <placeholder-or-literal>`
	std::string::size_type idPos = wherePos + 5;
	while (idPos < sql.size() && std::isspace(static_cast<unsigned char>(sql[idPos]))) {
		++idPos;
	}
	if (lowered.compare(idPos, 2, "id") != 0) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	idPos += 2;
	while (idPos < sql.size() && std::isspace(static_cast<unsigned char>(sql[idPos]))) {
		++idPos;
	}
	if (idPos >= sql.size() || sql[idPos] != '=') {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	const std::string idToken = trim(sql.substr(idPos + 1));

	ValueCursor cursor{values};
	Json patch(JsonType::Object);
	for (std::string& assignment : splitTopLevel(setClause, ',')) {
		const std::string::size_type eq = assignment.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string field = trim(assignment.substr(0, eq));
		field.erase(std::remove_if(field.begin(), field.end(),
								   [](char ch) { return ch == '`' || ch == '"'; }),
					field.end());
		if (field.empty()) {
			continue;
		}
		SqlValue value;
		if (!parseSqlValue(assignment.substr(eq + 1), value)) {
			return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
		}
		if (value.isPlaceholder) {
			Json bound;
			if (!cursor.next(bound)) {
				return makeResponse(STDBOPERATEERR, "not enough values to bind placeholders");
			}
			patch.add(field, bound);
		} else {
			patch.add(field, value.literal);
		}
	}

	SqlValue idValue;
	if (!parseSqlValue(idToken, idValue)) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	std::string id;
	if (idValue.isPlaceholder) {
		Json bound;
		if (!cursor.next(bound)) {
			return makeResponse(STDBOPERATEERR, "not enough values to bind placeholders");
		}
		id = trim(variantText(bound));
	} else {
		id = trim(variantText(idValue.literal));
	}

	const int tableIndex = findTableIndex(store, tableName);
	if (tableIndex < 0) {
		return makeRunResponse(0);
	}
	Json tableObject = asObject(store[tableIndex]);
	auto tableIt = tableIdxMap.find(tableName);
	const Json result = (tableIt != tableIdxMap.end())
		? updateRowIndexed(tableObject, patch, id, tableIt->second)
		: updateRow(tableObject, patch, id);
	replaceTable(store, tableObject);
	return result;
}

Json execDeleteSql(Json& store,
				   std::unordered_map<std::string, std::unordered_map<std::string, int>>& tableIdxMap,
				   const std::string& sql, const Json& values) {
	const std::string lowered = toLowerAscii(sql);
	std::string::size_type pos = lowered.find("from");
	if (pos == std::string::npos) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	pos += 4;  // "from"

	bool tablePlaceholder = false;
	std::string::size_type tablePos = pos;
	const std::string tableName = readTableNameToken(sql, tablePos, tablePlaceholder);
	if (tablePlaceholder) {
		if (!values.isArray() || values.size() < 1) {
			return makeResponse(STDBOPERATEERR, "DELETE FROM ?? needs the table name as the first value");
		}
		return execDeleteSql(store, tableIdxMap, "DELETE FROM " + values[0].toString() +
													 sql.substr(tablePos),
							 values.size() > 1 ? valuesFrom(values, 1) : Json(JsonType::Array));
	}
	if (tableName.empty()) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}

	const std::string::size_type wherePos = findWordLower(lowered, "where", tablePos);
	if (wherePos == std::string::npos) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	std::string::size_type idPos = wherePos + 5;
	while (idPos < sql.size() && std::isspace(static_cast<unsigned char>(sql[idPos]))) {
		++idPos;
	}
	if (lowered.compare(idPos, 2, "id") != 0) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	idPos += 2;
	while (idPos < sql.size() && std::isspace(static_cast<unsigned char>(sql[idPos]))) {
		++idPos;
	}
	if (idPos >= sql.size() || sql[idPos] != '=') {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}

	SqlValue idValue;
	if (!parseSqlValue(trim(sql.substr(idPos + 1)), idValue)) {
		return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
	}
	std::string id;
	if (idValue.isPlaceholder) {
		ValueCursor cursor{values};
		Json bound;
		if (!cursor.next(bound)) {
			return makeResponse(STDBOPERATEERR, "not enough values to bind placeholders");
		}
		id = trim(variantText(bound));
	} else {
		id = trim(variantText(idValue.literal));
	}

	const int tableIndex = findTableIndex(store, tableName);
	if (tableIndex < 0) {
		return makeRunResponse(0);
	}
	Json tableObject = asObject(store[tableIndex]);
	auto tableIt = tableIdxMap.find(tableName);
	const Json result = (tableIt != tableIdxMap.end())
		? deleteRowIndexed(tableObject, id, tableIt->second)
		: deleteRow(tableObject, id);
	replaceTable(store, tableObject);
	return result;
}

Json metadataQueryResult(const Json& store, const std::string& sql, const Json& values) {
	std::string tableName;
	if (values.isArray() && values.size() > 0) {
		tableName = trim(values[values.size() - 1].toString());
	}
	if (tableName.empty()) {
		tableName = resolveTableNameFromSql(sql);
	}

	Json rows(JsonType::Array);
	if (!tableName.empty() &&
		(containsNoCase(sql, "sqlite_master") || containsNoCase(sql, "information_schema.tables"))) {
		if (findTableIndex(store, tableName) >= 0) {
			Json row;
			setObjectValue(row, "TABLE_NAME", Json(tableName));
			rows.push_back(row);
		}
	}

	return makeQueryResult(rows, rows.size(), 0, 20, rows.size() == 0 ? STQUERYEMPTY : STSUCCESS);
}

Json execSqlInternalIndexed(Json& store,
							std::unordered_map<std::string, std::unordered_map<std::string, int>>& tableIdxMap,
							const std::string& sql,
							const Json& values) {
	const std::string normalized = toLowerAscii(trim(sql));
	if (startsWith(normalized, "begin") || startsWith(normalized, "commit") ||
		startsWith(normalized, "rollback")) {
		return makeRunResponse(0);
	}

	if (startsWith(normalized, "drop table")) {
		std::string tableName;
		if (values.isArray() && values.size() > 0 && containsNoCase(sql, "??")) {
			tableName = trim(values[0].toString());
		}
		if (tableName.empty()) {
			tableName = resolveTableNameFromSql(sql);
		}
		const int index = findTableIndex(store, tableName);
		if (index >= 0) {
			store.remove(index);
			tableIdxMap.erase(tableName);
			return makeRunResponse(1);
		}
		return makeRunResponse(0);
	}

	if (startsWith(normalized, "create table")) {
		std::string tableName;
		if (values.isArray() && values.size() > 0 && containsNoCase(sql, "??")) {
			tableName = trim(values[0].toString());
		}
		if (tableName.empty()) {
			tableName = resolveTableNameFromSql(sql);
		}
		const int tableIndex = findTableIndex(store, tableName);
		Json tableObject = tableIndex >= 0 ? store[tableIndex] : ensureTable(store, tableName);
		if (!hasChild(tableObject, "rows") || !childValue(tableObject, "rows").isArray()) {
			setObjectValue(tableObject, "rows", Json(JsonType::Array));
		}
		const std::vector<std::string> columns = parseCreateColumns(sql);
		if (!columns.empty()) {
			Json columnArray = childValue(tableObject, "columns");
			if (!columnArray.isArray()) {
				columnArray = Json(JsonType::Array);
			}
			for (const std::string& column : columns) {
				bool exists = false;
				for (int index = 0; index < columnArray.size(); ++index) {
					if (columnArray[index].toString() == column) {
						exists = true;
						break;
					}
				}
				if (!exists) {
					columnArray.push_back(column);
				}
			}
			setObjectValue(tableObject, "columns", columnArray);
		}
		replaceTable(store, tableObject);
		// Ensure an empty index entry exists for the new table
		tableIdxMap.emplace(tableName, std::unordered_map<std::string, int>());
		return makeRunResponse(1);
	}

	if (startsWith(normalized, "insert into")) {
		return execInsertSql(store, tableIdxMap, sql, values);
	}

	if (startsWith(normalized, "update")) {
		return execUpdateSql(store, tableIdxMap, sql, values);
	}

	if (startsWith(normalized, "delete from")) {
		return execDeleteSql(store, tableIdxMap, sql, values);
	}

	return makeResponse(STDBOPERATEERR, "Unsupported SQL: " + sql);
}

// Collect matching rows into a vector before sorting / projecting / paginating.
struct FilteredRows {
	std::vector<Json> rows;
	int totalCount = 0;  // total matches before pagination slicing
};

FilteredRows filterRows(const Json& rows, const QuerySpec& spec) {
	FilteredRows result;
	result.rows.reserve(static_cast<std::size_t>(rows.size() > 0 ? rows.size() : 0));
	for (int index = 0; index < rows.size(); ++index) {
		const Json row = asObject(rows[index]);
		bool matched = true;
		for (const auto& condition : spec.conditions) {
			if (!condition(row)) {
				matched = false;
				break;
			}
		}
		if (matched) {
			result.rows.push_back(row);
		}
	}
	result.totalCount = static_cast<int>(result.rows.size());
	return result;
}

Json buildGroupedRows(const Json& rows, const QuerySpec& spec) {
	// std::map keeps the group ordering that QMap produced (sorted by key).
	std::map<std::string, Json> grouped;
	std::map<std::string, Json> groupValues;
	for (int index = 0; index < rows.size(); ++index) {
		const Json row = asObject(rows[index]);
		const Json groupValue = childValue(row, spec.groupField);
		const std::string key = std::to_string(valueTypeCode(groupValue)) + ":" + valueText(groupValue);
		auto groupedIt = grouped.find(key);
		if (groupedIt == grouped.end()) {
			grouped.emplace(key, Json(JsonType::Array));
		}
		grouped[key].push_back(row);
		groupValues[key] = groupValue;
	}

	Json result(JsonType::Array);
	for (auto& entry : grouped) {
		Json row;
		setObjectValue(row, spec.groupField, groupValues[entry.first]);
		const Json aggregate = buildAggregateRow(entry.second, spec);
		for (auto it = aggregate.cbegin(); it != aggregate.cend(); ++it) {
			setObjectValue(row, ownedKey(it->key()), it->value());
		}
		result.push_back(row);
	}
	return result;
}

Json buildQuerySpec(Json params, std::vector<std::string>& fields, QuerySpec& spec);

Json queryTableRows(const Json& tableObject, Json params, std::vector<std::string> fields) {
	QuerySpec spec;
	Json status = buildQuerySpec(params, fields, spec);
	if (status["status"].toInt() != STSUCCESS) {
		return status;
	}

	const Json allRows = childValue(tableObject, "rows");

	// When there is no sort, no group, no aggregate, and pagination is active,
	// avoid sorting/projection overhead for rows beyond the page by
	// early-terminating the filter loop.
	if (spec.sortText.empty() && spec.groupField.empty() && !spec.hasAggregates && spec.page > 0) {
		const int start = (spec.page - 1) * spec.size;
		std::vector<Json> paged;
		paged.reserve(static_cast<std::size_t>(spec.size > 0 ? spec.size : 0));
		int totalMatches = 0;
		for (int index = 0; index < allRows.size(); ++index) {
			const Json row = asObject(allRows[index]);
			bool matched = true;
			for (const auto& condition : spec.conditions) {
				if (!condition(row)) {
					matched = false;
					break;
				}
			}
			if (!matched) {
				continue;
			}
			++totalMatches;
			if (totalMatches > start && static_cast<int>(paged.size()) < spec.size) {
				paged.push_back(row);
			}
		}
		Json pagedJson(JsonType::Array);
		for (const Json& row : paged) {
			pagedJson.push_back(row);
		}
		return makeQueryResult(pagedJson, totalMatches, spec.page, spec.size,
							   pagedJson.size() == 0 ? STQUERYEMPTY : STSUCCESS);
	}

	// General path: filter everything, then group / aggregate / sort / project.
	const FilteredRows filtered = filterRows(allRows, spec);

	Json filteredJson(JsonType::Array);
	for (const Json& row : filtered.rows) {
		filteredJson.push_back(row);
	}

	Json resultRows(JsonType::Array);
	if (!spec.groupField.empty()) {
		resultRows = buildGroupedRows(filteredJson, spec);
	} else if (spec.hasAggregates) {
		resultRows.push_back(buildAggregateRow(filteredJson, spec));
	} else {
		resultRows = filteredJson;
	}

	sortRows(resultRows, spec.sortText);
	resultRows = projectFields(resultRows, fields, spec);
	const int totalRecords = resultRows.size();

	if (spec.page > 0) {
		const int start = (spec.page - 1) * spec.size;
		Json pagedRows(JsonType::Array);
		const int end = std::min(resultRows.size(), start + spec.size);
		for (int index = start; index < end; ++index) {
			pagedRows.push_back(resultRows[index]);
		}
		return makeQueryResult(pagedRows, totalRecords, spec.page, spec.size,
							   pagedRows.size() == 0 ? STQUERYEMPTY : STSUCCESS);
	}

	return makeQueryResult(resultRows, totalRecords, 0, spec.size,
						   resultRows.size() == 0 ? STQUERYEMPTY : STSUCCESS);
}

Json buildQuerySpec(Json params, std::vector<std::string>& fields, QuerySpec& spec) {
	Json fieldStatus = parseRequestedFields(params, fields);
	if (fieldStatus["status"].toInt() != STSUCCESS) {
		return fieldStatus;
	}

	const std::string fuzzy = trim(takeChild(params, "fuzzy").toString());
	spec.sortText = trim(takeChild(params, "sort").toString());
	spec.page = parseInt(trim(takeChild(params, "page").toString()));
	spec.size = parseInt(trim(takeChild(params, "size").toString()));
	if (spec.size <= 0) {
		spec.size = 20;
	}
	spec.groupField = trim(takeChild(params, "group").toString());

	bool ok = true;
	const Json countValue = takeChild(params, "count");
	if (!countValue.isError()) {
		spec.countSpecs = parseAggregateSpecs(countValue, &ok);
		if (!ok) {
			return makeResponse(STPARAMERR, "count is wrong.");
		}
	}

	const Json sumValue = takeChild(params, "sum");
	if (!sumValue.isError()) {
		spec.sumSpecs = parseAggregateSpecs(sumValue, &ok);
		if (!ok) {
			return makeResponse(STPARAMERR, "sum is wrong.");
		}
	}
	spec.hasAggregates = !spec.countSpecs.empty() || !spec.sumSpecs.empty();

	const std::vector<std::string> comparePrefixes = {">,", ">=,", "<,", "<=,", "<>,", "=,"};

	const Json allKeys = params.getAllKeys();
	for (int index = 0; index < allKeys.size(); ++index) {
		const std::string key = allKeys[index].toString();
		const Json value = params[key];
		const std::string text = value.toString();

		if (key == "ins" || key == "lks" || key == "ors") {
			const std::vector<std::string> parts = split(text, ',', true);
			if (parts.size() < 2 ||
				((key == "lks" || key == "ors") && (parts.size() % 2 == 1))) {
				return makeResponse(STPARAMERR, key + " is wrong.");
			}

			if (key == "ins") {
				const std::string fieldName = parts[0];
				const std::vector<std::string> expectedValues(parts.begin() + 1, parts.end());
				spec.conditions.push_back([fieldName, expectedValues](const Json& row) {
					for (const std::string& expected : expectedValues) {
						if (valuesEqual(childValue(row, fieldName), Json(expected))) {
							return true;
						}
					}
					return false;
				});
			} else {
				spec.conditions.push_back([key, parts](const Json& row) {
					for (std::string::size_type pairIndex = 0; pairIndex + 1 < parts.size(); pairIndex += 2) {
						const std::string& fieldName = parts[pairIndex];
						const std::string& expected = parts[pairIndex + 1];
						const bool matches = key == "lks"
							? containsValue(childValue(row, fieldName), Json(expected))
							: valuesEqual(childValue(row, fieldName), Json(expected));
						if (matches) {
							return true;
						}
					}
					return false;
				});
			}
			continue;
		}

		bool handledComparison = false;
		for (const std::string& prefix : comparePrefixes) {
			if (!startsWith(text, prefix)) {
				continue;
			}

			const std::vector<std::string> parts = split(text, ',', true);
			if (parts.size() == 2) {
				const std::string op = parts[0] + ",";
				const std::string expected = parts[1];
				spec.conditions.push_back([key, op, expected](const Json& row) {
					return compareByOperator(childValue(row, key), op, Json(expected));
				});
				handledComparison = true;
			} else if (parts.size() == 4) {
				const std::string opA = parts[0] + ",";
				const std::string expectedA = parts[1];
				const std::string opB = parts[2] + ",";
				const std::string expectedB = parts[3];
				spec.conditions.push_back([key, opA, expectedA, opB, expectedB](const Json& row) {
					return compareByOperator(childValue(row, key), opA, Json(expectedA)) &&
						   compareByOperator(childValue(row, key), opB, Json(expectedB));
				});
				handledComparison = true;
			} else {
				return makeResponse(STPARAMERR, "not equal value is wrong.");
			}
			break;
		}
		if (handledComparison) {
			continue;
		}

		if (fuzzy == "1") {
			spec.conditions.push_back([key, text](const Json& row) {
				return containsValue(childValue(row, key), Json(text));
			});
			continue;
		}

		if (text == "null") {
			spec.conditions.push_back([key](const Json& row) {
				return isNil(childValue(row, key));
			});
			continue;
		}

		spec.conditions.push_back([key, text](const Json& row) {
			return valuesEqual(childValue(row, key), Json(text));
		});
	}

	return makeResponse(STSUCCESS);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// JsonFileDb
// ─────────────────────────────────────────────────────────────────────────────

namespace ZORM {
namespace JsonFile {

JsonFileDb::JsonFileDb(const std::string& filePath, bool logFlag)
	: filePath_(resolvedDatabasePath(filePath))
	, fileLock_(std::make_unique<FileLock>(filePath_ + ".lock")) {
	// logFlag keeps the constructor shape of the other backends; operational
	// problems (corrupt files, stale locks) are always reported to stderr.
	(void)logFlag;
	store_ = Json(JsonType::Array);
	sweepStaleTempFiles();
}

JsonFileDb::~JsonFileDb() = default;

// Removes leftovers of writeFileAtomic (`<db>.tmp.<pid>.<hex>`) whose owning
// process is gone.  Liveness of the recorded PID - not age - decides: a temp
// file whose writer is still running is left alone, because it may be an
// in-flight atomic replace of another process.
void JsonFileDb::sweepStaleTempFiles() {
	const std::string prefix = genericUtf8(fsPath(filePath_).filename()) + ".tmp.";

	std::error_code iteratorError;
	const fs::path directory = fsPath(filePath_).parent_path();
	if (directory.empty()) {
		return;
	}
	for (fs::directory_iterator it(directory, iteratorError), end; !iteratorError && it != end;
		 it.increment(iteratorError)) {
		const std::string name = genericUtf8(it->path().filename());
		if (name.compare(0, prefix.size(), prefix) != 0) {
			continue;
		}
		// "<db>.tmp.<pid>.<hex>" - extract the pid component.
		const std::string rest = name.substr(prefix.size());
		const std::string::size_type dot = rest.find('.');
		if (dot == std::string::npos) {
			continue;
		}
		const long long pid = std::strtoll(rest.substr(0, dot).c_str(), nullptr, 10);
		if (pid <= 0 || pid == currentProcessId() || isProcessAlive(pid)) {
			continue;
		}
		std::error_code removeError;
		fs::remove(it->path(), removeError);
		if (!removeError) {
			logMessage("warning", "Removed stale temp file: " + genericUtf8(it->path()));
		}
	}
}

void JsonFileDb::ensureLoadedLocked() {
	if (loaded_) {
		return;
	}
	bool readOk = true;
	Json loaded = readStore(filePath_, &readOk);
	if (!readOk) {
		// Do not cache an empty store as if it were the database: remember the
		// failure so reads report it, and do not retry on every read (a corrupt
		// file would otherwise be backed up again and again).  A later
		// successful write reloads and clears the flag.
		loadFailed_ = true;
		loaded_ = true;
		return;
	}
	loadFailed_ = false;
	store_ = std::move(loaded);
	loaded_ = true;
	rebuildIndexLocked();
}

Json JsonFileDb::writeWithLock(const std::function<Json()>& modify) {
	// 1) Serialize writers of this process, then take the cross-process lock.
	//    Both are acquired *before* storeMutex_ so that waiting for another
	//    process (up to 30 s) never blocks this process's readers.
	std::unique_lock<std::mutex> writerLock(writeMutex_);

	const bool locked = fileLock_->tryLock(30000);
	if (!locked) {
		long long holderPid = 0;
		std::string holderHost;
		std::string holderApp;
		if (fileLock_->readLockInfo(&holderPid, &holderHost, &holderApp) && isProcessAlive(holderPid)) {
			logMessage("warning", "Cross-process lock for " + filePath_ +
									  " is held by live process " + std::to_string(holderPid) +
									  " (" + holderApp + ") on " + holderHost +
									  " - refusing write to avoid data loss");
			return makeResponse(STDBOPERATEERR, "database locked by another process");
		}
		logMessage("warning", "Failed to acquire cross-process lock for " + filePath_ +
								  " (stale lock or timeout); proceeding without lock");
	}

	// 2) Exclusive access to the in-memory state (readers hold it shared).
	std::unique_lock<std::shared_mutex> storeLock(storeMutex_);

	// 3) Reload from disk: another process may have written since the last load.
	bool readOk = true;
	Json reloaded = readStore(filePath_, &readOk);
	if (!readOk) {
		// A read failure must not be treated as an empty database. Refuse
		// the write and keep store_/tableIdIndex_ exactly as they are, so reads
		// continue to serve the last known good state.  Only when nothing usable
		// is in memory (never loaded, or a previous load already failed) do we
		// mark the database as failed so reads report the error instead of an
		// empty result set.
		if (!loaded_ || loadFailed_) {
			loadFailed_ = true;
		}
		if (locked) {
			fileLock_->unlock();
		}
		return makeResponse(STDBOPERATEERR, "database read failed");
	}

	// Publish the freshly loaded store. `previous` keeps the last known good
	// state (moved, not copied) so a failed modification can be rolled back.
	const bool wasLoaded = loaded_;
	Json previous = std::move(store_);
	const auto indexBackup = tableIdIndex_;  // describes `previous`
	store_ = std::move(reloaded);
	loaded_ = true;
	loadFailed_ = false;
	rebuildIndexLocked();

	const auto rollback = [this, &previous, &indexBackup, wasLoaded]() {
		store_ = std::move(previous);
		tableIdIndex_ = indexBackup;
		loaded_ = wasLoaded;
	};

	Json result;
	try {
		result = modify();
	} catch (...) {
		// A throwing modify() must not leave the lock held nor the in-memory
		// state/index ahead of the on-disk content.
		rollback();
		if (locked) {
			fileLock_->unlock();
		}
		throw;
	}

	// Only persist if the operation succeeded, to avoid writing partial/corrupt
	// state on failure (e.g. execSql returning non-200).
	if (result["status"].toInt() == 200) {
		if (!writeStore(filePath_, store_)) {
			rollback();
			if (locked) {
				fileLock_->unlock();
			}
			return makeResponse(STDBOPERATEERR, "write store failed");
		}
	} else {
		rollback();
	}
	if (locked) {
		fileLock_->unlock();
	}
	return result;
}

void JsonFileDb::rebuildIndexLocked() {
	tableIdIndex_.clear();
	for (int tableIndex = 0; tableIndex < store_.size(); ++tableIndex) {
		const Json tableObj = asObject(store_[tableIndex]);
		const std::string tableName = childValue(tableObj, "table").toString();
		const Json rows = childValue(tableObj, "rows");
		auto& idIndex = tableIdIndex_[tableName];
		for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
			const std::string id = trim(variantText(childValue(asObject(rows[rowIndex]), "id")));
			if (!id.empty()) {
				idIndex[id] = rowIndex;
			}
		}
	}
}

Json JsonFileDb::select(const string& tablename,
						const Json& params,
						vector<string> fields,
						Json values) {
	(void)values;
	// Shared read path. Upgrade to exclusive only for the one-time lazy load.
	std::shared_lock<std::shared_mutex> readLock(storeMutex_);
	if (!loaded_) {
		readLock.unlock();
		std::unique_lock<std::shared_mutex> writeLock(storeMutex_);
		ensureLoadedLocked();
		writeLock.unlock();
		readLock.lock();
	}
	// Report an unreadable/corrupt database file as an error rather than as an
	// empty result set (set by ensureLoadedLocked / writeWithLock).
	if (loadFailed_) {
		return makeResponse(STDBOPERATEERR, "database read failed");
	}

	const int index = findTableIndex(store_, tablename);
	if (index < 0) {
		return makeQueryResult(Json(JsonType::Array), 0, 0, 20, STQUERYEMPTY);
	}

	const Json tableObj = asObject(store_[index]);
	const Json allRows = childValue(tableObj, "rows");

	// Fast path: when `id` is the *only* parameter the per-table id -> row index
	// answers the query in O(1) instead of scanning the table.  `fields`
	// (projection) is still honoured here; any extra condition falls through to
	// the full query path below.
	if (params.isObject() && hasChild(params, "id")) {
		bool onlyId = true;
		if (keyCount(params) != 1 || !params.contains("id")) {
			onlyId = false;
		}
		if (onlyId) {
			const std::string id = params["id"].toString();
			auto tableIt = tableIdIndex_.find(tablename);
			if (tableIt != tableIdIndex_.end()) {
				auto idIt = tableIt->second.find(id);
				if (idIt != tableIt->second.end() && idIt->second >= 0 && idIt->second < allRows.size()) {
					Json rows(JsonType::Array);
					rows.push_back(allRows[idIt->second]);
					return makeQueryResult(projectFields(rows, fields, QuerySpec{}), 1, 0, 20, STSUCCESS);
				}
				return makeQueryResult(Json(JsonType::Array), 0, 0, 20, STQUERYEMPTY);
			}
		}
	}

	// Fallback: full table scan for non-id or compound queries.
	return queryTableRows(tableObj, Json(params), std::move(fields));
}

Json JsonFileDb::create(const string& tablename, const Json& params) {
	if (params.isError() || (!params.isObject() && !params.isArray()) ||
		(params.isArray() ? params.size() == 0 : keyCount(params) == 0)) {
		return makeResponse(STPARAMERR);
	}
	if (params.isArray() && params.size() > 1) {
		return insertBatch(tablename, params);
	}

	return writeWithLock([&]() -> Json {
		Json tableObject = ensureTable(store_, tablename);
		const Json row = params.isArray() ? params[0] : params;
		const Json result = upsertRowIndexed(tableObject, row, tableIdIndex_[tablename]);
		replaceTable(store_, tableObject);
		return result;
	});
}

Json JsonFileDb::update(const string& tablename, const Json& params) {
	if (params.isError() || !params.isObject() || !hasChild(params, "id")) {
		return makeResponse(STPARAMERR);
	}

	return writeWithLock([&]() -> Json {
		const int tableIndex = findTableIndex(store_, tablename);
		if (tableIndex < 0) {
			return makeRunResponse(0);
		}
		Json tableObject = asObject(store_[tableIndex]);
		Json patch = params;
		const std::string id = takeChild(patch, "id").toString();
		const Json result = updateRowIndexed(tableObject, patch, id, tableIdIndex_[tablename]);
		replaceTable(store_, tableObject);
		return result;
	});
}

Json JsonFileDb::remove(const string& tablename, const Json& params) {
	if (params.isError() || !hasChild(params, "id")) {
		return makeResponse(STPARAMERR);
	}

	return writeWithLock([&]() -> Json {
		const int tableIndex = findTableIndex(store_, tablename);
		if (tableIndex < 0) {
			return makeRunResponse(0);
		}
		Json tableObject = asObject(store_[tableIndex]);
		const Json result = deleteRowIndexed(tableObject, params["id"].toString(), tableIdIndex_[tablename]);
		replaceTable(store_, tableObject);
		return result;
	});
}

Json JsonFileDb::querySql(const string& sql,
						  Json params,
						  Json values,
						  vector<string> fields) {
	(void)values;
	std::shared_lock<std::shared_mutex> readLock(storeMutex_);
	if (!loaded_) {
		readLock.unlock();
		std::unique_lock<std::shared_mutex> writeLock(storeMutex_);
		ensureLoadedLocked();
		writeLock.unlock();
		readLock.lock();
	}
	if (loadFailed_) {
		return makeResponse(STDBOPERATEERR, "database read failed");
	}

	const std::string lowered = toLowerAscii(trim(sql));
	// Metadata shims: table existence is answerable, view/column catalogs are not.
	if (containsNoCase(sql, "information_schema.views") ||
		containsNoCase(sql, "information_schema.columns")) {
		return makeQueryResult(Json(JsonType::Array), 0, 0, 20, STQUERYEMPTY);
	}
	if (containsNoCase(sql, "sqlite_master") || containsNoCase(sql, "information_schema.tables")) {
		return metadataQueryResult(store_, sql, values);
	}

	// Plain "select ... from <table>" without a literal WHERE clause: the
	// conditions come from `params`, exactly like select().
	if (startsWith(lowered, "select") && findWordLower(lowered, "where") == std::string::npos) {
		const std::string tableName = resolveTableNameFromSql(sql);
		const int tableIndex = tableName.empty() ? -1 : findTableIndex(store_, tableName);
		if (tableIndex < 0) {
			return makeQueryResult(Json(JsonType::Array), 0, 0, 20, STQUERYEMPTY);
		}
		return queryTableRows(asObject(store_[tableIndex]), Json(params), std::move(fields));
	}

	// Anything else (SELECT with a WHERE clause, JOINs, ...) is not the file
	// backend's business - report an empty result set instead of guessing.
	return makeQueryResult(Json(JsonType::Array), 0, 0, 20, STQUERYEMPTY);
}

Json JsonFileDb::execSql(const string& sql, Json params, Json values) {
	(void)params;
	return writeWithLock([&]() -> Json {
		// execSqlInternalIndexed maintains tableIdIndex_ for every SQL branch
		// (drop table, create table, insert into, update, delete from), so a
		// full rebuild is unnecessary here.
		return execSqlInternalIndexed(store_, tableIdIndex_, sql, values);
	});
}

Json JsonFileDb::insertBatch(const string& tablename,
							 const Json& elements,
							 string constraint) {
	(void)constraint;
	if (!elements.isArray() || elements.size() == 0) {
		return makeResponse(STPARAMERR);
	}

	return writeWithLock([&]() -> Json {
		Json tableObject = ensureTable(store_, tablename);
		auto& idIndex = tableIdIndex_[tablename];
		Json lastInsertId(0);
		for (int index = 0; index < elements.size(); ++index) {
			const Json result = upsertRowIndexed(tableObject, asObject(elements[index]), idIndex);
			lastInsertId = result["insertId"];
		}
		replaceTable(store_, tableObject);
		return makeRunResponse(elements.size(), lastInsertId);
	});
}

Json JsonFileDb::transGo(const Json& sqls, bool isAsync) {
	(void)isAsync;
	if (!sqls.isArray() || sqls.size() == 0) {
		return makeResponse(STPARAMERR);
	}

	return writeWithLock([&]() -> Json {
		// The transaction mutates store_ in place; if any step fails we return
		// its (non-200) result, which makes writeWithLock roll the store and the
		// id index back to the pre-transaction state.
		for (int index = 0; index < sqls.size(); ++index) {
			const Json element = asObject(sqls[index]);

			// SQL text style: {"text": sql, "values": [...]} (ZORM backends)
			// or {"sql": sql, "values": [...]} (orm project style).
			const Json* sqlChild = childOf(element, "sql");
			if (sqlChild == nullptr) {
				sqlChild = childOf(element, "text");
			}
			if (sqlChild != nullptr && sqlChild->isString()) {
				const Json valuesChild = childValue(element, "values");
				const Json values = valuesChild.isArray() ? valuesChild : Json(JsonType::Array);
				const Json result = execSqlInternalIndexed(store_, tableIdIndex_,
														   sqlChild->toString(), values);
				if (result["status"].toInt() != STSUCCESS) {
					return result;
				}
				continue;
			}

			// Structured style: {table, method, params, id}.
			const std::string tableName = childValue(element, "table").toString();
			const std::string method = childValue(element, "method").toString();
			Json tableObject = ensureTable(store_, tableName);
			Json result = makeResponse(STPARAMERR, "transaction element is wrong.");

			auto& idIndex = tableIdIndex_[tableName];
			const Json params = childValue(element, "params");
			const bool hasId = hasChild(element, "id");
			if (method == "Insert" && params.isObject()) {
				result = upsertRowIndexed(tableObject, params, idIndex);
			} else if (method == "Update" && params.isObject() && hasId) {
				result = updateRowIndexed(tableObject, params, childValue(element, "id").toString(), idIndex);
			} else if (method == "Delete" && hasId) {
				result = deleteRowIndexed(tableObject, childValue(element, "id").toString(), idIndex);
			} else if (method == "Batch" && params.isArray()) {
				result = makeRunResponse(0);
				for (int rowIndex = 0; rowIndex < params.size(); ++rowIndex) {
					result = upsertRowIndexed(tableObject, asObject(params[rowIndex]), idIndex);
				}
			}

			if (result["status"].toInt() != STSUCCESS) {
				return result;
			}
			replaceTable(store_, tableObject);
		}

		Json response = makeResponse(STSUCCESS, "trans run success");
		response.add("affectedRows", sqls.size());
		return response;
	});
}

// - Static members ------------------------------------------------------------
std::mutex JsonFileDb::s_mutex_;
std::unordered_map<std::string, std::weak_ptr<JsonFileDb>> JsonFileDb::s_instances_;

std::shared_ptr<JsonFileDb> JsonFileDb::createShared(const std::string& filePath, bool logFlag) {
	const std::string resolved = resolvedDatabasePath(trim(filePath).empty() ? std::string() : filePath);
	std::lock_guard<std::mutex> lock(s_mutex_);
	// Purge expired entries from the registry while we hold the lock.
	for (auto it = s_instances_.begin(); it != s_instances_.end();) {
		if (it->second.expired()) {
			it = s_instances_.erase(it);
		} else {
			++it;
		}
	}
	// Check for an existing (still-alive) instance.
	auto existing = s_instances_.find(resolved);
	if (existing != s_instances_.end()) {
		if (auto instance = existing->second.lock()) {
			return instance;
		}
	}
	// Create a new instance and register it.
	auto instance = std::make_shared<JsonFileDb>(resolved, logFlag);
	s_instances_.emplace(resolved, instance);
	return instance;
}

std::string JsonFileDb::storagePath() const {
	return filePath_;
}

std::string JsonFileDb::defaultStoragePath() {
	return defaultDatabasePath();
}

}  // namespace JsonFile
}  // namespace ZORM
