// The whole pipeline, in one process, with no sockets.
//
// A socket test proves the socket works. This proves the thing that actually
// matters, which is that a book rebuilt from a lossy, reordered, two line
// MoldUDP64 stream is identical to a book built straight from the same messages
// with nothing in between.
//
// Everything is deterministic. The messages come from the seeded synthetic
// generator, the loss and reordering come from a seeded generator too, and the
// retransmission server answers in line rather than over a socket. So a failure
// here reproduces exactly, which is not true of the loopback runs in
// scripts/run_wire_experiments.sh, and that is why this is the test and those
// are the experiments.

#include "tick/book_builder.hpp"
#include "tick/itch_decoder.hpp"
#include "tick/moldudp64.hpp"
#include "tick/recovery.hpp"
#include "tick/sequencer.hpp"
#include "tick/synthetic_feed.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <vector>

// A tiny helper so the packing loop above can fail a test from inside a plain
// function. GoogleTest's ASSERT_ only works in a void returning function.
#define ASSERT_OR_FAIL(cond) \
    do { if (!(cond)) { ADD_FAILURE() << "packing failed: " #cond; return WireRun{}; } } while (0)

namespace {

using Builder = tick::BookBuilder<tick::MapSide, 200000>;

// One ITCH message, owned, as it would sit in a publisher's hands.
using Message = std::vector<std::byte>;

std::vector<Message> generate_messages(uint64_t seed, uint32_t symbols, uint64_t count) {
    tick::SyntheticConfig cfg;
    cfg.seed     = seed;
    cfg.symbols  = symbols;
    cfg.messages = count;

    std::vector<Message> out;
    out.reserve(count);
    tick::SyntheticFeed feed(cfg);
    feed.generate([&](std::span<const std::byte> m) {
        out.emplace_back(m.begin(), m.end());
    });
    return out;
}

// The reference. Straight from the messages into the book, no framing, no
// sequencing, nothing to go wrong.
uint64_t direct_digest(const std::vector<Message>& msgs, uint64_t* volume_digest = nullptr) {
    auto                  book = std::make_unique<Builder>(1u << 16);
    tick::ZeroCopyDecoder dec;
    for (const Message& m : msgs) {
        dec.decode(std::span<const std::byte>(m.data(), m.size()), *book);
    }
    if (volume_digest) *volume_digest = book->volume_digest();
    return book->digest();
}

// A publisher, a network that loses and reorders, two lines, a receiver, and a
// retransmission server, all in one function and all deterministic.
struct WireRun {
    uint64_t digest        = 0;
    uint64_t volume_digest = 0;
    uint64_t delivered     = 0;
    uint64_t gaps          = 0;
    uint64_t recovered     = 0;
    uint64_t lost          = 0;
    uint64_t wins_a        = 0;
    uint64_t wins_b        = 0;
    uint64_t duplicates    = 0;
    tick::FeedState state  = tick::FeedState::Synced;
};

struct WireOptions {
    double      drop_a  = 0.0;
    double      drop_b  = 0.0;
    double      reorder = 0.0;
    int         batch   = 8;
    bool        recovery = true;
    std::size_t window  = 4096;
    uint64_t    seed    = 99;
    // The receiver's clock is driven by the test rather than by a real one, so
    // the grace period and the recovery timeout are reached deterministically.
    uint64_t    tick_ns = 1000;
};

WireRun replay_over_wire(const std::vector<Message>& msgs, const WireOptions& opt) {
    tick::SequencerConfig scfg;
    scfg.reorder_window       = opt.window;
    scfg.gap_grace_ns         = 20 * opt.tick_ns;
    scfg.recovery_timeout_ns  = 200 * opt.tick_ns;
    scfg.max_recovery_attempts = 3;

    tick::Sequencer seq(scfg);
    seq.reset(1);

    auto                  book = std::make_unique<Builder>(1u << 16);
    tick::ZeroCopyDecoder dec;
    uint64_t              delivered = 0;

    auto sink = [&](uint64_t, std::span<const std::byte> payload) {
        dec.decode(payload, *book);
        ++delivered;
    };

    // The publisher's in memory session store, which is what answers a
    // re-request. Bounded, exactly like the real one, so a request for
    // something that has aged out is a miss rather than wrong data.
    tick::RetransmitStore store(1u << 16);

    std::mt19937_64                        rng(opt.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<std::byte>    packet(tick::mold::kMaxPacketLen);
    tick::mold::PacketBuilder builder(packet.data(), packet.size(), "TESTSESS");

    uint64_t now = 1'000'000;
    uint64_t seq_no = 1;

    // Packets held back one place, which is what reordering looks like on a
    // path where one datagram overtakes another.
    struct Held { std::vector<std::byte> bytes; bool line_b; };
    std::vector<Held> held;

    std::vector<std::byte> retx_payload(64);

    auto pump_recovery = [&]() {
        // Drive the gap state machine and answer whatever it asks for. Doing it
        // in line means the recovery round trip is one call rather than a real
        // network delay, which is the right simplification for a correctness
        // test and the wrong one for a latency measurement. The latency
        // measurement lives in the wire experiments.
        for (int i = 0; i < 8; ++i) {
            now += opt.tick_ns * 40;
            auto req = seq.poll(now, sink);
            if (!req) break;
            if (!opt.recovery) continue;
            for (uint16_t k = 0; k < req->count; ++k) {
                std::size_t len = 0;
                if (!store.fetch(req->first_sequence + k, retx_payload, &len)) continue;
                seq.on_retransmit(req->first_sequence + k,
                                  std::span<const std::byte>(retx_payload.data(), len), now,
                                  sink);
            }
        }
    };

    auto deliver = [&](std::span<const std::byte> pkt, tick::LineId line) {
        const auto view = tick::mold::PacketView::parse(pkt);
        if (!view) return;
        uint64_t s = view->sequence();
        for (std::span<const std::byte> m : *view) {
            seq.on_message(line, s, m, now, sink);
            ++s;
        }
    };

    auto send_packet = [&](std::span<const std::byte> pkt) {
        now += opt.tick_ns;

        // Anything held back from the previous packet goes now, which puts it
        // behind the packet that was sent after it.
        std::vector<Held> to_send;
        to_send.swap(held);

        const bool hold = opt.reorder > 0.0 && unit(rng) < opt.reorder;

        if (unit(rng) >= opt.drop_a) {
            if (hold) {
                held.push_back({std::vector<std::byte>(pkt.begin(), pkt.end()), false});
            } else {
                deliver(pkt, tick::LineId::A);
            }
        }
        if (unit(rng) >= opt.drop_b) {
            if (hold) {
                held.push_back({std::vector<std::byte>(pkt.begin(), pkt.end()), true});
            } else {
                deliver(pkt, tick::LineId::B);
            }
        }

        for (const Held& h : to_send) {
            deliver(std::span<const std::byte>(h.bytes.data(), h.bytes.size()),
                    h.line_b ? tick::LineId::B : tick::LineId::A);
        }

        pump_recovery();
    };

    builder.reset(seq_no);
    for (const Message& m : msgs) {
        store.store(seq_no, std::span<const std::byte>(m.data(), m.size()));
        if (!builder.try_append(std::span<const std::byte>(m.data(), m.size()))) {
            send_packet(builder.finish());
            builder.reset(seq_no);
            ASSERT_OR_FAIL(builder.try_append(std::span<const std::byte>(m.data(), m.size())));
        }
        ++seq_no;
        if (builder.count() >= opt.batch) {
            send_packet(builder.finish());
            builder.reset(seq_no);
        }
    }
    if (builder.count() > 0) send_packet(builder.finish());

    // Whatever is still held back arrives, and then the state machine is given
    // enough time to give up on anything that is never coming.
    for (const Held& h : held) {
        deliver(std::span<const std::byte>(h.bytes.data(), h.bytes.size()),
                h.line_b ? tick::LineId::B : tick::LineId::A);
    }
    held.clear();
    for (int i = 0; i < 64; ++i) {
        now += opt.tick_ns * 500;
        auto req = seq.poll(now, sink);
        if (req && opt.recovery) {
            for (uint16_t k = 0; k < req->count; ++k) {
                std::size_t len = 0;
                if (!store.fetch(req->first_sequence + k, retx_payload, &len)) continue;
                seq.on_retransmit(req->first_sequence + k,
                                  std::span<const std::byte>(retx_payload.data(), len), now, sink);
            }
        }
    }

    WireRun r;
    r.digest        = book->digest();
    r.volume_digest = book->volume_digest();
    r.delivered     = delivered;
    r.gaps          = seq.stats().gaps_detected;
    r.recovered     = seq.stats().gaps_recovered;
    r.lost          = seq.stats().messages_dropped;
    r.wins_a        = seq.stats().wins_a;
    r.wins_b        = seq.stats().wins_b;
    r.duplicates    = seq.stats().duplicates;
    r.state         = seq.state();
    return r;
}

} // namespace

TEST(WireIntegration, CleanWireProducesTheSameBookAsTheFile) {
    const auto msgs = generate_messages(7, 20, 50000);
    uint64_t   ref_vol = 0;
    const uint64_t ref = direct_digest(msgs, &ref_vol);

    WireOptions opt;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.delivered, msgs.size());
    EXPECT_EQ(r.lost, 0u);
    EXPECT_EQ(r.gaps, 0u);
    EXPECT_EQ(r.state, tick::FeedState::Synced);
    EXPECT_EQ(r.digest, ref) << "a clean wire must reproduce the book exactly";
    EXPECT_EQ(r.volume_digest, ref_vol);
}

// Both lines carry everything, so every sequence arrives twice and the second
// copy has to be discarded rather than applied. Applying it would double count
// every execution in the day.
TEST(WireIntegration, BothLinesDeliverAndDuplicatesAreDiscarded) {
    const auto msgs = generate_messages(11, 10, 20000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.digest, ref);
    EXPECT_GT(r.duplicates, 0u) << "the second copy of each sequence must be counted";
    EXPECT_EQ(r.wins_a + r.wins_b, msgs.size());
}

// The case A and B lines exist for. Independent loss on each line, and almost
// nothing is lost on both, so arbitration alone rebuilds the book exactly.
TEST(WireIntegration, ArbitrationCoversIndependentLossOnEachLine) {
    const auto msgs = generate_messages(13, 20, 50000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.drop_a = 0.05;
    opt.drop_b = 0.05;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.digest, ref) << "arbitration plus recovery must reproduce the book exactly";
    EXPECT_EQ(r.lost, 0u);
    EXPECT_GT(r.wins_b, 0u) << "line B should have won some sequences at this loss rate";
}

TEST(WireIntegration, RecoveryClosesGapsThatHitBothLines) {
    const auto msgs = generate_messages(17, 20, 50000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.drop_a = 0.20;
    opt.drop_b = 0.20;
    opt.window = 8192;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_GT(r.gaps, 0u) << "at this rate some sequences must be lost on both lines";
    EXPECT_EQ(r.recovered, r.gaps) << "every gap should have been closed by a retransmission";
    EXPECT_EQ(r.lost, 0u);
    EXPECT_EQ(r.digest, ref);
    EXPECT_EQ(r.state, tick::FeedState::Synced);
}

// The control. The same loss with the recovery path switched off has to lose
// data and has to say so, rather than quietly producing a book that looks fine.
TEST(WireIntegration, WithoutRecoveryLossIsVisibleAndTheBookIsDeclaredBad) {
    const auto msgs = generate_messages(17, 20, 50000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.drop_a   = 0.20;
    opt.drop_b   = 0.20;
    opt.window   = 8192;
    opt.recovery = false;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_GT(r.gaps, 0u);
    EXPECT_GT(r.lost, 0u);
    EXPECT_NE(r.digest, ref) << "losing messages must change the book";
    EXPECT_EQ(r.state, tick::FeedState::Stale)
        << "a feed that lost data must not claim to be synced";
}

TEST(WireIntegration, ReorderedDatagramsAreResequencedNotApplied) {
    const auto msgs = generate_messages(23, 15, 40000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.reorder = 0.10;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.digest, ref) << "out of order arrival must not reach the book out of order";
    EXPECT_EQ(r.lost, 0u);
}

TEST(WireIntegration, LossAndReorderTogether) {
    const auto msgs = generate_messages(29, 25, 60000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.drop_a  = 0.10;
    opt.drop_b  = 0.10;
    opt.reorder = 0.10;
    opt.window  = 8192;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.digest, ref);
    EXPECT_EQ(r.lost, 0u);
}

// One line failing completely is a real operational event and the other line
// alone has to carry the feed.
TEST(WireIntegration, OneLineGoingDarkIsSurvivedByTheOther) {
    const auto msgs = generate_messages(31, 15, 40000);
    const uint64_t ref = direct_digest(msgs);

    WireOptions opt;
    opt.drop_a = 1.0;
    const WireRun r = replay_over_wire(msgs, opt);

    EXPECT_EQ(r.digest, ref);
    EXPECT_EQ(r.lost, 0u);
    EXPECT_EQ(r.wins_a, 0u);
    EXPECT_EQ(r.wins_b, msgs.size());
}

// Running the same thing twice has to give the same answer, including the
// failure injection, because a test that only sometimes reproduces is not a
// test of a system whose whole claim is determinism.
TEST(WireIntegration, TheSameSeedGivesTheSameRun) {
    const auto msgs = generate_messages(37, 20, 30000);

    WireOptions opt;
    opt.drop_a  = 0.08;
    opt.drop_b  = 0.08;
    opt.reorder = 0.05;

    const WireRun a = replay_over_wire(msgs, opt);
    const WireRun b = replay_over_wire(msgs, opt);

    EXPECT_EQ(a.digest, b.digest);
    EXPECT_EQ(a.gaps, b.gaps);
    EXPECT_EQ(a.recovered, b.recovered);
    EXPECT_EQ(a.wins_a, b.wins_a);
    EXPECT_EQ(a.lost, b.lost);
}
