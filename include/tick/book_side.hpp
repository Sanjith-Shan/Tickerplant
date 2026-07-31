#pragma once

#include "nano/price_level.hpp"
#include "nano/types.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

// One side of one book, holding nano::PriceLevel objects ordered so the best
// price is first.
//
// Why this is not nano::price_containers.hpp, which already does this.
//
// NanoExchange's containers were written for a matching engine, which only ever
// asks for the best level and for one level by price. A feed handler also has
// to walk the book, for a depth snapshot and for the digest that proves two
// runs produced the same state, and none of the three NanoExchange containers
// can be iterated. The second reason is sharper. The direct addressed array
// container won NanoExchange's own shootout, and it cannot be used here at all,
// because it costs memory proportional to the whole price window and real
// NASDAQ prices run from a few hundred ticks to over a hundred million. A
// window wide enough for the real data is gigabytes per side per symbol.
//
// So the levels are held here and nano::PriceLevel, nano::Order, and
// nano::MemoryPool do the work underneath, which is the reuse that matters. The
// intrusive list threading orders through a level is NanoExchange's, unchanged.
//
// Two implementations are provided for the same reason NanoExchange provided
// three, which is that the benchmark should pick and not the author.
//
// Both take their memory from a polymorphic allocator, and that is not
// decoration. The counting allocator found this. The order objects come from
// NanoExchange's pool and allocate nothing, which made it easy to believe the
// whole book path was allocation free, and it was not. Every new price level
// was a std::map node straight out of the general purpose heap, and on a real
// day that is millions of calls into the allocator on the hot path, which is
// exactly the tail latency this project is about.
//
// A pooling memory resource fixes it because the feed creates and destroys the
// same levels over and over. A level freed a microsecond ago is reused rather
// than returned, so after a short warmup a new level costs a pop off a free
// list. The warmup allocations are real and are reported rather than hidden,
// and the steady state is zero.

// Whether the levels come from a polymorphic allocator at all.
//
// TICK_LEVEL_ALLOCATOR is defined in book_builder.hpp and selects where price
// level nodes come from. Value 3 means a plain std::map on the general purpose
// heap with no polymorphic allocator in the way, which is the baseline the
// other options have to beat, because a memory resource is a virtual call on
// every node allocation and that is not free.
#ifndef TICK_LEVEL_ALLOCATOR
#define TICK_LEVEL_ALLOCATOR 2
#endif

#if TICK_LEVEL_ALLOCATOR == 3
#define TICK_PMR_LEVELS 0
#else
#define TICK_PMR_LEVELS 1
#endif

namespace tick {

using nano::Price;
using nano::PriceLevel;
using nano::Quantity;

// A red black tree keyed by price, ordered best first. Insert, find, and erase
// are all logarithmic, nodes are scattered, and iteration in price order is
// free. This is the safe default.
template <bool IsBid>
class MapSide {
    using Cmp = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;
#if TICK_PMR_LEVELS
    using Map = std::pmr::map<Price, PriceLevel, Cmp>;
#else
    using Map = std::map<Price, PriceLevel, Cmp>;
#endif

public:
    static constexpr const char* kName = IsBid ? "map(bid)" : "map(ask)";
    static constexpr bool        kIsBid = IsBid;

    // Default construction uses the default resource, which is the ordinary
    // heap. That keeps the tests and the benchmarks that hold a side on its own
    // simple. The book builder passes its own pooling resource.
    MapSide() = default;
#if TICK_PMR_LEVELS
    explicit MapSide(std::pmr::memory_resource* mr) noexcept : levels_(mr) {}
#else
    explicit MapSide(std::pmr::memory_resource*) noexcept {}
#endif

    // Find the level at this price, creating an empty one if it is not there.
    // Not nodiscard, because creating the level is a legitimate reason to call
    // it and the book builder does exactly that.
    PriceLevel& level(Price p) {
        auto it = levels_.find(p);
        if (it == levels_.end()) {
            it = levels_.emplace(p, PriceLevel(p)).first;
        }
        return it->second;
    }

    [[nodiscard]] PriceLevel* find(Price p) noexcept {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }

    void erase(Price p) { levels_.erase(p); }

    [[nodiscard]] const PriceLevel* best() const noexcept {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    [[nodiscard]] bool        empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return levels_.size(); }
    void clear() { levels_.clear(); }

    // Visit levels best first. Stops early when f returns false.
    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [price, lvl] : levels_) {
            if (!f(price, lvl)) return;
        }
    }

private:
    Map levels_;
};

// A sorted contiguous vector, best price at the front. Find is a binary search,
// insert and erase shift the tail. It wins when the number of live levels is
// small, because the whole side fits in a few cache lines and the best price is
// simply the first element.
//
// The reason it is worth measuring on this workload specifically is that a real
// book is not a few levels deep. A liquid NASDAQ name carries hundreds of live
// price levels, and adds arrive away from the top as often as at it, so the
// shift is not always short. That is a different shape from the matching engine
// benchmark and it is why the answer is not assumed to carry over.
template <bool IsBid>
class VectorSide {
    static constexpr bool before(Price a, Price b) noexcept {
        return IsBid ? a > b : a < b;
    }

public:
    static constexpr const char* kName = IsBid ? "vector(bid)" : "vector(ask)";
    static constexpr bool        kIsBid = IsBid;

    VectorSide() = default;
#if TICK_PMR_LEVELS
    explicit VectorSide(std::pmr::memory_resource* mr) noexcept : levels_(mr) {}
#else
    explicit VectorSide(std::pmr::memory_resource*) noexcept {}
#endif

    PriceLevel& level(Price p) {
        auto it = locate(p);
        if (it != levels_.end() && it->first == p) return it->second;
        it = levels_.insert(it, {p, PriceLevel(p)});
        return it->second;
    }

    [[nodiscard]] PriceLevel* find(Price p) noexcept {
        auto it = locate(p);
        return (it != levels_.end() && it->first == p) ? &it->second : nullptr;
    }

    void erase(Price p) {
        auto it = locate(p);
        if (it != levels_.end() && it->first == p) levels_.erase(it);
    }

    [[nodiscard]] const PriceLevel* best() const noexcept {
        return levels_.empty() ? nullptr : &levels_.front().second;
    }

    [[nodiscard]] bool        empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return levels_.size(); }
    void clear() { levels_.clear(); }

    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [price, lvl] : levels_) {
            if (!f(price, lvl)) return;
        }
    }

private:
    auto locate(Price p) {
        return std::lower_bound(levels_.begin(), levels_.end(), p,
                                [](const auto& e, Price x) { return before(e.first, x); });
    }

#if TICK_PMR_LEVELS
    std::pmr::vector<std::pair<Price, PriceLevel>> levels_;
#else
    std::vector<std::pair<Price, PriceLevel>> levels_;
#endif
};

} // namespace tick
