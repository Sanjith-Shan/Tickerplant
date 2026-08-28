#pragma once

#include "tick/book_builder.hpp"
#include "tick/ouch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Pre-trade risk. The gate every order passes through before it reaches the
// wire.
//
// WHAT THIS IS FOR.
//
// A strategy decides what it wants to do. Risk decides whether it is allowed
// to. The two are separate on purpose, because the strategy is the thing most
// likely to be wrong and the gate has to hold when it is. Every order either
// passes or is rejected with one specific reason, that reason is counted, and
// the counts are the thing a human looks at when something has gone strange.
//
// A reject reason of None is the only thing that means send.
//
// COST, BECAUSE THIS IS ON THE TICK TO TRADE PATH.
//
// check() sits between the strategy's decision and the encoder, so its cost is
// part of the number this whole repository exists to measure. Every check is
// O(1) and allocates nothing. The per symbol state lives in one flat array
// indexed by the feed's stock locate, and because stock locate is a uint16 and
// the array has sixty five thousand five hundred and thirty six entries, the
// index is always in range and there is no bounds check on the hot path at all.
// The open order table is fixed capacity open addressing, sized at construction
// from the max open orders limit, and it is the only structure here that can
// miss cache, which is why it is probed last.
//
// The function is written as a run of early returns rather than as branchless
// arithmetic. That is deliberate. On the passing path every branch falls
// through, the predictor sees an unbroken straight line, and the whole gate is
// a sequence of compares against values that are already hot. A branchless
// version would have to evaluate every check including the ones after the first
// failure, which is more work in the common case to make the rare case
// uniform. What is avoided is branching on anything unpredictable, so there are
// no data dependent loops, no lookups keyed on the order's contents except the
// one token probe, and no virtual calls.
//
// FAIL CLOSED.
//
// Wherever a check cannot be evaluated, the order is rejected. This shows up
// most sharply in the fat finger check, which needs a reference price and has
// none when the book is one sided. The temptation is to skip the check and let
// the order through. That is how firms lose money, because the moment the book
// goes one sided is exactly the moment prices are moving and a mistyped price
// is most expensive. A limit that is not set is zero and zero rejects
// everything, so a RiskEngine constructed with default limits passes nothing.
// That is the correct default for a gate.
//
// WHAT IS DELIBERATELY NOT HERE.
//
// So that the reader knows these were scoped out rather than forgotten.
//
// Short sale locate and the Reg SHO order marking rules. Marking an order sell
// short or sell short exempt has a borrow behind it, and the locate lives in a
// stock loan system this project does not have.
//
// Credit and margin. Buying power, house and Reg T requirements, and haircuts
// are a clearing relationship, not a wire protocol.
//
// SEC Rule 15c3-5 in full. This file implements the flavour of the controls the
// rule requires, per order capital thresholds, erroneous order checks, and a
// hard block, but the rule also requires the controls be under the exclusive
// control of the broker dealer, be reviewed annually, and cover every order the
// firm sends anywhere. The last part is the next item.
//
// Cross venue aggregate exposure. This engine sees one order flow to one
// destination. Real aggregate limits are enforced across every venue and every
// desk at once, which needs a shared position service and a consistency story
// this file does not attempt.
//
// Self match prevention beyond one process. The SelfCross check below only
// knows about orders this engine sent. Two strategies in two processes under
// the same MPID can still cross, and preventing that is the exchange's Self
// Match Prevention feature, not this file.

namespace tick {

// ---------------------------------------------------------------------------
// Reject reasons
// ---------------------------------------------------------------------------

enum class RiskReject : uint8_t {
    None = 0,
    MaxOrderSize,      // this one order is too many shares
    MaxOrderNotional,  // this one order is too much money
    FatFingerPrice,    // price too far from the reference, or no reference
    PositionLimit,     // worst case share position would breach the limit
    NotionalLimit,     // worst case money at risk in this symbol would breach
    MaxOpenOrders,     // too many orders working at once
    MessageRate,       // sending faster than the configured ceiling
    SymbolNotEnabled,  // trading has not been switched on for this symbol
    SymbolHalted,      // the feed says this symbol is halted
    KillSwitch,        // a human or a fault has stopped everything
    SelfCross,         // this order would trade against our own working order
    DuplicateToken,    // this token is already working, resending it is silent
    StaleMarketData,   // the book backing this decision is too old to trust
};

inline constexpr std::size_t kRiskRejectCount = 14;

[[nodiscard]] inline const char* to_string(RiskReject r) noexcept {
    switch (r) {
    case RiskReject::None:             return "None";
    case RiskReject::MaxOrderSize:     return "MaxOrderSize";
    case RiskReject::MaxOrderNotional: return "MaxOrderNotional";
    case RiskReject::FatFingerPrice:   return "FatFingerPrice";
    case RiskReject::PositionLimit:    return "PositionLimit";
    case RiskReject::NotionalLimit:    return "NotionalLimit";
    case RiskReject::MaxOpenOrders:    return "MaxOpenOrders";
    case RiskReject::MessageRate:      return "MessageRate";
    case RiskReject::SymbolNotEnabled: return "SymbolNotEnabled";
    case RiskReject::SymbolHalted:     return "SymbolHalted";
    case RiskReject::KillSwitch:       return "KillSwitch";
    case RiskReject::SelfCross:        return "SelfCross";
    case RiskReject::DuplicateToken:   return "DuplicateToken";
    case RiskReject::StaleMarketData:  return "StaleMarketData";
    }
    return "Invalid";
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

// There are no default values here on purpose. A plausible looking default is a
// limit that nobody chose, and a limit nobody chose is a limit nobody will
// notice is wrong. Zero rejects everything, so an engine whose limits were
// never configured sends nothing rather than sending with guesses.
struct RiskLimits {
    uint32_t max_order_shares       = 0;
    uint64_t max_order_notional     = 0;  // ten-thousandths of a dollar, as the feed uses
    uint32_t fat_finger_bps         = 0;  // reject a price this far from the reference
    int64_t  max_position_shares    = 0;  // absolute value, per symbol
    uint64_t max_symbol_notional    = 0;  // worst case money at risk, per symbol
    uint32_t max_open_orders        = 0;  // engine wide
    uint32_t max_messages_per_second = 0;
    uint64_t max_market_data_age_ns = 0;  // quoting on a stale book is its own risk
};

// ---------------------------------------------------------------------------
// The order being checked
// ---------------------------------------------------------------------------

// Everything check() is allowed to look at, other than engine state. The book
// and both timestamps travel with the request rather than being read from a
// shared object, which is what makes check() genuinely pure. The same request
// checked twice gives the same answer, so it can be called speculatively, from
// a test, or from a what-if tool, without moving anything.
//
// now_ns and book_ts_ns must come from the same clock. In this repository that
// is the steady monotonic clock in nano::now, never a wall clock, because a
// wall clock stepping backwards would make a stale book look fresh.
struct OrderRequest {
    uint16_t    locate      = 0;   // symbol, keyed exactly as the feed keys it
    ouch::Token token{};
    nano::Side  side        = nano::Side::Buy;
    uint32_t    shares      = 0;
    nano::Price price       = 0;   // ten-thousandths of a dollar
    TopOfBook   book{};            // the market this decision was made against
    uint64_t    book_ts_ns  = 0;   // when that top of book last changed
    uint64_t    now_ns      = 0;   // decision time, same clock as book_ts_ns

    [[nodiscard]] bool is_buy() const noexcept { return side == nano::Side::Buy; }
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct RiskStats {
    uint64_t checked  = 0;
    uint64_t passed   = 0;
    uint64_t rejected = 0;

    // One counter per reason. Indexed by the enum value, which is why the enum
    // is dense and starts at zero.
    std::array<uint64_t, kRiskRejectCount> by_reason{};

    uint64_t orders_sent      = 0;
    uint64_t fills            = 0;
    uint64_t cancels_acked    = 0;
    uint64_t exchange_rejects = 0;

    uint32_t open_orders      = 0;
    uint32_t open_orders_peak = 0;

    // Bookkeeping faults. None of these should ever be non-zero. Each one means
    // this engine's picture of the world disagrees with the exchange's, which
    // is the condition under which a risk engine is worse than no risk engine.
    uint64_t unknown_token_events = 0;  // a fill or cancel for a token not open
    uint64_t overfills            = 0;  // more filled than was working
    uint64_t table_insert_failed  = 0;  // open order table full, should be impossible
    uint64_t working_share_drift  = 0;  // a side emptied with shares still counted

    // Realised, in ten-thousandths of a dollar. Both sides add, so this is
    // turnover rather than profit. It exists so the fill price is recorded
    // somewhere rather than discarded, since a share based position does not
    // otherwise need it.
    uint64_t filled_notional = 0;

    uint64_t    kill_switch_trips = 0;
    const char* kill_reason       = nullptr;

    [[nodiscard]] uint64_t count(RiskReject r) const noexcept {
        return by_reason[static_cast<std::size_t>(r)];
    }
    void clear() noexcept { *this = RiskStats{}; }
};

// ---------------------------------------------------------------------------
// Message rate limiter
// ---------------------------------------------------------------------------

// A fixed ring of ten one hundred millisecond buckets, so the window is the
// current partial bucket plus the nine whole ones behind it.
//
// THE TRADEOFF VERSUS AN EXACT SLIDING WINDOW. An exact window has to remember
// when each message was sent, which is a ring of up to max_messages_per_second
// timestamps, a pointer chase to expire the old ones, and an amount of memory
// that grows with the limit. The bucket ring is ten words, the work per check
// is a fixed ten iteration loop over data in one cache line, and there is no
// allocation and no timestamp list at all.
//
// The cost is resolution. The measured span is somewhere between nine hundred
// milliseconds and one full second depending where in the current bucket the
// call lands, so the limiter is marginally conservative and never lets more
// than the limit through in any one second. A message can also be counted for
// up to one hundred milliseconds longer than an exact window would count it.
// For a safety fuse that is the right direction to be wrong in. If this were a
// billing meter rather than a fuse the answer would be different.
//
// Each slot stores the absolute bucket index it holds, so a slot from an older
// window is recognised and ignored rather than needing to be cleared on a
// timer. That is what lets the count be computed by a const function, which is
// what lets check() stay pure.
class RateLimiter {
public:
    static constexpr std::size_t kBuckets  = 10;
    static constexpr uint64_t    kBucketNs = 100'000'000ull;
    static constexpr uint64_t    kWindowNs = kBuckets * kBucketNs;

    // Messages counted in the window ending at now. Const, no side effects.
    [[nodiscard]] uint32_t count_at(uint64_t now_ns) const noexcept {
        const uint64_t b = now_ns / kBucketNs;
        uint32_t       n = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            // Live when the slot holds a bucket index inside the window. The
            // addition rather than a subtraction avoids underflow early in the
            // session when b is smaller than kBuckets.
            if (index_[i] <= b && index_[i] + kBuckets > b) n += count_[i];
        }
        return n;
    }

    void record(uint64_t now_ns) noexcept {
        const uint64_t    b    = now_ns / kBucketNs;
        const std::size_t slot = static_cast<std::size_t>(b % kBuckets);
        if (index_[slot] != b) {
            index_[slot] = b;
            count_[slot] = 0;
        }
        ++count_[slot];
    }

    void clear() noexcept {
        index_.fill(0);
        count_.fill(0);
        // Bucket index zero is a real index, so a freshly cleared limiter would
        // otherwise claim ten live empty buckets. They are empty, so the count
        // is still zero and this is harmless, but the fill keeps it obvious.
    }

private:
    std::array<uint64_t, kBuckets> index_{};
    std::array<uint32_t, kBuckets> count_{};
};

// ---------------------------------------------------------------------------
// Per symbol state
// ---------------------------------------------------------------------------

struct SymbolRisk {
    // Filled position, signed, positive is long. Shares, not lots.
    int64_t position = 0;

    // Shares on orders that are working at the exchange but have not filled.
    // Kept per side because a working buy and a working sell push the position
    // in opposite directions and the worst case needs both.
    int64_t working_buy_shares  = 0;
    int64_t working_sell_shares = 0;

    uint32_t working_buy_orders  = 0;
    uint32_t working_sell_orders = 0;

    // The most aggressive price on each side among our own working orders, used
    // by the self cross check.
    //
    // These are tightened when an order is added and reset only when that side
    // empties. Restoring the true extreme after a cancel would need a per
    // symbol price heap, which is neither O(1) nor allocation free. So the
    // stored extreme can be more aggressive than the real one for a while,
    // which means the check can reject an order that would not actually have
    // crossed. It can never miss one that would. Conservative in the safe
    // direction, and cheap, which is the trade.
    nano::Price best_working_buy  = 0;  // highest price we are bidding
    nano::Price best_working_sell = 0;  // lowest price we are offering

    bool enabled = false;  // trading switched on for this symbol
    bool halted  = false;  // the feed says halted
};

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

class RiskEngine {
public:
    // Stock locate is a uint16 on the wire, so an array of this size can be
    // indexed by any locate the feed can produce with no bounds check and no
    // branch. Allocated once in the constructor and never again.
    static constexpr std::size_t kLocateCapacity = 1u << 16;

    explicit RiskEngine(const RiskLimits& limits = {})
        : limits_(limits), symbols_(kLocateCapacity) {
        size_open_order_table();
    }

    // -------------------------------------------------------------------
    // The check
    // -------------------------------------------------------------------

    // Pure. Reads the request and this engine's state and touches nothing.
    // Returns the first reason the order fails, or None when it may be sent.
    //
    // Order of the checks. The kill switch is first because it is one flag and
    // because once it is set nothing else matters. Then the single load and
    // compare checks on state that is already hot. Then the arithmetic. The
    // token probe is last because the open order table is the only thing here
    // that can miss cache, and there is no reason to pay for that miss on an
    // order that was going to be rejected for its size anyway.
    [[nodiscard]] RiskReject check(const OrderRequest& r) const noexcept {
        // 1. Kill switch. One flag, one branch, first.
        if (tripped_) return RiskReject::KillSwitch;

        // 2. Per symbol enablement. One load from the flat array, no bounds
        //    check because locate is a uint16 and the array covers every value.
        const SymbolRisk& s = symbols_[r.locate];
        if (!s.enabled) return RiskReject::SymbolNotEnabled;
        if (s.halted)   return RiskReject::SymbolHalted;

        // 3. Shape of this one order. Cheap, and catches the fat fingered
        //    quantity before anything more expensive runs.
        if (r.shares == 0 || r.shares > limits_.max_order_shares) {
            return RiskReject::MaxOrderSize;
        }
        if (r.price <= 0) return RiskReject::FatFingerPrice;

        const uint64_t notional =
            static_cast<uint64_t>(r.shares) * static_cast<uint64_t>(r.price);
        if (notional > limits_.max_order_notional) return RiskReject::MaxOrderNotional;

        // 4. Market data freshness. Checked before the fat finger check because
        //    the fat finger reference comes out of the same book, and a
        //    reference from a stale book is worse than no reference. A book
        //    that has never updated has a timestamp of zero and is rejected,
        //    which is the fail closed case at startup.
        if (r.book_ts_ns == 0) return RiskReject::StaleMarketData;
        if (r.now_ns < r.book_ts_ns) return RiskReject::StaleMarketData;  // clock went backwards
        if (r.now_ns - r.book_ts_ns > limits_.max_market_data_age_ns) {
            return RiskReject::StaleMarketData;
        }

        // 5. Fat finger.
        //
        //    The reference is the size weighted midpoint of our own top of
        //    book. It needs both sides. WHEN THERE IS NO TWO SIDED MARKET THE
        //    CHECK CANNOT BE EVALUATED AND THE ORDER IS REJECTED. It is not
        //    skipped and the order is not passed.
        //
        //    That is the whole point of the check. A one sided book is not a
        //    quiet moment, it is a moment when the market is moving or the feed
        //    is broken, and a mistyped price is at its most expensive exactly
        //    then. Passing an unchecked order because the check was unavailable
        //    converts a safety control into a control that switches itself off
        //    when it is needed. Fail closed.
        if (!r.book.two_sided()) return RiskReject::FatFingerPrice;
        const nano::Price ref = r.book.micro_price();
        if (ref <= 0) return RiskReject::FatFingerPrice;

        const uint64_t away  = static_cast<uint64_t>(r.price > ref ? r.price - ref
                                                                   : ref - r.price);
        // Basis points, integer throughout. The multiply is bounded because
        // price and reference are both ten-thousandths of a dollar in a uint32
        // range, so away times ten thousand cannot overflow a uint64.
        if (away * 10000ull > static_cast<uint64_t>(ref) * limits_.fat_finger_bps) {
            return RiskReject::FatFingerPrice;
        }

        // 6. Position, counting working exposure.
        //
        //    THE DIFFERENCE THAT MATTERS. A limit checked against filled
        //    position alone asks how much this process owns right now. A limit
        //    checked against filled plus working exposure asks how much it
        //    could own if every order it has outstanding were filled in the
        //    next instant.
        //
        //    The first one is useless against the failure it exists to catch.
        //    Nothing has filled yet at the moment the orders go out, so a
        //    strategy in a loop can send the limit, and again, and again, and
        //    every one of them passes because the position is still flat. When
        //    they fill the position is a multiple of the limit and the limit
        //    never once said no.
        //
        //    So the conservative form is implemented. The worst case long is
        //    the current position plus every working buy plus this order if it
        //    is a buy. The worst case short is the position minus every working
        //    sell minus this order if it is a sell. Both are checked, because
        //    an order that reduces the position in one direction still cannot
        //    be allowed to overshoot into the other.
        const int64_t incoming = static_cast<int64_t>(r.shares);
        const int64_t buy_add  = r.is_buy() ? incoming : 0;
        const int64_t sell_add = r.is_buy() ? 0 : incoming;

        const int64_t worst_long  = s.position + s.working_buy_shares + buy_add;
        const int64_t worst_short = s.position - s.working_sell_shares - sell_add;

        if (worst_long > limits_.max_position_shares) return RiskReject::PositionLimit;
        if (worst_short < -limits_.max_position_shares) return RiskReject::PositionLimit;

        // 7. Symbol notional.
        //
        //    The same worst case shares, valued at the price this order is
        //    about to be sent at. Using the order's own price rather than an
        //    invented mark keeps this exact and O(1), and it means the answer
        //    is the plain statement "the most money this symbol can owe me if
        //    everything working fills, priced at what I am about to pay."
        const uint64_t worst_shares =
            static_cast<uint64_t>(abs64(worst_long) > abs64(worst_short) ? abs64(worst_long)
                                                                         : abs64(worst_short));
        if (worst_shares * static_cast<uint64_t>(r.price) > limits_.max_symbol_notional) {
            return RiskReject::NotionalLimit;
        }

        // 8. Self cross. A buy priced at or above our own best working offer
        //    would trade with us. Both sides of the same firm crossing is at
        //    best a wash trade and at worst a regulatory problem, and it is
        //    always a sign that two parts of the strategy disagree.
        if (r.is_buy()) {
            if (s.working_sell_orders > 0 && r.price >= s.best_working_sell) {
                return RiskReject::SelfCross;
            }
        } else {
            if (s.working_buy_orders > 0 && r.price <= s.best_working_buy) {
                return RiskReject::SelfCross;
            }
        }

        // 9. Engine wide open order count.
        if (open_orders_ >= limits_.max_open_orders) return RiskReject::MaxOpenOrders;

        // 10. Message rate. A fixed ten iteration loop over one cache line.
        if (rate_.count_at(r.now_ns) >= limits_.max_messages_per_second) {
            return RiskReject::MessageRate;
        }

        // 11. Duplicate token. Last, because this is the only probe that can
        //     miss cache.
        //
        //     A repeated token is the nastiest failure in OUCH, because the
        //     specification says an Enter Order with a previously used token is
        //     silently ignored. No reject comes back. The order simply never
        //     exists and the strategy waits forever for an acknowledgement.
        //
        //     This catches a token that is currently working. Uniqueness across
        //     the whole day, including tokens that have already retired, is the
        //     TokenGenerator's job, because a monotone counter of fixed width
        //     cannot repeat until it exhausts that width and it counts it when
        //     it does. Holding every token used today would be an unbounded set
        //     on the hot path, which is the thing this file does not do.
        if (find_open(r.token) != kNoSlot) return RiskReject::DuplicateToken;

        return RiskReject::None;
    }

    // The one the sending path calls. Runs the same check and records the
    // outcome. check() is left pure so it can be called without moving
    // anything, and the counting lives here rather than behind a mutable
    // member, because a comment claiming purity next to a mutable counter is
    // the kind of thing that makes a reader stop trusting the rest of the file.
    [[nodiscard]] RiskReject check_and_count(const OrderRequest& r) noexcept {
        const RiskReject reason = check(r);
        ++stats_.checked;
        ++stats_.by_reason[static_cast<std::size_t>(reason)];
        if (reason == RiskReject::None) {
            ++stats_.passed;
        } else {
            ++stats_.rejected;
        }
        return reason;
    }

    // -------------------------------------------------------------------
    // State updates
    // -------------------------------------------------------------------

    // Called once the order has actually been written to the wire, never
    // before. Until this runs the next check() will not see this order's
    // exposure, which is the bug that makes a risk engine ornamental, so the
    // send path must call it on every accepted order.
    void on_order_sent(const OrderRequest& r) noexcept {
        SymbolRisk& s = symbols_[r.locate];

        const std::size_t slot = insert_open(r);
        if (slot == kNoSlot) {
            // The table is sized so this cannot happen, because max open orders
            // is checked first and the table holds four times that. If it ever
            // does the engine has lost track of what is working, so it stops.
            ++stats_.table_insert_failed;
            trip_kill_switch("open order table full");
            return;
        }

        if (r.is_buy()) {
            s.working_buy_shares += static_cast<int64_t>(r.shares);
            if (s.working_buy_orders == 0 || r.price > s.best_working_buy) {
                s.best_working_buy = r.price;
            }
            ++s.working_buy_orders;
        } else {
            s.working_sell_shares += static_cast<int64_t>(r.shares);
            if (s.working_sell_orders == 0 || r.price < s.best_working_sell) {
                s.best_working_sell = r.price;
            }
            ++s.working_sell_orders;
        }

        ++open_orders_;
        rate_.record(r.now_ns);

        ++stats_.orders_sent;
        stats_.open_orders = open_orders_;
        if (open_orders_ > stats_.open_orders_peak) stats_.open_orders_peak = open_orders_;
    }

    // An Executed message came back. Shares are incremental, as OUCH sends
    // them. Working exposure comes down and filled position goes up by the same
    // amount, so the worst case the next check sees is unchanged by a fill,
    // which is exactly right. A fill is not new risk, it is risk becoming
    // certain.
    void on_fill(const ouch::Token& token, uint32_t shares, nano::Price price) noexcept {
        const std::size_t slot = find_open(token);
        if (slot == kNoSlot) {
            // A fill for a token this engine does not have working. The
            // position is real whether or not the bookkeeping expected it, so
            // this is counted loudly rather than dropped, but there is no
            // symbol to attribute it to and nothing safe to do with it.
            ++stats_.unknown_token_events;
            return;
        }
        OpenOrder&  o = slots_[slot];
        SymbolRisk& s = symbols_[o.locate];

        uint32_t n = shares;
        if (n > o.leaves) {
            ++stats_.overfills;
            n = o.leaves;
        }
        o.leaves -= n;

        const int64_t signed_n = static_cast<int64_t>(n);
        if (o.is_buy) {
            s.working_buy_shares -= signed_n;
            s.position           += signed_n;
        } else {
            s.working_sell_shares -= signed_n;
            s.position            -= signed_n;
        }
        // A share based position does not need the fill price, but throwing it
        // away means the engine cannot say what it traded, so it is totalled.
        stats_.filled_notional +=
            static_cast<uint64_t>(n) * static_cast<uint64_t>(price > 0 ? price : 0);

        ++stats_.fills;
        if (o.leaves == 0) retire(slot);
    }

    // A Canceled message came back. Decrement Shares is incremental and a
    // cancel does not have to be terminal, so the order only leaves the table
    // once nothing is working. Passing zero means the whole balance went.
    void on_cancel_acked(const ouch::Token& token, uint32_t decrement_shares) noexcept {
        const std::size_t slot = find_open(token);
        if (slot == kNoSlot) {
            ++stats_.unknown_token_events;
            return;
        }
        OpenOrder&  o = slots_[slot];
        SymbolRisk& s = symbols_[o.locate];

        uint32_t n = (decrement_shares == 0) ? o.leaves : decrement_shares;
        if (n > o.leaves) {
            ++stats_.overfills;
            n = o.leaves;
        }
        o.leaves -= n;

        const int64_t signed_n = static_cast<int64_t>(n);
        if (o.is_buy) {
            s.working_buy_shares -= signed_n;
        } else {
            s.working_sell_shares -= signed_n;
        }

        ++stats_.cancels_acked;
        if (o.leaves == 0) retire(slot);
    }

    // A Rejected message came back. The order never existed at the exchange, so
    // all of its working exposure goes away at once.
    void on_reject(const ouch::Token& token) noexcept {
        const std::size_t slot = find_open(token);
        if (slot == kNoSlot) {
            ++stats_.unknown_token_events;
            return;
        }
        OpenOrder&  o = slots_[slot];
        SymbolRisk& s = symbols_[o.locate];

        const int64_t signed_n = static_cast<int64_t>(o.leaves);
        if (o.is_buy) {
            s.working_buy_shares -= signed_n;
        } else {
            s.working_sell_shares -= signed_n;
        }
        o.leaves = 0;

        ++stats_.exchange_rejects;
        retire(slot);
    }

    // -------------------------------------------------------------------
    // Kill switch
    // -------------------------------------------------------------------

    // One way. Once tripped, check() returns KillSwitch for everything and
    // nothing goes out. There is no timeout, no automatic retry, and no path
    // by which a later event clears it.
    //
    // The reset is a separate, differently named call so it cannot be reached
    // by accident. Nothing inside this class calls it. A kill switch that any
    // code path can clear is a warning light, not a kill switch, and the whole
    // value of the thing is that a human has to look at the state of the world
    // and decide.
    //
    // The first reason is kept rather than the last, because the first one is
    // what actually went wrong and everything after it is consequence.
    void trip_kill_switch(const char* why) noexcept {
        if (!tripped_) {
            tripped_             = true;
            stats_.kill_reason   = why;
        }
        ++stats_.kill_switch_trips;
    }

    [[nodiscard]] bool tripped() const noexcept { return tripped_; }

    // The deliberate, explicit, human action. Named so it reads as one at the
    // call site and so it cannot be confused with anything routine.
    void reset_kill_switch_after_human_review() noexcept {
        tripped_           = false;
        stats_.kill_reason = nullptr;
    }

    // -------------------------------------------------------------------
    // Configuration and inspection
    // -------------------------------------------------------------------

    void enable_symbol(uint16_t locate, bool on) noexcept { symbols_[locate].enabled = on; }
    void set_halted(uint16_t locate, bool halted) noexcept { symbols_[locate].halted = halted; }

    // The feed's trading action states. 'T' is trading, everything else that
    // the ITCH trading action message carries means not trading. Taking the
    // feed's word for it is the point, so this is a thin translation and not a
    // policy.
    void on_trading_action(uint16_t locate, char state) noexcept {
        symbols_[locate].halted = (state != 'T');
    }

    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

    // Changing limits resizes the open order table, which reallocates, so it is
    // refused while anything is working. Limits are set at start of day or
    // deliberately by a human mid session with nothing outstanding, never on
    // the order path. Returns false when the call was refused.
    [[nodiscard]] bool set_limits(const RiskLimits& l) {
        if (open_orders_ != 0) return false;
        limits_ = l;
        size_open_order_table();
        return true;
    }

    [[nodiscard]] const RiskStats&  stats() const noexcept { return stats_; }
    [[nodiscard]] const SymbolRisk& symbol(uint16_t locate) const noexcept {
        return symbols_[locate];
    }
    [[nodiscard]] uint32_t open_orders() const noexcept { return open_orders_; }

    // For tests and for a start of day position load from an external system.
    void set_position(uint16_t locate, int64_t shares) noexcept {
        symbols_[locate].position = shares;
    }

private:
    // -------------------------------------------------------------------
    // The open order table
    // -------------------------------------------------------------------

    struct OpenOrder {
        ouch::Token token{};
        uint32_t    leaves   = 0;
        uint16_t    locate   = 0;
        bool        is_buy   = false;
        bool        occupied = false;
    };

    static constexpr std::size_t kNoSlot = ~std::size_t{0};

    // Open addressing with linear probing, fixed capacity, allocated once.
    //
    // Deletion uses backward shift rather than tombstones. Tombstones are
    // simpler but they never go away, so over a day of millions of orders a
    // table that is only ever a quarter full still degrades to long probe
    // chains made entirely of dead entries. Backward shift keeps the invariant
    // that a probe ends at the first empty slot, so the table performs the same
    // at the close as it did at the open. This is Knuth's algorithm R.
    void size_open_order_table() {
        // Four times the open order ceiling, rounded up to a power of two, so
        // the table is never more than a quarter full. Load factor is what
        // decides probe length, and a quarter keeps the expected probe at very
        // nearly one. The minimum is there so a zero limit still builds a table
        // rather than a zero length one that every probe would divide by.
        std::size_t want = static_cast<std::size_t>(limits_.max_open_orders) * 4;
        std::size_t cap  = 64;
        while (cap < want) cap <<= 1;
        slots_.assign(cap, OpenOrder{});
        mask_ = cap - 1;
    }

    // splitmix64's finalising constants over two overlapping eight byte loads,
    // which covers all fourteen token bytes. The constants are splitmix64's
    // published ones, not chosen here.
    [[nodiscard]] static std::size_t hash_token(const ouch::Token& t) noexcept {
        uint64_t a = 0, b = 0;
        std::memcpy(&a, t.b.data(), 8);
        std::memcpy(&b, t.b.data() + (ouch::kTokenLen - 8), 8);
        uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull);
        h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27; h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }

    [[nodiscard]] std::size_t home(const ouch::Token& t) const noexcept {
        return hash_token(t) & mask_;
    }

    [[nodiscard]] std::size_t find_open(const ouch::Token& t) const noexcept {
        std::size_t i = home(t);
        // Terminates because the table is never full, so there is always an
        // empty slot to stop at.
        while (slots_[i].occupied) {
            if (slots_[i].token == t) return i;
            i = (i + 1) & mask_;
        }
        return kNoSlot;
    }

    [[nodiscard]] std::size_t insert_open(const OrderRequest& r) noexcept {
        std::size_t i = home(r.token);
        std::size_t probes = 0;
        while (slots_[i].occupied) {
            if (slots_[i].token == r.token) return kNoSlot;  // already working
            i = (i + 1) & mask_;
            if (++probes > mask_) return kNoSlot;            // table full
        }
        slots_[i].token    = r.token;
        slots_[i].leaves   = r.shares;
        slots_[i].locate   = r.locate;
        slots_[i].is_buy   = r.is_buy();
        slots_[i].occupied = true;
        return i;
    }

    // Knuth algorithm R. Walk forward from the hole and pull back any entry
    // whose home slot is at or before the hole, so no probe chain is ever
    // broken and no tombstone is needed.
    void erase_at(std::size_t i) noexcept {
        for (;;) {
            slots_[i].occupied = false;
            std::size_t j = i;
            for (;;) {
                j = (j + 1) & mask_;
                if (!slots_[j].occupied) return;
                const std::size_t k = home(slots_[j].token);
                const bool movable =
                    (j >= i) ? (k <= i || k > j) : (k <= i && k > j);
                if (movable) break;
            }
            slots_[i] = slots_[j];
            i = j;
        }
    }

    void retire(std::size_t slot) noexcept {
        const bool     was_buy = slots_[slot].is_buy;
        const uint16_t loc     = slots_[slot].locate;

        erase_at(slot);
        if (open_orders_ > 0) --open_orders_;
        stats_.open_orders = open_orders_;

        // The best working price on a side is only meaningful while that side
        // has orders. Once it empties, clear it, which is the one point at
        // which the conservative extreme described in SymbolRisk becomes exact
        // again.
        SymbolRisk& sym = symbols_[loc];
        if (was_buy) {
            if (sym.working_buy_orders > 0) --sym.working_buy_orders;
            if (sym.working_buy_orders == 0) {
                sym.best_working_buy = 0;
                // No orders working means no shares outstanding. If the share
                // count disagrees the two tallies have drifted, which is a
                // bookkeeping fault worth surfacing rather than papering over,
                // so it is counted before the invariant is restored.
                if (sym.working_buy_shares != 0) ++stats_.working_share_drift;
                sym.working_buy_shares = 0;
            }
        } else {
            if (sym.working_sell_orders > 0) --sym.working_sell_orders;
            if (sym.working_sell_orders == 0) {
                sym.best_working_sell = 0;
                if (sym.working_sell_shares != 0) ++stats_.working_share_drift;
                sym.working_sell_shares = 0;
            }
        }
    }

    [[nodiscard]] static int64_t abs64(int64_t v) noexcept { return v < 0 ? -v : v; }

    RiskLimits              limits_{};
    std::vector<SymbolRisk> symbols_;
    std::vector<OpenOrder>  slots_;
    std::size_t             mask_        = 0;
    uint32_t                open_orders_ = 0;
    bool                    tripped_     = false;
    RateLimiter             rate_{};
    RiskStats               stats_{};
};

} // namespace tick
