#include "FileLock.h"

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

namespace detail {

long long currentProcessId() {
#ifdef _WIN32
	return static_cast<long long>(::GetCurrentProcessId());
#else
	return static_cast<long long>(::getpid());
#endif
}

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* text, int length) {
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

std::string hostName() {
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

bool isProcessAlive(long long pid) {
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

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// FileLock
// ─────────────────────────────────────────────────────────────────────────

FileLock::FileLock(std::string lockPath)
	: path_(std::move(lockPath)) {
}

FileLock::~FileLock() {
	unlock();
}

bool FileLock::tryLock(int timeoutMs) {
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

void FileLock::unlock() {
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

bool FileLock::readLockInfo(long long* pid, std::string* host, std::string* appName) const {
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
std::string FileLock::formatLockInfo(long long pid, const std::string& host, const std::string& appName,
									  long long millis) {
	return std::to_string(pid) + "\n" + host + "\n" + appName + "\n" + std::to_string(millis) + "\n";
}

bool FileLock::createExclusive() {
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

bool FileLock::isStale() const {
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

}  // namespace JsonFile
}  // namespace ZORM
