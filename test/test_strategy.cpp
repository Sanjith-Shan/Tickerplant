// The quoting rule.
//
// These tests are about behaviour that is checkable, which means the mechanics
// of the quote and not whether the quote is any good. There is no test here
// that the rule makes money, because it does not claim to and a test that
// asserted it would be measuring the generator rather than the rule.

#include "tick/strategy.hpp"

#include <gtest/gtest.h>

namespace {

using tick::MicroPriceQuoter;
using tick::Quote;
using tick::QuoteConfig;
using tick::QuoteDecision;
using tick::TopOfBook;

TopOfBook two_sided(int64_t bid, uint32_t bid_qty, int64_t ask, uint32_t ask_qty) {
    TopOfBook t;
    t.bid_price  = bid;
    t.bid_qty    = bid_qty;
    t.bid_orders = 1;
    t.ask_price  = ask;
    t.ask_qty    = ask_qty;
    t.ask_orders = 1;
    return t;
}

QuoteConfig fast_config() {
    QuoteConfig c;
    c.min_requote_interval_ns = 0; // hysteresis by price only, so tests are not timing tests
    return c;
}

} // namespace

TEST(Quoter, QuotesEitherSideOfFairValue) {
    MicroPriceQuoter q(fast_config());
    Quote            out;

    const auto d = q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out);
    EXPECT_EQ(d, QuoteDecision::Requote);
    EXPECT_TRUE(out.has_bid);
    EXPECT_TRUE(out.has_ask);
    EXPECT_LT(out.bid, out.ask);
    EXPECT_EQ(out.bid_qty, q.config().quote_size);
}

TEST(Quoter, QuotesSitOnTheTickGrid) {
    QuoteConfig cfg = fast_config();
    cfg.tick_size   = 100;
    MicroPriceQuoter q(cfg);
    Quote            out;

    q.on_top_of_book(two_sided(1'000'037, 100, 1'001'063, 100), 0, 1000, false, out);
    EXPECT_EQ(out.bid % 100, 0) << "a bid off the tick grid would be rejected by the venue";
    EXPECT_EQ(out.ask % 100, 0);
}

// Rounding outward can only ever make a quote less aggressive, which is the
// safe direction for a rounding error to go.
TEST(Quoter, RoundingNeverMakesTheQuoteMoreAggressive) {
    QuoteConfig cfg = fast_config();
    cfg.tick_size   = 100;
    cfg.half_spread_ticks = 2;
    MicroPriceQuoter q(cfg);
    Quote            out;

    const TopOfBook t = two_sided(1'000'000, 100, 1'000'400, 100);
    q.on_top_of_book(t, 0, 1000, false, out);
    const int64_t fair = t.micro_price();
    EXPECT_LE(out.bid, fair - cfg.half_spread_ticks * cfg.tick_size + cfg.tick_size);
    EXPECT_GE(out.ask, fair + cfg.half_spread_ticks * cfg.tick_size - cfg.tick_size);
}

TEST(Quoter, NoQuoteWithoutATwoSidedMarket) {
    MicroPriceQuoter q(fast_config());
    Quote            out;

    TopOfBook one_sided;
    one_sided.bid_price = 1'000'000;
    one_sided.bid_qty   = 100;

    const auto d = q.on_top_of_book(one_sided, 0, 1000, false, out);
    EXPECT_EQ(d, QuoteDecision::NoMarket);
    EXPECT_FALSE(out.has_bid);
    EXPECT_FALSE(out.has_ask);
}

// A live quote has to be pulled when the market it was quoting against stops
// existing. Leaving it up is quoting against a price that is gone.
TEST(Quoter, ALiveQuoteIsPulledWhenTheMarketGoesOneSided) {
    MicroPriceQuoter q(fast_config());
    Quote            out;

    ASSERT_EQ(q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out),
              QuoteDecision::Requote);
    EXPECT_TRUE(q.quoting());

    TopOfBook gone;
    gone.ask_price = 1'001'000;
    gone.ask_qty   = 100;
    EXPECT_EQ(q.on_top_of_book(gone, 0, 2000, false, out), QuoteDecision::Pull);
    EXPECT_FALSE(q.quoting());
}

TEST(Quoter, HaltedSymbolIsNotQuoted) {
    MicroPriceQuoter q(fast_config());
    Quote            out;
    const auto d = q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, true, out);
    EXPECT_EQ(d, QuoteDecision::Halted);
    EXPECT_FALSE(out.has_bid);
}

TEST(Quoter, AWideSpreadIsLeftAlone) {
    QuoteConfig cfg = fast_config();
    cfg.max_quotable_spread_ticks = 5;
    cfg.tick_size = 100;
    MicroPriceQuoter q(cfg);
    Quote            out;

    const auto d = q.on_top_of_book(two_sided(1'000'000, 100, 1'002'000, 100), 0, 1000, false, out);
    EXPECT_EQ(d, QuoteDecision::SpreadTooWide);
}

// Inventory skew is the reason this is a quoting rule rather than a spread
// around the mid. Being long should push both quotes down so the next fill is
// more likely to be a sale.
TEST(Quoter, BeingLongPushesTheQuoteDown) {
    MicroPriceQuoter q(fast_config());
    Quote            flat, longed;

    const TopOfBook t = two_sided(1'000'000, 100, 1'001'000, 100);
    q.on_top_of_book(t, 0, 1000, false, flat);

    MicroPriceQuoter q2(fast_config());
    q2.on_top_of_book(t, 500, 1000, false, longed);

    EXPECT_LT(longed.bid, flat.bid) << "a long position should lower the bid";
    EXPECT_LE(longed.ask, flat.ask) << "and lower the ask, so it is more likely to be lifted";
}

TEST(Quoter, BeingShortPushesTheQuoteUp) {
    const TopOfBook  t = two_sided(1'000'000, 100, 1'001'000, 100);
    MicroPriceQuoter q(fast_config()), q2(fast_config());
    Quote            flat, shorted;
    q.on_top_of_book(t, 0, 1000, false, flat);
    q2.on_top_of_book(t, -500, 1000, false, shorted);
    EXPECT_GT(shorted.ask, flat.ask);
}

// Past the inventory limit the side that would make the position worse stops
// quoting, and the other side keeps going so the position can come back in.
TEST(Quoter, TheInventoryLimitPullsOneSideAndLeavesTheOther) {
    QuoteConfig cfg = fast_config();
    cfg.max_inventory = 1000;
    cfg.skew_per_share = 0; // isolate the limit from the skew
    MicroPriceQuoter q(cfg);
    Quote            out;

    q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 1500, 1000, false, out);
    EXPECT_FALSE(out.has_bid) << "already too long to keep bidding";
    EXPECT_TRUE(out.has_ask) << "the offer is how the position comes back in";

    MicroPriceQuoter q2(cfg);
    q2.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), -1500, 1000, false, out);
    EXPECT_TRUE(out.has_bid);
    EXPECT_FALSE(out.has_ask);
}

// A quote that crosses the book takes liquidity instead of making it, which is
// not what this rule is for.
TEST(Quoter, TheQuoteNeverCrossesTheBook) {
    QuoteConfig cfg = fast_config();
    cfg.half_spread_ticks = 0;
    cfg.tick_size = 100;
    MicroPriceQuoter q(cfg);
    Quote            out;

    // A one tick wide market leaves no room, so a zero half spread would sit on
    // or through both sides.
    q.on_top_of_book(two_sided(1'000'000, 100, 1'000'100, 100), 0, 1000, false, out);
    // Braced, because the gtest macros expand to an if-else and an unbraced
    // guard around one is a dangling else that gcc warns about.
    if (out.has_bid) {
        EXPECT_LT(out.bid, 1'000'100) << "the bid must not reach the ask";
    }
    if (out.has_ask) {
        EXPECT_GT(out.ask, 1'000'000) << "the ask must not reach the bid";
    }
}

// Hysteresis. A one tick wobble is not worth two messages and the queue
// position they cost.
TEST(Quoter, SmallMovesDoNotRequote) {
    QuoteConfig cfg = fast_config();
    cfg.tick_size = 100;
    cfg.requote_threshold_ticks = 2;
    MicroPriceQuoter q(cfg);
    Quote            out;

    ASSERT_EQ(q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out),
              QuoteDecision::Requote);
    const Quote first = out;

    const auto d = q.on_top_of_book(two_sided(1'000'010, 100, 1'001'010, 100), 0, 2000, false, out);
    EXPECT_EQ(d, QuoteDecision::Unchanged);
    EXPECT_EQ(out, first) << "an unchanged decision must hand back the quote that is live";
}

TEST(Quoter, BigMovesDoRequote) {
    QuoteConfig cfg = fast_config();
    cfg.tick_size = 100;
    cfg.requote_threshold_ticks = 2;
    MicroPriceQuoter q(cfg);
    Quote            out;

    q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out);
    const auto d = q.on_top_of_book(two_sided(1'010'000, 100, 1'011'000, 100), 0, 2000, false, out);
    EXPECT_EQ(d, QuoteDecision::Requote);
}

TEST(Quoter, TheMinimumRequoteIntervalIsRespected) {
    QuoteConfig cfg;
    cfg.min_requote_interval_ns = 1'000'000;
    MicroPriceQuoter q(cfg);
    Quote            out;

    q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out);
    const auto d = q.on_top_of_book(two_sided(1'050'000, 100, 1'051'000, 100), 0, 1500, false, out);
    EXPECT_EQ(d, QuoteDecision::TooSoon) << "a big move inside the interval still has to wait";

    const auto d2 = q.on_top_of_book(two_sided(1'050'000, 100, 1'051'000, 100), 0,
                                     1000 + cfg.min_requote_interval_ns + 1, false, out);
    EXPECT_EQ(d2, QuoteDecision::Requote);
}

TEST(Quoter, FillsMoveThePosition) {
    MicroPriceQuoter q(fast_config());
    EXPECT_EQ(q.position(), 0);
    q.on_fill(nano::Side::Buy, 300);
    EXPECT_EQ(q.position(), 300);
    q.on_fill(nano::Side::Sell, 500);
    EXPECT_EQ(q.position(), -200);
    EXPECT_EQ(q.fills(), 2u);
}

TEST(Quoter, StatsCountEveryOutcome) {
    MicroPriceQuoter q(fast_config());
    Quote            out;
    q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 1000, false, out);
    q.on_top_of_book(two_sided(1'000'000, 100, 1'001'000, 100), 0, 2000, false, out);
    TopOfBook empty;
    q.on_top_of_book(empty, 0, 3000, false, out);

    EXPECT_EQ(q.stats().updates, 3u);
    EXPECT_EQ(q.stats().requotes, 1u);
    EXPECT_EQ(q.stats().unchanged, 1u);
    EXPECT_EQ(q.stats().no_market, 1u);
    EXPECT_EQ(q.stats().pulls, 1u);
}
