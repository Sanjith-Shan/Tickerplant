#pragma once

#include "tick/book_builder.hpp"

#include <cstdint>

// A quoting rule, for the purpose of having something at the end of the wire.
//
// Read this paragraph before anything else in the file. This is not a trading
// strategy and no claim is made that it makes money. It exists so that the tick
// to trade path has a decision at the end of it, because measuring latency from
// a packet arriving to an order leaving is meaningless if nothing ever decides
// to send an order. Everything interesting in this project is upstream of here.
// Saying that plainly is the point, and an interviewer who hears a student
// claim a profitable market making strategy stops listening.
//
// What it does. It computes a fair value from the top of book, shifts that fair
// value against the inventory it is already holding, quotes a fixed size either
// side, and refuses to requote unless something moved enough to be worth the
// message. Every one of those four is a real thing market makers do, in its
// simplest possible form.
//
// The three decisions worth defending.
//
// Fair value is the micro price rather than the mid. The mid ignores size, and
// when the bid is ten times the ask the next trade is far more likely to happen
// at the ask. The micro price is the size weighted midpoint and it costs two
// multiplies.
//
// Inventory skew is linear in the position. Holding a long position means the
// next fill you want is a sell, so both quotes move down, which makes your bid
// less likely to be hit and your ask more likely to be lifted. Without this a
// quoting rule accumulates a position until it is only a bet on direction.
//
// Requoting is hysteretic. Cancelling and replacing a quote for a one tick move
// costs two messages and loses queue position, and queue position at a price is
// worth more than the tick in most books. So a quote moves when the fair value
// has moved past a threshold, and never more often than a configured interval.

namespace tick {

using nano::Price;
using nano::Quantity;

struct QuoteConfig {
    // The smallest price increment the venue allows. NASDAQ quotes stocks above
    // one dollar in pennies, and the feed works in ten-thousandths, so a penny
    // is one hundred here. Quotes are rounded to this grid, outward, so a
    // rounding error can never make the quote more aggressive than intended.
    Price tick_size = 100;

    // How far either side of fair value to quote, in ticks.
    int64_t half_spread_ticks = 1;

    Quantity quote_size = 100;

    // How far to shift fair value per share of inventory, in ten-thousandths of
    // a dollar. Small by construction. The right value is a function of how
    // much risk the position represents and this one is arbitrary, which is
    // stated rather than dressed up.
    int64_t skew_per_share = 2;

    // Stop quoting the side that would make the position worse once it reaches
    // this size. The other side keeps quoting, which is how the position comes
    // back in.
    int64_t max_inventory = 1000;

    // Hysteresis. Requote when fair value has moved at least this many ticks,
    // and never more often than this interval.
    int64_t  requote_threshold_ticks = 1;
    uint64_t min_requote_interval_ns = 1'000'000; // one millisecond

    // Do not quote into a market whose spread is already wider than this, which
    // usually means something is happening that a rule this simple has no view
    // on.
    int64_t max_quotable_spread_ticks = 50;
};

struct Quote {
    bool     has_bid  = false;
    bool     has_ask  = false;
    Price    bid      = 0;
    Price    ask      = 0;
    Quantity bid_qty  = 0;
    Quantity ask_qty  = 0;

    [[nodiscard]] bool operator==(const Quote& o) const noexcept {
        return has_bid == o.has_bid && has_ask == o.has_ask && bid == o.bid &&
               ask == o.ask && bid_qty == o.bid_qty && ask_qty == o.ask_qty;
    }
};

// Why the quoter did or did not act. Counted rather than logged, because a log
// line on the hot path is a syscall.
enum class QuoteDecision : uint8_t {
    Unchanged,      // nothing moved enough to be worth a message
    Requote,        // the quote changed and should be sent
    Pull,           // there should be no quote at all now
    NoMarket,       // the book is not two sided
    SpreadTooWide,
    Halted,
    TooSoon,        // the minimum requote interval has not elapsed
};

struct QuoterStats {
    uint64_t updates      = 0;
    uint64_t requotes     = 0;
    uint64_t pulls        = 0;
    uint64_t unchanged    = 0;
    uint64_t no_market    = 0;
    uint64_t spread_wide  = 0;
    uint64_t too_soon     = 0;
    uint64_t halted       = 0;
};

class MicroPriceQuoter {
public:
    explicit MicroPriceQuoter(QuoteConfig cfg = {}) : cfg_(cfg) {}

    // Called on every top of book change for the one symbol this quoter trades.
    // Pure in the sense that it does not send anything. It returns what the
    // quote should be and the caller decides what to do about it, which keeps
    // the risk checks and the wire out of this file entirely.
    QuoteDecision on_top_of_book(const TopOfBook& top, int64_t position,
                                 uint64_t now_ns, bool halted, Quote& out) noexcept {
        ++stats_.updates;

        if (halted) {
            ++stats_.halted;
            out = Quote{};
            return pull_if_quoting(QuoteDecision::Halted);
        }
        if (!top.two_sided()) {
            // No fair value, so no quote. A quoting rule that keeps its last
            // quote alive when the market goes one sided is quoting against a
            // price that no longer exists.
            ++stats_.no_market;
            out = Quote{};
            return pull_if_quoting(QuoteDecision::NoMarket);
        }
        if (top.spread() > cfg_.max_quotable_spread_ticks * cfg_.tick_size) {
            ++stats_.spread_wide;
            out = Quote{};
            return pull_if_quoting(QuoteDecision::SpreadTooWide);
        }

        const Price fair = top.micro_price() - position * cfg_.skew_per_share;

        Quote q;
        // Round outward onto the tick grid. Rounding the bid down and the ask
        // up can only ever make the quote less aggressive, which is the safe
        // direction for a rounding error to go.
        q.bid = floor_to_tick(fair - cfg_.half_spread_ticks * cfg_.tick_size);
        q.ask = ceil_to_tick(fair + cfg_.half_spread_ticks * cfg_.tick_size);
        q.bid_qty = cfg_.quote_size;
        q.ask_qty = cfg_.quote_size;
        q.has_bid = position < cfg_.max_inventory;
        q.has_ask = position > -cfg_.max_inventory;

        // A quote that crosses the book would take liquidity rather than make
        // it, which is not what this rule is for. Back off to joining instead.
        if (q.has_bid && q.bid >= top.ask_price) q.bid = floor_to_tick(top.ask_price - cfg_.tick_size);
        if (q.has_ask && q.ask <= top.bid_price) q.ask = ceil_to_tick(top.bid_price + cfg_.tick_size);

        if (!q.has_bid && !q.has_ask) {
            out = Quote{};
            return pull_if_quoting(QuoteDecision::Pull);
        }

        if (!worth_requoting(q, now_ns)) {
            out = live_;
            ++stats_.unchanged;
            return last_reason_;
        }

        live_         = q;
        quoting_      = true;
        last_quote_ns_ = now_ns;
        out           = q;
        ++stats_.requotes;
        return QuoteDecision::Requote;
    }

    // The fill path. The caller tells the quoter what actually traded so the
    // inventory skew on the next update reflects reality rather than intent.
    void on_fill(nano::Side side, Quantity qty) noexcept {
        position_ += (side == nano::Side::Buy) ? static_cast<int64_t>(qty)
                                               : -static_cast<int64_t>(qty);
        ++fills_;
    }

    [[nodiscard]] int64_t  position()  const noexcept { return position_; }
    [[nodiscard]] uint64_t fills()     const noexcept { return fills_; }
    [[nodiscard]] const Quote& live()  const noexcept { return live_; }
    [[nodiscard]] bool     quoting()   const noexcept { return quoting_; }
    [[nodiscard]] const QuoterStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const QuoteConfig& config() const noexcept { return cfg_; }

    void reset() noexcept {
        live_    = Quote{};
        quoting_ = false;
        position_ = 0;
    }

private:
    [[nodiscard]] Price floor_to_tick(Price p) const noexcept {
        if (cfg_.tick_size <= 1) return p;
        return (p / cfg_.tick_size) * cfg_.tick_size;
    }
    [[nodiscard]] Price ceil_to_tick(Price p) const noexcept {
        if (cfg_.tick_size <= 1) return p;
        return ((p + cfg_.tick_size - 1) / cfg_.tick_size) * cfg_.tick_size;
    }

    QuoteDecision pull_if_quoting(QuoteDecision reason) noexcept {
        last_reason_ = reason;
        if (quoting_) {
            quoting_ = false;
            live_    = Quote{};
            ++stats_.pulls;
            return QuoteDecision::Pull;
        }
        return reason;
    }

    // The hysteresis. Two gates, and both have to open.
    [[nodiscard]] bool worth_requoting(const Quote& q, uint64_t now_ns) noexcept {
        if (!quoting_) return true;
        if (now_ns - last_quote_ns_ < cfg_.min_requote_interval_ns) {
            ++stats_.too_soon;
            last_reason_ = QuoteDecision::TooSoon;
            return false;
        }
        if (q.has_bid != live_.has_bid || q.has_ask != live_.has_ask) return true;

        const Price threshold = cfg_.requote_threshold_ticks * cfg_.tick_size;
        const Price dbid = q.bid > live_.bid ? q.bid - live_.bid : live_.bid - q.bid;
        const Price dask = q.ask > live_.ask ? q.ask - live_.ask : live_.ask - q.ask;
        if (dbid >= threshold || dask >= threshold) return true;

        last_reason_ = QuoteDecision::Unchanged;
        return false;
    }

    QuoteConfig   cfg_;
    Quote         live_{};
    bool          quoting_       = false;
    uint64_t      last_quote_ns_ = 0;
    int64_t       position_      = 0;
    uint64_t      fills_         = 0;
    QuoteDecision last_reason_   = QuoteDecision::Unchanged;
    QuoterStats   stats_{};
};

} // namespace tick
