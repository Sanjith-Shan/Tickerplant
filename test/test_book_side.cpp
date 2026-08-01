// The two price level containers.
//
// Both hold nano::PriceLevel objects ordered best first, and the only thing the
// book builder asks of them is that they agree. So the tests are mostly
// differential, run over both through the same template, and the few that are
// not are about the one property that differs, which is that a vector
// invalidates pointers on insert and a map does not.

#include "tick/book_side.hpp"

#include "nano/memory_pool.hpp"
#include "nano/order.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace {

using nano::Order;
using nano::Price;
using nano::Side;

using Pool = nano::MemoryPool<Order, 1024>;

Order* make_order(Pool& pool, uint64_t id, Side side, Price px, uint32_t qty) {
    return pool.allocate(id, side, nano::OrderType::Limit, px, qty, 0, 1);
}

} // namespace

template <typename T>
class BookSideTest : public ::testing::Test {};

using SideTypes = ::testing::Types<tick::MapSide<true>, tick::VectorSide<true>>;
TYPED_TEST_SUITE(BookSideTest, SideTypes);

TYPED_TEST(BookSideTest, StartsEmpty) {
    TypeParam side;
    EXPECT_TRUE(side.empty());
    EXPECT_EQ(side.size(), 0u);
    EXPECT_EQ(side.best(), nullptr);
    EXPECT_EQ(side.find(100), nullptr);
}

TYPED_TEST(BookSideTest, LevelCreatesOnFirstTouchAndFindsAfter) {
    TypeParam side;
    auto&     lvl = side.level(1000);
    EXPECT_EQ(lvl.price(), 1000);
    EXPECT_EQ(side.size(), 1u);
    ASSERT_NE(side.find(1000), nullptr);
    EXPECT_EQ(side.find(1000)->price(), 1000);
    EXPECT_EQ(side.find(999), nullptr);
}

TYPED_TEST(BookSideTest, BidSideOrdersHighestFirst) {
    TypeParam side;
    for (Price p : {1000, 1200, 900, 1100}) side.level(p);
    ASSERT_NE(side.best(), nullptr);
    EXPECT_EQ(side.best()->price(), 1200);

    std::vector<Price> seen;
    side.for_each([&](Price p, const nano::PriceLevel&) {
        seen.push_back(p);
        return true;
    });
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], 1200);
    EXPECT_EQ(seen[1], 1100);
    EXPECT_EQ(seen[2], 1000);
    EXPECT_EQ(seen[3], 900);
}

TYPED_TEST(BookSideTest, EraseRemovesAndRevealsTheNextBest) {
    TypeParam side;
    for (Price p : {1000, 1200, 1100}) side.level(p);
    side.erase(1200);
    ASSERT_NE(side.best(), nullptr);
    EXPECT_EQ(side.best()->price(), 1100);
    EXPECT_EQ(side.size(), 2u);
    EXPECT_EQ(side.find(1200), nullptr);

    // Erasing something that is not there is a no-op rather than a fault,
    // because the book builder calls it on a level it has just emptied and does
    // not want to check twice.
    side.erase(9999);
    EXPECT_EQ(side.size(), 2u);
}

TYPED_TEST(BookSideTest, ForEachStopsWhenTheVisitorSaysSo) {
    TypeParam side;
    for (Price p : {1000, 1200, 1100, 900}) side.level(p);
    int seen = 0;
    side.for_each([&](Price, const nano::PriceLevel&) {
        ++seen;
        return seen < 2;
    });
    EXPECT_EQ(seen, 2);
}

// The intrusive list inside nano::PriceLevel has to keep working when the level
// itself has been moved, which is what a sorted vector does to every level past
// an insertion point. The list threads through the Order objects in the pool
// and those never move, so this holds, and the test is here because it is the
// one place the vector container could have gone wrong.
TYPED_TEST(BookSideTest, OrdersSurviveContainerReshuffling) {
    Pool      pool;
    TypeParam side;

    Order* a = make_order(pool, 1, Side::Buy, 1000, 100);
    Order* b = make_order(pool, 2, Side::Buy, 1000, 200);
    side.level(1000).append(a);
    side.level(1000).append(b);

    // Inserting below and above forces a sorted vector to move the 1000 level.
    for (Price p : {900, 1100, 950, 1200, 800}) side.level(p);

    nano::PriceLevel* lvl = side.find(1000);
    ASSERT_NE(lvl, nullptr);
    EXPECT_EQ(lvl->total_quantity(), 300u);
    EXPECT_EQ(lvl->order_count(), 2u);
    ASSERT_NE(lvl->front(), nullptr);
    EXPECT_EQ(lvl->front()->id, 1u) << "the oldest order must still be at the front";
    ASSERT_NE(lvl->front()->next, nullptr);
    EXPECT_EQ(lvl->front()->next->id, 2u);

    lvl->remove(a);
    EXPECT_EQ(lvl->total_quantity(), 200u);
    EXPECT_EQ(lvl->order_count(), 1u);
    EXPECT_EQ(lvl->front()->id, 2u);
}

TYPED_TEST(BookSideTest, ClearEmptiesEverything) {
    TypeParam side;
    for (Price p : {1000, 1200, 1100}) side.level(p);
    side.clear();
    EXPECT_TRUE(side.empty());
    EXPECT_EQ(side.best(), nullptr);
}

// The ask side orders the other way, which is a separate instantiation and
// therefore a separate chance to get the comparator backwards.
TEST(BookSideAsk, AskSideOrdersLowestFirst) {
    tick::MapSide<false>    m;
    tick::VectorSide<false> v;
    for (Price p : {1200, 1000, 1100}) {
        m.level(p);
        v.level(p);
    }
    ASSERT_NE(m.best(), nullptr);
    ASSERT_NE(v.best(), nullptr);
    EXPECT_EQ(m.best()->price(), 1000);
    EXPECT_EQ(v.best()->price(), 1000);

    std::vector<Price> mp, vp;
    m.for_each([&](Price p, const nano::PriceLevel&) { mp.push_back(p); return true; });
    v.for_each([&](Price p, const nano::PriceLevel&) { vp.push_back(p); return true; });
    EXPECT_EQ(mp, vp);
    ASSERT_EQ(mp.size(), 3u);
    EXPECT_EQ(mp[0], 1000);
    EXPECT_EQ(mp[2], 1200);
}

// The differential test. The same operations into both containers, comparing
// the full ordered walk after every one. If they ever disagree, the book
// builder's choice of container would change the book, which is the thing the
// whole abstraction exists to make impossible.
TEST(BookSideAgreement, MapAndVectorAgreeUnderChurn) {
    tick::MapSide<true>    m;
    tick::VectorSide<true> v;
    std::mt19937_64        rng(12345);

    auto walk = [](auto& side) {
        std::vector<std::pair<Price, uint32_t>> out;
        side.for_each([&](Price p, const nano::PriceLevel& lvl) {
            out.emplace_back(p, lvl.total_quantity());
            return true;
        });
        return out;
    };

    for (int step = 0; step < 5000; ++step) {
        const Price p = 900 + static_cast<Price>(rng() % 200);
        if (rng() % 3 == 0) {
            m.erase(p);
            v.erase(p);
        } else {
            m.level(p);
            v.level(p);
        }
        if (step % 100 == 0) {
            ASSERT_EQ(walk(m), walk(v)) << "containers disagreed at step " << step;
            ASSERT_EQ(m.size(), v.size());
        }
    }
    ASSERT_EQ(walk(m), walk(v));
}
