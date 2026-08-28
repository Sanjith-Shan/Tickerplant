#include "tick/risk.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

// Tests for the pre-trade risk gate.
//
// Two tests per reason. One that proves the reason fires when it should, and
// one that proves it does not fire on an order that is legal in every respect.
// The second half is the half people skip, and it is the half that catches a
// gate which rejects everything and therefore looks very safe while being
// completely useless.
//
// Then the ones that matter more than the individual reasons. Fat finger with
// no two sided market must reject rather than pass. The position limit must
// count orders that are working and not only orders that have filled. The kill
// switch must block everything and must not clear itself. The rate limiter must
// let a legal rate through and stop a burst. Stale data must reject. And a
// passing order must move the engine's state so that the NEXT order sees the
// exposure the first one created, because an engine that checks correctly and
// then forgets is an engine that does nothing at all.

using namespace tick;

namespace {

constexpr uint16_t kLoc   = 4242;
constexpr uint16_t kLoc2  = 99;
constexpr int64_t  kOneNs = 1;

// One second, in the nanoseconds both timestamps in an OrderRequest use.
constexpr uint64_t kSecond = 1'000'000'000ull;

// Limits that are generous everywhere, so a test can tighten exactly one and
// know which one did the rejecting. Nothing here is a recommendation, these are
// test fixtures.
RiskLimits generous_limits() {
    RiskLimits l;
    l.max_order_shares        = 10'000;
    l.max_order_notional      = 1'000'000'000ull;   // ten-thousandths of a dollar
    l.fat_finger_bps          = 500;                // five percent
    l.max_position_shares     = 50'000;
    l.max_symbol_notional     = 10'000'000'000ull;
    l.max_open_orders         = 100;
    l.max_messages_per_second = 1000;
    l.max_market_data_age_ns  = 100'000'000ull;     // one hundred milliseconds
    return l;
}

// A two sided book with a mid of one hundred dollars, which is one million in
// the ten-thousandths the feed uses.
TopOfBook healthy_book() {
    TopOfBook b;
    b.bid_price  = 999'000;
    b.bid_qty    = 100;
    b.bid_orders = 1;
    b.ask_price  = 1'001'000;
    b.ask_qty    = 100;
    b.ask_orders = 1;
    return b;
}

ouch::Token tok(int n) {
    char buf[ouch::kTokenLen + 1] = {};
    std::snprintf(buf, sizeof(buf), "T%013d", n);
    return ouch::Token::from(buf);
}

// A request that passes every check under generous_limits.
OrderRequest good_order(int token_n = 1) {
    OrderRequest r;
    r.locate     = kLoc;
    r.token      = tok(token_n);
    r.side       = nano::Side::Buy;
    r.shares     = 100;
    r.price      = 1'000'000;      // at the mid
    r.book       = healthy_book();
    r.book_ts_ns = 10 * kSecond;
    r.now_ns     = 10 * kSecond + 1'000'000;   // one millisecond later
    return r;
}

// An engine with the generous limits and the symbol switched on and trading.
RiskEngine make_engine(const RiskLimits& l = generous_limits()) {
    RiskEngine e(l);
    e.enable_symbol(kLoc, true);
    e.enable_symbol(kLoc2, true);
    e.set_halted(kLoc, false);
    e.set_halted(kLoc2, false);
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// The baseline. If this fails, nothing else below means anything.
// ---------------------------------------------------------------------------

TEST(Risk, ALegalOrderPasses) {
    auto e = make_engine();
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

TEST(Risk, EveryReasonHasADistinctName) {
    const RiskReject all[] = {
        RiskReject::None,            RiskReject::MaxOrderSize,
        RiskReject::MaxOrderNotional, RiskReject::FatFingerPrice,
        RiskReject::PositionLimit,   RiskReject::NotionalLimit,
        RiskReject::MaxOpenOrders,   RiskReject::MessageRate,
        RiskReject::SymbolNotEnabled, RiskReject::SymbolHalted,
        RiskReject::KillSwitch,      RiskReject::SelfCross,
        RiskReject::DuplicateToken,  RiskReject::StaleMarketData,
    };
    std::vector<std::string> names;
    for (auto r : all) names.emplace_back(to_string(r));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
    EXPECT_EQ(std::size(all), kRiskRejectCount);
}

// A gate whose limits were never configured must pass nothing. Zero is not a
// permissive default, it is the strictest one, which is the correct direction
// for a control to fail in.
TEST(Risk, DefaultLimitsRejectEverything) {
    RiskEngine e{};
    e.enable_symbol(kLoc, true);
    EXPECT_NE(e.check(good_order()), RiskReject::None);
}

// ---------------------------------------------------------------------------
// One test each way, per reason
// ---------------------------------------------------------------------------

TEST(Risk, MaxOrderSizeFires) {
    auto l = generous_limits();
    l.max_order_shares = 50;
    auto e = make_engine(l);

    auto r = good_order();
    r.shares = 51;
    EXPECT_EQ(e.check(r), RiskReject::MaxOrderSize);

    r.shares = 0;   // a zero share order is also nonsense
    EXPECT_EQ(e.check(r), RiskReject::MaxOrderSize);
}

TEST(Risk, MaxOrderSizeDoesNotFireOnALegalOrder) {
    auto l = generous_limits();
    l.max_order_shares = 100;
    auto e = make_engine(l);

    auto r = good_order();
    r.shares = 100;   // exactly at the limit is allowed
    EXPECT_EQ(e.check(r), RiskReject::None);
}

TEST(Risk, MaxOrderNotionalFires) {
    auto l = generous_limits();
    // One hundred shares at one hundred dollars is ten thousand dollars, which
    // is one hundred million in ten-thousandths.
    l.max_order_notional = 99'999'999ull;
    auto e = make_engine(l);
    EXPECT_EQ(e.check(good_order()), RiskReject::MaxOrderNotional);
}

TEST(Risk, MaxOrderNotionalDoesNotFireAtTheLimit) {
    auto l = generous_limits();
    l.max_order_notional = 100'000'000ull;
    auto e = make_engine(l);
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

TEST(Risk, FatFingerPriceFiresOnAPriceFarFromTheReference) {
    auto l = generous_limits();
    l.fat_finger_bps = 100;   // one percent
    auto e = make_engine(l);

    auto r = good_order();
    r.price = 1'100'000;      // ten percent above the reference
    EXPECT_EQ(e.check(r), RiskReject::FatFingerPrice);

    r.price = 900'000;        // and ten percent below
    EXPECT_EQ(e.check(r), RiskReject::FatFingerPrice);
}

TEST(Risk, FatFingerPriceDoesNotFireOnAPriceNearTheReference) {
    auto l = generous_limits();
    l.fat_finger_bps = 100;
    auto e = make_engine(l);

    auto r = good_order();
    r.price = 1'005'000;   // fifty basis points from a mid of one million
    EXPECT_EQ(e.check(r), RiskReject::None);
}

TEST(Risk, PositionLimitFires) {
    auto l = generous_limits();
    l.max_position_shares = 1000;
    auto e = make_engine(l);
    e.set_position(kLoc, 950);

    auto r = good_order();
    r.shares = 100;   // nine hundred and fifty plus one hundred is over
    EXPECT_EQ(e.check(r), RiskReject::PositionLimit);
}

TEST(Risk, PositionLimitDoesNotFireBelowTheLimit) {
    auto l = generous_limits();
    l.max_position_shares = 1000;
    auto e = make_engine(l);
    e.set_position(kLoc, 900);

    auto r = good_order();
    r.shares = 100;   // exactly at the limit is allowed
    EXPECT_EQ(e.check(r), RiskReject::None);
}

// The limit is an absolute value, so a short position is bounded the same way.
TEST(Risk, PositionLimitBoundsTheShortSideToo) {
    auto l = generous_limits();
    l.max_position_shares = 1000;
    auto e = make_engine(l);
    e.set_position(kLoc, -950);

    auto r  = good_order();
    r.side   = nano::Side::Sell;
    r.shares = 100;
    EXPECT_EQ(e.check(r), RiskReject::PositionLimit);

    // Buying back is allowed, because it moves toward flat.
    r.side = nano::Side::Buy;
    EXPECT_EQ(e.check(r), RiskReject::None);
}

TEST(Risk, NotionalLimitFires) {
    auto l = generous_limits();
    l.max_symbol_notional = 50'000'000ull;   // five thousand dollars
    auto e = make_engine(l);

    auto r = good_order();   // one hundred shares at one hundred dollars
    EXPECT_EQ(e.check(r), RiskReject::NotionalLimit);
}

TEST(Risk, NotionalLimitDoesNotFireAtTheLimit) {
    auto l = generous_limits();
    l.max_symbol_notional = 100'000'000ull;
    auto e = make_engine(l);
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

TEST(Risk, MaxOpenOrdersFires) {
    auto l = generous_limits();
    l.max_open_orders = 3;
    auto e = make_engine(l);

    for (int i = 0; i < 3; ++i) {
        auto r = good_order(i + 1);
        ASSERT_EQ(e.check(r), RiskReject::None) << "order " << i;
        e.on_order_sent(r);
    }
    EXPECT_EQ(e.check(good_order(99)), RiskReject::MaxOpenOrders);
}

TEST(Risk, MaxOpenOrdersDoesNotFireBelowTheCeiling) {
    auto l = generous_limits();
    l.max_open_orders = 3;
    auto e = make_engine(l);

    for (int i = 0; i < 2; ++i) {
        auto r = good_order(i + 1);
        ASSERT_EQ(e.check(r), RiskReject::None);
        e.on_order_sent(r);
    }
    EXPECT_EQ(e.check(good_order(99)), RiskReject::None);
}

TEST(Risk, SymbolNotEnabledFires) {
    auto e = make_engine();
    e.enable_symbol(kLoc, false);
    EXPECT_EQ(e.check(good_order()), RiskReject::SymbolNotEnabled);
}

TEST(Risk, SymbolNotEnabledDoesNotFireWhenEnabled) {
    auto e = make_engine();
    e.enable_symbol(kLoc, true);
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

TEST(Risk, SymbolHaltedFires) {
    auto e = make_engine();
    e.set_halted(kLoc, true);
    EXPECT_EQ(e.check(good_order()), RiskReject::SymbolHalted);
}

TEST(Risk, SymbolHaltedDoesNotFireWhenTrading) {
    auto e = make_engine();
    e.set_halted(kLoc, false);
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

// The feed's trading action states drive the halt flag. 'T' is trading and
// anything else is not, which is the conservative reading.
TEST(Risk, TradingActionFromTheFeedDrivesTheHaltFlag) {
    auto e = make_engine();
    e.on_trading_action(kLoc, 'H');
    EXPECT_EQ(e.check(good_order()), RiskReject::SymbolHalted);
    e.on_trading_action(kLoc, 'T');
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
    e.on_trading_action(kLoc, 'Q');   // quotation only, not trading
    EXPECT_EQ(e.check(good_order()), RiskReject::SymbolHalted);
}

TEST(Risk, SelfCrossFires) {
    auto e = make_engine();

    auto sell  = good_order(1);
    sell.side   = nano::Side::Sell;
    sell.price  = 1'001'000;
    ASSERT_EQ(e.check(sell), RiskReject::None);
    e.on_order_sent(sell);

    // A buy at or above our own working offer would trade with us.
    auto buy = good_order(2);
    buy.price = 1'001'000;
    EXPECT_EQ(e.check(buy), RiskReject::SelfCross);

    buy.price = 1'002'000;
    EXPECT_EQ(e.check(buy), RiskReject::SelfCross);
}

TEST(Risk, SelfCrossDoesNotFireOnAnOrderThatCannotCross) {
    auto e = make_engine();

    auto sell = good_order(1);
    sell.side  = nano::Side::Sell;
    sell.price = 1'001'000;
    ASSERT_EQ(e.check(sell), RiskReject::None);
    e.on_order_sent(sell);

    auto buy = good_order(2);
    buy.price = 1'000'000;   // strictly below our own offer
    EXPECT_EQ(e.check(buy), RiskReject::None);
}

TEST(Risk, DuplicateTokenFires) {
    auto e = make_engine();
    auto r = good_order(7);
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    // The same token again. OUCH would silently ignore this, so the gate has to
    // be the thing that notices.
    auto again = good_order(7);
    again.price = 1'000'500;
    EXPECT_EQ(e.check(again), RiskReject::DuplicateToken);
}

TEST(Risk, DuplicateTokenDoesNotFireOnAFreshToken) {
    auto e = make_engine();
    auto r = good_order(7);
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);
    EXPECT_EQ(e.check(good_order(8)), RiskReject::None);
}

// Once an order is gone the token is no longer working, so a later order may
// use a different token freely and the table does not fill up.
TEST(Risk, RetiredOrdersLeaveTheOpenOrderTable) {
    auto e = make_engine();
    for (int i = 0; i < 500; ++i) {
        auto r = good_order(i + 1);
        ASSERT_EQ(e.check(r), RiskReject::None) << "order " << i;
        e.on_order_sent(r);
        e.on_cancel_acked(r.token, 0);
        EXPECT_EQ(e.open_orders(), 0u);
    }
    EXPECT_EQ(e.stats().unknown_token_events, 0u);
    EXPECT_EQ(e.stats().working_share_drift, 0u);
}

TEST(Risk, MessageRateFires) {
    auto l = generous_limits();
    l.max_messages_per_second = 5;
    auto e = make_engine(l);

    uint64_t now = 10 * kSecond;
    for (int i = 0; i < 5; ++i) {
        auto r = good_order(i + 1);
        r.book_ts_ns = now;
        r.now_ns     = now;
        ASSERT_EQ(e.check(r), RiskReject::None) << "message " << i;
        e.on_order_sent(r);
        now += 1'000'000;   // one millisecond apart, well inside the window
    }

    auto r = good_order(100);
    r.book_ts_ns = now;
    r.now_ns     = now;
    EXPECT_EQ(e.check(r), RiskReject::MessageRate);
}

TEST(Risk, MessageRateDoesNotFireAtALegalRate) {
    auto l = generous_limits();
    l.max_messages_per_second = 5;
    auto e = make_engine(l);

    // Four per second for ten seconds, which is comfortably under the ceiling
    // and must never be rejected.
    uint64_t now = 10 * kSecond;
    for (int sec = 0; sec < 10; ++sec) {
        for (int i = 0; i < 4; ++i) {
            auto r = good_order(sec * 10 + i + 1);
            r.book_ts_ns = now;
            r.now_ns     = now;
            ASSERT_EQ(e.check(r), RiskReject::None)
                << "second " << sec << " message " << i;
            e.on_order_sent(r);
            e.on_cancel_acked(r.token, 0);   // keep the open order count out of it
            now += 250'000'000ull;           // two hundred and fifty milliseconds
        }
    }
}

// The window has to actually expire, otherwise the limiter is a lifetime cap.
TEST(Risk, MessageRateRecoversOnceTheWindowPasses) {
    auto l = generous_limits();
    l.max_messages_per_second = 3;
    auto e = make_engine(l);

    uint64_t now = 10 * kSecond;
    for (int i = 0; i < 3; ++i) {
        auto r = good_order(i + 1);
        r.book_ts_ns = now;
        r.now_ns     = now;
        ASSERT_EQ(e.check(r), RiskReject::None);
        e.on_order_sent(r);
    }

    auto blocked = good_order(50);
    blocked.book_ts_ns = now;
    blocked.now_ns     = now;
    ASSERT_EQ(e.check(blocked), RiskReject::MessageRate);

    // Two seconds later every bucket from the burst has fallen out.
    auto later = good_order(51);
    later.book_ts_ns = now + 2 * kSecond;
    later.now_ns     = now + 2 * kSecond;
    EXPECT_EQ(e.check(later), RiskReject::None);
}

TEST(Risk, StaleMarketDataFires) {
    auto l = generous_limits();
    l.max_market_data_age_ns = 50'000'000ull;   // fifty milliseconds
    auto e = make_engine(l);

    auto r = good_order();
    r.book_ts_ns = 10 * kSecond;
    r.now_ns     = 10 * kSecond + 60'000'000ull;   // sixty milliseconds old
    EXPECT_EQ(e.check(r), RiskReject::StaleMarketData);
}

TEST(Risk, StaleMarketDataDoesNotFireOnAFreshBook) {
    auto l = generous_limits();
    l.max_market_data_age_ns = 50'000'000ull;
    auto e = make_engine(l);

    auto r = good_order();
    r.book_ts_ns = 10 * kSecond;
    r.now_ns     = 10 * kSecond + 40'000'000ull;
    EXPECT_EQ(e.check(r), RiskReject::None);
}

// A book that has never updated has a timestamp of zero. That is the state at
// startup and it must not be treated as infinitely fresh.
TEST(Risk, ABookThatHasNeverUpdatedIsStale) {
    auto e = make_engine();
    auto r = good_order();
    r.book_ts_ns = 0;
    EXPECT_EQ(e.check(r), RiskReject::StaleMarketData);
}

// A timestamp in the future means the two clocks disagree, and an engine that
// subtracts them would compute a tiny age and pass. Reject instead.
TEST(Risk, ABookTimestampInTheFutureIsStale) {
    auto e = make_engine();
    auto r = good_order();
    r.book_ts_ns = 10 * kSecond;
    r.now_ns     = 10 * kSecond - kOneNs;
    EXPECT_EQ(e.check(r), RiskReject::StaleMarketData);
}

TEST(Risk, KillSwitchFires) {
    auto e = make_engine();
    ASSERT_EQ(e.check(good_order()), RiskReject::None);
    e.trip_kill_switch("test");
    EXPECT_EQ(e.check(good_order()), RiskReject::KillSwitch);
}

TEST(Risk, KillSwitchDoesNotFireWhenItHasNotBeenTripped) {
    auto e = make_engine();
    EXPECT_FALSE(e.tripped());
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

// ---------------------------------------------------------------------------
// Fail closed. The ones that matter most.
// ---------------------------------------------------------------------------

// The single most important test in this file.
//
// A one sided book gives the fat finger check no reference price. The wrong
// answer is to skip the check and pass the order, because a one sided book is
// exactly when the market is moving and a mistyped price costs the most. The
// order must be rejected.
TEST(Risk, FatFingerWithNoTwoSidedMarketRejects) {
    auto e = make_engine();

    auto bid_only = good_order(1);
    bid_only.book.ask_qty    = 0;
    bid_only.book.ask_orders = 0;
    bid_only.book.ask_price  = 0;
    ASSERT_FALSE(bid_only.book.two_sided());
    EXPECT_EQ(e.check(bid_only), RiskReject::FatFingerPrice);

    auto ask_only = good_order(2);
    ask_only.book.bid_qty    = 0;
    ask_only.book.bid_orders = 0;
    ask_only.book.bid_price  = 0;
    ASSERT_FALSE(ask_only.book.two_sided());
    EXPECT_EQ(e.check(ask_only), RiskReject::FatFingerPrice);

    auto empty = good_order(3);
    empty.book = TopOfBook{};
    EXPECT_EQ(e.check(empty), RiskReject::FatFingerPrice);
}

// The same order that was rejected for having no reference passes the moment
// the other side appears, which proves the rejection was about the missing
// reference and not about the price.
TEST(Risk, TheSameOrderPassesOnceTheBookIsTwoSided) {
    auto e = make_engine();

    auto r = good_order();
    r.book.ask_qty = 0;
    ASSERT_EQ(e.check(r), RiskReject::FatFingerPrice);

    r.book.ask_qty = 100;
    ASSERT_TRUE(r.book.two_sided());
    EXPECT_EQ(e.check(r), RiskReject::None);
}

// A price of zero or below has no meaning and cannot be checked against
// anything, so it is refused rather than allowed through as free.
TEST(Risk, ANonPositivePriceIsRejected) {
    auto e = make_engine();
    auto r = good_order();
    r.price = 0;
    EXPECT_EQ(e.check(r), RiskReject::FatFingerPrice);
    r.price = -100;
    EXPECT_EQ(e.check(r), RiskReject::FatFingerPrice);
}

// ---------------------------------------------------------------------------
// Working exposure. The difference between a real limit and a decorative one.
// ---------------------------------------------------------------------------

// A limit checked against filled position alone would let all ten of these
// through, because nothing has filled yet and the position is still flat the
// whole time. Counting working exposure stops it at the limit.
TEST(Risk, PositionLimitCountsWorkingExposureNotOnlyFills) {
    auto l = generous_limits();
    l.max_position_shares = 500;
    l.max_order_shares    = 100;
    auto e = make_engine(l);

    int sent = 0;
    for (int i = 0; i < 10; ++i) {
        auto r = good_order(i + 1);
        r.shares = 100;
        if (e.check(r) != RiskReject::None) break;
        e.on_order_sent(r);
        ++sent;
    }

    // Five orders of one hundred shares is the whole limit, and nothing has
    // filled. The position is flat and the gate still said no.
    EXPECT_EQ(sent, 5);
    EXPECT_EQ(e.symbol(kLoc).position, 0);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 500);
    EXPECT_EQ(e.check(good_order(50)), RiskReject::PositionLimit);
}

// A fill converts working exposure into position. The worst case is unchanged,
// so the gate's answer must be unchanged too. A fill is not new risk, it is
// risk becoming certain.
TEST(Risk, AFillMovesExposureFromWorkingToPositionWithoutChangingTheWorstCase) {
    auto l = generous_limits();
    l.max_position_shares = 500;
    auto e = make_engine(l);

    auto r = good_order(1);
    r.shares = 500;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 500);
    EXPECT_EQ(e.symbol(kLoc).position, 0);
    ASSERT_EQ(e.check(good_order(2)), RiskReject::PositionLimit);

    e.on_fill(r.token, 500, 1'000'000);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 0);
    EXPECT_EQ(e.symbol(kLoc).position, 500);
    // Still at the limit, so still no.
    EXPECT_EQ(e.check(good_order(3)), RiskReject::PositionLimit);
}

// Cancelling an order gives the exposure back, so the next order may go.
TEST(Risk, ACancelReleasesWorkingExposure) {
    auto l = generous_limits();
    l.max_position_shares = 500;
    auto e = make_engine(l);

    auto r = good_order(1);
    r.shares = 500;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);
    ASSERT_EQ(e.check(good_order(2)), RiskReject::PositionLimit);

    e.on_cancel_acked(r.token, 0);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 0);
    EXPECT_EQ(e.open_orders(), 0u);
    EXPECT_EQ(e.check(good_order(2)), RiskReject::None);
}

// An exchange reject means the order never existed, so all of its exposure has
// to come back at once.
TEST(Risk, AnExchangeRejectReleasesTheWholeOrder) {
    auto l = generous_limits();
    l.max_position_shares = 500;
    auto e = make_engine(l);

    auto r = good_order(1);
    r.shares = 500;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);
    ASSERT_EQ(e.check(good_order(2)), RiskReject::PositionLimit);

    e.on_reject(r.token);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 0);
    EXPECT_EQ(e.symbol(kLoc).position, 0);
    EXPECT_EQ(e.stats().exchange_rejects, 1u);
    EXPECT_EQ(e.check(good_order(2)), RiskReject::None);
}

// Partial fills and partial cancels both reduce what is working, and OUCH sends
// both as incremental numbers.
TEST(Risk, PartialFillsAndPartialCancelsAreIncremental) {
    auto e = make_engine();

    auto r = good_order(1);
    r.shares = 1000;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    e.on_fill(r.token, 300, 1'000'000);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 700);
    EXPECT_EQ(e.symbol(kLoc).position, 300);
    EXPECT_EQ(e.open_orders(), 1u);

    e.on_cancel_acked(r.token, 200);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 500);
    EXPECT_EQ(e.open_orders(), 1u);   // five hundred still working

    e.on_fill(r.token, 500, 1'000'000);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 0);
    EXPECT_EQ(e.symbol(kLoc).position, 800);
    EXPECT_EQ(e.open_orders(), 0u);   // now it is gone
    EXPECT_EQ(e.stats().overfills, 0u);
    EXPECT_EQ(e.stats().working_share_drift, 0u);
}

// Exposure is per symbol. A position in one name must not gate a different one.
TEST(Risk, ExposureIsKeptPerSymbol) {
    auto l = generous_limits();
    l.max_position_shares = 500;
    auto e = make_engine(l);

    auto r = good_order(1);
    r.shares = 500;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);
    ASSERT_EQ(e.check(good_order(2)), RiskReject::PositionLimit);

    auto other = good_order(3);
    other.locate = kLoc2;
    EXPECT_EQ(e.check(other), RiskReject::None);
}

// ---------------------------------------------------------------------------
// The state update test. This is the bug that makes a risk engine useless.
// ---------------------------------------------------------------------------

// A gate that evaluates each order correctly in isolation and then forgets it
// ever saw one is not a gate. Every check below is against state the previous
// order created.
TEST(Risk, APassingOrderUpdatesStateSoTheNextOrderSeesIt) {
    auto l = generous_limits();
    l.max_position_shares = 300;
    l.max_open_orders     = 10;
    auto e = make_engine(l);

    const auto& sym = e.symbol(kLoc);
    EXPECT_EQ(sym.working_buy_shares, 0);
    EXPECT_EQ(e.open_orders(), 0u);
    EXPECT_EQ(e.stats().orders_sent, 0u);

    auto first = good_order(1);
    first.shares = 200;
    ASSERT_EQ(e.check(first), RiskReject::None);
    e.on_order_sent(first);

    // Everything the next order will be measured against has moved.
    EXPECT_EQ(sym.working_buy_shares, 200);
    EXPECT_EQ(sym.working_buy_orders, 1u);
    EXPECT_EQ(sym.best_working_buy, first.price);
    EXPECT_EQ(e.open_orders(), 1u);
    EXPECT_EQ(e.stats().orders_sent, 1u);
    EXPECT_EQ(e.stats().open_orders_peak, 1u);

    // The second order would have passed against a clean engine and does not
    // pass against this one, which is the whole point.
    auto second = good_order(2);
    second.shares = 200;
    EXPECT_EQ(e.check(second), RiskReject::PositionLimit);

    // One hundred fits in what is left.
    auto third = good_order(3);
    third.shares = 100;
    ASSERT_EQ(e.check(third), RiskReject::None);
    e.on_order_sent(third);
    EXPECT_EQ(sym.working_buy_shares, 300);
    EXPECT_EQ(e.open_orders(), 2u);

    // And now nothing more fits at all.
    auto fourth = good_order(4);
    fourth.shares = 1;
    EXPECT_EQ(e.check(fourth), RiskReject::PositionLimit);
}

// The duplicate token check has to see the token the previous order used, which
// is another form of the same requirement.
TEST(Risk, TheTokenOfASentOrderIsVisibleToTheNextCheck) {
    auto e = make_engine();
    auto r = good_order(11);
    ASSERT_EQ(e.check(r), RiskReject::None);
    ASSERT_EQ(e.check(r), RiskReject::None);   // check is pure, nothing moved
    e.on_order_sent(r);
    EXPECT_EQ(e.check(r), RiskReject::DuplicateToken);
}

// check() must not change anything. Calling it a thousand times has to leave
// the engine exactly where it started, otherwise the pure comment on it is a
// lie and a what-if tool would move real state.
TEST(Risk, CheckIsPureAndMovesNothing) {
    auto e = make_engine();
    const auto before_open = e.open_orders();
    const auto before_pos  = e.symbol(kLoc).position;
    const auto before_work = e.symbol(kLoc).working_buy_shares;
    const auto before_sent = e.stats().orders_sent;

    for (int i = 0; i < 1000; ++i) EXPECT_EQ(e.check(good_order(1)), RiskReject::None);

    EXPECT_EQ(e.open_orders(), before_open);
    EXPECT_EQ(e.symbol(kLoc).position, before_pos);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, before_work);
    EXPECT_EQ(e.stats().orders_sent, before_sent);
}

// ---------------------------------------------------------------------------
// The kill switch
// ---------------------------------------------------------------------------

// One flag, checked first, and it beats every other consideration including an
// order that is legal in every other way.
TEST(Risk, KillSwitchBlocksEverythingIncludingPerfectlyLegalOrders) {
    auto e = make_engine();
    e.trip_kill_switch("a human pressed the button");

    EXPECT_EQ(e.check(good_order(1)), RiskReject::KillSwitch);

    auto tiny = good_order(2);
    tiny.shares = 1;
    EXPECT_EQ(e.check(tiny), RiskReject::KillSwitch);

    auto sell = good_order(3);
    sell.side = nano::Side::Sell;
    EXPECT_EQ(e.check(sell), RiskReject::KillSwitch);

    auto other_symbol = good_order(4);
    other_symbol.locate = kLoc2;
    EXPECT_EQ(e.check(other_symbol), RiskReject::KillSwitch);
}

// Nothing that happens afterwards may clear it. Not time, not a cancel, not a
// fill, not a reject, not a successful check of anything.
TEST(Risk, KillSwitchDoesNotResetItself) {
    auto e = make_engine();

    auto r = good_order(1);
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    e.trip_kill_switch("fault");
    ASSERT_TRUE(e.tripped());

    e.on_fill(r.token, 50, 1'000'000);
    e.on_cancel_acked(r.token, 0);
    e.on_reject(tok(999));
    e.enable_symbol(kLoc, true);
    e.set_halted(kLoc, false);
    for (int i = 0; i < 100; ++i) (void)e.check(good_order(i + 10));

    EXPECT_TRUE(e.tripped());
    EXPECT_EQ(e.check(good_order(500)), RiskReject::KillSwitch);
}

// Clearing it is a separate, differently named call, so it cannot be reached by
// habit or by a mistyped method name.
TEST(Risk, KillSwitchClearsOnlyOnTheExplicitHumanCall) {
    auto e = make_engine();
    e.trip_kill_switch("fault");
    ASSERT_EQ(e.check(good_order()), RiskReject::KillSwitch);

    e.reset_kill_switch_after_human_review();
    EXPECT_FALSE(e.tripped());
    EXPECT_EQ(e.check(good_order()), RiskReject::None);
}

// The first reason is kept, because that is what went wrong. Everything after
// it is consequence.
TEST(Risk, KillSwitchKeepsTheFirstReasonAndCountsTheRest) {
    auto e = make_engine();
    e.trip_kill_switch("the first thing");
    e.trip_kill_switch("the second thing");
    EXPECT_STREQ(e.stats().kill_reason, "the first thing");
    EXPECT_EQ(e.stats().kill_switch_trips, 2u);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST(Risk, RejectsAreCountedPerReason) {
    auto l = generous_limits();
    l.max_order_shares = 10;
    auto e = make_engine(l);

    auto big = good_order(1);
    big.shares = 100;
    for (int i = 0; i < 3; ++i) EXPECT_EQ(e.check_and_count(big), RiskReject::MaxOrderSize);

    // Shares are inside the tightened size limit, so the reason this one fails
    // is the stale book and not the size. check() returns the first failure.
    auto stale = good_order(2);
    stale.shares     = 10;
    stale.book_ts_ns = 0;
    EXPECT_EQ(e.check_and_count(stale), RiskReject::StaleMarketData);

    auto ok = good_order(3);
    ok.shares = 10;
    EXPECT_EQ(e.check_and_count(ok), RiskReject::None);

    EXPECT_EQ(e.stats().checked, 5u);
    EXPECT_EQ(e.stats().rejected, 4u);
    EXPECT_EQ(e.stats().passed, 1u);
    EXPECT_EQ(e.stats().count(RiskReject::MaxOrderSize), 3u);
    EXPECT_EQ(e.stats().count(RiskReject::StaleMarketData), 1u);
    EXPECT_EQ(e.stats().count(RiskReject::None), 1u);
    EXPECT_EQ(e.stats().count(RiskReject::FatFingerPrice), 0u);
}

// A fill or a cancel for a token this engine never sent means the two sides
// disagree about what is working, which is the condition under which a risk
// engine is worse than no risk engine. It is counted loudly.
TEST(Risk, EventsForUnknownTokensAreCounted) {
    auto e = make_engine();
    e.on_fill(tok(1234), 100, 1'000'000);
    e.on_cancel_acked(tok(1235), 50);
    e.on_reject(tok(1236));
    EXPECT_EQ(e.stats().unknown_token_events, 3u);
    // Nothing moved, because there was nothing to move.
    EXPECT_EQ(e.symbol(kLoc).position, 0);
    EXPECT_EQ(e.open_orders(), 0u);
}

// More shares filled than were working. The exchange's number is the truth, so
// the working count is clamped rather than allowed to go negative, and the
// discrepancy is counted.
TEST(Risk, AnOverfillIsClampedAndCounted) {
    auto e = make_engine();
    auto r = good_order(1);
    r.shares = 100;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    e.on_fill(r.token, 250, 1'000'000);
    EXPECT_EQ(e.stats().overfills, 1u);
    EXPECT_EQ(e.symbol(kLoc).working_buy_shares, 0);
    EXPECT_EQ(e.open_orders(), 0u);
}

TEST(Risk, FillNotionalIsTotalled) {
    auto e = make_engine();
    auto r = good_order(1);
    r.shares = 100;
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);
    e.on_fill(r.token, 40, 1'000'000);
    e.on_fill(r.token, 60, 1'000'500);
    EXPECT_EQ(e.stats().filled_notional, 40ull * 1'000'000 + 60ull * 1'000'500);
    EXPECT_EQ(e.stats().fills, 2u);
}

// ---------------------------------------------------------------------------
// The rate limiter on its own
// ---------------------------------------------------------------------------

TEST(RiskRateLimiter, CountsWithinTheWindowAndForgetsOutsideIt) {
    RateLimiter rl;
    const uint64_t t0 = 100 * kSecond;

    for (int i = 0; i < 7; ++i) rl.record(t0 + static_cast<uint64_t>(i) * 10'000'000ull);
    EXPECT_EQ(rl.count_at(t0 + 60'000'000ull), 7u);

    // One full window later nothing from that burst is still counted.
    EXPECT_EQ(rl.count_at(t0 + RateLimiter::kWindowNs + RateLimiter::kBucketNs), 0u);
}

// The window is the current partial bucket plus the nine behind it, so the
// measured span is between nine hundred milliseconds and one second. That makes
// the limiter marginally strict and never permissive, which is the right
// direction for a fuse.
TEST(RiskRateLimiter, WindowIsNeverLongerThanOneSecond) {
    RateLimiter rl;
    const uint64_t t0 = 0;
    rl.record(t0);
    EXPECT_EQ(rl.count_at(t0), 1u);
    EXPECT_EQ(rl.count_at(t0 + RateLimiter::kWindowNs - 1), 1u);
    EXPECT_EQ(rl.count_at(t0 + RateLimiter::kWindowNs), 0u);
}

TEST(RiskRateLimiter, CountingIsConstAndRepeatable) {
    RateLimiter rl;
    rl.record(5 * kSecond);
    const auto first = rl.count_at(5 * kSecond);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(rl.count_at(5 * kSecond), first);
}

// ---------------------------------------------------------------------------
// The open order table
// ---------------------------------------------------------------------------

// Backward shift deletion rather than tombstones, so the table performs the
// same after a day of churn as it did at the open. Interleaving inserts and
// erases is what would break a probe chain if the deletion were wrong.
TEST(Risk, OpenOrderTableSurvivesHeavyChurn) {
    auto l = generous_limits();
    l.max_open_orders = 64;
    auto e = make_engine(l);

    // Time advances, because every order counts against the rate limiter and a
    // loop that sends thousands of orders at one instant is rate limited long
    // before it says anything about the table. check() returns the FIRST reason
    // an order fails, so a test about one reason has to keep the others clean.
    uint64_t now = 10 * kSecond;
    auto at = [&now](OrderRequest r) {
        r.book_ts_ns = now;
        r.now_ns     = now;
        return r;
    };

    std::vector<ouch::Token> live;
    int next_token = 1;
    for (int round = 0; round < 200; ++round) {
        while (live.size() < 40) {
            auto r = at(good_order(next_token++));
            r.shares = 1;
            ASSERT_EQ(e.check(r), RiskReject::None) << "round " << round;
            e.on_order_sent(r);
            live.push_back(r.token);
            now += 2'000'000;   // two milliseconds, so the rate stays legal
        }
        // Retire from the middle, which is where a broken probe chain shows up.
        for (int k = 0; k < 20; ++k) {
            const std::size_t idx = live.size() / 2;
            e.on_cancel_acked(live[idx], 0);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        // Everything still live must still be findable. These are check() calls
        // only, so they cost nothing against the rate.
        for (const auto& t : live) {
            auto probe  = at(good_order(0));
            probe.token = t;
            probe.shares = 1;
            EXPECT_EQ(e.check(probe), RiskReject::DuplicateToken) << "round " << round;
        }
    }
    EXPECT_EQ(e.stats().unknown_token_events, 0u);
    EXPECT_EQ(e.stats().working_share_drift, 0u);
    EXPECT_EQ(e.open_orders(), live.size());
}

// Limits may only change with nothing working, because changing them resizes
// the open order table.
TEST(Risk, LimitsCannotChangeWhileOrdersAreWorking) {
    auto e = make_engine();
    auto r = good_order(1);
    ASSERT_EQ(e.check(r), RiskReject::None);
    e.on_order_sent(r);

    auto tighter = generous_limits();
    tighter.max_order_shares = 1;
    EXPECT_FALSE(e.set_limits(tighter));
    EXPECT_EQ(e.limits().max_order_shares, 10'000u);

    e.on_cancel_acked(r.token, 0);
    EXPECT_TRUE(e.set_limits(tighter));
    EXPECT_EQ(e.limits().max_order_shares, 1u);
}
