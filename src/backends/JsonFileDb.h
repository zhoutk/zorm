#pragma once

#include "Idb.h"

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace ZORM {
namespace JsonFile {

class FileLock;

// JSON file backend for Idb - pure C++17, no external dependency beyond ZJSON.
//
// File layout: a JSON array of table objects
//   [ { "table": "routes", "columns": ["id", ...], "rows": [ {...}, ... ] }, ... ]
//
// Semantics worth knowing when consuming this backend:
//   * object key order is insertion order; the `rows` array order is not stable
//     after a delete - the last row is swapped into the hole (O(1) id-index
//     maintenance);
//   * reads report STDBOPERATEERR ("database read failed") when the file is
//     unreadable or corrupt, instead of pretending the database is empty;
//   * a write reloads from disk first, so a failed write never publishes
//     in-memory state that is ahead of the file.
//
// Hardening invariants (keep when touching this file):
//   * one shared instance per resolved file path (createShared / s_instances_)
//   * writers take a cross-process lock file (<path>.lock) and reload from disk
//     before modifying, so a failed disk write never leaves memory ahead of disk
//   * in-memory mirror guarded by a shared_mutex (concurrent readers, exclusive
//     writers), plus a per-table id -> row-index map
//   * unreadable/corrupt files are backed up instead of being silently overwritten
class ZORM_API JsonFileDb : public Idb {
public:
	explicit JsonFileDb(const std::string& filePath = std::string(), bool logFlag = false);
	~JsonFileDb() override;

	// Static factory: returns a shared instance for the given file path.
	// If an instance already exists for this path (tracked via weak_ptr),
	// returns the existing one. Otherwise creates a new one.
	// When all shared_ptrs are released, the instance is freed and the
	// next call creates a fresh instance (e.g. for tests that reset the db).
	static std::shared_ptr<JsonFileDb> createShared(const std::string& filePath = std::string(),
													bool logFlag = false);

	Json select(const string& tablename, const Json& params,
				vector<string> fields = vector<string>(),
				Json values = Json(JsonType::Array)) override;
	Json create(const string& tablename, const Json& params) override;
	Json update(const string& tablename, const Json& params) override;
	Json remove(const string& tablename, const Json& params) override;
	Json querySql(const string& sql, Json params = Json(),
				  Json values = Json(JsonType::Array),
				  vector<string> fields = vector<string>()) override;
	Json execSql(const string& sql, Json params = Json(),
				 Json values = Json(JsonType::Array)) override;
	Json insertBatch(const string& tablename, const Json& elements, string constraint = "id") override;
	Json transGo(const Json& sqls, bool isAsync = false) override;

	std::string storagePath() const;
	static std::string defaultStoragePath();

private:
	// Cross-process lock file (defined in FileLock.h). Held by unique_ptr so
	// this header does not have to pull in the platform headers.
	using Lock = FileLock;

	// Must be called with storeMutex_ held exclusively (unique_lock).
	// Loads the on-disk JSON array into store_ once; subsequent calls are no-ops.
	// A failed load sets loadFailed_ (reads then report STDBOPERATEERR) and is
	// not retried on every read, so a corrupt file is backed up only once.
	void ensureLoadedLocked();

	// Rebuild tableIdIndex_ from store_. Must hold storeMutex_ exclusively.
	void rebuildIndexLocked();

	// Removes leftovers of writeFileAtomic (`<db>.tmp.<pid>.<hex>`) whose owning
	// process is gone. Called once per instance from the constructor.
	void sweepStaleTempFiles();

	// Cross-process safe write. Takes, in this order: writeMutex_ (serializes
	// writers inside this process), the cross-process lock file, and storeMutex_
	// (exclusive). It reloads the file, runs modify() against store_ directly
	// (no full-store copy), persists atomically, and rolls back to the previous
	// in-memory state if modify() fails or the write fails.
	// Returns the result from modify() or an error response.
	//
	// Lock order matters: the file lock is taken *before* storeMutex_ so that
	// waiting up to 30 s for another process never blocks local readers.
	Json writeWithLock(const std::function<Json()>& modify);

	// Serializes writers of this process so fileLock_ is only touched by one
	// thread at a time. Lock order: writeMutex_ -> fileLock_ -> storeMutex_.
	std::mutex writeMutex_;

	// Shared lock for concurrent readers after load; exclusive for first load + writers.
	mutable std::shared_mutex storeMutex_;

	// NOTE: declaration order = initialization order.  filePath_ must come
	// before fileLock_ because the lock file path is derived from filePath_.
	std::string filePath_;

	// Cross-process file lock: prevents two processes from writing to the same
	// data file simultaneously. The lock file is "<filePath>.lock". Acquired
	// before each read-modify-write cycle and held for the duration of the write.
	// Stale locks (dead holder, or older than the stale window) are reclaimed;
	// unlock() only removes the file while this process still owns it.
	mutable std::unique_ptr<FileLock> fileLock_;

	Json store_;        // in-memory mirror of the JSON file (array of table objects)
	bool loaded_ = false;
	// Set when the last load/read of the database file failed (unreadable or
	// corrupt). Reads then return STDBOPERATEERR instead of an empty result set.
	bool loadFailed_ = false;

	// Per-table id -> row-index map for O(1) create/update/remove by id.
	std::unordered_map<std::string, std::unordered_map<std::string, int>> tableIdIndex_;

	// - Static instance registry (shared singleton per file path) ------------
	static std::mutex s_mutex_;
	static std::unordered_map<std::string, std::weak_ptr<JsonFileDb>> s_instances_;
};

}  // namespace JsonFile
}  // namespace ZORM
