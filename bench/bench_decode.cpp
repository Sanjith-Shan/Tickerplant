// Decode throughput: zero-copy against copying, over real ITCH messages.
//
// UNPINNED. The numbers this file produces on the macOS development box are
// unpinned. macOS gives a user space program no way to bind a thread to a core,
// so nothing here pins and nothing here could. Read the median as the honest
// measure of the decoder and read the spread as operating system scheduling
// noise rather than as a property of the code. The pinned numbers, where a tail
// figure means something, come from the Linux box and are reported separately.
//
// No measured number appears in this file. Numbers live in results/.
//
// WHAT IS BEING COMPARED, AND WHY BOTH HANDLERS APPEAR.
//
// ZeroCopyDecoder reads fields out of the buffer where they already sit, at the
// moment the handler asks for them, and a field the handler ignores is never
// loaded because the handler is a template parameter and the load is dead code.
// CopyingDecoder stages the message into its own buffer and byteswaps every
// field of that message type into an owned struct whether anyone wants it or
// not.
//
// Run against NullHandler, that description flatters the zero-copy path by
// construction: the handler ignores every field, so every load the zero-copy
// decoder would have performed is dead and the comparison degenerates into
// "doing nothing is faster than doing something". That case is still worth
// having, because it is the upper bound on what lazy decode can buy and because
// it isolates dispatch from field extraction. But reporting only that case
// would be an argument rather than a measurement, so the same pair is also run
// with a real BookBuilder as the handler, which uses the reference, side,
// shares and price on every add and ignores the eight byte ticker. That second
// number is the one that describes a feed handler doing its job.
//
// The per message type breakdown exists because the mix decides the average. A
// day is add, delete, replace and cancel with a thin sprinkling of everything
// else, and the copying decoder's penalty is not uniform across those: an add
// carries a ticker to copy and a delete carries nothing but a reference.
//
// The endian benchmarks separate byteswap cost from dispatch cost. If the swaps
// turn out to be free next to the branch, that is worth knowing before anyone
// spends effort on them.

#include "bench_common.hpp"

#include "tick/book_builder.hpp"
#include "tick/book_side.hpp"
#include "tick/endian.hpp"
#include "tick/itch.hpp"
#include "tick/itch_decoder.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// Registered ahead of the benchmark registrations below so the machine label is
// in the JSON context block before the reporter writes it.
[[maybe_unused]] const bool kMachineRegistered = tickbench::register_machine_context();

// Sized above the peak live order count the loaded window actually reaches,
// which the replay reports, rather than at the day sized production default.
// A day sized pool would spend most of a benchmark run building a free list.
constexpr std::size_t kBenchPoolCapacity = 2'000'000;
constexpr std::size_t kBenchOrderMapCap  = 1u << 22;

using MapBook = tick::BookBuilder<tick::MapSide, kBenchPoolCapacity>;

// Replay a whole message buffer through one decoder and one handler.
template <typename Decoder, typename Handler>
void run_buffer(const tickbench::MessageStore& store, Decoder& dec, Handler& h) {
    const std::size_t n = store.size();
    for (std::size_t i = 0; i < n; ++i) {
        dec.decode(store.msg(i), h);
    }
}

// ---------------------------------------------------------------------------
// Whole stream, NullHandler
// ---------------------------------------------------------------------------

template <typename Decoder>
void decode_null(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    tick::NullHandler              handler;

    for (auto _ : state) {
        Decoder dec;
        run_buffer(store, dec, handler);
        uint64_t seen = dec.stats().messages;
        benchmark::DoNotOptimize(seen);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(store.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(store.byte_count()));
    state.SetLabel(std::string(Decoder::kName) + ", NullHandler");
}

// ---------------------------------------------------------------------------
// Whole stream, BookBuilder
// ---------------------------------------------------------------------------
//
// The builder is rebuilt inside a paused region every iteration, so every
// iteration replays the window into an empty book and no iteration inherits the
// previous one's resting orders. Rebuilding is not free, which is exactly why
// it is outside the timed region.
//
// The window starts at the opening bell, so executions and deletes that refer
// to orders which were added before the window began have nothing to find. Those
// are counted as orphans by the builder and reported here as counters rather
// than hidden, because a reader is entitled to know that a few percent of the
// messages in this benchmark do less work than they would mid-session.
template <typename Decoder>
void decode_book(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    auto builder = std::make_unique<MapBook>(kBenchOrderMapCap);

    uint64_t orphans = 0, adds = 0, peak_live = 0;

    for (auto _ : state) {
        state.PauseTiming();
        builder = std::make_unique<MapBook>(kBenchOrderMapCap);
        Decoder dec;
        state.ResumeTiming();

        run_buffer(store, dec, *builder);
        uint64_t seen = dec.stats().messages;
        benchmark::DoNotOptimize(seen);
        benchmark::ClobberMemory();

        state.PauseTiming();
        const tick::BookStats& s = builder->stats();
        orphans   = s.orphan_executes + s.orphan_cancels + s.orphan_deletes + s.orphan_replaces;
        adds      = s.adds;
        peak_live = s.peak_live_orders;
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(store.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(store.byte_count()));
    state.counters["adds"]            = static_cast<double>(adds);
    state.counters["peak_live"]       = static_cast<double>(peak_live);
    state.counters["orphans"]         = static_cast<double>(orphans);
    state.counters["orphan_fraction"] =
        store.size() == 0 ? 0.0 : static_cast<double>(orphans) / static_cast<double>(store.size());
    state.SetLabel(std::string(Decoder::kName) + ", BookBuilder<MapSide>");
}

// ---------------------------------------------------------------------------
// Per message type
// ---------------------------------------------------------------------------

// A buffer of real messages of exactly one type, in the order they arrived.
// Held per type in a function local static so the five type cases and the two
// decoders share one copy.
const tickbench::MessageStore& type_store(char type) {
    // A node based map rather than a vector, so a reference handed out for one
    // type stays valid when the next type is filtered in.
    static std::map<char, tickbench::MessageStore> cache;
    auto it = cache.find(type);
    if (it == cache.end()) {
        it = cache.emplace(type, tickbench::filter_type(type, 1'000'000)).first;
    }
    return it->second;
}

template <typename Decoder>
void decode_one_type(benchmark::State& state, char type) {
    const tickbench::MessageStore& store = type_store(type);
    if (store.size() == 0) {
        state.SkipWithError("no messages of this type in the loaded window");
        return;
    }
    tick::NullHandler handler;

    for (auto _ : state) {
        Decoder dec;
        run_buffer(store, dec, handler);
        uint64_t seen = dec.stats().messages;
        benchmark::DoNotOptimize(seen);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(store.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(store.byte_count()));
    state.counters["messages"] = static_cast<double>(store.size());
    state.SetLabel(std::string(1, type) + ", " + std::string(Decoder::kName));
}

// BENCHMARK_CAPTURE cannot take a templated function name, so each decoder gets
// a thin non template entry point. The benchmark name then reads
// decode_type_zerocopy/A, which is what the plotting script parses.
void decode_type_zerocopy(benchmark::State& state, char type) {
    decode_one_type<tick::ZeroCopyDecoder>(state, type);
}

void decode_type_copying(benchmark::State& state, char type) {
    decode_one_type<tick::CopyingDecoder>(state, type);
}

// ---------------------------------------------------------------------------
// Endian helpers on their own
// ---------------------------------------------------------------------------
//
// Loads run over the real buffer at the real offsets, so they are as unaligned
// as the feed makes them. A synthetic aligned array would measure a load this
// code never performs. The index walks by a stride that is not a power of two
// so the loads do not all land in the same place.

template <typename T>
void endian_load(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    const std::byte*               base  = store.bytes.data();
    const std::size_t              limit = store.byte_count() - sizeof(T);
    constexpr std::size_t          kStride = 13;
    constexpr std::size_t          kLoads  = 4096;

    std::size_t i = 0;
    for (auto _ : state) {
        T acc{};
        for (std::size_t k = 0; k < kLoads; ++k) {
            i += kStride;
            if (i >= limit) i -= limit;
            acc = static_cast<T>(acc + tick::be_load<T>(base + i));
        }
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kLoads));
    state.SetLabel("unaligned big-endian load, " + std::to_string(sizeof(T) * 8) + " bit");
}

// The six byte ITCH timestamp, which is two loads and a shift rather than one
// instruction, and which every single message carries.
void endian_load_u48(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    const std::byte*               base  = store.bytes.data();
    const std::size_t              limit = store.byte_count() - 6;
    constexpr std::size_t          kStride = 13;
    constexpr std::size_t          kLoads  = 4096;

    std::size_t i = 0;
    for (auto _ : state) {
        uint64_t acc = 0;
        for (std::size_t k = 0; k < kLoads; ++k) {
            i += kStride;
            if (i >= limit) i -= limit;
            acc += tick::be_load_u48(base + i);
        }
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kLoads));
    state.SetLabel("unaligned big-endian load, 48 bit ITCH timestamp");
}

// The header every message carries, read the way the decoder reads it: type,
// locate and timestamp before any dispatch happens. This is the floor under the
// whole stream number.
void endian_header(benchmark::State& state) {
    const tickbench::MessageStore& store = tickbench::messages();
    const std::size_t              n     = store.size();

    for (auto _ : state) {
        uint64_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::byte* p = store.msg(i).data();
            acc += static_cast<uint64_t>(tick::itch::msg_type(p));
            acc += tick::itch::locate(p);
            acc += tick::itch::timestamp(p);
        }
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(store.byte_count()));
    state.SetLabel("type, locate and timestamp only, no dispatch");
}

// Whole stream cases replay millions of messages per iteration, so an iteration
// is tens of milliseconds and the default iteration count would be too small to
// say anything about spread. Repetitions give the median that this platform's
// lack of pinning makes the only honest summary.
void stream_args(benchmark::internal::Benchmark* bm) {
    bm->Unit(benchmark::kMillisecond)->Repetitions(7)->DisplayAggregatesOnly(true);
}

void small_args(benchmark::internal::Benchmark* bm) {
    bm->Repetitions(7)->DisplayAggregatesOnly(true);
}

} // namespace

BENCHMARK_TEMPLATE(decode_null, tick::ZeroCopyDecoder)->Apply(stream_args);
BENCHMARK_TEMPLATE(decode_null, tick::CopyingDecoder)->Apply(stream_args);

BENCHMARK_TEMPLATE(decode_book, tick::ZeroCopyDecoder)->Apply(stream_args);
BENCHMARK_TEMPLATE(decode_book, tick::CopyingDecoder)->Apply(stream_args);

// The five types that are almost the whole of a trading day.
BENCHMARK_CAPTURE(decode_type_zerocopy, A, 'A')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_copying,  A, 'A')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_zerocopy, D, 'D')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_copying,  D, 'D')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_zerocopy, E, 'E')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_copying,  E, 'E')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_zerocopy, X, 'X')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_copying,  X, 'X')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_zerocopy, U, 'U')->Apply(stream_args);
BENCHMARK_CAPTURE(decode_type_copying,  U, 'U')->Apply(stream_args);

BENCHMARK_TEMPLATE(endian_load, uint16_t)->Apply(small_args);
BENCHMARK_TEMPLATE(endian_load, uint32_t)->Apply(small_args);
BENCHMARK_TEMPLATE(endian_load, uint64_t)->Apply(small_args);
BENCHMARK(endian_load_u48)->Apply(small_args);
BENCHMARK(endian_header)->Apply(stream_args);
