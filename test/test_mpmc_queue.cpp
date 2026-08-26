// The bounded multi producer multi consumer queue.
//
// The single threaded tests here are the cheap ones and they exist to pin down
// the ring arithmetic, because a queue that is wrong at the wrap is wrong in a
// way that only appears after the four billionth message on a quiet Tuesday.
// The tests that matter are the concurrent ones, and they check three separate
// properties that a broken memory ordering breaks in different ways.
//
// Conservation. Every item pushed is popped exactly once. A lost item means a
// producer overwrote a slot a consumer had not finished with, and a duplicated
// item means two consumers claimed one position.
//
// Per producer order. This is the ordering property the queue actually promises
// and it is weaker than people assume. There is no global FIFO across producers,
// because two producers racing for a position can end up in either order and
// that is not a bug. What must hold is that a single producer's items reach any
// one consumer in the order it pushed them, since positions are handed out in
// increasing order and each consumer works its own positions in increasing
// order. With one consumer that becomes a plain global check.
//
// Contention. The tiny queue cases run a ring of two or four slots under eight
// threads, so it is full and empty many times per millisecond and almost every
// push and pop takes the retry path. That is where a relaxed load that should
// have been an acquire actually shows up, and it is the reason these cases
// exist separately from the large ones.
//
// These tests are run under ThreadSanitizer, which is the only tool that will
// catch the ordering mistakes on a machine whose hardware will happily reorder
// what x86 would not.

#include "tick/mpmc_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using tick::MpmcQueue;

namespace {

// Eight bytes, trivially copyable, and self describing. Every item carries the
// producer that made it and that producer's own running index, which is what
// makes both conservation and per producer order checkable after the fact
// without any shared bookkeeping during the run.
struct Item {
    uint32_t producer = 0;
    uint32_t index    = 0;
};

} // namespace

TEST(MpmcQueue, FillAndDrainSingleThreaded) {
    MpmcQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.capacity(), 8u);

    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(q.try_push(i));
    }
    EXPECT_EQ(q.size(), 8u);

    // Full. The ring holds exactly Capacity items, unlike the SPSC ring which
    // burns one slot to tell full from empty. The per slot sequence carries
    // that information here, so nothing is wasted.
    int reject = 99;
    EXPECT_FALSE(q.try_push(reject));

    for (int i = 0; i < 8; ++i) {
        int out = -1;
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }

    int out = -1;
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_TRUE(q.empty());
}

TEST(MpmcQueue, WrapAround) {
    // Push and pop one at a time many times round, so every slot is used on
    // many laps and the sequence has to be advancing by Capacity each time
    // rather than by anything that only happens to work on the first lap.
    MpmcQueue<int, 4> q;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(q.try_push(i));
        int out = -1;
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());

    // Now half fill, drain, and refill so the two positions sit at different
    // offsets within the ring rather than both at zero.
    for (int lap = 0; lap < 300; ++lap) {
        EXPECT_TRUE(q.try_push(lap * 2));
        EXPECT_TRUE(q.try_push(lap * 2 + 1));
        int a = -1, b = -1;
        EXPECT_TRUE(q.try_pop(a));
        EXPECT_TRUE(q.try_pop(b));
        EXPECT_EQ(a, lap * 2);
        EXPECT_EQ(b, lap * 2 + 1);
    }
}

TEST(MpmcQueue, FullThenEmptyThenFullAgain) {
    MpmcQueue<Item, 4> q;
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(q.try_push(Item{0, i}));
    }
    EXPECT_FALSE(q.try_push(Item{0, 4}));
    EXPECT_FALSE(q.try_push(Item{0, 5}));   // still full after a refused push

    Item out{};
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.index, 0u);
    // One slot freed, so exactly one push succeeds and the next does not.
    EXPECT_TRUE(q.try_push(Item{0, 4}));
    EXPECT_FALSE(q.try_push(Item{0, 5}));

    for (uint32_t expect = 1; expect <= 4; ++expect) {
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.index, expect);
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(MpmcQueue, PushRvalue) {
    MpmcQueue<Item, 4> q;
    Item a{7, 11};
    EXPECT_TRUE(q.try_push(a));               // lvalue overload
    EXPECT_TRUE(q.try_push(Item{8, 12}));     // rvalue overload

    Item out{};
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.producer, 7u);
    EXPECT_EQ(out.index, 11u);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.producer, 8u);
    EXPECT_EQ(out.index, 12u);
}

namespace {

// The shared body of every concurrent case. Producers push a known set and
// spin on a full queue rather than dropping, consumers pop until the expected
// total has been taken, and each consumer keeps its own vector so that the
// per consumer ordering property can be checked without any locking during the
// run that would itself change the interleaving being tested.
struct StressResult {
    std::vector<std::vector<Item>> per_consumer;
};

template <typename Queue>
StressResult run_stress(Queue& q, uint32_t producers, uint32_t consumers,
                        uint32_t per_producer) {
    const std::size_t total = static_cast<std::size_t>(producers) * per_producer;

    std::atomic<std::size_t> popped{0};
    StressResult             result;
    result.per_consumer.resize(consumers);
    for (auto& v : result.per_consumer) v.reserve(total / consumers + per_producer);

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&q, p, per_producer] {
            for (uint32_t i = 0; i < per_producer; ++i) {
                // Spin rather than drop. A dropped item would make the
                // conservation check meaningless, and the point of the tiny
                // queue cases is precisely to make this loop spin often.
                while (!q.try_push(Item{p, i})) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (uint32_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&q, &popped, &result, c, total] {
            std::vector<Item>& mine = result.per_consumer[c];
            for (;;) {
                if (popped.load(std::memory_order_relaxed) >= total) break;
                Item it{};
                if (q.try_pop(it)) {
                    mine.push_back(it);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::thread& t : threads) t.join();
    return result;
}

// Conservation, and per consumer per producer order, in one pass.
void check_exactly_once_and_ordered(const StressResult& r, uint32_t producers,
                                    uint32_t per_producer) {
    std::vector<std::vector<uint8_t>> seen(producers, std::vector<uint8_t>(per_producer, 0));

    for (const std::vector<Item>& stream : r.per_consumer) {
        // Highest index this consumer has already taken from each producer.
        // Items from one producer must reach one consumer in increasing index
        // order, because positions are claimed in increasing order and a
        // consumer works its own positions in increasing order.
        std::vector<int64_t> last(producers, -1);
        for (const Item& it : stream) {
            ASSERT_LT(it.producer, producers);
            ASSERT_LT(it.index, per_producer);
            EXPECT_EQ(seen[it.producer][it.index], 0u)
                << "item " << it.producer << "/" << it.index << " delivered twice";
            seen[it.producer][it.index] = 1;
            EXPECT_GT(static_cast<int64_t>(it.index), last[it.producer])
                << "producer " << it.producer << " items arrived out of order at one consumer";
            last[it.producer] = static_cast<int64_t>(it.index);
        }
    }

    for (uint32_t p = 0; p < producers; ++p) {
        for (uint32_t i = 0; i < per_producer; ++i) {
            EXPECT_EQ(seen[p][i], 1u) << "item " << p << "/" << i << " was lost";
        }
    }
}

} // namespace

TEST(MpmcQueue, FourProducersFourConsumersNothingLostOrDuplicated) {
    // Four of each and a few hundred thousand items, on a ring large enough
    // that the queue spends most of its time neither full nor empty. This is
    // the throughput shaped case.
    constexpr uint32_t kProducers   = 4;
    constexpr uint32_t kConsumers   = 4;
    constexpr uint32_t kPerProducer = 100'000;   // 400,000 items

    MpmcQueue<Item, 1024> q;
    const StressResult    r = run_stress(q, kProducers, kConsumers, kPerProducer);
    check_exactly_once_and_ordered(r, kProducers, kPerProducer);
}

TEST(MpmcQueue, EightProducersEightConsumers) {
    // More threads than the machine has performance cores, so the scheduler
    // preempts threads mid push and mid pop. A design that quietly relied on a
    // thread finishing what it started fails here and nowhere else.
    constexpr uint32_t kProducers   = 8;
    constexpr uint32_t kConsumers   = 8;
    constexpr uint32_t kPerProducer = 40'000;

    MpmcQueue<Item, 256> q;
    const StressResult   r = run_stress(q, kProducers, kConsumers, kPerProducer);
    check_exactly_once_and_ordered(r, kProducers, kPerProducer);
}

TEST(MpmcQueue, TinyRingUnderConstantContention) {
    // Two slots and eight threads. The queue is full or empty essentially all
    // the time, the compare and exchange fails constantly, and every slot is
    // reused thousands of times a second, which is the regime where a missing
    // acquire on the slot sequence turns into a torn or stale item rather than
    // staying invisible.
    constexpr uint32_t kProducers   = 4;
    constexpr uint32_t kConsumers   = 4;
    constexpr uint32_t kPerProducer = 25'000;

    MpmcQueue<Item, 2> q;
    const StressResult r = run_stress(q, kProducers, kConsumers, kPerProducer);
    check_exactly_once_and_ordered(r, kProducers, kPerProducer);
}

TEST(MpmcQueue, SingleConsumerGivesGlobalPerProducerFifo) {
    // With exactly one consumer the pop order is the position order, so the
    // per producer ordering property becomes directly observable on the single
    // output stream rather than per consumer. This is the strongest ordering
    // statement the queue is entitled to make and this test is where it is
    // stated.
    constexpr uint32_t kProducers   = 4;
    constexpr uint32_t kPerProducer = 50'000;

    MpmcQueue<Item, 64> q;
    const StressResult  r = run_stress(q, kProducers, 1, kPerProducer);
    ASSERT_EQ(r.per_consumer.size(), 1u);
    ASSERT_EQ(r.per_consumer[0].size(),
              static_cast<std::size_t>(kProducers) * kPerProducer);

    std::vector<int64_t> last(kProducers, -1);
    for (const Item& it : r.per_consumer[0]) {
        ASSERT_LT(it.producer, kProducers);
        ASSERT_EQ(static_cast<int64_t>(it.index), last[it.producer] + 1)
            << "producer " << it.producer << " lost global FIFO order";
        last[it.producer] = static_cast<int64_t>(it.index);
    }
    for (uint32_t p = 0; p < kProducers; ++p) {
        EXPECT_EQ(last[p], static_cast<int64_t>(kPerProducer) - 1);
    }
}

TEST(MpmcQueue, SingleProducerSingleConsumer) {
    // The degenerate shape, included because it is the one that must keep
    // working when someone reaches for this queue where the SPSC ring would
    // have done. It is also the case the benchmark uses to price what the
    // generality costs.
    constexpr uint32_t kPerProducer = 200'000;

    MpmcQueue<Item, 512> q;
    const StressResult   r = run_stress(q, 1, 1, kPerProducer);
    check_exactly_once_and_ordered(r, 1, kPerProducer);
}
