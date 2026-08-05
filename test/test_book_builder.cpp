// The resting order state machine.
//
// Every transition gets a test, and so does every illegal transition, because
// the illegal ones are the ones that actually happen. An execution against an
// order reference that is not resting is what a sequence gap looks like from
// inside the book builder, and the required behaviour is to count it and carry
// on rather than to crash or to invent the order.
//
// The builder is driven through its handler methods directly rather than
// through encoded ITCH bytes. The decoder has its own tests with hand built
// byte vectors, and mixing the two would mean a decoder bug could hide a book
// bug and the other way round.

#include "tick/book_builder.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace {

using tick::BookBuilder;
using tick::MapSide;
using tick::VectorSide;

constexpr uint16_t kAapl = 13;
constexpr uint16_t kMsft = 99;

// A pool of four million orders is the production size and it is far more than
// a unit test needs. These use a small pool so the suite does not allocate a
// quarter of a gigabyte per test case.
template <template <bool> class Side = MapSide>
using TestBuilder = BookBuilder<Side, 4096>;

template <template <bool> class Side = MapSide>
std::unique_ptr<TestBuilder<Side>> make_builder() {
    auto b = std::make_unique<TestBuilder<Side>>(1024);
    b->on_stock_directory(kAapl, 1, "AAPL", 100);
    b->on_stock_directory(kMsft, 1, "MSFT", 100);
    return b;
}

} // namespace

TEST(BookBuilder, AddRestsOnTheRightSide) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_add(kAapl, 101, 2, 'S', 300, 1'000'500, false);

    const auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_price, 1'000'000);
    EXPECT_EQ(t.bid_qty, 500u);
    EXPECT_EQ(t.bid_orders, 1u);
    EXPECT_EQ(t.ask_price, 1'000'500);
    EXPECT_EQ(t.ask_qty, 300u);
    EXPECT_EQ(t.spread(), 500);
    EXPECT_EQ(t.mid(), 1'000'250);
    EXPECT_EQ(b->stats().adds, 2u);
    EXPECT_EQ(b->stats().live_orders, 2u);
}

TEST(BookBuilder, BestBidIsTheHighestAndBestAskIsTheLowest) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    b->on_add(kAapl, 101, 2, 'B', 100, 1'001'000, false);
    b->on_add(kAapl, 102, 3, 'B', 100, 999'000, false);
    b->on_add(kAapl, 103, 4, 'S', 100, 1'005'000, false);
    b->on_add(kAapl, 104, 5, 'S', 100, 1'003'000, false);
    b->on_add(kAapl, 105, 6, 'S', 100, 1'009'000, false);

    const auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_price, 1'001'000);
    EXPECT_EQ(t.ask_price, 1'003'000);
}

// Orders at one price trade oldest first, and the intrusive list in
// nano::PriceLevel is what holds that. A partial execution has to take from the
// front, which here means the aggregate at the level falls by the executed size
// and the order that was added first is the one that shrinks.
TEST(BookBuilder, TimePriorityWithinALevel) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    b->on_add(kAapl, 101, 2, 'B', 200, 1'000'000, false);
    b->on_add(kAapl, 102, 3, 'B', 300, 1'000'000, false);

    auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_qty, 600u);
    EXPECT_EQ(t.bid_orders, 3u);

    b->on_execute(kAapl, 103, 1, 60, 5000);
    t = b->top(kAapl);
    EXPECT_EQ(t.bid_qty, 540u);
    EXPECT_EQ(t.bid_orders, 3u) << "a partial fill must not remove the order";

    b->on_execute(kAapl, 104, 1, 40, 5001);
    t = b->top(kAapl);
    EXPECT_EQ(t.bid_qty, 500u);
    EXPECT_EQ(t.bid_orders, 2u) << "a fill that takes the rest removes the order";
}

TEST(BookBuilder, PartialExecuteKeepsTheOrderResting) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_execute(kAapl, 101, 1, 200, 7000);

    const auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_qty, 300u);
    EXPECT_EQ(b->stats().live_orders, 1u);
    EXPECT_EQ(b->totals(kAapl).executed_shares, 200u);
    EXPECT_EQ(b->totals(kAapl).executed_notional, 200ULL * 1'000'000ULL);
}

TEST(BookBuilder, FullExecuteRemovesTheOrderAndTheEmptyLevel) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_execute(kAapl, 101, 1, 500, 7000);

    const auto t = b->top(kAapl);
    EXPECT_FALSE(t.has_bid());
    EXPECT_EQ(b->stats().live_orders, 0u);
    EXPECT_EQ(b->book(kAapl).bids.size(), 0u) << "an empty level must be erased, not kept";
    EXPECT_EQ(b->totals(kAapl).executed_shares, 500u);
}

// C carries its own price because the print happened away from the display
// price. Volume has to use the execution price, and a non printable execution
// is a leg of something reported elsewhere, so counting it would double count
// the day.
TEST(BookBuilder, ExecuteWithPriceUsesTheExecutionPrice) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_execute_price(kAapl, 101, 1, 100, 7000, true, 999'000);

    EXPECT_EQ(b->totals(kAapl).executed_shares, 100u);
    EXPECT_EQ(b->totals(kAapl).executed_notional, 100ULL * 999'000ULL);
    EXPECT_EQ(b->top(kAapl).bid_qty, 400u);
}

TEST(BookBuilder, NonPrintableExecutionStillReducesTheBookButNotTheVolume) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_execute_price(kAapl, 101, 1, 100, 7000, false, 999'000);

    EXPECT_EQ(b->totals(kAapl).executed_shares, 0u) << "non printable must not count to volume";
    EXPECT_EQ(b->top(kAapl).bid_qty, 400u) << "the shares still left the book";
}

TEST(BookBuilder, CancelReducesAndDeleteRemoves) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'S', 900, 1'002'000, false);

    b->on_cancel(kAapl, 101, 1, 400);
    EXPECT_EQ(b->top(kAapl).ask_qty, 500u);
    EXPECT_EQ(b->stats().live_orders, 1u);
    EXPECT_EQ(b->totals(kAapl).executed_shares, 0u) << "a cancel is not a trade";

    b->on_delete(kAapl, 102, 1);
    EXPECT_FALSE(b->top(kAapl).has_ask());
    EXPECT_EQ(b->stats().live_orders, 0u);
}

TEST(BookBuilder, CancelOfEverythingRemovesTheOrder) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'S', 900, 1'002'000, false);
    b->on_cancel(kAapl, 101, 1, 900);
    EXPECT_FALSE(b->top(kAapl).has_ask());
    EXPECT_EQ(b->stats().live_orders, 0u);
    EXPECT_EQ(b->stats().overfills, 0u) << "taking exactly the resting size is legal";
}

// U deletes the old reference and adds a new one. The old reference is gone for
// good, the new order sits at the back of its level, and the side and the
// symbol come from the order being replaced because the message does not carry
// them.
TEST(BookBuilder, ReplaceRetiresTheOldReferenceAndMovesThePrice) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_replace(kAapl, 101, 1, 2, 300, 1'000'500);

    EXPECT_EQ(b->stats().live_orders, 1u);
    const auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_price, 1'000'500);
    EXPECT_EQ(t.bid_qty, 300u);
    EXPECT_EQ(b->book(kAapl).bids.size(), 1u) << "the old level must be gone";

    // The old reference must never work again.
    b->on_delete(kAapl, 102, 1);
    EXPECT_EQ(b->stats().orphan_deletes, 1u);
    EXPECT_EQ(b->stats().live_orders, 1u);
}

TEST(BookBuilder, ReplaceKeepsTheSideOfTheOrderItReplaces) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'S', 500, 1'002'000, false);
    b->on_replace(kAapl, 101, 1, 2, 500, 1'003'000);

    const auto t = b->top(kAapl);
    EXPECT_FALSE(t.has_bid());
    EXPECT_EQ(t.ask_price, 1'003'000);
}

// ----- the illegal transitions, which are the ones that really happen --------

TEST(BookBuilder, ExecuteAgainstAnUnknownReferenceIsCountedNotFatal) {
    auto b = make_builder();
    b->on_execute(kAapl, 100, 12345, 100, 7000);
    EXPECT_EQ(b->stats().orphan_executes, 1u);
    EXPECT_EQ(b->stats().live_orders, 0u);
    EXPECT_EQ(b->totals(kAapl).executed_shares, 0u)
        << "an orphan execution must not invent volume";
}

TEST(BookBuilder, CancelAndDeleteAndReplaceOrphansAreEachCountedSeparately) {
    auto b = make_builder();
    b->on_cancel(kAapl, 100, 111, 10);
    b->on_delete(kAapl, 101, 222);
    b->on_replace(kAapl, 102, 333, 444, 10, 1'000'000);

    EXPECT_EQ(b->stats().orphan_cancels, 1u);
    EXPECT_EQ(b->stats().orphan_deletes, 1u);
    EXPECT_EQ(b->stats().orphan_replaces, 1u);
    EXPECT_EQ(b->stats().live_orders, 0u)
        << "an orphaned replace must not create an order of unknown side";
}

TEST(BookBuilder, ExecutingMoreThanIsRestingIsCountedAndTheOrderGoes) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    b->on_execute(kAapl, 101, 1, 250, 7000);

    EXPECT_EQ(b->stats().overfills, 1u);
    EXPECT_EQ(b->stats().live_orders, 0u);
    EXPECT_FALSE(b->top(kAapl).has_bid());
}

TEST(BookBuilder, AddingAReferenceThatIsAlreadyRestingIsCounted) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    b->on_add(kAapl, 101, 1, 'B', 200, 1'001'000, false);

    EXPECT_EQ(b->stats().duplicate_refs, 1u);
    EXPECT_EQ(b->stats().live_orders, 1u) << "the stale order must not be left on the book";
    const auto t = b->top(kAapl);
    EXPECT_EQ(t.bid_price, 1'001'000);
    EXPECT_EQ(t.bid_qty, 200u);
}

TEST(BookBuilder, AnUnknownSideIsRejectedRatherThanGuessed) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, '?', 100, 1'000'000, false);
    EXPECT_EQ(b->stats().unknown_side, 1u);
    EXPECT_EQ(b->stats().live_orders, 0u);
}

// ----- messages that report trades without touching the book -----------------

TEST(BookBuilder, HiddenTradeCountsVolumeAndLeavesTheBookAlone) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    const uint64_t before = b->digest();

    b->on_trade(kAapl, 101, 999, 'B', 250, 1'000'250, 7000);

    EXPECT_EQ(b->digest(), before) << "a P message must never change the visible book";
    EXPECT_EQ(b->totals(kAapl).executed_shares, 250u);
    EXPECT_EQ(b->totals(kAapl).hidden_shares, 250u);
}

TEST(BookBuilder, CrossTradeCountsVolumeAndLeavesTheBookAlone) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    const uint64_t before = b->digest();

    b->on_cross_trade(kAapl, 101, 100000, 1'000'100, 7000, 'O');

    EXPECT_EQ(b->digest(), before);
    EXPECT_EQ(b->totals(kAapl).cross_shares, 100000u);
    EXPECT_EQ(b->totals(kAapl).executed_shares, 100000u);
}

TEST(BookBuilder, BrokenTradeIsCounted) {
    auto b = make_builder();
    b->on_broken_trade(kAapl, 100, 7000);
    EXPECT_EQ(b->stats().broken_trades, 1u);
}

// ----- the digest, which is the oracle everything else leans on --------------

TEST(BookBuilder, DigestIsTheSameForTheSameBookReachedTwoWays) {
    auto a = make_builder();
    a->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    a->on_add(kAapl, 101, 2, 'S', 300, 1'000'500, false);

    auto c = make_builder();
    // Same end state, different route. Add, cancel some, add the rest back.
    c->on_add(kAapl, 100, 7, 'S', 300, 1'000'500, false);
    c->on_add(kAapl, 101, 8, 'B', 900, 1'000'000, false);
    c->on_cancel(kAapl, 102, 8, 400);

    EXPECT_EQ(a->digest(), c->digest())
        << "the digest must depend on the book and not on how it got there";
}

TEST(BookBuilder, DigestChangesWhenTheBookDoes) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    const uint64_t before = b->digest();
    b->on_add(kMsft, 101, 2, 'B', 100, 2'000'000, false);
    EXPECT_NE(b->digest(), before);
}

TEST(BookBuilder, EmptyBookHasAStableDigest) {
    auto a = make_builder();
    auto b = make_builder();
    EXPECT_EQ(a->digest(), b->digest());

    a->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    a->on_delete(kAapl, 101, 1);
    EXPECT_EQ(a->digest(), b->digest())
        << "adding and removing an order must return the book to where it was";
}

// ----- symbols are independent ----------------------------------------------

TEST(BookBuilder, SymbolsDoNotBleedIntoEachOther) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 500, 1'000'000, false);
    b->on_add(kMsft, 101, 2, 'B', 700, 1'500'000, false);

    EXPECT_EQ(b->top(kAapl).bid_price, 1'000'000);
    EXPECT_EQ(b->top(kAapl).bid_qty, 500u);
    EXPECT_EQ(b->top(kMsft).bid_price, 1'500'000);
    EXPECT_EQ(b->top(kMsft).bid_qty, 700u);

    b->on_delete(kAapl, 102, 1);
    EXPECT_FALSE(b->top(kAapl).has_bid());
    EXPECT_TRUE(b->top(kMsft).has_bid());
}

TEST(BookBuilder, SymbolTableResolvesTickers) {
    auto b = make_builder();
    EXPECT_EQ(b->symbols().find("AAPL"), static_cast<int>(kAapl));
    EXPECT_EQ(b->symbols().find("MSFT"), static_cast<int>(kMsft));
    EXPECT_EQ(b->symbols().find("NOPE"), -1);
    EXPECT_EQ(b->symbols().ticker(kAapl), "AAPL");
}

// ----- depth and the derived prices -----------------------------------------

TEST(BookBuilder, DepthComesBackBestFirst) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    b->on_add(kAapl, 101, 2, 'B', 200, 999'000, false);
    b->on_add(kAapl, 102, 3, 'B', 300, 998'000, false);

    TestBuilder<>::DepthLevel lv[5];
    const std::size_t         n = b->depth(kAapl, true, lv, 5);
    ASSERT_EQ(n, 3u);
    EXPECT_EQ(lv[0].price, 1'000'000);
    EXPECT_EQ(lv[1].price, 999'000);
    EXPECT_EQ(lv[2].price, 998'000);
    EXPECT_EQ(lv[0].qty, 100u);
}

// The micro price leans toward the side with less size, because that is the
// side the next trade is more likely to take.
TEST(BookBuilder, MicroPriceLeansTowardTheThinSide) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 900, 1'000'000, false);
    b->on_add(kAapl, 101, 2, 'S', 100, 1'001'000, false);

    const auto t = b->top(kAapl);
    EXPECT_EQ(t.mid(), 1'000'500);
    EXPECT_GT(t.micro_price(), t.mid()) << "a heavy bid should pull the micro price up";
    EXPECT_LT(t.micro_price(), t.ask_price);
}

TEST(BookBuilder, OneSidedBookHasNoMidOrSpread) {
    auto b = make_builder();
    b->on_add(kAapl, 100, 1, 'B', 100, 1'000'000, false);
    const auto t = b->top(kAapl);
    EXPECT_FALSE(t.two_sided());
    EXPECT_EQ(t.mid(), 0);
    EXPECT_EQ(t.spread(), 0);
    EXPECT_EQ(t.micro_price(), 0);
}

TEST(BookBuilder, PeakRestingOrdersIsTheHighWaterMarkAndNotTheCurrentCount) {
    auto b = make_builder();
    for (uint64_t i = 1; i <= 10; ++i) b->on_add(kAapl, 100 + i, i, 'B', 10, 1'000'000, false);
    for (uint64_t i = 1; i <= 10; ++i) b->on_delete(kAapl, 200 + i, i);

    EXPECT_EQ(b->stats().live_orders, 0u);
    EXPECT_EQ(b->stats().peak_live_orders, 10u);
}

// ----- the two price level containers have to agree --------------------------

TEST(BookBuilder, MapSideAndVectorSideProduceTheSameBook) {
    auto m = make_builder<MapSide>();
    auto v = make_builder<VectorSide>();

    struct Op { uint16_t sym; uint64_t ts; uint64_t ref; char side; uint32_t qty; uint32_t px; };
    const Op ops[] = {
        {kAapl, 100, 1, 'B', 500, 1'000'000}, {kAapl, 101, 2, 'S', 300, 1'000'500},
        {kAapl, 102, 3, 'B', 200, 999'500},   {kAapl, 103, 4, 'S', 400, 1'001'000},
        {kMsft, 104, 5, 'B', 700, 1'500'000}, {kMsft, 105, 6, 'S', 600, 1'500'500},
        {kAapl, 106, 7, 'B', 100, 1'000'000}, {kAapl, 107, 8, 'S', 900, 1'000'500},
    };
    for (const Op& o : ops) {
        m->on_add(o.sym, o.ts, o.ref, o.side, o.qty, o.px, false);
        v->on_add(o.sym, o.ts, o.ref, o.side, o.qty, o.px, false);
    }
    for (uint64_t ref : {2ULL, 5ULL}) {
        m->on_execute(kAapl, 200, ref, 100, 900);
        v->on_execute(kAapl, 200, ref, 100, 900);
    }
    for (uint64_t ref : {3ULL, 7ULL}) {
        m->on_delete(kAapl, 201, ref);
        v->on_delete(kAapl, 201, ref);
    }
    m->on_replace(kAapl, 202, 4, 44, 150, 1'002'000);
    v->on_replace(kAapl, 202, 4, 44, 150, 1'002'000);

    EXPECT_EQ(m->digest(), v->digest())
        << "the price level container must not change the book it produces";
    EXPECT_EQ(m->stats().live_orders, v->stats().live_orders);
    EXPECT_EQ(m->top(kAapl).bid_price, v->top(kAapl).bid_price);
    EXPECT_EQ(m->top(kAapl).ask_price, v->top(kAapl).ask_price);
}
