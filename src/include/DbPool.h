#pragma once

// Connection pool with RAII leases, shared by the SQL backends (O-2/O-3).
//
// The previous per-backend pools had no checkout/checkin semantics: two
// threads could obtain the SAME connection and interleave statements (and a
// transaction would have shared its session with unrelated work). This pool
// hands out exclusive RAII leases - a connection belongs to exactly one
// thread for as long as the lease lives, transactions hold it end-to-end,
// and a caller that needs snapshot-consistent auxiliary queries (the
// records/pages count) simply keeps its lease.
//
// Handles are created lazily up to maxConn; acquire() blocks when all
// handles are leased.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ZORM {
namespace DbPool {

template <typename Handle>
class HandlePool {
public:
	using ConnectFn = std::function<Handle(std::string& err)>;
	using DestroyFn = std::function<void(Handle)>;

	HandlePool(ConnectFn connect, DestroyFn destroy, int maxConn)
		: connect_(std::move(connect)), destroy_(std::move(destroy)), maxConn_(maxConn < 1 ? 1 : maxConn) {}

	~HandlePool() {
		for (Slot& slot : slots_) {
			if (slot.handle) {
				destroy_(slot.handle);
			}
		}
	}

	HandlePool(const HandlePool&) = delete;
	HandlePool& operator=(const HandlePool&) = delete;

	// Must be called before the first acquire().
	void setMaxConn(int maxConn) {
		if (maxConn >= 1)
			maxConn_ = maxConn;
	}

	// Exclusive lease: releases the slot back to the pool on destruction.
	class Lease {
	public:
		Lease() = default;
		Lease(HandlePool* pool, Handle handle) : pool_(pool), handle_(handle) {}
		~Lease() {
			if (pool_ != nullptr) {
				pool_->release(handle_);
			}
		}
		Lease(Lease&& other) noexcept : pool_(other.pool_), handle_(other.handle_) {
			other.pool_ = nullptr;
		}
		Lease& operator=(Lease&& other) noexcept {
			if (this != &other) {
				if (pool_ != nullptr) {
					pool_->release(handle_);
				}
				pool_ = other.pool_;
				handle_ = other.handle_;
				other.pool_ = nullptr;
			}
			return *this;
		}
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;

		Handle get() const {
			return handle_;
		}

	private:
		HandlePool* pool_ = nullptr;
		Handle handle_{};
	};

	// Self-healing after a fatal connection error: destroys all idle handles;
	// leased ones are destroyed as they are released and lazily recreated.
	void invalidate() {
		std::lock_guard<std::mutex> lock(mutex_);
		poisoned_ = true;
		for (Slot& slot : slots_) {
			if (slot.handle && !slot.busy) {
				destroy_(slot.handle);
				slot.handle = Handle{};
			}
		}
	}

	// Acquires an exclusive connection. Blocks (up to the caller's patience)
	// while all handles are leased. Returns an empty Lease on connect failure
	// with `err` filled.
	Lease acquire(std::string& err) {
		std::unique_lock<std::mutex> lock(mutex_);
		for (;;) {
			for (Slot& slot : slots_) {
				if (!slot.busy) {
					if (!slot.handle) {
						slot.handle = connect_(err);
						if (!slot.handle) {
							return Lease();
						}
						poisoned_ = false;  // healthy handle: clear the flag
					}
					slot.busy = true;
					return Lease(this, slot.handle);
				}
			}
			if (static_cast<int>(slots_.size()) < maxConn_) {
				slots_.emplace_back();
				continue;
			}
			idle_.wait(lock);
		}
	}

private:
	struct Slot {
		Handle handle{};
		bool busy = false;
	};

	void release(Handle handle) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (Slot& slot : slots_) {
				if (slot.handle == handle) {
					if (poisoned_) {
						destroy_(slot.handle);
						slot.handle = Handle{};
					}
					slot.busy = false;
					break;
				}
			}
		}
		idle_.notify_one();
	}

	std::vector<Slot> slots_;
	std::mutex mutex_;
	std::condition_variable idle_;
	bool poisoned_ = false;
	ConnectFn connect_;
	DestroyFn destroy_;
	int maxConn_;
};

}  // namespace DbPool
}  // namespace ZORM
