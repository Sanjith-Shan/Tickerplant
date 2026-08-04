#pragma once

#include "nano/memory_pool.hpp"
#include "nano/order.hpp"
#include "nano/price_level.hpp"
#include "nano/types.hpp"

#include "tick/book_side.hpp"
#include "tick/flat_order_map.hpp"
#include "tick/pool_resource.hpp"
#include "tick/symbol_table.hpp"

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

// Rebuilding the book from the feed.
//
// The single most important thing in this file is what it does not do. A feed
// handler does not match. It applies.
//
// NanoExchange's MatchingEngine decides which orders trade. A book built from
// ITCH must not, because the exchange has already decided and the feed is
// telling you what it decided. Running an E message through a matching engine
// would execute against whatever is at the top of the local book, which is not
// necessarily the order the exchange filled, and the two books would diverge
// within seconds and never converge again. So BookBuilder exposes handlers that
// apply events and nothing that matches, and it reuses nano::Order,
// nano::PriceLevel, and nano::MemoryPool, which are the parts that were always
// about holding a book rather than about running an auction.
//
// The resting order state machine, which is where the invariants live.
//
//           A/F ──▶ [ RESTING qty=N ]
//                        │
//         E/C/X (partial)│──▶ [ RESTING qty=N-k ]   k < N
//         E/C/X (full)   │──▶ [ GONE ]              k == N
//         D              │──▶ [ GONE ]
//         U              │──▶ [ GONE ] and a new reference is added
//
// Every illegal transition is counted and survived rather than asserted. An
// execution against an order reference that is not resting is a real thing that
// happens after a sequence gap, and the correct behaviour is to count it and
// keep going. Crashing loses the rest of the day, and silently creating the
// order invents liquidity that does not exist.

namespace tick {

using nano::Order;
using nano::Price;
using nano::Quantity;
using nano::Side;

// Sized for the peak number of orders resting at once across all symbols, not
// for the number of adds in a day. A NASDAQ day carries tens of millions of
// adds and a much smaller live set, and the replay tool reports the peak it
// actually saw so this constant can be checked against reality rather than
// guessed at. Override with -DTICK_ORDER_POOL_CAPACITY at configure time.
#ifndef TICK_ORDER_POOL_CAPACITY
#define TICK_ORDER_POOL_CAPACITY 4000000
#endif

inline constexpr std::size_t kDefaultOrderPoolCapacity = TICK_ORDER_POOL_CAPACITY;

// Where the price level nodes come from.
//
// This is a build option rather than a decision baked in, because the three
// answers were measured against each other on the real file and anyone should
// be able to reproduce that rather than take it on trust.
//
//   0  the general purpose heap, reached through a memory resource
//   1  std::pmr::unsynchronized_pool_resource
//   2  tick::PoolResource, a free list per size class
//   3  a plain std::map with no polymorphic allocator at all, the baseline
//
// Option 0 allocates on the hot path, which is what the counting allocator
// found. Option 1 fixes that and is slower than option 0. Option 2 fixes it and
// is faster than both. The numbers are in results/RESULTS.md.
#ifndef TICK_LEVEL_ALLOCATOR
#define TICK_LEVEL_ALLOCATOR 2
#endif

#if TICK_LEVEL_ALLOCATOR == 3
// No polymorphic allocator at all. std::map straight onto the heap, which is
// where this started and what the other options have to beat.
class UnusedLevelMemory {};
using LevelMemory = UnusedLevelMemory;
inline constexpr const char* kLevelAllocatorName = "plain-heap";
#elif TICK_LEVEL_ALLOCATOR == 0
// A resource that simply forwards to the default one, so the book code is the
// same in all three configurations and only the allocation strategy differs.
class HeapLevelMemory final : public std::pmr::memory_resource {
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        return std::pmr::get_default_resource()->allocate(bytes, align);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        std::pmr::get_default_resource()->deallocate(p, bytes, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
};
using LevelMemory = HeapLevelMemory;
inline constexpr const char* kLevelAllocatorName = "heap";
#elif TICK_LEVEL_ALLOCATOR == 1
using LevelMemory = std::pmr::unsynchronized_pool_resource;
inline constexpr const char* kLevelAllocatorName = "pmr-pool";
#else
using LevelMemory = PoolResource;
inline constexpr const char* kLevelAllocatorName = "tick-pool";
#endif

struct TopOfBook {
    Price    bid_price  = 0;
    Quantity bid_qty    = 0;
    uint32_t bid_orders = 0;
    Price    ask_price  = 0;
    Quantity ask_qty    = 0;
    uint32_t ask_orders = 0;

    [[nodiscard]] bool  has_bid() const noexcept { return bid_qty > 0; }
    [[nodiscard]] bool  has_ask() const noexcept { return ask_qty > 0; }
    [[nodiscard]] bool  two_sided() const noexcept { return has_bid() && has_ask(); }
    [[nodiscard]] Price spread() const noexcept {
        return two_sided() ? ask_price - bid_price : 0;
    }

    // The midpoint, in the same ten-thousandths of a dollar the feed uses.
    // Integer arithmetic throughout, so a half tick midpoint rounds down rather
    // than introducing a double into a path that has none.
    [[nodiscard]] Price mid() const noexcept {
        return two_sided() ? (bid_price + ask_price) / 2 : 0;
    }

    // The size weighted midpoint. When the bid is three times the size of the
    // ask, the next trade is more likely to happen near the ask, and this is
    // the cheapest expression of that. Phase two quotes around it.
    [[nodiscard]] Price micro_price() const noexcept {
        if (!two_sided()) return 0;
        const uint64_t bq = bid_qty, aq = ask_qty;
        const uint64_t total = bq + aq;
        if (total == 0) return mid();
        return static_cast<Price>(
            (static_cast<uint64_t>(bid_price) * aq + static_cast<uint64_t>(ask_price) * bq) /
            total);
    }
};

struct BookStats {
    uint64_t messages = 0;

    uint64_t adds      = 0;
    uint64_t adds_mpid = 0;
    uint64_t executes  = 0;
    uint64_t executes_with_price = 0;
    uint64_t cancels   = 0;
    uint64_t deletes   = 0;
    uint64_t replaces  = 0;
    uint64_t trades    = 0;   // P, hidden liquidity, never touches the book
    uint64_t cross_trades = 0;
    uint64_t broken_trades = 0;
    uint64_t directory_entries = 0;
    uint64_t trading_actions = 0;
    uint64_t system_events = 0;
    uint64_t other = 0;

    // Invariant violations. None of these should be non-zero on a clean full
    // day replay from a file. On a wire replay with injected loss they will be,
    // and their size is the measure of what the gap cost.
    uint64_t duplicate_refs   = 0; // an add for a reference already resting
    uint64_t orphan_executes  = 0; // E or C against a reference not resting
    uint64_t orphan_cancels   = 0;
    uint64_t orphan_deletes   = 0;
    uint64_t orphan_replaces  = 0;
    uint64_t overfills        = 0; // executed or cancelled more than was resting
    uint64_t pool_exhausted   = 0;
    uint64_t unknown_side     = 0;

    uint64_t live_orders      = 0;
    uint64_t peak_live_orders = 0;
    uint64_t peak_levels      = 0;

    uint64_t last_timestamp   = 0;
    char     session_state    = '\0';
};

// Per symbol running totals, kept flat and indexed by stock locate for the same
// reason the symbol table is.
struct SymbolTotals {
    uint64_t executed_shares = 0; // E, C printable, P and Q, which is how NASDAQ counts
    uint64_t executed_notional = 0; // in ten-thousandths of a dollar
    uint64_t hidden_shares  = 0;  // P only, liquidity that never showed on the book
    uint64_t cross_shares   = 0;  // Q only, the opening and closing auctions
    uint64_t messages       = 0;
};

template <template <bool> class SideT = MapSide,
          std::size_t PoolCapacity    = kDefaultOrderPoolCapacity>
class BookBuilder {
public:
    using Bids = SideT<true>;
    using Asks = SideT<false>;

    struct Book {
        Bids bids;
        Asks asks;

        Book() = default;
        explicit Book(std::pmr::memory_resource* mr) : bids(mr), asks(mr) {}
    };

    // The order map holds a raw pointer into the pool. It never owns, and the
    // pool outlives it because both are members here.
    using OrderMap = FlatOrderMap<Order*>;

    explicit BookBuilder(std::size_t order_map_capacity = 1u << 22)
        : orders_(order_map_capacity), totals_(SymbolTable::kCapacity) {
        // Every book takes its price level storage from one pooling resource.
        // The feed creates and destroys the same levels continuously, so a
        // level freed a moment ago is reused instead of going back to the
        // heap, and after a short warmup a new level costs a pop off a free
        // list rather than a call into the allocator.
        //
        // This was not the first design. The order objects come from
        // NanoExchange's pool and allocate nothing, which made it easy to
        // assume the book path was allocation free, and the counting allocator
        // showed it was not. A std::map node per new price level is millions of
        // allocator calls on a real day.
        books_.reserve(SymbolTable::kCapacity);
        for (std::size_t i = 0; i < SymbolTable::kCapacity; ++i) {
#if TICK_LEVEL_ALLOCATOR == 3
            books_.emplace_back();
#else
            books_.emplace_back(&level_memory_);
#endif
        }
    }

    // ----- the ItchHandler surface -------------------------------------------------

    void on_system_event(uint64_t ts, char code) noexcept {
        stats_.session_state = code;
        touch(ts);
        ++stats_.system_events;
    }

    void on_stock_directory(uint16_t locate, uint64_t ts, std::string_view ticker,
                            uint32_t round_lot) noexcept {
        symbols_.define(locate, ticker, round_lot);
        touch(ts);
        ++stats_.directory_entries;
    }

    void on_trading_action(uint16_t locate, uint64_t ts, char state) noexcept {
        symbols_.set_trading_state(locate, state);
        touch(ts);
        ++stats_.trading_actions;
    }

    void on_add(uint16_t locate, uint64_t ts, uint64_t ref, char side_char,
                uint32_t shares, uint32_t price, bool has_mpid) noexcept {
        touch(ts);
        ++stats_.adds;
        if (has_mpid) ++stats_.adds_mpid;
        ++totals_[locate].messages;

        Side side;
        if (side_char == 'B') {
            side = Side::Buy;
        } else if (side_char == 'S') {
            side = Side::Sell;
        } else {
            ++stats_.unknown_side;
            return;
        }

        if (orders_.find(ref) != nullptr) {
            // The exchange never reuses a live reference. Seeing one means a
            // message was missed, so the old order is abandoned rather than
            // leaked and the new one takes the reference.
            ++stats_.duplicate_refs;
            remove_order(ref);
        }

        Order* o = pool_.allocate(static_cast<nano::OrderId>(ref), side,
                                  nano::OrderType::Limit, static_cast<Price>(price),
                                  static_cast<Quantity>(shares), ts,
                                  static_cast<nano::SymbolId>(locate));
        if (o == nullptr) {
            // Running out of pool is a configuration error, not a feed error.
            // It is counted rather than thrown so a replay finishes and reports
            // how far it got, which is more useful than a stack trace.
            ++stats_.pool_exhausted;
            return;
        }

        Book& b = books_[locate];
        if (side == Side::Buy) {
            b.bids.level(static_cast<Price>(price)).append(o);
        } else {
            b.asks.level(static_cast<Price>(price)).append(o);
        }
        orders_.insert(ref, o);
        ++stats_.live_orders;
        if (stats_.live_orders > stats_.peak_live_orders) {
            stats_.peak_live_orders = stats_.live_orders;
        }
    }

    // E. Shares trade at the order's display price, so the notional uses the
    // resting price and not a price from the message, which E does not carry.
    void on_execute(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                    uint64_t /*match*/) noexcept {
        touch(ts);
        ++stats_.executes;
        ++totals_[locate].messages;

        Order** slot = orders_.find(ref);
        if (slot == nullptr) {
            ++stats_.orphan_executes;
            return;
        }
        Order* o = *slot;
        record_volume(static_cast<uint16_t>(o->symbol), shares,
                      static_cast<uint64_t>(o->price));
        reduce(ref, o, shares);
    }

    // C. Same as E except the print happened at a different price, and a non
    // printable execution is one leg of something that is reported elsewhere,
    // so counting it here would double count the day's volume.
    void on_execute_price(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                          uint64_t /*match*/, bool printable, uint32_t price) noexcept {
        touch(ts);
        ++stats_.executes;
        ++stats_.executes_with_price;
        ++totals_[locate].messages;

        Order** slot = orders_.find(ref);
        if (slot == nullptr) {
            ++stats_.orphan_executes;
            return;
        }
        Order* o = *slot;
        if (printable) {
            record_volume(static_cast<uint16_t>(o->symbol), shares, price);
        }
        reduce(ref, o, shares);
    }

    // X. A partial cancel. The order stays with less size unless the cancel
    // takes all of it, which the specification allows and which is then the
    // same thing as a delete.
    void on_cancel(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) noexcept {
        touch(ts);
        ++stats_.cancels;
        ++totals_[locate].messages;

        Order** slot = orders_.find(ref);
        if (slot == nullptr) {
            ++stats_.orphan_cancels;
            return;
        }
        reduce(ref, *slot, shares);
    }

    void on_delete(uint16_t locate, uint64_t ts, uint64_t ref) noexcept {
        touch(ts);
        ++stats_.deletes;
        ++totals_[locate].messages;

        if (orders_.find(ref) == nullptr) {
            ++stats_.orphan_deletes;
            return;
        }
        remove_order(ref);
    }

    // U. Delete the old reference and add a new one. The side and the symbol
    // come from the order being replaced, because the message does not carry
    // them, which is exactly why an orphaned replace cannot be recovered from
    // and is counted instead of guessed at.
    void on_replace(uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                    uint32_t shares, uint32_t price) noexcept {
        touch(ts);
        ++stats_.replaces;
        ++totals_[locate].messages;

        Order** slot = orders_.find(old_ref);
        if (slot == nullptr) {
            ++stats_.orphan_replaces;
            return;
        }
        const Side     side = (*slot)->side;
        const uint16_t sym  = static_cast<uint16_t>((*slot)->symbol);
        remove_order(old_ref);

        Order* o = pool_.allocate(static_cast<nano::OrderId>(new_ref), side,
                                  nano::OrderType::Limit, static_cast<Price>(price),
                                  static_cast<Quantity>(shares), ts,
                                  static_cast<nano::SymbolId>(sym));
        if (o == nullptr) {
            ++stats_.pool_exhausted;
            return;
        }
        Book& b = books_[sym];
        if (side == Side::Buy) {
            b.bids.level(static_cast<Price>(price)).append(o);
        } else {
            b.asks.level(static_cast<Price>(price)).append(o);
        }
        orders_.insert(new_ref, o);
        ++stats_.live_orders;
        if (stats_.live_orders > stats_.peak_live_orders) {
            stats_.peak_live_orders = stats_.live_orders;
        }
    }

    // P. A trade against hidden liquidity. It never rested on the visible book,
    // so there is nothing to remove, and the order reference it carries is not
    // one that was ever added. It counts toward volume and nothing else.
    void on_trade(uint16_t locate, uint64_t ts, uint64_t /*ref*/, char /*side*/,
                  uint32_t shares, uint32_t price, uint64_t /*match*/) noexcept {
        touch(ts);
        ++stats_.trades;
        ++totals_[locate].messages;
        totals_[locate].hidden_shares += shares;
        record_volume(locate, shares, price);
    }

    // Q. The opening, closing, and halt auctions. Also does not touch the book.
    void on_cross_trade(uint16_t locate, uint64_t ts, uint32_t shares, uint32_t price,
                        uint64_t /*match*/, char /*cross_type*/) noexcept {
        touch(ts);
        ++stats_.cross_trades;
        ++totals_[locate].messages;
        totals_[locate].cross_shares += shares;
        record_volume(locate, shares, price);
    }

    // B. A trade that was previously reported has been broken. Backing the
    // volume out would need every match number retained for the day, which is
    // hundreds of megabytes to correct a number that moves by a rounding error.
    // These are counted and reported next to the reconciliation instead, so the
    // residual is explained rather than hidden.
    void on_broken_trade(uint16_t /*locate*/, uint64_t ts, uint64_t /*match*/) noexcept {
        touch(ts);
        ++stats_.broken_trades;
    }

    void on_other(char /*type*/, uint16_t /*locate*/, uint64_t ts) noexcept {
        touch(ts);
        ++stats_.other;
    }

    // ----- queries -----------------------------------------------------------------

    [[nodiscard]] TopOfBook top(uint16_t locate) const noexcept {
        TopOfBook t;
        const Book& b = books_[locate];
        if (const nano::PriceLevel* lvl = b.bids.best()) {
            t.bid_price  = lvl->price();
            t.bid_qty    = lvl->total_quantity();
            t.bid_orders = lvl->order_count();
        }
        if (const nano::PriceLevel* lvl = b.asks.best()) {
            t.ask_price  = lvl->price();
            t.ask_qty    = lvl->total_quantity();
            t.ask_orders = lvl->order_count();
        }
        return t;
    }

    struct DepthLevel {
        Price    price  = 0;
        Quantity qty    = 0;
        uint32_t orders = 0;
    };

    // Fill up to depth levels a side, best first. Returns how many were filled.
    std::size_t depth(uint16_t locate, bool bid_side, DepthLevel* out,
                      std::size_t depth) const {
        std::size_t n = 0;
        auto        visit = [&](Price p, const nano::PriceLevel& lvl) {
            if (n >= depth) return false;
            out[n++] = DepthLevel{p, lvl.total_quantity(), lvl.order_count()};
            return true;
        };
        if (bid_side) {
            books_[locate].bids.for_each(visit);
        } else {
            books_[locate].asks.for_each(visit);
        }
        return n;
    }

    // A 64 bit fingerprint of the entire visible book across every symbol.
    //
    // This is the oracle for two claims the project makes. That the zero-copy
    // and copying decoders produce the same book, and that a run recovered from
    // injected packet loss is bit identical to the lossless run. Both reduce to
    // comparing two of these, and a fingerprint is the only way to say "bit
    // identical" about a structure this large without keeping two of them.
    //
    // FNV-1a over a canonical walk. Symbols in locate order, bids best first
    // then asks best first, and price, total quantity, and order count at every
    // level. Order identity within a level is deliberately not included,
    // because the feed does not guarantee it across a recovery and the book is
    // the same book either way.
    [[nodiscard]] uint64_t digest() const {
        uint64_t h = 1469598103934665603ULL;
        auto     mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                h ^= static_cast<uint64_t>((v >> (i * 8)) & 0xFF);
                h *= 1099511628211ULL;
            }
        };
        for (std::size_t locate = 0; locate < books_.size(); ++locate) {
            const Book& b = books_[locate];
            if (b.bids.empty() && b.asks.empty()) continue;
            mix(locate);
            mix(0xB1D);
            b.bids.for_each([&](Price p, const nano::PriceLevel& lvl) {
                mix(static_cast<uint64_t>(p));
                mix(lvl.total_quantity());
                mix(lvl.order_count());
                return true;
            });
            mix(0xA5C);
            b.asks.for_each([&](Price p, const nano::PriceLevel& lvl) {
                mix(static_cast<uint64_t>(p));
                mix(lvl.total_quantity());
                mix(lvl.order_count());
                return true;
            });
        }
        return h;
    }

    // The same idea over the traded volume, which is the number that gets
    // reconciled against NASDAQ's published totals.
    [[nodiscard]] uint64_t volume_digest() const {
        uint64_t h = 1469598103934665603ULL;
        for (std::size_t i = 0; i < totals_.size(); ++i) {
            if (totals_[i].executed_shares == 0) continue;
            h ^= i;
            h *= 1099511628211ULL;
            h ^= totals_[i].executed_shares;
            h *= 1099511628211ULL;
        }
        return h;
    }

    [[nodiscard]] uint64_t total_executed_shares() const noexcept {
        uint64_t sum = 0;
        for (const SymbolTotals& t : totals_) sum += t.executed_shares;
        return sum;
    }

    [[nodiscard]] const BookStats&    stats()   const noexcept { return stats_; }
    [[nodiscard]] const SymbolTable&  symbols() const noexcept { return symbols_; }
    [[nodiscard]] const SymbolTotals& totals(uint16_t locate) const noexcept {
        return totals_[locate];
    }
    [[nodiscard]] const OrderMap& order_map() const noexcept { return orders_; }
    [[nodiscard]] const Book&     book(uint16_t locate) const noexcept {
        return books_[locate];
    }

    // How many price levels are live across every symbol. Walked rather than
    // counted incrementally, so it is for reporting and never for the hot path.
    [[nodiscard]] std::size_t live_levels() const {
        std::size_t n = 0;
        for (const Book& b : books_) n += b.bids.size() + b.asks.size();
        return n;
    }

private:
    void touch(uint64_t ts) noexcept {
        stats_.last_timestamp = ts;
        ++stats_.messages;
    }

    void record_volume(uint16_t locate, uint32_t shares, uint64_t price) noexcept {
        SymbolTotals& t = totals_[locate];
        t.executed_shares += shares;
        t.executed_notional += static_cast<uint64_t>(shares) * price;
    }

    // Take shares off a resting order, removing it when nothing is left.
    void reduce(uint64_t ref, Order* o, uint32_t shares) noexcept {
        if (shares >= o->remaining_qty) {
            // The specification does not allow taking more than is resting, so
            // this is either a gap or a bug, and either way the order is gone.
            if (shares > o->remaining_qty) ++stats_.overfills;
            remove_order(ref);
            return;
        }
        Book&            b   = books_[static_cast<uint16_t>(o->symbol)];
        nano::PriceLevel* lvl = (o->side == Side::Buy) ? b.bids.find(o->price)
                                                       : b.asks.find(o->price);
        o->remaining_qty -= static_cast<Quantity>(shares);
        if (lvl != nullptr) lvl->reduce(static_cast<Quantity>(shares));
    }

    // Unlink from its level, drop the empty level, forget the reference, and
    // give the storage back to the pool. All four, in that order, every time.
    void remove_order(uint64_t ref) noexcept {
        Order** slot = orders_.find(ref);
        if (slot == nullptr) return;
        Order* o = *slot;

        Book& b = books_[static_cast<uint16_t>(o->symbol)];
        if (o->side == Side::Buy) {
            if (nano::PriceLevel* lvl = b.bids.find(o->price)) {
                lvl->remove(o);
                if (lvl->empty()) b.bids.erase(o->price);
            }
        } else {
            if (nano::PriceLevel* lvl = b.asks.find(o->price)) {
                lvl->remove(o);
                if (lvl->empty()) b.asks.erase(o->price);
            }
        }
        orders_.erase(ref);
        pool_.deallocate(o);
        if (stats_.live_orders > 0) --stats_.live_orders;
    }

    // Declared before books_, because every book holds a pointer into it and
    // members are destroyed in reverse declaration order.
#if TICK_LEVEL_ALLOCATOR != 3
    LevelMemory level_memory_;
#endif

    nano::MemoryPool<Order, PoolCapacity> pool_;
    OrderMap                              orders_;
    std::vector<Book>                     books_;
    std::vector<SymbolTotals>             totals_;
    SymbolTable                           symbols_;
    BookStats                             stats_{};
};

} // namespace tick
