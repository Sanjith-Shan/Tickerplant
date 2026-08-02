// The order reference table.
//
// Two kinds of test here. Direct tests of the operations, and a differential
// test against std::unordered_map under a churn pattern shaped like a real
// feed. The differential test is the one that matters, because backward shift
// deletion is easy to write in a way that passes every simple test and quietly
// loses a key when the probe runs wrap the end of the table.

#include "tick/flat_order_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

using tick::FlatOrderMap;
using tick::IdentityHash;
using tick::SplitMix64Hash;

TEST(FlatOrderMap, InsertFindErase) {
    FlatOrderMap<uint64_t> m(64);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find(1), nullptr);

    EXPECT_TRUE(m.insert(1, 100));
    EXPECT_TRUE(m.insert(2, 200));
    EXPECT_EQ(m.size(), 2u);

    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 100u);
    ASSERT_NE(m.find(2), nullptr);
    EXPECT_EQ(*m.find(2), 200u);

    EXPECT_TRUE(m.erase(1));
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_FALSE(m.erase(1));
    ASSERT_NE(m.find(2), nullptr);
    EXPECT_EQ(*m.find(2), 200u);
}

TEST(FlatOrderMap, InsertOverExistingKeyReplaces) {
    FlatOrderMap<uint64_t> m(64);
    EXPECT_TRUE(m.insert(7, 1));
    EXPECT_FALSE(m.insert(7, 2));
    EXPECT_EQ(m.size(), 1u);
    ASSERT_NE(m.find(7), nullptr);
    EXPECT_EQ(*m.find(7), 2u);
}

// The point of a power of two table with a mask is that the capacity is always
// a power of two, whatever the caller asked for.
TEST(FlatOrderMap, CapacityRoundsUpToPowerOfTwo) {
    FlatOrderMap<uint64_t> m(100);
    EXPECT_EQ(m.capacity(), 128u);
}

// A table with the identity hash and a tiny capacity forces long probe runs
// that wrap the end, which is exactly the case backward shift deletion gets
// wrong when the cyclic interval test is written the obvious way.
TEST(FlatOrderMap, BackwardShiftSurvivesWrappingProbeRuns) {
    constexpr std::size_t kCap = 16;
    FlatOrderMap<uint64_t, IdentityHash> m(kCap);

    // Every one of these hashes to slot 14 or 15 and then wraps.
    const std::vector<uint64_t> keys = {14, 30, 46, 62, 15, 31, 47};
    for (uint64_t k : keys) ASSERT_TRUE(m.insert(k, k * 10));

    // Erase from the middle of the wrapped run, which is where entries have to
    // shift backwards across the wrap point.
    ASSERT_TRUE(m.erase(46));
    for (uint64_t k : keys) {
        if (k == 46) {
            EXPECT_EQ(m.find(k), nullptr) << "key " << k;
        } else {
            ASSERT_NE(m.find(k), nullptr) << "key " << k << " lost after a wrapping erase";
            EXPECT_EQ(*m.find(k), k * 10);
        }
    }

    ASSERT_TRUE(m.erase(14));
    for (uint64_t k : {30ULL, 62ULL, 15ULL, 31ULL, 47ULL}) {
        ASSERT_NE(m.find(k), nullptr) << "key " << k << " lost after erasing the run head";
    }
}

TEST(FlatOrderMap, GrowsAndKeepsEverything) {
    FlatOrderMap<uint64_t> m(16);
    const std::size_t      before = m.capacity();
    for (uint64_t k = 1; k <= 200; ++k) ASSERT_TRUE(m.insert(k, k));
    EXPECT_GT(m.capacity(), before);
    EXPECT_GT(m.growth_events(), 0u);
    for (uint64_t k = 1; k <= 200; ++k) {
        ASSERT_NE(m.find(k), nullptr) << "key " << k << " lost in a rehash";
        EXPECT_EQ(*m.find(k), k);
    }
    EXPECT_EQ(m.size(), 200u);
}

TEST(FlatOrderMap, ForEachVisitsEveryLiveEntryExactlyOnce) {
    FlatOrderMap<uint64_t> m(64);
    for (uint64_t k = 1; k <= 20; ++k) m.insert(k, k * 3);
    for (uint64_t k = 1; k <= 20; k += 2) m.erase(k);

    std::unordered_map<uint64_t, int> seen;
    m.for_each([&](uint64_t k, uint64_t v) {
        ++seen[k];
        EXPECT_EQ(v, k * 3);
    });
    EXPECT_EQ(seen.size(), 10u);
    for (const auto& [k, n] : seen) {
        EXPECT_EQ(n, 1) << "key " << k << " visited more than once";
        EXPECT_EQ(k % 2, 0u);
    }
}

// The differential test. Mirror every operation into a std::unordered_map and
// assert the two agree after each one. The churn pattern is deliberately feed
// shaped, which means mostly sequential new keys with deletes of recent ones,
// because that is what an exchange order reference stream looks like and a
// uniform random test would not exercise the same probe runs.
template <typename Hash>
void differential_churn(const char* label) {
    FlatOrderMap<uint64_t, Hash>          fast(1u << 12);
    std::unordered_map<uint64_t, uint64_t> model;
    std::vector<uint64_t>                  live;
    std::mt19937_64                        rng(0xC0FFEE);

    uint64_t next_ref = 1;
    for (int step = 0; step < 200000; ++step) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 55 || live.empty()) {
            const uint64_t k = next_ref++;
            const uint64_t v = rng();
            ASSERT_EQ(fast.insert(k, v), model.emplace(k, v).second) << label;
            live.push_back(k);
        } else if (roll < 80) {
            // Delete something recent, the way a feed cancels near the top.
            const std::size_t i = live.size() - 1 - (rng() % std::min<std::size_t>(live.size(), 64));
            const uint64_t    k = live[i];
            live.erase(live.begin() + static_cast<long>(i));
            ASSERT_EQ(fast.erase(k), model.erase(k) == 1) << label;
        } else {
            const uint64_t k  = live[rng() % live.size()];
            const uint64_t* p = fast.find(k);
            ASSERT_NE(p, nullptr) << label << " lost key " << k;
            ASSERT_EQ(*p, model.at(k)) << label;
        }

        if (step % 5000 == 0) {
            ASSERT_EQ(fast.size(), model.size()) << label << " size drifted at step " << step;
        }
    }

    ASSERT_EQ(fast.size(), model.size()) << label;
    for (const auto& [k, v] : model) {
        const uint64_t* p = fast.find(k);
        ASSERT_NE(p, nullptr) << label << " missing key " << k << " at the end";
        EXPECT_EQ(*p, v) << label;
    }
}

TEST(FlatOrderMap, DifferentialChurnSplitMix) {
    differential_churn<SplitMix64Hash>("splitmix64");
}

TEST(FlatOrderMap, DifferentialChurnIdentity) {
    differential_churn<IdentityHash>("identity");
}

// Key zero is the empty marker, so it can never be stored. ITCH order
// references start at one, so this costs nothing in practice, but a caller that
// tries it should not corrupt the table.
TEST(FlatOrderMap, KeyZeroIsTheEmptyMarker) {
    FlatOrderMap<uint64_t> m(16);
    EXPECT_EQ(m.find(0), nullptr);
    EXPECT_FALSE(m.erase(0));
    m.insert(5, 50);
    EXPECT_EQ(m.find(0), nullptr);
    ASSERT_NE(m.find(5), nullptr);
}
