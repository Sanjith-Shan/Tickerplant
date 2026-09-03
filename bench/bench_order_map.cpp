// The order reference table shootout, replaying the real operation trace.
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
// WHY THE TRACE IS REAL AND NOT RANDOM.
//
// Everything interesting about this comparison is a property of the keys, not
// of the tables. NASDAQ hands out order reference numbers in close to
// increasing order through a day. The expectation this benchmark exists to test
// is that IdentityHash should therefore be the friendliest possible input to
// linear probing, because consecutive keys land in consecutive slots and
// collide with nothing, while SplitMix64Hash scatters the same keys uniformly
// and pays a handful of arithmetic operations per probe for the privilege.
//
// That is a hypothesis and not a conclusion. "Close to increasing" is not the
// same as consecutive, and a dense but gappy key set is the exact input linear
// probing handles worst, because occupied slots run together into long chains
// and every probe that lands in one walks to its end. Which effect dominates is
// a measurement, which is why the probe length is reported as a counter next to
// the timing rather than argued for in a comment. Read the two together, and
// believe the probe count.
//
// A benchmark fed random uint64 keys would flatten the difference to nothing
// and would report an answer about a feed that does not exist. So the trace
// here is extracted from real messages: insert on A and F, look up on E, C, X
// and U, erase on D and U, and erase as well when an execution or a cancel
// takes the whole resting quantity, which is how an order really leaves the
// book.
//
// bench_common.hpp reports how nearly sequential the references actually are,
// and those numbers are attached to every case below as counters, so the result
// and the property that explains it cannot be separated.
//
// WHY THE RESERVED UNORDERED MAP IS HERE.
//
// Beating an unreserved std::unordered_map is not a result. It rehashes its way
// up from one bucket while the flat map was handed its final size, and any
// interviewer will say so in the first thirty seconds. Both are present. The
// reserved one is told to prepare for the same number of elements as the flat
// map's slot count, which is more headroom than it strictly needs, so nothing
// in the comparison turns on sizing.
//
// WHY THE ERASE HEAVY CASE IS SEPARATE.
//
// Backward shift deletion is the design decision FlatOrderMap exists to
// justify. It is more expensive than dropping a tombstone and it pays for
// itself by keeping probe lengths from drifting upward over a whole day. That
// only shows up when erases are a large share of the work, and averaging it
// into the mixed trace hides it, so it gets its own case built from the insert
// and erase events of the same real trace.

#include "bench_common.hpp"

#include "nano/order.hpp"
#include "tick/flat_order_map.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Registered ahead of the benchmark registrations below so the machine label is
// in the JSON context block before the reporter writes it.
[[maybe_unused]] const bool kMachineRegistered = tickbench::register_machine_context();

// Slots in the flat table. Both flat maps and the reserved unordered map are
// sized from this one number, so no case is handicapped by its sizing. The
// replay reports the peak live order count it actually reached and the flat
// map's growth event count, so a reader can check that this was enough and that
// no case paid for a mid-run rehash.
constexpr std::size_t kCapacity = 1u << 22;

// The values. The tables store a pointer to a pooled order and never look
// through it, so what matters is that the pointer is a real address of the
// right size and that storing it costs what storing it costs. A small arena
// handed out cyclically keeps this benchmark about the table rather than about
// the allocator, which has its own benchmark in NanoExchange.
std::vector<nano::Order>& value_arena() {
    static std::vector<nano::Order> arena(1u << 16);
    return arena;
}

[[nodiscard]] inline nano::Order* value_for(std::size_t i) {
    std::vector<nano::Order>& a = value_arena();
    return &a[i & (a.size() - 1)];
}

// Attach the properties of the trace that explain the result, so a row in the
// results file carries its own explanation.
void annotate(benchmark::State& state, const tickbench::RefTrace& t) {
    state.counters["events"]    = static_cast<double>(t.events.size());
    state.counters["inserts"]   = static_cast<double>(t.inserts);
    state.counters["lookups"]   = static_cast<double>(t.lookups);
    state.counters["erases"]    = static_cast<double>(t.erases);
    state.counters["peak_live"] = static_cast<double>(t.peak_live);
    state.counters["monotone_insert_fraction"] =
        t.inserts == 0 ? 0.0 : static_cast<double>(t.monotone_inserts) /
                                   static_cast<double>(t.inserts);
    // Reference numbers spread over this many values for that many inserts. A
    // ratio near one means the day's references are close to a dense run.
    state.counters["key_span_per_insert"] =
        t.inserts == 0 ? 0.0
                       : static_cast<double>(t.insert_key_span) / static_cast<double>(t.inserts);
}

// ---------------------------------------------------------------------------
// FlatOrderMap
// ---------------------------------------------------------------------------
//
// The probe counters inside FlatOrderMap increment on the timed path, two
// increments per find. They are part of what is being measured here and they
// are not subtracted out, so the flat map numbers are if anything conservative
// against the unordered map, which carries no such instrumentation.

template <typename Hash>
void flat_mixed(benchmark::State& state) {
    const tickbench::RefTrace& trace = tickbench::ref_trace();

    double probes_per_lookup = 0.0, load = 0.0, grows = 0.0, final_size = 0.0;

    // The table is created and destroyed inside the paused region. A table
    // declared inside the timed loop body would be destroyed inside the timed
    // region too, and freeing a million nodes is not part of what a feed
    // handler does per message.
    using Map = tick::FlatOrderMap<nano::Order*, Hash>;
    std::unique_ptr<Map> owner;

    for (auto _ : state) {
        state.PauseTiming();
        owner = std::make_unique<Map>(kCapacity);
        Map& map = *owner;
        state.ResumeTiming();

        std::size_t i = 0;
        for (const tickbench::RefEvent& e : trace.events) {
            switch (e.op) {
            case tickbench::RefOp::Insert:
                map.insert(e.key, value_for(i++));
                break;
            case tickbench::RefOp::Lookup:
                benchmark::DoNotOptimize(map.find(e.key));
                break;
            case tickbench::RefOp::Erase:
                benchmark::DoNotOptimize(map.erase(e.key));
                break;
            }
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        probes_per_lookup = map.lookup_count() == 0
                                ? 0.0
                                : static_cast<double>(map.probe_count()) /
                                      static_cast<double>(map.lookup_count());
        load       = map.load_factor();
        grows      = static_cast<double>(map.growth_events());
        final_size = static_cast<double>(map.size());
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(trace.events.size()));
    annotate(state, trace);
    // The number the whole design argument rests on. Probes beyond the first
    // slot, per find. Zero would mean every lookup hit on its first try.
    state.counters["probes_per_lookup"] = probes_per_lookup;
    state.counters["final_load_factor"] = load;
    state.counters["growth_events"]     = grows;
    state.counters["final_size"]        = final_size;
    state.SetLabel(std::string("FlatOrderMap, ") + Hash::kName);
}

template <typename Hash>
void flat_erase_heavy(benchmark::State& state) {
    const std::vector<tickbench::RefEvent>& events = tickbench::erase_heavy_trace();
    const tickbench::RefTrace&              trace  = tickbench::ref_trace();

    double load = 0.0, final_size = 0.0;

    using Map = tick::FlatOrderMap<nano::Order*, Hash>;
    std::unique_ptr<Map> owner;

    for (auto _ : state) {
        state.PauseTiming();
        owner = std::make_unique<Map>(kCapacity);
        Map& map = *owner;
        state.ResumeTiming();

        std::size_t i = 0;
        for (const tickbench::RefEvent& e : events) {
            if (e.op == tickbench::RefOp::Insert) {
                map.insert(e.key, value_for(i++));
            } else {
                benchmark::DoNotOptimize(map.erase(e.key));
            }
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        load       = map.load_factor();
        final_size = static_cast<double>(map.size());
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(events.size()));
    annotate(state, trace);
    state.counters["erase_events"]      = static_cast<double>(events.size());
    state.counters["final_load_factor"] = load;
    state.counters["final_size"]        = final_size;
    state.SetLabel(std::string("FlatOrderMap backward shift erase, ") + Hash::kName);
}

// ---------------------------------------------------------------------------
// std::unordered_map
// ---------------------------------------------------------------------------

void unordered_mixed(benchmark::State& state, bool reserve) {
    const tickbench::RefTrace& trace = tickbench::ref_trace();

    double load = 0.0, buckets = 0.0, final_size = 0.0;

    using Map = std::unordered_map<uint64_t, nano::Order*>;
    std::unique_ptr<Map> owner;

    for (auto _ : state) {
        state.PauseTiming();
        owner = std::make_unique<Map>();
        if (reserve) owner->reserve(kCapacity);
        Map& map = *owner;
        state.ResumeTiming();

        std::size_t i = 0;
        for (const tickbench::RefEvent& e : trace.events) {
            switch (e.op) {
            case tickbench::RefOp::Insert:
                // insert_or_assign rather than operator[], because that is what
                // FlatOrderMap::insert does: overwrite on a duplicate reference
                // without first default constructing a value.
                map.insert_or_assign(e.key, value_for(i++));
                break;
            case tickbench::RefOp::Lookup: {
                auto it = map.find(e.key);
                benchmark::DoNotOptimize(it == map.end() ? nullptr : it->second);
                break;
            }
            case tickbench::RefOp::Erase:
                benchmark::DoNotOptimize(map.erase(e.key));
                break;
            }
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        load       = static_cast<double>(map.load_factor());
        buckets    = static_cast<double>(map.bucket_count());
        final_size = static_cast<double>(map.size());
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(trace.events.size()));
    annotate(state, trace);
    state.counters["final_load_factor"] = load;
    state.counters["bucket_count"]      = buckets;
    state.counters["final_size"]        = final_size;
    state.SetLabel(reserve ? "std::unordered_map, reserved up front"
                           : "std::unordered_map, no reserve");
}

void unordered_erase_heavy(benchmark::State& state, bool reserve) {
    const std::vector<tickbench::RefEvent>& events = tickbench::erase_heavy_trace();
    const tickbench::RefTrace&              trace  = tickbench::ref_trace();

    double load = 0.0, final_size = 0.0;

    using Map = std::unordered_map<uint64_t, nano::Order*>;
    std::unique_ptr<Map> owner;

    for (auto _ : state) {
        state.PauseTiming();
        owner = std::make_unique<Map>();
        if (reserve) owner->reserve(kCapacity);
        Map& map = *owner;
        state.ResumeTiming();

        std::size_t i = 0;
        for (const tickbench::RefEvent& e : events) {
            if (e.op == tickbench::RefOp::Insert) {
                map.insert_or_assign(e.key, value_for(i++));
            } else {
                benchmark::DoNotOptimize(map.erase(e.key));
            }
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        load       = static_cast<double>(map.load_factor());
        final_size = static_cast<double>(map.size());
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(events.size()));
    annotate(state, trace);
    state.counters["erase_events"]      = static_cast<double>(events.size());
    state.counters["final_load_factor"] = load;
    state.counters["final_size"]        = final_size;
    state.SetLabel(reserve ? "std::unordered_map erase, reserved up front"
                           : "std::unordered_map erase, no reserve");
}

// Each iteration replays millions of operations, so an iteration is tens of
// milliseconds. Repetitions give the median that this platform's lack of
// pinning makes the only honest summary.
void trace_args(benchmark::internal::Benchmark* bm) {
    bm->Unit(benchmark::kMillisecond)->Repetitions(7)->DisplayAggregatesOnly(true);
}

} // namespace

BENCHMARK_TEMPLATE(flat_mixed, tick::SplitMix64Hash)->Apply(trace_args);
BENCHMARK_TEMPLATE(flat_mixed, tick::IdentityHash)->Apply(trace_args);
BENCHMARK_CAPTURE(unordered_mixed, plain, false)->Apply(trace_args);
BENCHMARK_CAPTURE(unordered_mixed, reserved, true)->Apply(trace_args);

BENCHMARK_TEMPLATE(flat_erase_heavy, tick::SplitMix64Hash)->Apply(trace_args);
BENCHMARK_TEMPLATE(flat_erase_heavy, tick::IdentityHash)->Apply(trace_args);
BENCHMARK_CAPTURE(unordered_erase_heavy, plain, false)->Apply(trace_args);
BENCHMARK_CAPTURE(unordered_erase_heavy, reserved, true)->Apply(trace_args);
