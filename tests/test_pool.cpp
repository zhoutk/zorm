// tests/test_pool.cpp
// ----------------------------------------------------------------------------
// Unit tests for DbPool::HandlePool - the exclusive-lease connection pool
// shared by every SQL backend. Until the .h/.cpp refactor these semantics
// (lease exclusivity, blocking checkout, invalidate self-heal, connect
// failure) had no direct coverage; they run entirely offline.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "DbPool.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace ZORM::DbPool;

// Lease is a nested type of the concrete instantiation.
using Lease = HandlePool<int*>::Lease;

namespace {

struct PoolStats {
	int constructed = 0;
	int destroyed = 0;
};

// A pool over fake "handles" (heap ints) so no database is needed.
class FakePool : public ::testing::Test {
protected:
	void SetUp() override {
		stats_ = new PoolStats();
		pool_ = new HandlePool<int*>(
			[this](std::string& err) -> int* {
				(void)err;
				stats_->constructed++;
				return new int(nextId_++);
			},
			[this](int* h) {
				stats_->destroyed++;
				delete h;
			},
			maxConn_);
	}

	void TearDown() override {
		delete pool_;
		// remaining leased handles (if a test leaks them) still count
		delete stats_;
	}

	PoolStats* stats_ = nullptr;
	HandlePool<int*>* pool_ = nullptr;
	std::string err_;
	int maxConn_ = 2;
	int nextId_ = 1;
};

}  // namespace

TEST_F(FakePool, AcquireCreatesAndReleasesReuse) {
	{
		Lease lease = pool_->acquire(err_);
		ASSERT_NE(lease.get(), nullptr);
		EXPECT_EQ(*lease.get(), 1);
		EXPECT_EQ(stats_->constructed, 1);
	}
	{
		// released: the same slot (and handle) is handed out again
		Lease lease = pool_->acquire(err_);
		ASSERT_NE(lease.get(), nullptr);
		EXPECT_EQ(*lease.get(), 1);
		EXPECT_EQ(stats_->constructed, 1);
	}
	EXPECT_EQ(stats_->destroyed, 0);
}

TEST_F(FakePool, ConnectFailureYieldsEmptyLeaseAndError) {
	HandlePool<int*> failing(
		[](std::string& err) -> int* {
			err = "no route to host";
			return nullptr;
		},
		[](int* h) { delete h; },
		1);
	std::string err;
	Lease lease = failing.acquire(err);
	EXPECT_EQ(lease.get(), nullptr);
	EXPECT_EQ(err, "no route to host");
}

TEST_F(FakePool, ConcurrentLeasesGetDistinctHandles) {
	Lease a = pool_->acquire(err_);
	Lease b = pool_->acquire(err_);
	ASSERT_NE(a.get(), nullptr);
	ASSERT_NE(b.get(), nullptr);
	// distinct slots within maxConn
	EXPECT_NE(a.get(), b.get());
}

TEST_F(FakePool, AcquireBlocksUntilLeaseReleased) {
	pool_->setMaxConn(1);  // before the first acquire
	Lease holder = pool_->acquire(err_);
	ASSERT_NE(holder.get(), nullptr);

	std::atomic<bool> gotHandle{false};
	std::thread waiter([&]() {
		std::string werr;
		Lease lease = pool_->acquire(werr);
		gotHandle = lease.get() != nullptr;
	});

	// the waiter must still be blocked while the lease is held
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_FALSE(gotHandle.load());

	holder = Lease();  // release
	waiter.join();
	EXPECT_TRUE(gotHandle.load());
}

TEST_F(FakePool, InvalidateSelfHealDestroysIdleHandles) {
	{
		Lease lease = pool_->acquire(err_);
		ASSERT_NE(lease.get(), nullptr);
	}
	EXPECT_EQ(stats_->constructed, 1);
	// A "fatal connection error" poisons the pool: idle handles are destroyed
	// so the next acquire lazily creates a FRESH one.
	pool_->invalidate();
	{
		Lease lease = pool_->acquire(err_);
		ASSERT_NE(lease.get(), nullptr);
		EXPECT_EQ(*lease.get(), 2);  // fresh handle, new id
	}
	EXPECT_EQ(stats_->destroyed, 1);
	EXPECT_EQ(stats_->constructed, 2);
}

TEST_F(FakePool, InvalidateSparesLeasedHandlesUntilRelease) {
	Lease holder = pool_->acquire(err_);
	ASSERT_NE(holder.get(), nullptr);
	const int* held = holder.get();

	pool_->invalidate();
	// still leased: must not be destroyed under our feet
	EXPECT_EQ(stats_->destroyed, 0);
	EXPECT_EQ(held, holder.get());

	holder = Lease();  // release -> poisoned slot destroys the handle
	EXPECT_EQ(stats_->destroyed, 1);
	// and the next acquire creates a fresh one
	Lease lease = pool_->acquire(err_);
	ASSERT_NE(lease.get(), nullptr);
	EXPECT_EQ(stats_->constructed, 2);
}

TEST_F(FakePool, MoveTransfersOwnership) {
	Lease a = pool_->acquire(err_);
	int* handle = a.get();
	Lease b = std::move(a);
	EXPECT_EQ(b.get(), handle);
	EXPECT_EQ(a.get(), nullptr);  // source emptied
	b = Lease();  // release ownership exactly once
	// a released handle returns to the pool for reuse - it is NOT destroyed
	// (destruction happens on invalidate or pool teardown)
	EXPECT_EQ(stats_->destroyed, 0);
	Lease c = pool_->acquire(err_);
	EXPECT_EQ(c.get(), handle);  // same slot re-served
}
