#pragma once

// Shared fixtures for the Tickerplant microbenchmarks.
//
// MACHINE AND PINNING. The numbers produced by any benchmark that includes this
// header, when it is run on the macOS development box, are UNPINNED. macOS
// offers no way for a user space program to bind a thread to a particular core,
// so there is no affinity call that was skipped here and no flag that would
// enable one. Every sample competes with the scheduler and with whatever else
// the machine is doing. Read the median as the honest measure of the structure
// and read the tail as operating system scheduling noise rather than as a
// property of the code. Pinned numbers, where the tail means something, come
// from the Linux box and are reported separately. Nothing in this suite pins
// anything.
//
// WHY REAL MESSAGES. Every fixture below is built from the real NASDAQ
// TotalView-ITCH 5.0 sample file. Synthetic messages would be wrong here in a
// way that is not a matter of taste. Three properties of the real feed decide
// the results this suite reports:
//
//   1. The message type mix. Add, delete, and execute are almost the whole
//      stream and the rest is a rounding error, so a uniform mix over the
//      twenty three message types would measure a dispatch path that does not
//      exist on a real day.
//   2. Order reference numbers are nearly sequential. That single property is
//      the entire subject of the hash comparison in bench_order_map.cpp. A
//      trace of random uint64 keys would answer a different question and would
//      get the opposite answer.
//   3. Real prices are wide and lumpy. A liquid name carries hundreds of live
//      levels, adds land away from the top as often as at it, and the price
//      window spans from a few hundred ticks to over a hundred million.
//
// The file is read once at startup into one contiguous buffer and every timed
// region replays from memory, so no benchmark in this suite measures gzip or
// the kernel.

#include "tick/itch.hpp"
#include "tick/itch_file.hpp"
#include "tick/flat_order_map.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

namespace tickbench {

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

inline constexpr const char* kDefaultItchPath = "data/12302019.NASDAQ_ITCH50.gz";

// How many messages the shared buffer holds. Four million is about a hundred
// megabytes of ITCH, which is small enough that a benchmark run is interactive
// and large enough that the live order set is far past what fits in cache.
// Override with TICK_ITCH_MESSAGES for a quick smoke run or for a longer one.
inline constexpr std::size_t kDefaultMessageCount = 4'000'000;

// WHERE IN THE DAY THE WINDOW STARTS, and why this is not a detail.
//
// A NASDAQ file begins hours before the open. The messages at the front of it
// are real, but they are pre-market quoting in a handful of foreign listings:
// almost all add and delete, almost no executions, and a live order set small
// enough to sit in cache. Benchmarking that window and calling the result a
// feed handler number would be measuring the quietest part of the day.
//
// So the default window starts at the system event that declares the start of
// market hours, the S message carrying event code Q, and runs forward from
// there. Reaching it costs about a third of a second of inflate and it is the
// difference between a fixture that looks like a trading day and one that does
// not. TICK_ITCH_START overrides it: "open" for the default, "file" to start at
// the first message in the file, or a decimal message index to start there.
inline constexpr char kStartOfMarketHours = 'Q';

[[nodiscard]] inline std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string(fallback);
}

[[nodiscard]] inline std::size_t env_size(const char* name, std::size_t fallback,
                                          long long min_value = 1) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    char*           end = nullptr;
    const long long n   = std::strtoll(v, &end, 10);
    if (end == v || n < min_value) {
        std::fprintf(stderr, "%s is set to \"%s\", which is not a count of at least %lld.\n",
                     name, v, min_value);
        std::exit(2);
    }
    return static_cast<std::size_t>(n);
}

// A benchmark that silently measured an empty buffer would report an enormous
// and meaningless throughput, and that number could end up on a resume. So a
// missing file is a hard stop with an instruction, never a warning.
[[noreturn]] inline void fail_missing_file(const std::string& path) {
    std::fprintf(stderr,
                 "\n"
                 "Tickerplant benchmarks: cannot open the ITCH file.\n"
                 "  tried: %s\n"
                 "\n"
                 "These benchmarks replay real NASDAQ TotalView-ITCH 5.0 messages and\n"
                 "refuse to run on an empty or synthetic buffer, because the numbers they\n"
                 "produce are reported as measurements of real data.\n"
                 "\n"
                 "  fetch the sample file:   ./scripts/fetch_itch.sh\n"
                 "  or point at your own:    TICK_ITCH_FILE=/path/to/file.NASDAQ_ITCH50.gz\n"
                 "  smaller, faster run:     TICK_ITCH_MESSAGES=200000\n"
                 "\n",
                 path.c_str());
    std::exit(2);
}

// ---------------------------------------------------------------------------
// The shared message buffer
// ---------------------------------------------------------------------------

// One contiguous blob of ITCH message bodies, with the length prefixes already
// stripped, plus the offset of every message. Messages are packed back to back
// with no padding, exactly as they arrive, so a replay walks memory the same
// way the receive path does and the unaligned loads in endian.hpp are exercised
// at the same unaligned offsets the real feed produces.
struct MessageStore {
    std::vector<std::byte>    bytes;
    std::vector<uint32_t>     offsets;   // one per message, plus a trailing end
    std::array<uint64_t, 256> by_type{};
    std::size_t               first_message_index = 0;  // position in the file

    // Stock locate to ticker, harvested from the R stock directory messages.
    // Those are delivered once, before the open, so a window that starts at the
    // open contains none of them and would otherwise have no way to name a
    // symbol. They are collected while skipping forward rather than inferred.
    std::vector<std::string>  tickers;

    [[nodiscard]] std::string_view ticker(uint16_t locate) const noexcept {
        return locate < tickers.size() ? std::string_view(tickers[locate]) : std::string_view{};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return offsets.empty() ? 0 : offsets.size() - 1;
    }

    [[nodiscard]] std::span<const std::byte> msg(std::size_t i) const noexcept {
        return {bytes.data() + offsets[i], offsets[i + 1] - offsets[i]};
    }

    [[nodiscard]] std::size_t byte_count() const noexcept { return bytes.size(); }
};

// Read up to count messages into one buffer. Collection begins after skip
// messages have gone by, or, when from_open is set, at the first message at or
// after the start of market hours system event, whichever comes later.
inline MessageStore load_messages(const std::string& path, std::size_t count,
                                  std::size_t skip, bool from_open) {
    std::FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr) fail_missing_file(path);
    std::fclose(probe);

    MessageStore store;
    store.tickers.resize(1u << 16);
    // About twenty six bytes a message on a real day. Reserving stops the
    // vector from doubling half a dozen times while the file streams in, and is
    // capped so that an enormous TICK_ITCH_MESSAGES does not reserve a buffer
    // the machine cannot hold before a single message has been read.
    constexpr std::size_t kReserveCap = std::size_t{1} << 29;  // 512 MiB
    store.bytes.reserve(std::min(count * 28, kReserveCap));
    store.offsets.reserve(std::min(count + 1, kReserveCap / 28));

    tick::ItchFile              file(path);
    std::span<const std::byte>  m;
    std::size_t                 seen      = 0;
    bool                        collecting = !from_open;

    while (store.offsets.size() < count && file.next(m)) {
        const char type = tick::itch::msg_type(m.data());
        if (type == 'R') {
            store.tickers[tick::itch::locate(m.data())] =
                std::string(tick::itch::stock(m.data() + tick::itch::off::kDirStock));
        }
        if (!collecting) {
            if (type == 'S' &&
                static_cast<char>(m[tick::itch::off::kEventCode]) == kStartOfMarketHours) {
                collecting = true;
            }
            ++seen;
            continue;
        }
        if (seen++ < skip) continue;
        store.offsets.push_back(static_cast<uint32_t>(store.bytes.size()));
        store.bytes.insert(store.bytes.end(), m.begin(), m.end());
        ++store.by_type[static_cast<uint8_t>(type)];
    }
    store.offsets.push_back(static_cast<uint32_t>(store.bytes.size()));
    store.first_message_index = seen - store.offsets.size();

    if (store.size() == 0) {
        std::fprintf(stderr,
                     "Read zero messages from %s. The file exists but produced nothing;\n"
                     "it is either truncated or is not an ITCH 5.0 capture.\n",
                     path.c_str());
        std::exit(2);
    }
    return store;
}

// The one shared copy. Loaded on first use and shared by every benchmark case
// in the process, so a file with several cases pays the inflate cost once.
[[nodiscard]] inline const MessageStore& messages() {
    static const MessageStore store = [] {
        const std::string path  = env_or("TICK_ITCH_FILE", kDefaultItchPath);
        const std::size_t count = env_size("TICK_ITCH_MESSAGES", kDefaultMessageCount);
        const std::string start = env_or("TICK_ITCH_START", "open");

        bool        from_open = false;
        std::size_t skip      = 0;
        if (start == "open") {
            from_open = true;
        } else if (start == "file") {
            from_open = false;
        } else {
            char*           end = nullptr;
            const long long n   = std::strtoll(start.c_str(), &end, 10);
            if (end == start.c_str() || *end != '\0' || n < 0) {
                std::fprintf(stderr,
                             "TICK_ITCH_START is \"%s\". Use \"open\", \"file\", or a message "
                             "index.\n",
                             start.c_str());
                std::exit(2);
            }
            skip = static_cast<std::size_t>(n);
        }

        std::fprintf(stderr, "loading up to %zu ITCH messages from %s, starting at %s ...\n",
                     count, path.c_str(), start.c_str());
        MessageStore s = load_messages(path, count, skip, from_open);
        std::fprintf(stderr, "loaded %zu messages (%zu bytes) beginning at file message %zu\n",
                     s.size(), s.byte_count(), s.first_message_index);
        return s;
    }();
    return store;
}

// A buffer holding only messages of one type, taken from the shared buffer in
// stream order. This is how the per message type decode breakdown gets a clean
// measurement of one type's field extraction without the other types' branches
// in the way. The messages are still real and still in the order they arrived.
[[nodiscard]] inline MessageStore filter_type(char type, std::size_t max_messages) {
    const MessageStore& all = messages();
    MessageStore        out;
    const std::size_t   len = tick::itch::message_length(type);
    out.bytes.reserve(std::min(max_messages, all.size()) * (len == 0 ? 32 : len));
    out.offsets.reserve(std::min(max_messages, all.size()) + 1);

    for (std::size_t i = 0; i < all.size() && out.size() < max_messages; ++i) {
        const std::span<const std::byte> m = all.msg(i);
        if (tick::itch::msg_type(m.data()) != type) continue;
        out.offsets.push_back(static_cast<uint32_t>(out.bytes.size()));
        out.bytes.insert(out.bytes.end(), m.begin(), m.end());
        ++out.by_type[static_cast<uint8_t>(type)];
    }
    out.offsets.push_back(static_cast<uint32_t>(out.bytes.size()));
    return out;
}

// ---------------------------------------------------------------------------
// The real order reference operation trace
// ---------------------------------------------------------------------------
//
// This is the most important fixture in the file, because the order map
// shootout is decided by a property of the keys and not by a property of the
// tables. NASDAQ hands out order reference numbers in close to increasing
// order within a day. Under an identity hash and linear probing that is the
// best case a hash table can be handed: consecutive keys land in consecutive
// slots and collide with nothing. Under splitmix64 the same keys are scattered
// uniformly, which is the case the textbook analysis assumes. Replaying random
// uint64 keys would erase the difference between those two worlds and would
// report a result that says nothing about this feed.
//
// The mapping from message type to operation follows what BookBuilder actually
// does with the table:
//
//   A, F   insert the new reference
//   E, C   look the reference up, and erase it when the execution takes the
//          whole resting quantity, which is what removes an order from the
//          book on a real day
//   X      look it up, and erase it when the cancel takes the whole quantity
//   D      erase it
//   U      look the old reference up, erase it, and insert the new one
//
// The full fill erase matters. Without it the replayed table only ever loses
// keys to D messages, the live set grows past anything a real book holds, and
// the load factor and probe lengths being measured would be a property of the
// fixture rather than of the feed. Resting quantity is tracked here, outside
// the timed region, using the same flat map the benchmark measures.
enum class RefOp : uint8_t { Insert, Lookup, Erase };

struct RefEvent {
    uint64_t key;
    RefOp    op;
};

struct RefTrace {
    std::vector<RefEvent> events;
    std::size_t           inserts     = 0;
    std::size_t           lookups     = 0;
    std::size_t           erases      = 0;
    std::size_t           peak_live   = 0;
    uint64_t              min_key     = 0;
    uint64_t              max_key     = 0;
    // How nearly sequential the inserted references actually are. This is the
    // property the hash comparison turns on, so it is measured rather than
    // asserted, and bench_order_map.cpp reports it next to the timings.
    //   monotone_inserts  inserts whose reference is larger than the previous
    //   insert_key_span   last inserted reference minus the first
    std::size_t           monotone_inserts = 0;
    uint64_t              insert_key_span  = 0;
};

[[nodiscard]] inline RefTrace build_ref_trace() {
    const MessageStore& all = messages();
    RefTrace            t;
    t.events.reserve(all.size());

    // Resting quantity by reference, so a full fill becomes an erase. Sized
    // generously because it is a fixture and never a measured structure.
    tick::FlatOrderMap<uint32_t> resting(1u << 22);
    std::size_t                  live       = 0;
    uint64_t                     prev_ins   = 0;
    uint64_t                     first_ins  = 0;
    bool                         have_prev  = false;

    auto push = [&](RefOp op, uint64_t key) {
        t.events.push_back(RefEvent{key, op});
        switch (op) {
        case RefOp::Insert: ++t.inserts; ++live; break;
        case RefOp::Lookup: ++t.lookups; break;
        case RefOp::Erase:  ++t.erases; if (live > 0) --live; break;
        }
        if (live > t.peak_live) t.peak_live = live;
        if (t.min_key == 0 || key < t.min_key) t.min_key = key;
        if (key > t.max_key) t.max_key = key;
    };

    auto do_insert = [&](uint64_t ref, uint32_t shares) {
        if (ref == 0) return;
        if (!have_prev) first_ins = ref;
        if (have_prev && ref > prev_ins) ++t.monotone_inserts;
        prev_ins  = ref;
        have_prev = true;
        t.insert_key_span = prev_ins > first_ins ? prev_ins - first_ins : 0;
        resting.insert(ref, shares == 0 ? 1u : shares);
        push(RefOp::Insert, ref);
    };

    // A reduction: look the reference up, then erase it when nothing is left.
    auto do_reduce = [&](uint64_t ref, uint32_t shares) {
        if (ref == 0) return;
        push(RefOp::Lookup, ref);
        uint32_t* q = resting.find(ref);
        if (q == nullptr) return;
        if (shares >= *q) {
            resting.erase(ref);
            push(RefOp::Erase, ref);
        } else {
            *q -= shares;
        }
    };

    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::byte* p    = all.msg(i).data();
        const char       type = tick::itch::msg_type(p);
        switch (type) {
        case 'A':
        case 'F':
            do_insert(tick::be_load<uint64_t>(p + tick::itch::off::kAddRef),
                      tick::be_load<uint32_t>(p + tick::itch::off::kAddShares));
            break;
        case 'E':
            do_reduce(tick::be_load<uint64_t>(p + tick::itch::off::kExecRef),
                      tick::be_load<uint32_t>(p + tick::itch::off::kExecShares));
            break;
        case 'C':
            do_reduce(tick::be_load<uint64_t>(p + tick::itch::off::kExecPxRef),
                      tick::be_load<uint32_t>(p + tick::itch::off::kExecPxShares));
            break;
        case 'X':
            do_reduce(tick::be_load<uint64_t>(p + tick::itch::off::kCancelRef),
                      tick::be_load<uint32_t>(p + tick::itch::off::kCancelShares));
            break;
        case 'D': {
            const uint64_t ref = tick::be_load<uint64_t>(p + tick::itch::off::kDeleteRef);
            if (ref != 0 && resting.erase(ref)) push(RefOp::Erase, ref);
            break;
        }
        case 'U': {
            const uint64_t old_ref = tick::be_load<uint64_t>(p + tick::itch::off::kReplaceOldRef);
            const uint64_t new_ref = tick::be_load<uint64_t>(p + tick::itch::off::kReplaceNewRef);
            if (old_ref == 0) break;
            push(RefOp::Lookup, old_ref);
            if (resting.erase(old_ref)) push(RefOp::Erase, old_ref);
            do_insert(new_ref, tick::be_load<uint32_t>(p + tick::itch::off::kReplaceShares));
            break;
        }
        default:
            break;
        }
    }
    return t;
}

[[nodiscard]] inline const RefTrace& ref_trace() {
    static const RefTrace t = build_ref_trace();
    return t;
}

// The erase heavy slice of the same trace. Backward shift deletion is the
// design decision FlatOrderMap exists to justify, and it only shows up when
// erases are a large fraction of the work, so it gets its own fixture rather
// than being averaged into the mixed trace. Every key here is one the preceding
// inserts really put in the table, so no erase is a miss.
[[nodiscard]] inline const std::vector<RefEvent>& erase_heavy_trace() {
    static const std::vector<RefEvent> v = [] {
        const RefTrace&       t = ref_trace();
        std::vector<RefEvent> out;
        out.reserve(t.events.size());
        for (const RefEvent& e : t.events) {
            if (e.op != RefOp::Lookup) out.push_back(e);
        }
        return out;
    }();
    return v;
}

// ---------------------------------------------------------------------------
// Per symbol book streams
// ---------------------------------------------------------------------------

// One book side event for one symbol, already decoded. The book side benchmarks
// replay these straight into a MapSide or a VectorSide with no decoder in the
// way, so the number is the container's and nothing else's.
struct LevelEvent {
    int64_t  price;
    uint32_t shares;
    uint64_t ref;
    // For a remove, the index of the add event in this same vector that put the
    // order on the book. Resolved here, once, so the timed region of the book
    // side benchmark holds no reference lookup at all and measures the level
    // container and nothing else. For an add it is the event's own index.
    uint32_t link;
    bool     is_bid;
    bool     is_add;   // false means the resting order goes away
};

struct SymbolStream {
    uint16_t                locate = 0;
    std::string             ticker;
    uint64_t                message_count = 0;
    std::vector<LevelEvent> events;
};

// The most active symbols in the loaded window, by how many book touching
// messages they carry. Activity is measured and never assumed, because which
// names are busiest in the first minutes of a session is not the same list as
// the ones that are busiest at the close, and picking AAPL by reputation would
// be exactly the kind of guess this project does not make.
[[nodiscard]] inline std::vector<SymbolStream> busiest_symbols(std::size_t how_many,
                                                               std::size_t max_events) {
    const MessageStore& all = messages();

    // Pass one: count book touching messages per locate.
    std::vector<uint64_t> counts(1u << 16, 0);
    // Side and symbol by reference, so a delete can be attributed to the book
    // it belongs to. D, E, C, X and U do not carry either.
    tick::FlatOrderMap<uint64_t> owner(1u << 22);  // ref -> (locate << 1) | is_bid

    auto add_ref = [&](uint64_t ref, uint16_t locate, bool is_bid) {
        if (ref != 0) {
            owner.insert(ref, (static_cast<uint64_t>(locate) << 1) | (is_bid ? 1ull : 0ull));
        }
    };

    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::byte* p    = all.msg(i).data();
        const char       type = tick::itch::msg_type(p);
        const uint16_t   loc  = tick::itch::locate(p);
        switch (type) {
        case 'A':
        case 'F': {
            ++counts[loc];
            add_ref(tick::be_load<uint64_t>(p + tick::itch::off::kAddRef), loc,
                    static_cast<char>(p[tick::itch::off::kAddSide]) == 'B');
            break;
        }
        case 'D': {
            const uint64_t* o = owner.find(tick::be_load<uint64_t>(p + tick::itch::off::kDeleteRef));
            if (o != nullptr) ++counts[static_cast<uint16_t>(*o >> 1)];
            break;
        }
        case 'X': {
            const uint64_t* o = owner.find(tick::be_load<uint64_t>(p + tick::itch::off::kCancelRef));
            if (o != nullptr) ++counts[static_cast<uint16_t>(*o >> 1)];
            break;
        }
        case 'E': {
            const uint64_t* o = owner.find(tick::be_load<uint64_t>(p + tick::itch::off::kExecRef));
            if (o != nullptr) ++counts[static_cast<uint16_t>(*o >> 1)];
            break;
        }
        default:
            break;
        }
    }

    std::vector<std::size_t> order;
    order.reserve(counts.size());
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] > 0) order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return counts[a] > counts[b]; });
    if (order.size() > how_many) order.resize(how_many);

    std::vector<SymbolStream> out;
    out.reserve(order.size());
    for (std::size_t locate : order) {
        SymbolStream s;
        s.locate        = static_cast<uint16_t>(locate);
        const std::string_view tk = all.ticker(static_cast<uint16_t>(locate));
        s.ticker        = tk.empty() ? ("locate" + std::to_string(locate)) : std::string(tk);
        s.message_count = counts[locate];
        out.push_back(std::move(s));
    }

    // Pass two: collect the real add and remove stream for the chosen symbols.
    // Price, share count and reference all come off the wire.
    std::vector<int> want(1u << 16, -1);
    for (std::size_t k = 0; k < out.size(); ++k) want[out[k].locate] = static_cast<int>(k);

    struct Resting {
        int64_t  price;
        uint32_t shares;
        uint16_t locate;
        bool     is_bid;
        int64_t  event_index;  // where the add landed in its symbol's vector, or -1
    };
    tick::FlatOrderMap<uint64_t> live_idx(1u << 22);   // ref -> index into resting
    std::vector<Resting>         resting;
    resting.reserve(1u << 20);

    auto remember = [&](uint64_t ref, const Resting& r) {
        if (ref == 0) return;
        live_idx.insert(ref, static_cast<uint64_t>(resting.size()));
        resting.push_back(r);
    };
    auto recall = [&](uint64_t ref) -> const Resting* {
        if (ref == 0) return nullptr;
        const uint64_t* idx = live_idx.find(ref);
        return idx == nullptr ? nullptr : &resting[static_cast<std::size_t>(*idx)];
    };

    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::byte* p    = all.msg(i).data();
        const char       type = tick::itch::msg_type(p);
        switch (type) {
        case 'A':
        case 'F': {
            const uint16_t loc    = tick::itch::locate(p);
            const uint64_t ref    = tick::be_load<uint64_t>(p + tick::itch::off::kAddRef);
            const bool     is_bid = static_cast<char>(p[tick::itch::off::kAddSide]) == 'B';
            Resting        r{static_cast<int64_t>(tick::be_load<uint32_t>(p + tick::itch::off::kAddPrice)),
                             tick::be_load<uint32_t>(p + tick::itch::off::kAddShares), loc, is_bid, -1};
            const int      k = want[loc];
            if (k >= 0 && out[static_cast<std::size_t>(k)].events.size() < max_events) {
                std::vector<LevelEvent>& ev = out[static_cast<std::size_t>(k)].events;
                r.event_index = static_cast<int64_t>(ev.size());
                ev.push_back(LevelEvent{r.price, r.shares, ref,
                                        static_cast<uint32_t>(ev.size()), is_bid, true});
            }
            remember(ref, r);
            break;
        }
        case 'D': {
            const uint64_t ref = tick::be_load<uint64_t>(p + tick::itch::off::kDeleteRef);
            const Resting* r   = recall(ref);
            if (r == nullptr) break;
            const int k = want[r->locate];
            if (k >= 0 && r->event_index >= 0 &&
                out[static_cast<std::size_t>(k)].events.size() < max_events) {
                out[static_cast<std::size_t>(k)].events.push_back(
                    LevelEvent{r->price, r->shares, ref,
                               static_cast<uint32_t>(r->event_index), r->is_bid, false});
            }
            live_idx.erase(ref);
            break;
        }
        default:
            break;
        }
    }
    return out;
}

[[nodiscard]] inline const std::vector<SymbolStream>& symbol_streams() {
    static const std::vector<SymbolStream> v = busiest_symbols(8, 400'000);
    return v;
}

// ---------------------------------------------------------------------------
// Machine label
// ---------------------------------------------------------------------------
//
// The label travels with the numbers. A chart or a table that has been
// separated from the machine it was measured on is not a result, so these keys
// go into the Google Benchmark JSON context block and scripts/plot_bench.py
// prints them on every chart. The field names are the ones
// scripts/box_label.sh emits, so a results file written by either can be
// compared field by field.
//
// Anything that cannot be read says "unknown". Nothing here is defaulted to a
// plausible value. In particular "pinned" is reported false, because this suite
// does not pin and macOS could not honour it if it did.
struct MachineContext {
    MachineContext() {
        std::string cpu = "unknown", os = "unknown", kernel = "unknown";
        std::string cores = "0";

#if defined(__APPLE__)
        auto sysctl_str = [](const char* name) -> std::string {
            std::size_t n = 0;
            if (::sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return {};
            std::string v(n, '\0');
            if (::sysctlbyname(name, v.data(), &n, nullptr, 0) != 0) return {};
            while (!v.empty() && (v.back() == '\0' || v.back() == '\n')) v.pop_back();
            return v;
        };
        auto sysctl_int = [](const char* name) -> long {
            long        v = 0;
            std::size_t n = sizeof(v);
            if (::sysctlbyname(name, &v, &n, nullptr, 0) != 0) return 0;
            return v;
        };
        if (std::string v = sysctl_str("machdep.cpu.brand_string"); !v.empty()) cpu = v;
        if (std::string v = sysctl_str("kern.osproductversion"); !v.empty()) os = "macOS " + v;
        if (std::string v = sysctl_str("kern.osrelease"); !v.empty()) kernel = "Darwin " + v;
        if (long n = sysctl_int("hw.logicalcpu"); n > 0) cores = std::to_string(n);
#endif

        benchmark::AddCustomContext("cpu_model", cpu);
        benchmark::AddCustomContext("os", os);
        benchmark::AddCustomContext("kernel", kernel);
        benchmark::AddCustomContext("cores", cores);
        benchmark::AddCustomContext("compiler", compiler_string());
        benchmark::AddCustomContext("build_type", env_or("TICK_BUILD_TYPE", "unknown"));
        // Not a setting that was left off. There is no per core pinning on this
        // platform at all, so every tail number from this box is scheduler noise
        // on top of the structure being measured.
        benchmark::AddCustomContext("pinned", "false");
        benchmark::AddCustomContext("isolated", "false");
        benchmark::AddCustomContext("nohz_full", "false");
        benchmark::AddCustomContext("governor", "unknown");
        benchmark::AddCustomContext("data_file", env_or("TICK_ITCH_FILE", kDefaultItchPath));
        benchmark::AddCustomContext(
            "notes",
            "unpinned run: this platform offers no per core pinning, so the median is the "
            "honest measure and the tail is operating system scheduling noise");
    }

    [[nodiscard]] static std::string compiler_string() {
#if defined(__apple_build_version__)
        return "Apple clang " + std::to_string(__clang_major__) + "." +
               std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__clang__)
        return "clang " + std::to_string(__clang_major__) + "." +
               std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
        return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
        return "unknown";
#endif
    }
};

// Each benchmark translation unit registers the label from a namespace scope
// initialiser declared ahead of its BENCHMARK registrations, so the context is
// in place before the reporter writes it, whichever main runs the suite. A
// function local static rather than an inline variable, because an inline
// variable's dynamic initialisation may be deferred until it is first used and
// nothing ever uses this one.
inline bool register_machine_context() {
    static const MachineContext ctx{};
    (void)ctx;
    return true;
}

} // namespace tickbench
