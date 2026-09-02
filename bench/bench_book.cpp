// Book side containers and full book building, driven by real ITCH messages.
//
// UNPINNED. The numbers this file produces on the macOS development box are
// unpinned. macOS gives a user space program no way to bind a thread to a core,
// so nothing here pins and nothing here could. Read the median as the honest
// measure of the structure and read the spread as operating system scheduling
// noise rather than as a property of the code. The pinned numbers, where a tail
// figure means something, come from the Linux box and are reported separately.
//
// No measured number appears in this file. Numbers live in results/.
//
// WHY nano::ArrayContainer IS NOT IN THIS COMPARISON.
//
// The direct addressed array won NanoExchange's own price level container
// shootout, comfortably, and it is absent here. That is a finding and not an
// oversight, so it is written down rather than left for someone to wonder about.
//
// ArrayContainer is addressed by price offset: it holds one slot for every tick
// between a low and a high bound, and a lookup is an index rather than a search.
// That is unbeatable when the price window is narrow and known, which is the
// case a matching engine built for one instrument can arrange. A feed handler
// cannot arrange it. ITCH prices arrive in ten-thousandths of a dollar, and on
// a real NASDAQ day they run from a few hundred of those units for a sub penny
// stock to well over a hundred million for the most expensive names, with every
// listed symbol carrying its own window. One slot per tick across that span is
// gigabytes per side per symbol, multiplied by the thousands of symbols a feed
// handler carries at once. The structure that wins the matching engine
// benchmark is not merely slower here, it does not fit in memory, and no amount
// of tuning changes that.
//
// So the comparison that can actually be run is the tree against the sorted
// vector, and that is what is below. The window bound is the real reason a feed
// handler and a matching engine end up with different containers, which is
// worth more than a third bar on a chart.
//
// WHAT THE SIDE BENCHMARK REPLAYS.
//
// The add and delete stream of the busiest symbols in the loaded window, with
// the real prices, share counts and arrival order from the file. Which symbols
// those are is measured by counting book touching messages, never assumed from
// reputation. Each delete is linked to its add offline, so the timed region
// contains no reference lookup and measures the level container alone.
//
// Depth is swept because it is the axis the answer turns on. A side is prewarmed
// with that many distinct real price levels before the replay starts, so the
// vector's insert and erase shifts have that much tail to move and the tree has
// that many nodes to walk.

#include "bench_common.hpp"

#include "nano/memory_pool.hpp"
#include "nano/order.hpp"
#include "nano/price_level.hpp"
#include "nano/types.hpp"

#include "tick/book_builder.hpp"
#include "tick/book_side.hpp"
#include "tick/itch_decoder.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

// Registered ahead of the benchmark registrations below so the machine label is
// in the JSON context block before the reporter writes it.
[[maybe_unused]] const bool kMachineRegistered = tickbench::register_machine_context();

// One symbol's live order count never approaches this in the loaded window, and
// the replay reports how many orders it actually held so the headroom can be
// checked rather than trusted.
constexpr std::size_t kSidePoolCapacity = 1u << 19;

// Sized above the peak live order count the whole window reaches, rather than
// at the day sized production default, so a benchmark run is not dominated by
// building a free list it never uses.
constexpr std::size_t kBookPoolCapacity  = 2'000'000;
constexpr std::size_t kBookOrderMapCap   = 1u << 22;

using Pool = nano::MemoryPool<nano::Order, kSidePoolCapacity>;

// ---------------------------------------------------------------------------
// One side pair, driven by one symbol's real add and delete stream
// ---------------------------------------------------------------------------

template <template <bool> class SideT>
struct SideFixture {
    SideT<true>                bids;
    SideT<false>               asks;
    std::unique_ptr<Pool>      pool = std::make_unique<Pool>();
    std::vector<nano::Order*>  slots;   // by add event index
    std::vector<nano::Order*>  warm;    // the prewarmed resting orders
    bool                       exhausted = false;

    explicit SideFixture(std::size_t event_count) : slots(event_count, nullptr) {}

    // Hand every order back and empty both sides, so the next iteration starts
    // from the same state as the last one did. Runs in a paused region.
    void reset() {
        for (nano::Order*& o : slots) {
            if (o != nullptr) {
                pool->deallocate(o);
                o = nullptr;
            }
        }
        for (nano::Order* o : warm) pool->deallocate(o);
        warm.clear();
        bids.clear();
        asks.clear();
        exhausted = false;
    }

    // Put depth distinct real price levels on each side, so the replay starts
    // against a book of a known depth instead of an empty one.
    void prewarm(const std::vector<nano::Price>& bid_px,
                 const std::vector<nano::Price>& ask_px) {
        for (nano::Price p : bid_px) {
            nano::Order* o = pool->allocate(0, nano::Side::Buy, nano::OrderType::Limit, p, 100,
                                            0, 0);
            if (o == nullptr) { exhausted = true; return; }
            bids.level(p).append(o);
            warm.push_back(o);
        }
        for (nano::Price p : ask_px) {
            nano::Order* o = pool->allocate(0, nano::Side::Sell, nano::OrderType::Limit, p, 100,
                                            0, 0);
            if (o == nullptr) { exhausted = true; return; }
            asks.level(p).append(o);
            warm.push_back(o);
        }
    }

    // Replay the real event stream. Track is only set for the untimed dry run
    // that reports how deep the book actually got.
    template <bool Track>
    std::size_t replay(const std::vector<tickbench::LevelEvent>& events) {
        std::size_t max_levels = 0;
        for (const tickbench::LevelEvent& e : events) {
            if (e.is_add) {
                nano::Order* o = pool->allocate(
                    static_cast<nano::OrderId>(e.ref),
                    e.is_bid ? nano::Side::Buy : nano::Side::Sell, nano::OrderType::Limit,
                    static_cast<nano::Price>(e.price), static_cast<nano::Quantity>(e.shares),
                    0, 0);
                if (o == nullptr) { exhausted = true; break; }
                if (e.is_bid) {
                    bids.level(static_cast<nano::Price>(e.price)).append(o);
                } else {
                    asks.level(static_cast<nano::Price>(e.price)).append(o);
                }
                slots[e.link] = o;
            } else {
                nano::Order* o = slots[e.link];
                if (o == nullptr) continue;
                if (e.is_bid) {
                    if (nano::PriceLevel* lvl = bids.find(o->price)) {
                        lvl->remove(o);
                        if (lvl->empty()) bids.erase(o->price);
                    }
                } else {
                    if (nano::PriceLevel* lvl = asks.find(o->price)) {
                        lvl->remove(o);
                        if (lvl->empty()) asks.erase(o->price);
                    }
                }
                pool->deallocate(o);
                slots[e.link] = nullptr;
            }
            if constexpr (Track) {
                const std::size_t n = bids.size() + asks.size();
                if (n > max_levels) max_levels = n;
            }
        }
        return max_levels;
    }
};

// The prewarm levels that put the side at a known depth before the replay.
//
// They are anchored to the symbol's own real prices rather than invented: the
// lowest real bid and the highest real ask in the stream, extended outward one
// cent at a time. That keeps them at real tick spacing and real magnitude, and
// it keeps them clear of every price the replay itself touches, so a prewarmed
// order can never hold alive a level the replay expects to erase.
//
// They sit away from the top of book on purpose. On a sorted vector the resting
// depth is the tail that every insert near the touch has to shift, and on a
// tree it is the node count every descent pays for, so putting the prewarm
// where a real deep book is is the whole point of the sweep.
constexpr nano::Price kCent = 100;  // ITCH prices are ten-thousandths of a dollar

void prewarm_prices(const std::vector<tickbench::LevelEvent>& events, std::size_t depth,
                    std::vector<nano::Price>& bid_px, std::vector<nano::Price>& ask_px) {
    if (depth == 0) return;

    nano::Price min_bid = 0, max_ask = 0;
    bool        have_bid = false, have_ask = false;
    for (const tickbench::LevelEvent& e : events) {
        if (!e.is_add) continue;
        const nano::Price p = static_cast<nano::Price>(e.price);
        if (e.is_bid) {
            if (!have_bid || p < min_bid) { min_bid = p; have_bid = true; }
        } else {
            if (!have_ask || p > max_ask) { max_ask = p; have_ask = true; }
        }
    }

    for (std::size_t k = 1; k <= depth && have_bid; ++k) {
        const nano::Price p = min_bid - static_cast<nano::Price>(k) * kCent;
        if (p <= 0) break;   // never a negative price, even on a penny stock
        bid_px.push_back(p);
    }
    for (std::size_t k = 1; k <= depth && have_ask; ++k) {
        ask_px.push_back(max_ask + static_cast<nano::Price>(k) * kCent);
    }
}

template <template <bool> class SideT>
void side_replay(benchmark::State& state) {
    const std::vector<tickbench::SymbolStream>& syms = tickbench::symbol_streams();
    if (syms.empty() || syms.front().events.empty()) {
        state.SkipWithError("no symbol stream in the loaded window");
        return;
    }
    const tickbench::SymbolStream&            sym    = syms.front();
    const std::vector<tickbench::LevelEvent>& events = sym.events;
    const std::size_t                         depth  = static_cast<std::size_t>(state.range(0));

    std::vector<nano::Price> bid_px, ask_px;
    prewarm_prices(events, depth, bid_px, ask_px);

    auto fix = std::make_unique<SideFixture<SideT>>(events.size());

    // One untimed dry run, only to learn how deep the book really got. Doing
    // this inside the timed loop would add two size calls to every event.
    fix->prewarm(bid_px, ask_px);
    const std::size_t max_levels = fix->template replay<true>(events);
    fix->reset();

    for (auto _ : state) {
        state.PauseTiming();
        fix->reset();
        fix->prewarm(bid_px, ask_px);
        state.ResumeTiming();

        fix->template replay<false>(events);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(events.size()));
    state.counters["events"]       = static_cast<double>(events.size());
    state.counters["prewarm"]      = static_cast<double>(bid_px.size() + ask_px.size());
    state.counters["peak_levels"]  = static_cast<double>(max_levels);
    // Zero unless the pool ran out, in which case the replay stopped early and
    // the row is not a throughput number. Reported rather than swallowed.
    state.counters["pool_exhausted"] = fix->exhausted ? 1.0 : 0.0;
    state.SetLabel(std::string(SideT<true>::kName) + " / " + std::string(SideT<false>::kName) +
                   ", " + sym.ticker);
}

// ---------------------------------------------------------------------------
// Whole book, both side containers, over the same real message stream
// ---------------------------------------------------------------------------
//
// The same messages, the same decoder, the same order reference table. The only
// difference is the container holding the price levels, which is what makes
// this a fair comparison rather than two unrelated numbers.
//
// The book digest is reported in the label. It is a fingerprint of every level
// of every symbol, so two rows carrying the same digest are a statement that
// swapping the container did not change the book, which is the only thing that
// makes the faster one worth having.

template <template <bool> class SideT>
void book_throughput(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    using Builder = tick::BookBuilder<SideT, kBookPoolCapacity>;

    std::unique_ptr<Builder> builder;
    uint64_t                 digest = 0;
    uint64_t                 levels = 0, peak_live = 0, orphans = 0;

    for (auto _ : state) {
        state.PauseTiming();
        builder = std::make_unique<Builder>(kBookOrderMapCap);
        tick::ZeroCopyDecoder dec;
        Builder&              b = *builder;
        state.ResumeTiming();

        const std::size_t n = store.size();
        for (std::size_t i = 0; i < n; ++i) dec.decode(store.msg(i), b);
        uint64_t seen = dec.stats().messages;
        benchmark::DoNotOptimize(seen);
        benchmark::ClobberMemory();

        state.PauseTiming();
        digest    = b.digest();
        levels    = b.live_levels();
        peak_live = b.stats().peak_live_orders;
        const tick::BookStats& s = b.stats();
        orphans   = s.orphan_executes + s.orphan_cancels + s.orphan_deletes + s.orphan_replaces;
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(store.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(store.byte_count()));
    state.counters["live_levels"] = static_cast<double>(levels);
    state.counters["peak_live"]   = static_cast<double>(peak_live);
    state.counters["orphans"]     = static_cast<double>(orphans);

    char label[128];
    std::snprintf(label, sizeof(label), "%s, book digest 0x%016llx",
                  SideT<true>::kName, static_cast<unsigned long long>(digest));
    state.SetLabel(label);
}

// Shallow through deep. A liquid NASDAQ name really does carry hundreds of live
// levels, so the sweep runs well past the handful a matching engine benchmark
// would look at.
void depth_args(benchmark::internal::Benchmark* bm) {
    bm->Arg(0)->Arg(8)->Arg(64)->Arg(512)
      ->Unit(benchmark::kMillisecond)
      ->Repetitions(7)
      ->DisplayAggregatesOnly(true);
}

void stream_args(benchmark::internal::Benchmark* bm) {
    bm->Unit(benchmark::kMillisecond)->Repetitions(7)->DisplayAggregatesOnly(true);
}

} // namespace

BENCHMARK_TEMPLATE(side_replay, tick::MapSide)->Apply(depth_args);
BENCHMARK_TEMPLATE(side_replay, tick::VectorSide)->Apply(depth_args);

BENCHMARK_TEMPLATE(book_throughput, tick::MapSide)->Apply(stream_args);
BENCHMARK_TEMPLATE(book_throughput, tick::VectorSide)->Apply(stream_args);
