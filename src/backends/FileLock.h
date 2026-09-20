#pragma once

// Cross-process lock file + the small file/process/time primitives it needs.
//
// Internal header (not part of the public API): shared by JsonFileDb.cpp and
// by the unit tests, which exercise FileLock's ownership and stale-reclaim
// rules directly.
//
// The lock file records "<pid>\n<hostname>\n<appname>\n<unix-millis>\n" (the
// first three lines match QLockFile's layout). A lock is reclaimed as stale when
// its timestamp is older than kLockStaleMillis or when the holder PID on this
// host is no longer alive. unlock() only removes the file while it is still
// owned by this process - see the comment on unlock().
//
// The detail:: helpers stay header-only (inline) because the unit tests use
// them directly; the FileLock method bodies live in FileLock.cpp so the
// platform headers (windows.h / fcntl.h) are not pulled into test TUs.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ZORM {
namespace JsonFile {

// Mirrors QLockFile's default staleLockTime: a lock file older than this (or a
// lock whose holder process is gone) is considered abandoned and reclaimed.
inline constexpr long long kLockStaleMillis = 30000;
inline constexpr const char* kLockAppName = "ZORM/JsonFileDb";

namespace detail {

inline std::filesystem::path fsPath(const std::string& utf8Path) {
#if defined(__cpp_char8_t)
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
#else
	return std::filesystem::u8path(utf8Path);
#endif
}

inline std::string genericUtf8(const std::filesystem::path& path) {
	const auto encoded = path.generic_u8string();
#if defined(__cpp_char8_t)
	return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
	return encoded;
#endif
}

inline std::string trim(const std::string& text) {
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

// QString::split(delimiter, KeepEmptyParts|SkipEmptyParts)
inline std::vector<std::string> split(const std::string& text, char delimiter, bool keepEmpty) {
	std::vector<std::string> parts;
	std::string current;
	for (const char ch : text) {
		if (ch == delimiter) {
			if (keepEmpty || !current.empty()) {
				parts.push_back(current);
			}
			current.clear();
		} else {
			current.push_back(ch);
		}
	}
	if (keepEmpty || !current.empty()) {
		parts.push_back(current);
	}
	return parts;
}

inline bool equalsIgnoreCaseAscii(const std::string& left, const std::string& right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::string::size_type i = 0; i < left.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
			return false;
		}
	}
	return true;
}

inline std::string toLowerAscii(const std::string& text) {
	std::string lowered = text;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return lowered;
}

inline long long nowMillis() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch())
		.count();
}

// Platform-dependent primitives - defined in FileLock.cpp so the platform
// headers (windows.h / fcntl.h, signal.h) stay out of this header.
long long currentProcessId();
std::string hostName();
// Returns true if the PID still refers to a live process. Used to distinguish a
// stale lock from a lock held by a running instance, so that stale locks can be
// safely broken while a lock held by a live process is respected.
bool isProcessAlive(long long pid);

inline bool readWholeFile(const std::filesystem::path& path, std::string& out) {
	std::ifstream input(path, std::ios::binary);
	if (!input.is_open()) {
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	if (size < 0) {
		return false;
	}
	input.seekg(0, std::ios::beg);
	out.assign(static_cast<std::string::size_type>(size), '\0');
	if (size > 0) {
		input.read(&out[0], size);
	}
	if (input.bad()) {
		out.clear();
		return false;
	}
	return true;
}

inline void logMessage(const char* level, const std::string& message) {
	static std::mutex logMutex;
	std::lock_guard<std::mutex> guard(logMutex);
	std::cerr << "[JsonFileDb] " << level << ": " << message << std::endl;
}

}  // namespace detail

// A cross-process lock implemented with an exclusively created lock file.
//
// Ownership rules:
//   * tryLock() creates the file with <pid>/<host>/<app>/<timestamp>; when the
//     file already exists it waits, reclaiming the file only if it is stale
//     (holder PID dead on this host, or timestamp older than kLockStaleMillis).
//   * unlock() removes the file *only while it is still owned by this process*.
//     If the lock was reclaimed as stale by somebody else (or replaced
//     externally) the file belongs to the new owner and must be left alone -
//     deleting it would admit a third writer while the new owner still believes
//     it holds the lock.
class FileLock {
public:
	explicit FileLock(std::string lockPath);
	~FileLock();

	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;

	bool tryLock(int timeoutMs);
	void unlock();
	bool readLockInfo(long long* pid, std::string* host, std::string* appName) const;

	// Text written to the lock file for a given owner (exposed for tests).
	static std::string formatLockInfo(long long pid, const std::string& host, const std::string& appName,
									  long long millis);

	const std::string& path() const {
		return path_;
	}

private:
	bool createExclusive();
	bool isStale() const;

	std::string path_;
	bool locked_ = false;
};

}  // namespace JsonFile
}  // namespace ZORM
