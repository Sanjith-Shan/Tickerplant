#include "tick/recovery.hpp"
#include "tick/sequencer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <span>
#include <vector>

// Tests for the A/B line sequencer and for the retransmission policy that sits
// next to it.
//
// Every test in this file routes delivery through Recorder, and Recorder checks
// on every single call that the sequence it was handed is greater than the one
// before it. That check is not a nice extra. Delivering an order book update out
// of sequence is the failure this whole subsystem exists to prevent, and a test
// that only looks at the final counters would pass while the book was being
// corrupted in the middle. So the ordering assertion runs inside the sink, in
// every test, including the ones about duplicates and the ones about giving up.
//
// The payload of every test message is its own sequence number, so Recorder can
// also prove the reorder window handed back the message it claimed to hold. The
// window is indexed modulo its capacity, and the failure mode of getting that
// wrong is returning a real message from an earlier lap of the ring, which has
// a valid length and a plausible body and is completely wrong.

namespace {

using namespace tick;

struct Recorder {
    std::vector<uint64_t> seqs;
    bool                  ordered    = true;   // sink never saw a non increasing sequence
    bool                  payload_ok = true;   // every payload matched its sequence

    void operator()(uint64_t seq, std::span<const std::byte> payload) {
        if (!seqs.empty() && seq <= seqs.back()) ordered = false;
        seqs.push_back(seq);
        if (payload.size() >= sizeof(uint64_t)) {
            uint64_t v = 0;
            std::memcpy(&v, payload.data(), sizeof(v));
            if (v != seq) payload_ok = false;
        }
    }

    // True when the delivered run is 1, 2, 3, ... with nothing missing.
    [[nodiscard]] bool contiguous_from(uint64_t first) const {
        for (std::size_t i = 0; i < seqs.size(); ++i) {
            if (seqs[i] != first + i) return false;
        }
        return true;
    }
};

// A message whose body is its own sequence number.
using Msg = std::array<std::byte, sizeof(uint64_t)>;

Msg make_msg(uint64_t seq) {
    Msg m{};
    std::memcpy(m.data(), &seq, sizeof(seq));
    return m;
}

std::span<const std::byte> view(const Msg& m) { return {m.data(), m.size()}; }

// Shorthand so the tests read as a list of arrivals rather than a list of spans.
void feed(Sequencer& q, LineId line, uint64_t seq, uint64_t now_ns, Recorder& out) {
    const Msg m = make_msg(seq);
    q.on_message(line, seq, view(m), now_ns, out);
}

void feed_retransmit(Sequencer& q, uint64_t seq, uint64_t now_ns, Recorder& out) {
    const Msg m = make_msg(seq);
    q.on_retransmit(seq, view(m), now_ns, out);
}

// ---------------------------------------------------------------------------
// Ordinary running
// ---------------------------------------------------------------------------

TEST(Sequencer, DeliversInOrderOnOneLine) {
    Sequencer q;
    Recorder  out;

    for (uint64_t s = 1; s <= 100; ++s) feed(q, LineId::A, s, s * 10, out);

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 100u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.expected(), 101u);
    EXPECT_EQ(q.stats().accepted, 100u);
    EXPECT_EQ(q.stats().duplicates, 0u);
    EXPECT_EQ(q.stats().gaps_detected, 0u);
    EXPECT_EQ(q.stats().out_of_order, 0u);
    EXPECT_EQ(q.stats().window_high_water, 0u);
}

TEST(Sequencer, FirstMessageSeedsTheStream) {
    // Joining a live group, there is no way to know the session started at one.
    Sequencer q;
    Recorder  out;

    feed(q, LineId::A, 5000, 0, out);

    EXPECT_EQ(q.expected(), 5001u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(out.seqs.size(), 1u);
    EXPECT_EQ(out.seqs.front(), 5000u);
}

// ---------------------------------------------------------------------------
// Arbitration
// ---------------------------------------------------------------------------

TEST(Sequencer, SecondCopyOfASequenceIsDropped) {
    Sequencer q;
    Recorder  out;

    for (uint64_t s = 1; s <= 50; ++s) {
        feed(q, LineId::A, s, s * 10, out);
        feed(q, LineId::B, s, s * 10 + 1, out);   // B is always late here
    }

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 50u);
    EXPECT_EQ(q.stats().accepted, 50u);
    EXPECT_EQ(q.stats().duplicates, 50u);
}

TEST(Sequencer, WhicheverLineArrivesFirstWins) {
    Sequencer q;
    Recorder  out;

    // Odd sequences won by A, even sequences won by B.
    for (uint64_t s = 1; s <= 10; ++s) {
        const LineId first  = (s % 2 == 0) ? LineId::B : LineId::A;
        const LineId second = (s % 2 == 0) ? LineId::A : LineId::B;
        feed(q, first, s, s * 10, out);
        feed(q, second, s, s * 10 + 1, out);
    }

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(q.stats().wins_a, 5u);
    EXPECT_EQ(q.stats().wins_b, 5u);
    EXPECT_EQ(q.stats().duplicates, 10u);
    EXPECT_EQ(q.stats().accepted, 10u);
}

TEST(Sequencer, ALateDuplicateOfABufferedSequenceIsStillADuplicate) {
    // The duplicate arrives while its first copy is sitting in the window, not
    // after it has been delivered, which is a different branch.
    Sequencer q;
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 3, 10, out);     // buffered, first copy, A wins it
    feed(q, LineId::B, 3, 20, out);     // duplicate of a buffered sequence
    feed(q, LineId::B, 2, 30, out);

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 3u);
    EXPECT_EQ(q.stats().duplicates, 1u);
    EXPECT_EQ(q.stats().wins_a, 2u);
    EXPECT_EQ(q.stats().wins_b, 1u);
}

// ---------------------------------------------------------------------------
// Reordering
// ---------------------------------------------------------------------------

TEST(Sequencer, OutOfOrderArrivalsAreHeldThenDrainedContiguously) {
    Sequencer q;
    Recorder  out;

    feed(q, LineId::A, 1, 100, out);
    feed(q, LineId::A, 4, 110, out);
    feed(q, LineId::A, 3, 120, out);

    EXPECT_EQ(q.state(), FeedState::Gap);
    EXPECT_EQ(q.buffered(), 2u);
    EXPECT_EQ(out.seqs.size(), 1u) << "nothing behind the hole may be delivered early";

    feed(q, LineId::B, 2, 130, out);   // the other line closes the hole

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 4u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.expected(), 5u);
    EXPECT_EQ(q.stats().out_of_order, 2u);
    EXPECT_EQ(q.stats().window_high_water, 2u);
    EXPECT_EQ(q.stats().gaps_detected, 1u);
    EXPECT_EQ(q.stats().gaps_recovered, 1u);
}

TEST(Sequencer, ReversedBurstStillDrainsInOrder) {
    Sequencer q;
    Recorder  out;
    q.reset(1);

    for (uint64_t s = 64; s >= 1; --s) feed(q, LineId::A, s, 1000 + (64 - s), out);

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 64u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.stats().window_high_water, 63u);
}

// ---------------------------------------------------------------------------
// The gap state machine
// ---------------------------------------------------------------------------

TEST(Sequencer, GraceExpiryEmitsExactlyTheMissingRangeAndRecoveryClosesIt) {
    SequencerConfig cfg;
    cfg.gap_grace_ns        = 500;
    cfg.recovery_timeout_ns = 5000;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 4, 100, out);   // 2 and 3 lost on both lines
    feed(q, LineId::A, 5, 100, out);
    EXPECT_EQ(q.state(), FeedState::Gap);

    EXPECT_FALSE(q.poll(400, out).has_value()) << "inside the grace period, say nothing";
    EXPECT_EQ(q.state(), FeedState::Gap);

    const auto req = q.poll(700, out);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->first_sequence, 2u);
    EXPECT_EQ(req->count, 2u) << "ask for the missing run and not one sequence more";
    EXPECT_EQ(q.state(), FeedState::Recovering);
    EXPECT_EQ(q.stats().recovery_requests, 1u);

    feed_retransmit(q, 2, 800, out);
    EXPECT_EQ(q.state(), FeedState::Recovering) << "half an answer is not an answer";
    EXPECT_EQ(out.seqs.size(), 2u);

    feed_retransmit(q, 3, 810, out);

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 5u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.expected(), 6u);
    EXPECT_EQ(q.stats().from_recovery, 2u);
    EXPECT_EQ(q.stats().gaps_recovered, 1u);
    EXPECT_EQ(q.stats().messages_dropped, 0u);
}

TEST(Sequencer, GapClosedByTheOtherLineNeverAsksForAnything) {
    SequencerConfig cfg;
    cfg.gap_grace_ns = 500;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 2, 100, out);
    feed(q, LineId::A, 4, 200, out);
    EXPECT_EQ(q.state(), FeedState::Gap);

    feed(q, LineId::B, 3, 300, out);   // inside the grace period

    EXPECT_FALSE(q.poll(10'000, out).has_value());
    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.stats().recovery_requests, 0u)
        << "arbitration handled this one, the retransmission server never hears about it";
}

TEST(Sequencer, RecoveryTimesOutIntoRetriesThenGivesUpIntoStale) {
    SequencerConfig cfg;
    cfg.gap_grace_ns          = 500;
    cfg.recovery_timeout_ns   = 5000;
    cfg.max_recovery_attempts = 3;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 3, 100, out);   // 2 is gone for good

    const auto first = q.poll(700, out);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->first_sequence, 2u);
    EXPECT_EQ(first->count, 1u);

    EXPECT_FALSE(q.poll(1000, out).has_value()) << "inside the recovery timeout";

    const auto retry1 = q.poll(6000, out);
    ASSERT_TRUE(retry1.has_value());
    EXPECT_EQ(retry1->first_sequence, 2u);

    const auto retry2 = q.poll(11500, out);
    ASSERT_TRUE(retry2.has_value());
    EXPECT_EQ(q.state(), FeedState::Recovering);
    EXPECT_EQ(q.stats().recovery_requests, 3u);
    EXPECT_EQ(q.stats().recovery_retries, 2u);

    EXPECT_FALSE(q.poll(17'000, out).has_value()) << "attempts exhausted, stop asking";
    EXPECT_EQ(q.state(), FeedState::Stale);
    EXPECT_EQ(q.stats().messages_dropped, 1u);
    EXPECT_EQ(q.expected(), 4u);

    // Sequence 3 was released rather than held hostage, and the sink still saw
    // a strictly increasing stream, just one with a hole in it.
    EXPECT_TRUE(out.ordered);
    ASSERT_EQ(out.seqs.size(), 2u);
    EXPECT_EQ(out.seqs[0], 1u);
    EXPECT_EQ(out.seqs[1], 3u);
}

TEST(Sequencer, StaleIsAOneWayDoorUntilReset) {
    SequencerConfig cfg;
    cfg.gap_grace_ns          = 500;
    cfg.recovery_timeout_ns   = 5000;
    cfg.max_recovery_attempts = 1;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 3, 100, out);
    ASSERT_TRUE(q.poll(700, out).has_value());
    EXPECT_FALSE(q.poll(6000, out).has_value());
    ASSERT_EQ(q.state(), FeedState::Stale);

    // A perfectly clean run of messages does not earn the feed its good name
    // back, because the book downstream is still missing sequence 2.
    for (uint64_t s = 4; s <= 40; ++s) feed(q, LineId::A, s, 7000 + s, out);
    EXPECT_EQ(q.state(), FeedState::Stale);
    EXPECT_TRUE(out.ordered);

    q.reset(41);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.expected(), 41u);
    EXPECT_EQ(q.buffered(), 0u);
}

TEST(Sequencer, StaleFeedStillDeliversInOrderAndDoesNotStrandMessages) {
    // Once the feed is Stale nobody is coming to fill its holes, so it must not
    // sit on messages it already has.
    SequencerConfig cfg;
    cfg.gap_grace_ns          = 500;
    cfg.recovery_timeout_ns   = 5000;
    cfg.max_recovery_attempts = 1;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 3, 100, out);
    ASSERT_TRUE(q.poll(700, out).has_value());
    ASSERT_FALSE(q.poll(6000, out).has_value());
    ASSERT_EQ(q.state(), FeedState::Stale);

    feed(q, LineId::A, 6, 7000, out);   // 4 and 5 lost as well
    EXPECT_EQ(q.buffered(), 1u);

    q.poll(7000 + cfg.gap_grace_ns, out);

    EXPECT_EQ(q.buffered(), 0u) << "a Stale feed must not hold messages forever";
    EXPECT_EQ(q.state(), FeedState::Stale);
    EXPECT_TRUE(out.ordered);
    EXPECT_EQ(q.stats().messages_dropped, 3u);   // 2, 4 and 5
    EXPECT_EQ(q.stats().accepted + q.stats().messages_dropped, 6u);
}

TEST(Sequencer, RequestCoversTheWholeMissingRunAndNotTheGapsBehindIt) {
    SequencerConfig cfg;
    cfg.gap_grace_ns = 500;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    // 2, 3, 4 missing. 5 present. 6 missing. 7 present.
    feed(q, LineId::A, 5, 100, out);
    feed(q, LineId::A, 7, 100, out);

    const auto req = q.poll(700, out);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->first_sequence, 2u);
    EXPECT_EQ(req->count, 3u) << "the run stops at the first sequence already held";
}

TEST(Sequencer, AHoleUncoveredByRecoveryStartsItsOwnGrace) {
    // Two sequences lost with one present message between them look like a
    // single gap. Filling the first request must not leave the sequencer
    // waiting out a recovery timeout on a question that has been answered.
    SequencerConfig cfg;
    cfg.gap_grace_ns        = 500;
    cfg.recovery_timeout_ns = 1'000'000;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 3, 100, out);   // 2 missing
    feed(q, LineId::A, 5, 100, out);   // 4 missing too

    const auto req = q.poll(700, out);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->first_sequence, 2u);
    EXPECT_EQ(req->count, 1u);

    feed_retransmit(q, 2, 800, out);   // answered in full, uncovering the hole at 4

    EXPECT_EQ(q.state(), FeedState::Gap) << "a new hole is a new gap, not an old request";
    EXPECT_EQ(q.buffered(), 1u);

    const auto req2 = q.poll(800 + cfg.gap_grace_ns, out);
    ASSERT_TRUE(req2.has_value()) << "the new hole must get its own request quickly";
    EXPECT_EQ(req2->first_sequence, 4u);
    EXPECT_EQ(req2->count, 1u);

    feed_retransmit(q, 4, 2000, out);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 5u);
}

TEST(Sequencer, AFullWindowAsksForHelpWithoutWaitingOutTheGrace) {
    // The grace period is useless if the window overflows before it expires.
    SequencerConfig cfg;
    cfg.reorder_window = 16;
    cfg.gap_grace_ns   = 1'000'000'000;   // deliberately far too long
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    bool asked = false;
    for (uint64_t s = 3; s <= 16 && !asked; ++s) {   // 2 is missing
        feed(q, LineId::A, s, 10, out);
        if (const auto req = q.poll(10, out)) {
            asked = true;
            EXPECT_EQ(req->first_sequence, 2u);
        }
    }

    EXPECT_TRUE(asked) << "window pressure must trigger a request on its own";
    EXPECT_EQ(q.state(), FeedState::Recovering);
    EXPECT_NE(q.state(), FeedState::Stale);
}

// ---------------------------------------------------------------------------
// Window overflow
// ---------------------------------------------------------------------------

TEST(Sequencer, OverflowGoesStaleAndDropsOnlyWhatWasNeverReceived) {
    SequencerConfig cfg;
    cfg.reorder_window = 8;
    Sequencer q(cfg);
    Recorder  out;
    ASSERT_EQ(q.capacity(), 8u);

    feed(q, LineId::A, 1, 0, out);
    // 2 is lost. 3 through 9 fill every slot the window has to spare.
    for (uint64_t s = 3; s <= 9; ++s) feed(q, LineId::A, s, 10, out);
    EXPECT_EQ(q.state(), FeedState::Gap);
    EXPECT_EQ(q.buffered(), 7u);
    EXPECT_EQ(out.seqs.size(), 1u);

    feed(q, LineId::A, 10, 20, out);   // one past what the window can hold

    EXPECT_EQ(q.state(), FeedState::Stale);
    EXPECT_EQ(q.stats().messages_dropped, 1u)
        << "only sequence 2 was ever missing, the rest were held and must be delivered";
    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    ASSERT_EQ(out.seqs.size(), 9u);
    EXPECT_EQ(out.seqs.front(), 1u);
    EXPECT_EQ(out.seqs[1], 3u);
    EXPECT_EQ(out.seqs.back(), 10u);
    EXPECT_EQ(q.stats().accepted + q.stats().messages_dropped, 10u);
}

TEST(Sequencer, AJumpWiderThanTheWindowResyncsOnTheNewSequence) {
    SequencerConfig cfg;
    cfg.reorder_window = 8;
    Sequencer q(cfg);
    Recorder  out;

    feed(q, LineId::A, 1, 0, out);
    feed(q, LineId::A, 1000, 10, out);

    EXPECT_EQ(q.state(), FeedState::Stale);
    EXPECT_EQ(q.expected(), 1001u);
    EXPECT_EQ(q.buffered(), 0u) << "nothing below the jump can arrive, so nothing waits for it";
    EXPECT_EQ(q.stats().messages_dropped, 998u);   // 2 through 999
    EXPECT_TRUE(out.ordered);
    ASSERT_EQ(out.seqs.size(), 2u);
    EXPECT_EQ(out.seqs[0], 1u);
    EXPECT_EQ(out.seqs[1], 1000u);
}

TEST(Sequencer, APayloadTooWideForASlotIsTreatedAsLostNotTruncated) {
    SequencerConfig cfg;
    cfg.max_payload = 4;
    Sequencer q(cfg);
    Recorder  out;

    const Msg m1 = make_msg(1);
    q.on_message(LineId::A, 1, view(m1), 0, out);   // straight through, never buffered

    const Msg m3 = make_msg(3);
    q.on_message(LineId::A, 3, view(m3), 10, out);  // eight bytes will not fit a four byte slot

    EXPECT_EQ(q.buffered(), 0u);
    EXPECT_EQ(q.stats().messages_dropped, 1u);
    EXPECT_TRUE(out.ordered);
    EXPECT_EQ(out.seqs.size(), 1u);
}

TEST(Sequencer, ModularIndexingNeverHandsBackAMessageFromAnEarlierLap) {
    // The window is indexed by sequence modulo capacity, so slot n holds
    // sequences n, n + capacity, n + 2 * capacity and so on. The slot has to
    // prove which one it is holding, and payload_ok is what catches it if not.
    SequencerConfig cfg;
    cfg.reorder_window = 8;
    Sequencer q(cfg);
    Recorder  out;
    q.reset(1);

    for (uint64_t lap = 0; lap < 500; ++lap) {
        const uint64_t base = 1 + lap * 8;
        feed(q, LineId::A, base + 1, lap * 100, out);   // one ahead
        feed(q, LineId::A, base + 0, lap * 100 + 1, out);
        for (uint64_t s = base + 2; s < base + 8; ++s) feed(q, LineId::A, s, lap * 100 + 2, out);
    }

    EXPECT_TRUE(out.ordered);
    EXPECT_TRUE(out.payload_ok);
    EXPECT_TRUE(out.contiguous_from(1));
    EXPECT_EQ(out.seqs.size(), 4000u);
    EXPECT_EQ(q.state(), FeedState::Synced);
    EXPECT_EQ(q.stats().messages_dropped, 0u);
}

// ---------------------------------------------------------------------------
// Soak
// ---------------------------------------------------------------------------

// Two lines losing datagrams independently, arrivals shuffled inside a jitter
// window, and a retransmission server answering out of a RetransmitStore. This
// is the test that finds the bug. Every hand written case above is a situation
// somebody thought of, and the interesting failures in a sequencer are the ones
// nobody thought of, like a second hole appearing underneath a recovery request
// that has already been answered.
struct SoakResult {
    Recorder       out;
    SequencerStats stats;
    FeedState      state = FeedState::Synced;
};

SoakResult run_soak(unsigned seed, double drop_per_line, bool serve_retransmits,
                    uint64_t count = 200000) {
    SequencerConfig cfg;
    cfg.reorder_window        = 1024;
    cfg.gap_grace_ns          = 500'000;
    cfg.recovery_timeout_ns   = 2'000'000;
    cfg.max_recovery_attempts = 3;

    SoakResult result;
    Sequencer  q(cfg);
    q.reset(1);   // a session start knows the sequence it is starting from

    // Sized for the whole run so that a store miss is not a second variable in
    // this test. The store's own aliasing behaviour is tested separately.
    RetransmitStore store(1u << 18, sizeof(uint64_t));

    struct Arrival {
        uint64_t at_ns;
        uint64_t seq;
        uint8_t  line;
    };
    std::vector<Arrival> arrivals;
    arrivals.reserve(static_cast<std::size_t>(count * 2));

    std::mt19937_64                         rng(seed);
    std::uniform_real_distribution<double>  loss(0.0, 1.0);
    std::uniform_int_distribution<uint64_t> jitter(0, 30'000);   // up to 30 microseconds

    for (uint64_t s = 1; s <= count; ++s) {
        const Msg m = make_msg(s);
        store.store(s, view(m));
        const uint64_t published = s * 1000;   // a million messages a second
        for (uint8_t line = 0; line < 2; ++line) {
            if (loss(rng) < drop_per_line) continue;
            arrivals.push_back(Arrival{published + jitter(rng) + line, s, line});
        }
    }
    std::stable_sort(arrivals.begin(), arrivals.end(),
                     [](const Arrival& a, const Arrival& b) { return a.at_ns < b.at_ns; });

    Msg answer{};
    auto serve = [&](const RetransmitRequest& req, uint64_t now_ns) {
        if (!serve_retransmits) return;
        for (uint32_t i = 0; i < req.count; ++i) {
            std::size_t len = 0;
            if (store.fetch(req.first_sequence + i, answer, &len)) {
                q.on_retransmit(req.first_sequence + i,
                                std::span<const std::byte>(answer.data(), len), now_ns,
                                result.out);
            }
        }
    };

    for (const Arrival& a : arrivals) {
        const Msg m = make_msg(a.seq);
        q.on_message(a.line == 0 ? LineId::A : LineId::B, a.seq, view(m), a.at_ns, result.out);
        if (const auto req = q.poll(a.at_ns, result.out)) serve(*req, a.at_ns);
    }

    // Let the clock run past the end of the feed so anything still open resolves
    // one way or the other rather than being left in the window.
    const uint64_t last = arrivals.empty() ? 0 : arrivals.back().at_ns;
    for (int i = 1; i <= 200000 && (q.buffered() != 0 || i <= 40); ++i) {
        const uint64_t now = last + static_cast<uint64_t>(i) * 3'000'000;
        if (const auto req = q.poll(now, result.out)) serve(*req, now);
    }

    result.stats = q.stats();
    result.state = q.state();
    return result;
}

TEST(SequencerSoak, LossOnBothLinesWithRecoveryLosesNothing) {
    for (double drop : {0.01, 0.05, 0.20}) {
        const SoakResult r = run_soak(static_cast<unsigned>(drop * 1000), drop, true);
        EXPECT_TRUE(r.out.ordered) << "drop " << drop;
        EXPECT_TRUE(r.out.payload_ok) << "drop " << drop;
        EXPECT_TRUE(r.out.contiguous_from(1)) << "drop " << drop;
        EXPECT_EQ(r.out.seqs.size(), 200000u) << "drop " << drop;
        EXPECT_EQ(r.stats.messages_dropped, 0u) << "drop " << drop;
        EXPECT_NE(r.state, FeedState::Stale) << "drop " << drop;
        EXPECT_GT(r.stats.wins_a, 0u);
        EXPECT_GT(r.stats.wins_b, 0u);
    }
}

TEST(SequencerSoak, WithoutARetransmissionServerNothingIsDeliveredOutOfOrder) {
    // No recovery available, so messages are genuinely lost. What must still
    // hold is that the stream downstream is strictly increasing and that every
    // sequence is accounted for exactly once, as delivered or as dropped.
    for (double drop : {0.02, 0.30}) {
        const SoakResult r = run_soak(static_cast<unsigned>(drop * 1000) + 7, drop, false);
        EXPECT_TRUE(r.out.ordered) << "drop " << drop;
        EXPECT_TRUE(r.out.payload_ok) << "drop " << drop;
        EXPECT_EQ(r.stats.accepted, r.out.seqs.size()) << "drop " << drop;
        EXPECT_EQ(r.stats.accepted + r.stats.messages_dropped, 200000u) << "drop " << drop;
        EXPECT_GT(r.stats.messages_dropped, 0u) << "drop " << drop;
        EXPECT_EQ(r.state, FeedState::Stale) << "drop " << drop;
    }
}

// ---------------------------------------------------------------------------
// Recovery policy
// ---------------------------------------------------------------------------

TEST(RecoveryClient, RetriesWithDoublingBackoffThenAbandons) {
    RecoveryConfig cfg;
    cfg.initial_timeout_ns = 1000;
    cfg.max_timeout_ns     = 4000;
    cfg.max_attempts       = 3;
    RecoveryClient rc(cfg);

    ASSERT_EQ(rc.need(10, 3), 1u);

    const auto first = rc.next_request(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->first_sequence, 10u);
    EXPECT_EQ(first->count, 3u);

    EXPECT_FALSE(rc.next_request(999).has_value()) << "inside the first timeout";
    ASSERT_TRUE(rc.next_request(1000).has_value()) << "first timeout expired";

    EXPECT_FALSE(rc.next_request(2999).has_value()) << "backoff doubled to 2000";
    ASSERT_TRUE(rc.next_request(3000).has_value());

    EXPECT_FALSE(rc.next_request(6999).has_value()) << "backoff capped at 4000";
    EXPECT_FALSE(rc.next_request(7000).has_value()) << "three attempts used, stop asking";

    EXPECT_EQ(rc.outstanding(), 0u);
    EXPECT_EQ(rc.stats().requests_sent, 3u);
    EXPECT_EQ(rc.stats().retries, 2u);
    EXPECT_EQ(rc.stats().ranges_abandoned, 1u);
}

TEST(RecoveryClient, SplitsAGapWiderThanTheSixteenBitCountField) {
    RecoveryClient rc;
    ASSERT_EQ(rc.need(1, 70000), 2u);

    const auto a = rc.next_request(0);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->first_sequence, 1u);
    EXPECT_EQ(a->count, 65535u);

    const auto b = rc.next_request(0);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->first_sequence, 65536u);
    EXPECT_EQ(b->count, 4465u);
    EXPECT_FALSE(rc.next_request(0).has_value());

    rc.on_response(1, 65535, 1);
    EXPECT_EQ(rc.outstanding(), 1u);
    rc.on_response(65536, 4465, 2);
    EXPECT_EQ(rc.outstanding(), 0u);
    EXPECT_EQ(rc.stats().sequences_filled, 70000u);
}

TEST(RecoveryClient, APartialAnswerTrimsTheFrontOfTheRange) {
    RecoveryClient rc;
    rc.need(100, 10);
    ASSERT_TRUE(rc.next_request(0).has_value());

    rc.on_response(100, 4, 1);
    ASSERT_EQ(rc.outstanding(), 1u);

    const auto again = rc.next_request(60'000'000);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->first_sequence, 104u) << "do not ask again for what already came back";
    EXPECT_EQ(again->count, 6u);
}

TEST(RecoveryClient, RefusesRatherThanQueuesPastTheOutstandingLimit) {
    RecoveryConfig cfg;
    cfg.max_outstanding = 2;
    RecoveryClient rc(cfg);

    rc.need(1, 1);
    rc.need(2, 1);
    rc.need(3, 1);

    EXPECT_EQ(rc.outstanding(), 2u);
    EXPECT_EQ(rc.stats().ranges_refused, 1u);
}

TEST(RecoveryClient, ExpiredRequestsAreRetriedAheadOfFreshOnes) {
    RecoveryConfig cfg;
    cfg.initial_timeout_ns = 1000;
    RecoveryClient rc(cfg);

    rc.need(10, 1);
    ASSERT_TRUE(rc.next_request(0).has_value());
    rc.need(50, 1);

    const auto due = rc.next_request(2000);
    ASSERT_TRUE(due.has_value());
    EXPECT_EQ(due->first_sequence, 10u) << "the older hole blocks the book, serve it first";
}

// ---------------------------------------------------------------------------
// Publisher side store
// ---------------------------------------------------------------------------

TEST(RetransmitStore, FetchesBackWhatWasStored) {
    RetransmitStore store(16, sizeof(uint64_t));
    for (uint64_t s = 1; s <= 16; ++s) {
        const Msg m = make_msg(s);
        EXPECT_TRUE(store.store(s, view(m)));
    }

    Msg         out{};
    std::size_t len = 0;
    ASSERT_TRUE(store.fetch(9, out, &len));
    EXPECT_EQ(len, sizeof(uint64_t));
    uint64_t v = 0;
    std::memcpy(&v, out.data(), sizeof(v));
    EXPECT_EQ(v, 9u);
}

TEST(RetransmitStore, AnOverwrittenSlotReadsAsAMissNotAsTheWrongMessage) {
    RetransmitStore store(4, sizeof(uint64_t));
    ASSERT_EQ(store.capacity(), 4u);

    for (uint64_t s = 1; s <= 4; ++s) {
        const Msg m = make_msg(s);
        store.store(s, view(m));
    }
    const Msg fifth = make_msg(5);
    store.store(5, view(fifth));   // lands in the slot sequence 1 was using

    Msg         out{};
    std::size_t len = 0;
    EXPECT_FALSE(store.fetch(1, out, &len)) << "older than the window means a miss";
    EXPECT_FALSE(store.contains(1));
    EXPECT_TRUE(store.fetch(5, out, &len));
    EXPECT_TRUE(store.contains(5));
}

TEST(RetransmitStore, RefusesAPayloadTooWideForASlot) {
    RetransmitStore                store(8, 8);
    std::array<std::byte, 32>      big{};
    EXPECT_FALSE(store.store(1, std::span<const std::byte>(big.data(), big.size())));
    EXPECT_FALSE(store.contains(1));
}

TEST(RetransmitStore, RefusesToWriteIntoATooSmallOutputBuffer) {
    RetransmitStore store(8, sizeof(uint64_t));
    const Msg       m = make_msg(3);
    ASSERT_TRUE(store.store(3, view(m)));

    std::array<std::byte, 2> small{};
    std::size_t              len = 0;
    EXPECT_FALSE(store.fetch(3, small, &len));
}

}  // namespace
