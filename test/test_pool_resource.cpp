// The price level memory resource.
//
// This structure exists because the counting allocator caught the book
// allocating, so the tests for it are mostly about proving it really does stop
// going to the heap rather than about the pointers it hands back. A resource
// that quietly falls through to the general allocator would pass every
// correctness test in this file and fail its only purpose.

#include "tick/pool_resource.hpp"

#include "tick/alloc_counter.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <memory_resource>
#include <set>
#include <vector>

using tick::PoolResource;

TEST(PoolResource, HandsBackUsableDistinctMemory) {
    PoolResource r;
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* p = r.allocate(48, 8);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0xAB, 48);
        blocks.push_back(p);
    }
    std::set<void*> unique(blocks.begin(), blocks.end());
    EXPECT_EQ(unique.size(), blocks.size()) << "two live blocks must never overlap";
    for (void* p : blocks) r.deallocate(p, 48, 8);
}

// The whole point. A freed block comes back rather than a new one being carved.
TEST(PoolResource, FreedBlocksAreReused) {
    PoolResource r;
    void* first = r.allocate(48, 8);
    r.deallocate(first, 48, 8);
    void* second = r.allocate(48, 8);
    EXPECT_EQ(first, second) << "a block freed a moment ago should come straight back";
    EXPECT_EQ(r.reused(), 1u);
    r.deallocate(second, 48, 8);
}

TEST(PoolResource, SizeClassesDoNotMix) {
    PoolResource r;
    void* small = r.allocate(16, 8);
    r.deallocate(small, 16, 8);
    // A different size class must not be handed the block that was freed in
    // another one, because it would be too small.
    void* large = r.allocate(128, 8);
    EXPECT_NE(small, large);
    r.deallocate(large, 128, 8);
}

TEST(PoolResource, OversizedBlocksGoUpstream) {
    PoolResource r;
    void* p = r.allocate(PoolResource::kMaxBlock * 4, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(r.oversized(), 1u);
    r.deallocate(p, PoolResource::kMaxBlock * 4, 8);
}

TEST(PoolResource, IsOnlyEqualToItself) {
    PoolResource a, b;
    EXPECT_TRUE(a.is_equal(a));
    EXPECT_FALSE(a.is_equal(b));
}

// The measurement that matters. A steady state workload that creates and
// destroys the same number of nodes has to stop touching the heap entirely
// after the first chunk is carved.
TEST(PoolResource, SteadyStateChurnStopsAllocatingFromUpstream) {
    PoolResource r(1u << 16);

    // Warm up, which is where the chunks get taken.
    std::vector<void*> live;
    for (int i = 0; i < 500; ++i) live.push_back(r.allocate(48, 8));
    for (void* p : live) r.deallocate(p, 48, 8);
    live.clear();

    const std::size_t chunks_after_warmup = r.chunks();
    const std::size_t bytes_after_warmup  = r.bytes_from_upstream();
    ASSERT_GT(chunks_after_warmup, 0u);

    // Now churn. Same high water mark, many times over.
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 500; ++i) live.push_back(r.allocate(48, 8));
        for (void* p : live) r.deallocate(p, 48, 8);
        live.clear();
    }

    EXPECT_EQ(r.chunks(), chunks_after_warmup)
        << "steady state churn must not take another chunk";
    EXPECT_EQ(r.bytes_from_upstream(), bytes_after_warmup);
    EXPECT_GT(r.reused(), 90000u) << "almost every block should have been a reuse";
}

// Driving it through the container it was written for, which is the case that
// actually runs in the book.
TEST(PoolResource, BacksAPmrMapWithoutTouchingTheHeapInSteadyState) {
    PoolResource                 r;
    std::pmr::map<int, uint64_t> m(&r);

    // Warm up so the chunks are taken and the free lists are populated.
    for (int i = 0; i < 2000; ++i) m.emplace(i, static_cast<uint64_t>(i));
    m.clear();

    const std::size_t chunks_after_warmup = r.chunks();

    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 2000; ++i) m.emplace(i, static_cast<uint64_t>(i));
        for (int i = 0; i < 2000; ++i) EXPECT_EQ(m.at(i), static_cast<uint64_t>(i));
        m.clear();
    }

    EXPECT_EQ(r.chunks(), chunks_after_warmup)
        << "the map should be recycling nodes rather than taking new memory";
}

// And the same thing measured with the global counting allocator, which is the
// instrument the original defect was found with. This is the test that would
// have failed before the resource existed.
TEST(PoolResource, GlobalAllocationCounterSeesNothingInSteadyState) {
    if (!tick::alloc::counter_is_installed()) {
        GTEST_SKIP() << "no counting allocator in this binary";
    }

    PoolResource                 r;
    std::pmr::map<int, uint64_t> m(&r);
    for (int i = 0; i < 4000; ++i) m.emplace(i, static_cast<uint64_t>(i));
    m.clear();

    const uint64_t before = tick::alloc::allocations();
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 4000; ++i) m.emplace(i, static_cast<uint64_t>(i));
        m.clear();
    }
    const uint64_t after = tick::alloc::allocations();

    EXPECT_EQ(after, before)
        << "two hundred thousand node insertions took " << (after - before)
        << " heap allocations, so the pool is not doing its job";
}
