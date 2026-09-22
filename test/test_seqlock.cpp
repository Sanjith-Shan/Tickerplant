// The seqlock.
//
// A seqlock test that only checks that a reader eventually sees the last value
// proves almost nothing, because a seqlock with every fence deleted still
// passes it. The property worth testing is the one the structure exists to
// provide, which is that a reader never returns a mixture of two values. So the
// payload here is self checking. Every field is a function of one counter and
// the struct carries a checksum over all of them, so a snapshot that mixes two
// writes fails arithmetic that no amount of scheduling luck can repair.
//
// The struct is also deliberately far wider than any atomic on this target, so
// the writer's store really is several stores and there really is a window in
// which a reader can see half of it. A sixteen byte payload would be a weaker
// test on arm64, where a pair of registers can be stored together.
//
// EXPECT A THREADSANITIZER REPORT FROM THIS FILE. The seqlock reads the value
// bytes while the writer is writing them, with no atomic on those bytes, which
// is a data race by the letter of the C++ memory model even though the fences
// make the algorithm correct. ThreadSanitizer is right and the header says so
// at length. The report is a property of the technique, not a defect in this
// code, and it is left visible rather than suppressed so that anyone reading
// the suite is forced to meet the argument.

#include "tick/seqlock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

using tick::SeqLock;

namespace {

// Shaped like a real top of book snapshot and wide enough that no single
// instruction can write it. Every field is derived from tick_id, and checksum
// closes the loop, so consistent() is a complete statement of "this snapshot is
// exactly one of the values the writer published".
struct TopOfBook {
    uint64_t tick_id  = 0;
    int64_t  bid_px   = 0;
    int64_t  ask_px   = 0;
    uint32_t bid_size = 0;
    uint32_t ask_size = 0;
    uint64_t venue_ts = 0;
    uint64_t checksum = 0;

    static TopOfBook make(uint64_t k) noexcept {
        TopOfBook b;
        b.tick_id  = k;
        b.bid_px   = static_cast<int64_t>(1'000'000 + (k % 5000));
        b.ask_px   = b.bid_px + 1 + static_cast<int64_t>(k % 7);
        b.bid_size = static_cast<uint32_t>(100 + (k % 900));
        b.ask_size = static_cast<uint32_t>(200 + (k % 800));
        b.venue_ts = k * 1'000'003ull;
        b.checksum = fold(b);
        return b;
    }

    // Deliberately a mixing function and not a sum. A sum over fields that all
    // move together would collide often enough that a torn snapshot could pass,
    // and a test that can pass while broken is worse than no test.
    static uint64_t fold(const TopOfBook& b) noexcept {
        uint64_t h = 0xcbf29ce484222325ull;
        auto     mix = [&h](uint64_t v) {
            h ^= v;
            h *= 0x100000001b3ull;
            h ^= h >> 29;
        };
        mix(b.tick_id);
        mix(static_cast<uint64_t>(b.bid_px));
        mix(static_cast<uint64_t>(b.ask_px));
        mix(b.bid_size);
        mix(b.ask_size);
        mix(b.venue_ts);
        return h;
    }

    [[nodiscard]] bool consistent() const noexcept {
        // Two independent checks. The checksum catches any mixture of two
        // writes, and rebuilding from tick_id catches the case where a torn
        // read somehow produced a self consistent checksum over fields that
        // were never published together.
        if (checksum != fold(*this)) return false;
        const TopOfBook expected = make(tick_id);
        return expected.bid_px == bid_px && expected.ask_px == ask_px &&
               expected.bid_size == bid_size && expected.ask_size == ask_size &&
               expected.venue_ts == venue_ts;
    }
};

static_assert(std::is_trivially_copyable_v<TopOfBook>);
static_assert(sizeof(TopOfBook) > 16, "the payload must be too wide for one store");

// A payload with no interesting structure at all, used only to prove the round
// trip copies every byte including any tail the compiler pads in.
struct WideBlob {
    unsigned char bytes[96]{};
};

} // namespace

TEST(SeqLock, SingleThreadedRoundTrip) {
    SeqLock<TopOfBook> sl;

    // A freshly constructed seqlock reads as a zeroed value rather than as
    // garbage, which matters because a strategy may read top of book before the
    // first message of the day arrives.
    const TopOfBook initial = sl.load();
    EXPECT_EQ(initial.tick_id, 0u);
    EXPECT_EQ(sl.sequence(), 0u);

    for (uint64_t k = 1; k <= 1000; ++k) {
        sl.store(TopOfBook::make(k));
        const TopOfBook got = sl.load();
        EXPECT_EQ(got.tick_id, k);
        EXPECT_TRUE(got.consistent());
    }

    // Two counter steps per write, and even after every one of them.
    EXPECT_EQ(sl.sequence(), 2000u);
    EXPECT_EQ(sl.sequence() % 2, 0u);
}

TEST(SeqLock, TryLoadSucceedsWhenNoWriterIsRunning) {
    SeqLock<TopOfBook> sl(TopOfBook::make(42));
    TopOfBook          out{};
    // With no concurrent writer a single attempt must always succeed. A
    // try_load that can fail against a quiet writer would mean the counter is
    // being left odd somewhere.
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(sl.try_load(out));
        EXPECT_EQ(out.tick_id, 42u);
        EXPECT_TRUE(out.consistent());
    }
}

TEST(SeqLock, CopiesEveryByteOfAWidePayload) {
    SeqLock<WideBlob> sl;
    WideBlob          in;
    for (std::size_t i = 0; i < sizeof(in.bytes); ++i) {
        in.bytes[i] = static_cast<unsigned char>(i * 7 + 1);
    }
    sl.store(in);

    const WideBlob out = sl.load();
    for (std::size_t i = 0; i < sizeof(in.bytes); ++i) {
        ASSERT_EQ(out.bytes[i], in.bytes[i]) << "byte " << i << " did not survive the round trip";
    }
}

// ThreadSanitizer instruments every load and store in this file, and the two
// contention tests below are deliberately the densest code in the suite. A
// million writes against six spinning readers does not finish inside any
// sensible ctest timeout once that instrumentation is on, so the counts come
// down under the sanitizer.
//
// Nothing is lost by that. TSan is looking for a missing fence, which is a
// property of the code rather than of how many times it runs, and its own
// slowdown widens the window between the sequence counter and the payload so
// the interleavings are denser per iteration rather than sparser. The full
// counts still run in every build that is not sanitized, which is where the
// rare interleaving argument in the comment at the top of this file applies.
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define TICK_TEST_UNDER_TSAN 1
#  endif
#endif
#if !defined(TICK_TEST_UNDER_TSAN) && defined(__SANITIZE_THREAD__)
#  define TICK_TEST_UNDER_TSAN 1
#endif
#if !defined(TICK_TEST_UNDER_TSAN)
#  define TICK_TEST_UNDER_TSAN 0
#endif

TEST(SeqLock, ReadersNeverSeeAMixtureOfTwoWrites) {
    // One writer, six readers, which is the real shape. The writer runs flat
    // out so the readers race it constantly rather than occasionally, since a
    // seqlock whose fences are wrong is still almost always right and only a
    // very high write rate turns "almost" into a failure the test can catch.
    constexpr int      kReaders    = 6;
    constexpr uint64_t kWrites     = TICK_TEST_UNDER_TSAN ? 20'000 : 1'000'000;

    SeqLock<TopOfBook> sl(TopOfBook::make(0));
    std::atomic<bool>  writing{true};

    std::atomic<uint64_t> torn{0};          // must stay zero
    std::atomic<uint64_t> observed{0};      // clean snapshots, for the record
    std::atomic<uint64_t> retries{0};       // try_load failures, expected to be nonzero
    std::atomic<uint64_t> max_tick{0};

    std::thread writer([&] {
        for (uint64_t k = 1; k <= kWrites; ++k) {
            sl.store(TopOfBook::make(k));
        }
        writing.store(false, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            uint64_t local_bad = 0, local_ok = 0, local_retry = 0, local_max = 0;
            uint64_t last_seen = 0;
            while (writing.load(std::memory_order_acquire)) {
                // Half the reads go through the retrying path and half through
                // the non retrying one, so both are exercised under the same
                // contention rather than only the easy one.
                TopOfBook snap{};
                if ((local_ok & 1u) == 0u) {
                    snap = sl.load();
                } else if (!sl.try_load(snap)) {
                    ++local_retry;
                    continue;
                }

                if (!snap.consistent()) ++local_bad;
                ++local_ok;
                if (snap.tick_id > local_max) local_max = snap.tick_id;

                // A published value must never go backwards, because a single
                // writer publishes strictly increasing tick ids. Seeing a
                // smaller one than this reader already accepted would mean a
                // stale value slipped through a counter check.
                if (snap.tick_id < last_seen) ++local_bad;
                last_seen = snap.tick_id;
            }
            torn.fetch_add(local_bad, std::memory_order_relaxed);
            observed.fetch_add(local_ok, std::memory_order_relaxed);
            retries.fetch_add(local_retry, std::memory_order_relaxed);
            uint64_t prev = max_tick.load(std::memory_order_relaxed);
            while (local_max > prev &&
                   !max_tick.compare_exchange_weak(prev, local_max,
                                                   std::memory_order_relaxed)) {
            }
        });
    }

    writer.join();
    for (std::thread& t : readers) t.join();

    EXPECT_EQ(torn.load(), 0u) << "a reader accepted a snapshot that was a mixture of two writes";
    EXPECT_GT(observed.load(), 0u) << "the readers never got a snapshot at all";

    // The final value must be the last one written, and it must be clean.
    const TopOfBook final_value = sl.load();
    EXPECT_EQ(final_value.tick_id, kWrites);
    EXPECT_TRUE(final_value.consistent());
}

TEST(SeqLock, WriterIsNeverBlockedByReaders) {
    // The defining property. The writer completes a fixed number of stores
    // while readers hammer the same value, and it is asserted only that it
    // finishes, because the timing is scheduler dependent and a threshold on it
    // would be a flaky test rather than a measurement. The measurement lives in
    // bench/bench_concurrency.cpp where it belongs.
    constexpr int      kReaders = 4;
    constexpr uint64_t kWrites  = TICK_TEST_UNDER_TSAN ? 20'000 : 200'000;

    SeqLock<TopOfBook>    sl(TopOfBook::make(0));
    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> bad{0};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            uint64_t local_bad = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const TopOfBook snap = sl.load();
                if (!snap.consistent()) ++local_bad;
            }
            bad.fetch_add(local_bad, std::memory_order_relaxed);
        });
    }

    for (uint64_t k = 1; k <= kWrites; ++k) {
        sl.store(TopOfBook::make(k));
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& t : readers) t.join();

    EXPECT_EQ(bad.load(), 0u);
    EXPECT_EQ(sl.load().tick_id, kWrites);
}
