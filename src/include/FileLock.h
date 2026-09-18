#pragma once

// Cross-process lock file + the small file/process/time primitives it needs.
//
// Internal header (not part of the public API): shared by JsonFileDb.cc and by
// the unit tests, which exercise FileLock's ownership and stale-reclaim rules
// directly.
//
// The lock file records "<pid>\n<hostname>\n<appname>\n<unix-millis>\n" (the
// first three lines match QLockFile's layout). A lock is reclaimed as stale when
// its timestamp is older than kLockStaleMillis or when the holder PID on this
// host is no longer alive. unlock() only removes the file while it is still
// owned by this process - see the comment on unlock().

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
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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

inline long long currentProcessId() {
#ifdef _WIN32
	return static_cast<long long>(::GetCurrentProcessId());
#else
	return static_cast<long long>(::getpid());
#endif
}

#ifdef _WIN32
inline std::string wideToUtf8(const wchar_t* text, int length) {
	if (text == nullptr || length <= 0) {
		return std::string();
	}
	const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		return std::string();
	}
	std::string out(static_cast<std::string::size_type>(needed), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], needed, nullptr, nullptr);
	return out;
}
#endif

inline std::string hostName() {
#ifdef _WIN32
	wchar_t buffer[256] = {0};
	DWORD length = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
	if (::GetComputerNameW(buffer, &length) != 0) {
		return wideToUtf8(buffer, static_cast<int>(length));
	}
	return std::string("localhost");
#else
	char buffer[256] = {0};
	if (::gethostname(buffer, sizeof(buffer) - 1) == 0) {
		return std::string(buffer);
	}
	return std::string("localhost");
#endif
}

// Returns true if the PID still refers to a live process. Used to distinguish a
// stale lock from a lock held by a running instance, so that stale locks can be
// safely broken while a lock held by a live process is respected.
inline bool isProcessAlive(long long pid) {
	if (pid <= 0) {
		return false;
	}
#ifdef _WIN32
	HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (!handle) {
		return false;
	}
	DWORD exitCode = 0;
	const bool alive = ::GetExitCodeProcess(handle, &exitCode) && (exitCode == STILL_ACTIVE);
	::CloseHandle(handle);
	return alive;
#else
	return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

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
	explicit FileLock(std::string lockPath)
		: path_(std::move(lockPath)) {
	}

	~FileLock() {
		unlock();
	}

	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;

	bool tryLock(int timeoutMs) {
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const std::chrono::steady_clock::time_point deadline =
			started + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
		int reclaimAttempts = 0;

		for (;;) {
			if (createExclusive()) {
				locked_ = true;
				return true;
			}
			if (isStale()) {
				// Do not loop forever if the stale file cannot be removed.
				if (++reclaimAttempts > 5) {
					return false;
				}
				std::error_code removeError;
				std::filesystem::remove(detail::fsPath(path_), removeError);
				continue;
			}
			if (timeoutMs <= 0 || std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			// Short poll first: contention is normally a sibling process
			// finishing a write (single-digit milliseconds), so a 50 ms poll
			// would add latency for no reason. Back off after 200 ms.
			const long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(
										 std::chrono::steady_clock::now() - started)
										 .count();
			std::this_thread::sleep_for(std::chrono::milliseconds(waited < 200 ? 5 : 50));
		}
	}

	void unlock() {
		if (!locked_) {
			return;
		}
		locked_ = false;

		long long pid = 0;
		std::string host;
		std::string appName;
		// Remove when the file is unreadable (missing / truncated by us) or when
		// it still records this process as the owner.
		const bool readable = readLockInfo(&pid, &host, &appName);
		const bool ownedByUs = readable && pid == detail::currentProcessId() &&
							   detail::equalsIgnoreCaseAscii(host, detail::hostName());
		if (!readable || ownedByUs) {
			std::error_code removeError;
			std::filesystem::remove(detail::fsPath(path_), removeError);
		}
	}

	bool readLockInfo(long long* pid, std::string* host, std::string* appName) const {
		std::string content;
		if (!detail::readWholeFile(detail::fsPath(path_), content)) {
			return false;
		}
		const std::vector<std::string> lines = detail::split(content, '\n', true);
		if (lines.size() < 3) {
			return false;
		}
		if (pid) {
			*pid = std::strtoll(detail::trim(lines[0]).c_str(), nullptr, 10);
		}
		if (host) {
			*host = detail::trim(lines[1]);
		}
		if (appName) {
			*appName = detail::trim(lines[2]);
		}
		return true;
	}

	// Text written to the lock file for a given owner (exposed for tests).
	static std::string formatLockInfo(long long pid, const std::string& host, const std::string& appName,
									  long long millis) {
		return std::to_string(pid) + "\n" + host + "\n" + appName + "\n" + std::to_string(millis) + "\n";
	}

	const std::string& path() const {
		return path_;
	}

private:
	bool createExclusive() {
		const std::string info = formatLockInfo(detail::currentProcessId(), detail::hostName(),
												kLockAppName, detail::nowMillis());
#ifdef _WIN32
		HANDLE handle = ::CreateFileW(detail::fsPath(path_).c_str(), GENERIC_WRITE, 0, nullptr,
									  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return false;
		}
		DWORD written = 0;
		::WriteFile(handle, info.data(), static_cast<DWORD>(info.size()), &written, nullptr);
		::CloseHandle(handle);
		return true;
#else
		const int descriptor = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (descriptor < 0) {
			return false;
		}
		const ssize_t ignored = ::write(descriptor, info.data(), info.size());
		(void)ignored;
		::close(descriptor);
		return true;
#endif
	}

	bool isStale() const {
		std::string content;
		if (!detail::readWholeFile(detail::fsPath(path_), content)) {
			// Cannot inspect the lock: stay conservative and do not steal it.
			return false;
		}
		const std::vector<std::string> lines = detail::split(content, '\n', true);
		if (lines.size() < 3) {
			// Garbled / truncated lock file: leave it to the caller's
			// "proceed without lock" fallback rather than stealing blindly.
			return false;
		}

		if (lines.size() >= 4) {
			const long long stamp = std::strtoll(detail::trim(lines[3]).c_str(), nullptr, 10);
			if (stamp > 0 && detail::nowMillis() - stamp > kLockStaleMillis) {
				return true;
			}
		}

		const long long pid = std::strtoll(detail::trim(lines[0]).c_str(), nullptr, 10);
		const std::string host = detail::trim(lines[1]);
		if (pid > 0 && (host.empty() || detail::equalsIgnoreCaseAscii(host, detail::hostName()))) {
			if (!detail::isProcessAlive(pid)) {
				return true;
			}
		}
		return false;
	}

	std::string path_;
	bool locked_ = false;
};

}  // namespace JsonFile
}  // namespace ZORM
