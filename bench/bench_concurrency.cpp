// Concurrency microbenchmarks.
//
// MACHINE AND PINNING, WHICH DECIDES HOW TO READ EVERY NUMBER BELOW. These
// numbers are UNPINNED. macOS offers no way for a user space program to bind a
// thread to a particular core, so there is no affinity call that was skipped
// here and no flag that would turn one on. Every thread in every case below is
// at the mercy of the scheduler, which is free to move it between performance
// and efficiency cores mid run. Read the median as the honest measure of the
// structure and read the tail as operating system scheduling noise rather than
// as a property of the code. That caveat is heavier here than in any other
// benchmark in this project, because a multi threaded queue result is exactly
// the kind of number that moves when two threads land on the same core or on
// cores that do not share a cache level.
//
// This file deliberately does not include bench_common.hpp. The queues here
// carry a fixed sixteen byte payload rather than a decoded ITCH message,
// because what is being measured is the cost of moving one item between
// threads and not the cost of the item. Feeding real messages through would
// add a variable that has nothing to do with the question.
//
// WHAT IS BEING COMPARED.
//
//   Throughput. One producer and one consumer through the SPSC ring, through
//   this queue, and through a mutex and a std::deque, then the same queues with
//   several producers and several consumers. The single producer case against
//   this queue is the one that prices what multi producer support costs when it
//   is not being used, which is the number that decides whether the SPSC ring
//   stays the default.
//
//   Round trip latency. An item pushed by one thread, taken by a second, and
//   returned. This is the number that matters for a feed handler, because the
//   question is never how many messages a queue can move in a second, it is how
//   long a single message waits. A structure can win throughput and lose this.
//
//   False sharing. The same queue with the two positions packed into one cache
//   line against the same queue with them separated, plus a stripped down two
//   counter case that isolates the effect with nothing else in the way. The
//   claim that padding matters is worth nothing without the measurement beside
//   it.
//
//   The seqlock against a mutex protected struct, both publishing a top of book
//   snapshot. Measured from the writer's side, because a seqlock exists to make
//   the writer's cost independent of how many readers there are, and from the
//   reader's side under a live writer, because that is the case a reader faces.

#include "tick/mpmc_queue.hpp"
#include "tick/seqlock.hpp"

#include "nano/spsc_queue.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

// Sixteen bytes, which is about what a decoded book update costs and small
// enough that the queue rather than the copy is what is being measured.
struct Tick {
    uint64_t ts    = 0;
    int32_t  price = 0;
    uint32_t size  = 0;
};

// Forty bytes, too wide for any atomic on this target, which is the case the
// seqlock exists for. A struct that fitted in one atomic would not need one.
struct TopOfBook {
    int64_t  bid_px   = 0;
    int64_t  ask_px   = 0;
    uint32_t bid_size = 0;
    uint32_t ask_size = 0;
    uint64_t ts       = 0;
    uint64_t seqno    = 0;
};

constexpr std::size_t kRing  = 1024;    // slots, for every ring based case
constexpr std::size_t kItems = 200'000; // items moved per benchmark iteration

// ---------------------------------------------------------------------------
// The deliberately unpadded queue, for the false sharing case
// ---------------------------------------------------------------------------
//
// This is a copy of tick::MpmcQueue with one change. The two positions sit next
// to each other in one cache line instead of on their own. Everything else,
// including the per slot padding and every memory ordering, is identical, so
// the difference between this case and the padded one is the layout and
// nothing else. It lives here rather than as a template parameter on the real
// queue because the production header should not carry a knob whose only
// setting is wrong.
template <typename T, std::size_t Capacity>
class PackedMpmcQueue {
public:
    static constexpr std::size_t kMask = Capacity - 1;

    PackedMpmcQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_push(const T& item) noexcept {
        std::size_t pos = pos_.tail.load(std::memory_order_relaxed);
        for (;;) {
            Slot&               slot = slots_[pos & kMask];
            const std::size_t   seq  = slot.seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (pos_.tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                    slot.value = item;
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = pos_.tail.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        std::size_t pos = pos_.head.load(std::memory_order_relaxed);
        for (;;) {
            Slot&               slot = slots_[pos & kMask];
            const std::size_t   seq  = slot.seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (pos_.head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                    out = slot.value;
                    slot.seq.store(pos + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = pos_.head.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Slot {
        alignas(tick::kCacheLineSize) std::atomic<std::size_t> seq;
        T value{};
    };

    // Both positions in one line, on purpose. This is the layout the padded
    // queue exists to avoid.
    struct alignas(tick::kCacheLineSize) Positions {
        std::atomic<std::size_t> head{0};
        std::atomic<std::size_t> tail{0};
    };

    Positions pos_;
    alignas(tick::kCacheLineSize) Slot slots_[Capacity];
};

// ---------------------------------------------------------------------------
// The baseline everybody writes first
// ---------------------------------------------------------------------------
//
// A std::mutex around a std::deque. This is the honest comparison point,
// because it is what the code being replaced actually looks like, and because
// a lock free structure that does not beat it has not earned its complexity.
// The bound is enforced so it is compared on the same terms as the rings and
// cannot win by growing without limit.
template <typename T>
class MutexDeque {
public:
    explicit MutexDeque(std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool try_push(const T& item) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.size() >= capacity_) return false;
        q_.push_back(item);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex     m_;
    std::deque<T>  q_;
    std::size_t    capacity_;
};

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------

// Move kItems through a queue with the given number of producers and
// consumers, and return nothing, since the benchmark measures the wall time of
// the whole transfer. Producers spin on a full queue and consumers spin on an
// empty one, because a benchmark that dropped items would be measuring a
// different structure than the one under test.
template <typename Queue>
void transfer(Queue& q, std::size_t producers, std::size_t consumers, std::size_t items) {
    std::atomic<std::size_t> popped{0};
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    const std::size_t per_producer = items / producers;
    const std::size_t total        = per_producer * producers;

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&q, per_producer] {
            Tick t{};
            for (std::size_t i = 0; i < per_producer; ++i) {
                t.ts = i;
                while (!q.try_push(t)) {
                }
            }
        });
    }
    for (std::size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&q, &popped, total] {
            Tick t{};
            while (popped.load(std::memory_order_relaxed) < total) {
                if (q.try_pop(t)) {
                    benchmark::DoNotOptimize(t);
                    popped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();
}

// One item out and back. Thread A pushes into the forward queue and waits on
// the return queue, thread B does the reverse. The reported time is one full
// round trip, so half of it is one hop, which is the number to compare against
// a wire.
template <typename Queue>
void round_trip(Queue& forward, Queue& back, std::size_t rounds) {
    std::thread echo([&forward, &back, rounds] {
        Tick t{};
        for (std::size_t i = 0; i < rounds; ++i) {
            while (!forward.try_pop(t)) {
            }
            while (!back.try_push(t)) {
            }
        }
    });

    Tick t{};
    for (std::size_t i = 0; i < rounds; ++i) {
        t.ts = i;
        while (!forward.try_push(t)) {
        }
        while (!back.try_pop(t)) {
        }
        benchmark::DoNotOptimize(t);
    }
    echo.join();
}

// ---------------------------------------------------------------------------
// Throughput
// ---------------------------------------------------------------------------

void BM_Spsc_1p1c(benchmark::State& state) {
    for (auto _ : state) {
        // Constructed inside the loop so every iteration starts from an empty
        // ring and from cold positions, which is the same starting condition
        // every other case gets.
        auto q = std::make_unique<nano::SPSCQueue<Tick, kRing>>();
        transfer(*q, 1, 1, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_Spsc_1p1c)->UseRealTime();

void BM_Mpmc_1p1c(benchmark::State& state) {
    for (auto _ : state) {
        auto q = std::make_unique<tick::MpmcQueue<Tick, kRing>>();
        transfer(*q, 1, 1, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_Mpmc_1p1c)->UseRealTime();

void BM_MutexDeque_1p1c(benchmark::State& state) {
    for (auto _ : state) {
        auto q = std::make_unique<MutexDeque<Tick>>(kRing);
        transfer(*q, 1, 1, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_MutexDeque_1p1c)->UseRealTime();

void BM_Mpmc_NpNc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto q = std::make_unique<tick::MpmcQueue<Tick, kRing>>();
        transfer(*q, n, n, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_Mpmc_NpNc)->Arg(2)->Arg(4)->Arg(6)->UseRealTime();

void BM_MutexDeque_NpNc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto q = std::make_unique<MutexDeque<Tick>>(kRing);
        transfer(*q, n, n, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_MutexDeque_NpNc)->Arg(2)->Arg(4)->Arg(6)->UseRealTime();

// ---------------------------------------------------------------------------
// False sharing
// ---------------------------------------------------------------------------

void BM_Mpmc_Packed_1p1c(benchmark::State& state) {
    for (auto _ : state) {
        auto q = std::make_unique<PackedMpmcQueue<Tick, kRing>>();
        transfer(*q, 1, 1, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_Mpmc_Packed_1p1c)->UseRealTime();

void BM_Mpmc_Packed_NpNc(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto q = std::make_unique<PackedMpmcQueue<Tick, kRing>>();
        transfer(*q, n, n, kItems);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kItems);
}
BENCHMARK(BM_Mpmc_Packed_NpNc)->Arg(2)->Arg(4)->UseRealTime();

// The effect with everything else stripped away. Two threads, two counters,
// each thread touching only its own counter, so the program has no shared
// variable at all. The only thing the two threads can contend on is the cache
// line. Three separations are measured because sixty four bytes is the common
// line size but not a universal one, and a case that separated by less than
// the real line would look exactly like the packed case.
struct SharedCounters {
    std::atomic<uint64_t> a{0};
    std::atomic<uint64_t> b{0};
};
struct Separated64 {
    alignas(64) std::atomic<uint64_t> a{0};
    alignas(64) std::atomic<uint64_t> b{0};
};
struct Separated128 {
    alignas(128) std::atomic<uint64_t> a{0};
    alignas(128) std::atomic<uint64_t> b{0};
};

template <typename Counters>
void counter_pair(benchmark::State& state, std::size_t bumps) {
    for (auto _ : state) {
        auto        c = std::make_unique<Counters>();
        std::thread other([&c, bumps] {
            for (std::size_t i = 0; i < bumps; ++i) {
                c->b.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (std::size_t i = 0; i < bumps; ++i) {
            c->a.fetch_add(1, std::memory_order_relaxed);
        }
        other.join();
        benchmark::DoNotOptimize(c->a.load(std::memory_order_relaxed));
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2 *
                            static_cast<int64_t>(bumps));
}

constexpr std::size_t kBumps = 2'000'000;

void BM_FalseSharing_SameLine(benchmark::State& state) { counter_pair<SharedCounters>(state, kBumps); }
BENCHMARK(BM_FalseSharing_SameLine)->UseRealTime();

void BM_FalseSharing_Split64(benchmark::State& state) { counter_pair<Separated64>(state, kBumps); }
BENCHMARK(BM_FalseSharing_Split64)->UseRealTime();

void BM_FalseSharing_Split128(benchmark::State& state) { counter_pair<Separated128>(state, kBumps); }
BENCHMARK(BM_FalseSharing_Split128)->UseRealTime();

// ---------------------------------------------------------------------------
// Round trip latency
// ---------------------------------------------------------------------------

constexpr std::size_t kRounds = 50'000;

// The mutex case gets far fewer rounds, and the reason is itself a result. Two
// threads spinning on try_pop against a mutex protected deque spend most of
// their time acquiring the lock only to find the queue empty, releasing it, and
// immediately contending for it again, which is a convoy and not a queue. Each
// round trip there costs orders of magnitude more than through either ring, so
// the same round count would make this one case dominate the whole suite's
// runtime. Both numbers are reported per item, so the comparison is unaffected.
constexpr std::size_t kMutexRounds = 2'000;

void BM_Spsc_RoundTrip(benchmark::State& state) {
    auto fwd = std::make_unique<nano::SPSCQueue<Tick, 64>>();
    auto bck = std::make_unique<nano::SPSCQueue<Tick, 64>>();
    for (auto _ : state) {
        round_trip(*fwd, *bck, kRounds);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRounds);
}
BENCHMARK(BM_Spsc_RoundTrip)->UseRealTime();

void BM_Mpmc_RoundTrip(benchmark::State& state) {
    auto fwd = std::make_unique<tick::MpmcQueue<Tick, 64>>();
    auto bck = std::make_unique<tick::MpmcQueue<Tick, 64>>();
    for (auto _ : state) {
        round_trip(*fwd, *bck, kRounds);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRounds);
}
BENCHMARK(BM_Mpmc_RoundTrip)->UseRealTime();

void BM_MutexDeque_RoundTrip(benchmark::State& state) {
    auto fwd = std::make_unique<MutexDeque<Tick>>(64);
    auto bck = std::make_unique<MutexDeque<Tick>>(64);
    for (auto _ : state) {
        round_trip(*fwd, *bck, kMutexRounds);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kMutexRounds);
}
BENCHMARK(BM_MutexDeque_RoundTrip)->UseRealTime();

// ---------------------------------------------------------------------------
// Top of book publish and read
// ---------------------------------------------------------------------------

// A mutex protected struct, which is what this would otherwise be.
class MutexTopOfBook {
public:
    void store(const TopOfBook& v) {
        std::lock_guard<std::mutex> lock(m_);
        v_ = v;
    }
    [[nodiscard]] TopOfBook load() const {
        std::lock_guard<std::mutex> lock(m_);
        return v_;
    }

private:
    mutable std::mutex m_;
    TopOfBook          v_{};
};

// The writer's cost with nobody reading, which is the floor.
void BM_SeqLock_PublishUncontended(benchmark::State& state) {
    tick::SeqLock<TopOfBook> sl;
    TopOfBook                v{};
    for (auto _ : state) {
        v.seqno++;
        benchmark::DoNotOptimize(v);
        sl.store(v);
        // Without this the whole loop disappears. Nothing in this function
        // reads the seqlock back, so the compiler is entitled to delete every
        // store into it, and a benchmark that reported a free publish would be
        // reporting the optimiser rather than the structure. The mutex case
        // below carries the same two calls so the two are measured on the same
        // terms rather than one of them paying for a barrier the other avoids.
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SeqLock_PublishUncontended);

void BM_Mutex_PublishUncontended(benchmark::State& state) {
    MutexTopOfBook tob;
    TopOfBook      v{};
    for (auto _ : state) {
        v.seqno++;
        benchmark::DoNotOptimize(v);
        tob.store(v);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Mutex_PublishUncontended);

// The writer's cost with readers hammering the same value. This pair is the
// point of the whole structure. The seqlock writer should not notice the
// readers at all, because no reader ever takes anything the writer needs. The
// mutex writer has to queue behind every reader that holds the lock.
template <typename Publisher>
void publish_under_readers(benchmark::State& state, int readers) {
    Publisher         pub;
    std::atomic<bool> stop{false};

    std::vector<std::thread> rs;
    rs.reserve(static_cast<std::size_t>(readers));
    for (int i = 0; i < readers; ++i) {
        rs.emplace_back([&pub, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                TopOfBook snap = pub.load();
                benchmark::DoNotOptimize(snap);
            }
        });
    }

    TopOfBook v{};
    for (auto _ : state) {
        v.seqno++;
        pub.store(v);
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& t : rs) t.join();
    state.SetItemsProcessed(state.iterations());
}

void BM_SeqLock_PublishUnderReaders(benchmark::State& state) {
    publish_under_readers<tick::SeqLock<TopOfBook>>(state, static_cast<int>(state.range(0)));
}
BENCHMARK(BM_SeqLock_PublishUnderReaders)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();

void BM_Mutex_PublishUnderReaders(benchmark::State& state) {
    publish_under_readers<MutexTopOfBook>(state, static_cast<int>(state.range(0)));
}
BENCHMARK(BM_Mutex_PublishUnderReaders)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();

// The reader's cost while a writer is running flat out, which is the case a
// strategy thread actually sees.
template <typename Publisher>
void read_under_writer(benchmark::State& state) {
    Publisher         pub;
    std::atomic<bool> stop{false};

    std::thread writer([&pub, &stop] {
        TopOfBook v{};
        while (!stop.load(std::memory_order_relaxed)) {
            v.seqno++;
            v.bid_px = static_cast<int64_t>(v.seqno);
            v.ask_px = static_cast<int64_t>(v.seqno) + 1;
            pub.store(v);
        }
    });

    for (auto _ : state) {
        TopOfBook snap = pub.load();
        benchmark::DoNotOptimize(snap);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    state.SetItemsProcessed(state.iterations());
}

// The seqlock reader gets its own body rather than going through the template,
// so it can report how often it had to retry. That counter is what makes the
// number underneath it readable. The writer here publishes in a tight loop with
// no work at all between updates, which is the worst case a reader can face and
// is not what a book thread does, so a high retry rate is the expected result
// and not a defect. Reported per read so it can be compared across runs.
void BM_SeqLock_ReadUnderWriter(benchmark::State& state) {
    tick::SeqLock<TopOfBook> sl;
    std::atomic<bool>        stop{false};

    std::thread writer([&sl, &stop] {
        TopOfBook v{};
        while (!stop.load(std::memory_order_relaxed)) {
            v.seqno++;
            v.bid_px = static_cast<int64_t>(v.seqno);
            v.ask_px = static_cast<int64_t>(v.seqno) + 1;
            sl.store(v);
        }
    });

    uint64_t retries = 0;
    for (auto _ : state) {
        TopOfBook snap{};
        while (!sl.try_load(snap)) ++retries;
        benchmark::DoNotOptimize(snap);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    state.SetItemsProcessed(state.iterations());
    state.counters["retries_per_read"] =
        benchmark::Counter(static_cast<double>(retries) /
                           static_cast<double>(state.iterations()));
}
BENCHMARK(BM_SeqLock_ReadUnderWriter)->UseRealTime();

void BM_Mutex_ReadUnderWriter(benchmark::State& state) {
    read_under_writer<MutexTopOfBook>(state);
}
BENCHMARK(BM_Mutex_ReadUnderWriter)->UseRealTime();

// The same reader against a writer that does a little work between publishes,
// which is what a book thread does. This case exists because the flat out case
// above is easy to misread. A seqlock is for a value that is read far more
// often than it is written, and when the writer leaves no gap at all the reader
// spends its life retrying. Pacing the writer by a few hundred cycles is still
// a far higher update rate than any real top of book, and it is enough to show
// what a reader actually pays. The retry counter is reported here too so the
// two cases can be compared on the thing that explains them.
void BM_SeqLock_ReadUnderPacedWriter(benchmark::State& state) {
    tick::SeqLock<TopOfBook> sl;
    std::atomic<bool>        stop{false};
    const auto               pace = static_cast<std::size_t>(state.range(0));

    std::thread writer([&sl, &stop, pace] {
        TopOfBook v{};
        while (!stop.load(std::memory_order_relaxed)) {
            v.seqno++;
            v.bid_px = static_cast<int64_t>(v.seqno);
            v.ask_px = static_cast<int64_t>(v.seqno) + 1;
            sl.store(v);
            // Stand in for the book work between two top of book changes. It
            // is a dependent chain so the compiler cannot hoist or shorten it.
            uint64_t spin = v.seqno;
            for (std::size_t i = 0; i < pace; ++i) {
                spin = spin * 6364136223846793005ull + 1442695040888963407ull;
                benchmark::DoNotOptimize(spin);
            }
        }
    });

    uint64_t retries = 0;
    for (auto _ : state) {
        TopOfBook snap{};
        while (!sl.try_load(snap)) ++retries;
        benchmark::DoNotOptimize(snap);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    state.SetItemsProcessed(state.iterations());
    state.counters["retries_per_read"] =
        benchmark::Counter(static_cast<double>(retries) /
                           static_cast<double>(state.iterations()));
}
BENCHMARK(BM_SeqLock_ReadUnderPacedWriter)->Arg(16)->Arg(64)->Arg(256)->UseRealTime();

} // namespace

BENCHMARK_MAIN();
