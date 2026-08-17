#pragma once

#include "tick/recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// Turning two lossy copies of a feed into one ordered stream.
//
// A venue publishes the same messages on two multicast groups, A and B, from
// two separate paths. A datagram lost on one line is usually not lost on the
// other, so a receiver that listens to both and takes whichever copy of a
// sequence arrives first sees far fewer gaps than either line alone. What comes
// out of that is one stream, in sequence order, with no duplicates.
//
// Three things live in this file and they are deliberately not three classes.
//
// Arbitration is the first. It is not a stage and there is no buffer for it.
// The rule "first copy wins" is the same rule as "a sequence below the next one
// I owe downstream has already been dealt with", which sequence tracking has to
// answer anyway. Writing arbitration as its own component would mean a second
// copy of the same state, and the two copies would disagree the first time a
// duplicate and a gap happened in the same microsecond. The line a sequence
// arrived on is recorded when its first copy is accepted, which is what makes
// the A versus B win rate measurable without any extra bookkeeping.
//
// The reorder window is the second. It is a fixed ring, sized once in the
// constructor, indexed by masking the sequence. It is bounded on purpose. An
// unbounded buffer converts one lost packet into unbounded memory growth and
// unbounded latency, and a process that dies from memory pressure is worse for
// a trading system than a book it knows is wrong.
//
// The gap state machine is the third, and it is the reason the other two are
// here rather than in the socket layer. Deciding between waiting, asking for a
// retransmission, and giving up is policy, and policy needs to be testable by
// feeding it sequences and timestamps rather than packets. Nothing in this file
// touches a socket or reads a clock. Time arrives as a parameter.
//
// The one rule that outranks everything else is that downstream sees strictly
// increasing sequences. An order book fed out of order is not approximately
// right, it is wrong, and it stays wrong silently. So when this file cannot
// deliver in order it says so by going Stale, and Stale is a one way door.

namespace tick {

enum class LineId : uint8_t { A = 0, B = 1 };

enum class FeedState : uint8_t {
    Synced,      // every sequence up to expected_ - 1 has been delivered
    Gap,         // a sequence is missing and the reorder window is holding later messages
    Recovering,  // a retransmission request is outstanding for the missing range
    Stale        // the gap exceeded what the window can hold and the book is no longer trustworthy
};

struct SequencerConfig {
    // How many out-of-order messages may be held while waiting for a missing
    // sequence. Bounded on purpose. An unbounded buffer turns a lost packet
    // into unbounded memory growth, which is worse than a known bad book.
    // Rounded up to a power of two so the sequence to slot map is a mask.
    std::size_t reorder_window = 1024;
    // How long to wait for a late copy on the other line before treating a gap
    // as real and asking for a retransmission. Below this, arbitration handles
    // it. The number wants to be a few times the worst case skew between the
    // two lines and nothing more, because every nanosecond of it is latency
    // added to every message behind the hole.
    //
    // This and reorder_window are not independent. The window has to be able to
    // hold everything that arrives during the grace period, which at a peak
    // rate of a million messages a second means 1024 slots buys about a
    // millisecond. Set the grace longer than that and the window overflows
    // before the clock expires, and the feed goes Stale without ever asking for
    // the missing messages. poll() carries a second trigger for exactly this
    // reason, but the configuration should not be relying on it.
    uint64_t gap_grace_ns = 500'000;  // 500 microseconds
    // How long a retransmission request may be outstanding before it is retried.
    uint64_t recovery_timeout_ns = 50'000'000;  // 50 milliseconds
    uint32_t max_recovery_attempts = 3;
    // One ITCH 5.0 message never exceeds 50 bytes. A payload wider than this
    // cannot be held in the window, see the comment on the buffering path.
    std::size_t max_payload = 64;
};

struct SequencerStats {
    uint64_t accepted = 0;          // messages delivered downstream in order
    uint64_t duplicates = 0;        // second copy of a sequence already seen
    uint64_t out_of_order = 0;      // arrived ahead of expected and was buffered
    uint64_t gaps_detected = 0;
    uint64_t gaps_recovered = 0;
    uint64_t messages_dropped = 0;  // given up on, window overflow or attempts exhausted
    uint64_t window_high_water = 0;
    uint64_t wins_a = 0;            // sequences whose first copy came on line A
    uint64_t wins_b = 0;
    uint64_t recovery_requests = 0;
    uint64_t recovery_retries = 0;
    uint64_t from_recovery = 0;     // messages taken in through on_retransmit
};

// Sink is a template parameter rather than a virtual interface for the same
// reason the ITCH handler is. A feed doing millions of messages a second pays
// the indirect call on every one of them, and worse, the call blocks inlining
// of the delivery path into the consumer.
class Sequencer {
public:
    explicit Sequencer(SequencerConfig cfg = {})
        : cfg_(cfg),
          capacity_(round_up_pow2(cfg.reorder_window == 0 ? 1 : cfg.reorder_window)),
          mask_(capacity_ - 1),
          max_payload_(cfg.max_payload == 0 ? 1 : cfg.max_payload),
          slots_(capacity_),
          bytes_(capacity_ * max_payload_) {
        if (cfg_.max_recovery_attempts == 0) cfg_.max_recovery_attempts = 1;
    }

    // Feed one message that arrived on one line at time now_ns, which is
    // monotonic nanoseconds supplied by the caller so that every path in here
    // is reachable from a test without a clock and without a sleep.
    // Delivers zero or more messages downstream, in strict sequence order.
    template <typename Sink>
    void on_message(LineId line, uint64_t sequence, std::span<const std::byte> payload,
                    uint64_t now_ns, Sink&& sink) {
        ingest(sequence, payload, now_ns, &line, sink);
    }

    // The retransmission server answered. Same path, except the message is not
    // credited to either line. Keeping retransmits out of wins_a and wins_b is
    // what makes the win rate a measurement of the two live lines rather than a
    // number contaminated by a slow unicast recovery socket.
    template <typename Sink>
    void on_retransmit(uint64_t sequence, std::span<const std::byte> payload, uint64_t now_ns,
                       Sink&& sink) {
        if (!ingest(sequence, payload, now_ns, nullptr, sink)) return;
        ++stats_.from_recovery;
        // Any progress at all means the server is answering, so restart the
        // clock. Without this a range being streamed back one message at a time
        // would time out mid answer and be asked for all over again.
        if (state_ == FeedState::Recovering) {
            recovery_deadline_ns_ = now_ns + cfg_.recovery_timeout_ns;
        }
    }

    // Drive the state machine forward with time alone. This is what turns a
    // grace period expiring into a retransmission request and a recovery
    // timeout into a retry or a give up. Returns a request the caller should
    // send, and nullopt otherwise. The caller owns the sending, because this
    // header knows nothing about sockets.
    template <typename Sink>
    std::optional<RetransmitRequest> poll(uint64_t now_ns, Sink&& sink) {
        if (state_ == FeedState::Gap) {
            // Waiting out the grace period is only affordable while there is
            // still room to hold what keeps arriving behind the hole. A feed
            // fast enough to fill the window inside the grace period would
            // otherwise overflow and go Stale without ever having asked anyone
            // for the missing data, which is the worst of both choices. So the
            // window filling up is a second trigger alongside the clock. Three
            // quarters rather than completely full, to leave room for the
            // messages that arrive while the request is in flight.
            const bool under_pressure = pending_ * 4 >= static_cast<uint64_t>(capacity_) * 3;
            if (!under_pressure && now_ns < gap_since_ns_ + cfg_.gap_grace_ns) {
                return std::nullopt;
            }
            // Ask for exactly the run that is missing, no more, since every
            // sequence in the request is work for a retransmission server that
            // is probably answering the same question for everyone else on the
            // feed right now.
            state_                = FeedState::Recovering;
            recovery_attempts_    = 1;
            recovery_deadline_ns_ = now_ns + cfg_.recovery_timeout_ns;
            ++stats_.recovery_requests;
            return issue_request();
        }

        if (state_ == FeedState::Recovering) {
            if (now_ns < recovery_deadline_ns_) return std::nullopt;
            if (recovery_attempts_ >= cfg_.max_recovery_attempts) {
                // Out of attempts. Abandon the hole, count what was never seen,
                // release everything held behind it, and say the book is bad.
                // Releasing matters. The alternative is holding the window shut
                // forever on data that is never coming.
                skip_hole(sink);
                enter_stale();
                return std::nullopt;
            }
            ++recovery_attempts_;
            ++stats_.recovery_requests;
            ++stats_.recovery_retries;
            recovery_deadline_ns_ = now_ns + cfg_.recovery_timeout_ns;
            return issue_request();
        }

        if (state_ == FeedState::Stale && pending_ != 0 &&
            now_ns >= gap_since_ns_ + cfg_.gap_grace_ns) {
            // A Stale feed still sequences and still delivers in order, it just
            // never claims to be Synced again, because the caller's book is
            // missing messages and the only honest fix is a rebuild. What it
            // must not do is leave messages sitting in the window forever
            // because the state machine has run out of things to say. Nobody is
            // going to ask for this hole, so once the other line has had its
            // grace period to produce the copy, step over it.
            skip_hole(sink);
            gap_since_ns_ = now_ns;
        }

        // Synced has nothing to do.
        return std::nullopt;
    }

    [[nodiscard]] FeedState             state() const noexcept { return state_; }
    [[nodiscard]] uint64_t              expected() const noexcept { return expected_; }
    [[nodiscard]] const SequencerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const SequencerConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] std::size_t           capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t           buffered() const noexcept { return pending_; }

    // Session start, and the only way out of Stale. Counters are left alone
    // because they are cumulative for the run and a reset is exactly the event
    // you want to still be able to see in them afterwards.
    void reset(uint64_t first_sequence) noexcept {
        for (Slot& s : slots_) s.present = false;
        pending_              = 0;
        expected_             = first_sequence;
        state_                = FeedState::Synced;
        started_              = true;
        gap_since_ns_         = 0;
        clear_recovery();
    }

private:
    struct Slot {
        // The sequence this slot holds. Modular indexing aliases every
        // capacity_ sequences, so a present flag alone would happily hand back
        // a message from an earlier lap of the ring. The slot has to prove it
        // holds the sequence being asked for.
        uint64_t seq     = 0;
        uint32_t len     = 0;
        bool     present = false;
    };

    static std::size_t round_up_pow2(std::size_t n) noexcept {
        std::size_t v = 1;
        while (v < n) v <<= 1;
        return v;
    }

    [[nodiscard]] Slot& slot_of(uint64_t seq) noexcept {
        return slots_[static_cast<std::size_t>(seq & mask_)];
    }
    [[nodiscard]] std::byte* bytes_of(uint64_t seq) noexcept {
        return bytes_.data() + static_cast<std::size_t>(seq & mask_) * max_payload_;
    }

    // The one place a message enters. line is null for a retransmit. Returns
    // true when the message was taken, either delivered straight through or
    // held in the window, and false when it was a duplicate or was thrown away.
    template <typename Sink>
    bool ingest(uint64_t sequence, std::span<const std::byte> payload, uint64_t now_ns,
                const LineId* line, Sink& sink) {
        if (!started_) {
            // Joining a live multicast group, the first datagram defines where
            // the stream starts. Anything older than it was published before
            // this process existed and cannot be asked for on the live feed.
            reset(sequence);
        }

        if (sequence < expected_) {
            // Already delivered. This is the second line arriving with a copy of
            // something the first line already won, which on a healthy feed is
            // close to half of all traffic.
            ++stats_.duplicates;
            return false;
        }

        if (sequence == expected_) {
            deliver_now(sequence, payload, now_ns, line, sink);
            return true;
        }

        // Ahead of what is owed downstream. Either it is the first copy of a
        // sequence arriving early, or it is the second copy of one already
        // sitting in the window.
        {
            const Slot& s = slot_of(sequence);
            if (s.present && s.seq == sequence) {
                ++stats_.duplicates;
                return false;
            }
        }

        // Offset zero is the slot belonging to expected_ itself, which is by
        // construction never occupied, so the window really holds capacity_ - 1
        // messages. Anything further out has nowhere to live, and the hole at
        // the front cannot be held open any longer.
        if (sequence - expected_ >= capacity_) {
            // Move the front forward by the least that makes room, delivering
            // everything that was stacked up behind the hole and counting
            // everything that was never seen. Skipping the least possible is
            // what keeps a message that was already received from being thrown
            // away. Delivery is still in strictly increasing order. What it is
            // not is complete, and that is what Stale says.
            if (skip_to(sequence - (capacity_ - 1), sink)) {
                // That jump was wider than the entire window, so nothing that
                // was being held survived it and nothing below this message can
                // still arrive in time to be useful. Resync on this message
                // rather than leaving a hole open that no arrival can fill and
                // stranding this message in the window behind it.
                skip_to(sequence, sink);
            }
            enter_stale();
            // Draining during the skip can carry expected_ all the way up to
            // this message, in which case it is no longer an early arrival.
            if (sequence == expected_) {
                deliver_now(sequence, payload, now_ns, line, sink);
                return true;
            }
        }

        if (payload.size() > max_payload_) {
            // A payload too wide for a slot is treated as a lost message rather
            // than stored truncated. A gap is loud and a silently truncated
            // message is not, and the second one corrupts a book in a way that
            // takes hours to find.
            ++stats_.messages_dropped;
            return false;
        }

        const bool first_hole = (pending_ == 0);
        Slot&      dst        = slot_of(sequence);
        if (!payload.empty()) {
            std::memcpy(bytes_of(sequence), payload.data(), payload.size());
        }
        dst.seq     = sequence;
        dst.len     = static_cast<uint32_t>(payload.size());
        dst.present = true;
        ++pending_;
        if (pending_ > stats_.window_high_water) {
            stats_.window_high_water = pending_;
        }
        ++stats_.out_of_order;
        credit_line(line);

        // The window went from holding nothing to holding something, so this is
        // the moment a hole opened at the front. The clock starts here whatever
        // state the feed is in, because a Stale feed still needs to know how
        // long it has been waiting before it steps over a hole, and the count
        // is taken here rather than on the state change so that it keeps
        // counting after the feed has gone Stale.
        //
        // Worth being clear about what gaps_detected counts. It is a hole in
        // sequence space at this instant, not a lost message. On a two line
        // feed most of these close within microseconds when the other line
        // delivers the missing copy, which is the whole point of running two
        // lines, and those still count here.
        if (first_hole) {
            gap_since_ns_ = now_ns;
            ++stats_.gaps_detected;
        }
        if (state_ == FeedState::Synced) state_ = FeedState::Gap;
        return true;
    }

    // Hand a message straight from the caller's receive buffer to the sink.
    // Nothing is copied on this path because nothing needs to outlive the call.
    template <typename Sink>
    void deliver_now(uint64_t sequence, std::span<const std::byte> payload, uint64_t now_ns,
                     const LineId* line, Sink& sink) {
        credit_line(line);
        sink(sequence, payload);
        ++stats_.accepted;
        ++expected_;
        drain(sink);
        settle(now_ns);
    }

    void credit_line(const LineId* line) noexcept {
        if (line == nullptr) return;
        if (*line == LineId::A) {
            ++stats_.wins_a;
        } else {
            ++stats_.wins_b;
        }
    }

    // Deliver as far forward as the window is contiguous. Called after anything
    // that advances expected_, because filling the hole at expected_ is exactly
    // what unblocks everything queued behind it.
    template <typename Sink>
    void drain(Sink& sink) {
        for (;;) {
            Slot& s = slot_of(expected_);
            if (!s.present || s.seq != expected_) break;
            sink(expected_, std::span<const std::byte>(bytes_of(expected_), s.len));
            s.present = false;
            --pending_;
            ++stats_.accepted;
            ++expected_;
        }
    }

    // Delivery has stopped moving, so work out what state that leaves the feed
    // in. Stale is never touched here, because Stale never returns to Synced.
    void settle(uint64_t now_ns) noexcept {
        if (pending_ == 0) {
            // Nothing is held any more, so there is no hole either, because a
            // hole only exists while something above it is waiting.
            if (state_ == FeedState::Gap || state_ == FeedState::Recovering) {
                ++stats_.gaps_recovered;
                state_ = FeedState::Synced;
                clear_recovery();
            }
            return;
        }

        // Something is still held, so there is still a hole, but it may not be
        // the hole that was asked for. Two sequences lost close together look
        // like one gap, and filling the first one uncovers the second. Without
        // this the sequencer would sit in Recovering waiting out a timeout on a
        // request that has already been answered in full, while the window
        // filled up behind a hole nobody had asked about yet. The soak test
        // found exactly that, and it cost messages every time.
        if (state_ == FeedState::Recovering && expected_ >= recovery_end_) {
            ++stats_.gaps_recovered;
            ++stats_.gaps_detected;
            state_        = FeedState::Gap;
            gap_since_ns_ = now_ns;
            clear_recovery();
        }
    }

    void clear_recovery() noexcept {
        recovery_attempts_    = 0;
        recovery_deadline_ns_ = 0;
        recovery_end_         = 0;
    }

    // How many consecutive sequences are missing starting at expected_. Bounded
    // by the window, and only ever walked when a gap changes state, so this is
    // not on the per message path. Offset zero is always absent, so the answer
    // is at least one whenever it is asked.
    [[nodiscard]] uint64_t missing_run() noexcept {
        for (uint64_t i = 0; i < capacity_; ++i) {
            Slot& s = slot_of(expected_ + i);
            if (s.present && s.seq == expected_ + i) return i;
        }
        return capacity_;
    }

    // Build the request for the hole sitting at the front right now, and
    // remember what it covers. Remembering matters because a request is only
    // the right question for as long as the hole has not moved, see settle().
    [[nodiscard]] RetransmitRequest issue_request() noexcept {
        const uint64_t run = missing_run();
        const uint64_t n   = run > 65535 ? 65535 : run;
        recovery_end_      = expected_ + n;
        return RetransmitRequest{expected_, static_cast<uint16_t>(n)};
    }

    // Give up on the run of sequences missing at the front, then release
    // whatever was being held behind it.
    template <typename Sink>
    void skip_hole(Sink& sink) {
        (void)skip_to(expected_ + missing_run(), sink);
    }

    // Move the front of the stream forward to target. Sequences held in the
    // window on the way are delivered, sequences that were never received are
    // counted as dropped, and either way the stream downstream stays strictly
    // increasing. messages_dropped therefore means exactly one thing, the
    // number of sequences this object will never hand downstream.
    //
    // Returns true when the jump was wide enough to wipe the whole window,
    // which the caller needs to know because it changes what is still worth
    // waiting for.
    template <typename Sink>
    bool skip_to(uint64_t target, Sink& sink) {
        if (target <= expected_) return false;
        const uint64_t span = target - expected_;

        if (span >= capacity_) {
            // Every buffered message lives within one window of expected_, so a
            // jump this wide abandons all of them. Clearing the ring in one
            // pass rather than walking the span matters because a corrupt or
            // hostile sequence number could make that walk effectively endless.
            for (Slot& s : slots_) s.present = false;
            pending_ = 0;
            stats_.messages_dropped += span;
            expected_ = target;
            return true;
        }

        while (expected_ < target) {
            Slot& s = slot_of(expected_);
            if (s.present && s.seq == expected_) {
                sink(expected_, std::span<const std::byte>(bytes_of(expected_), s.len));
                s.present = false;
                --pending_;
                ++stats_.accepted;
            } else {
                ++stats_.messages_dropped;
            }
            ++expected_;
        }
        drain(sink);
        return false;
    }

    void enter_stale() noexcept {
        state_ = FeedState::Stale;
        clear_recovery();
    }

    SequencerConfig cfg_;
    std::size_t     capacity_;
    std::size_t     mask_;
    std::size_t     max_payload_;

    // Both allocated once, here, and never again. A message arriving never
    // touches the allocator.
    std::vector<Slot>      slots_;
    std::vector<std::byte> bytes_;

    SequencerStats stats_{};
    uint64_t       expected_             = 0;
    uint64_t       pending_              = 0;
    uint64_t       gap_since_ns_         = 0;
    uint64_t       recovery_deadline_ns_ = 0;
    // One past the last sequence the outstanding request covers.
    uint64_t       recovery_end_         = 0;
    uint32_t       recovery_attempts_    = 0;
    FeedState      state_                = FeedState::Synced;
    bool           started_              = false;
};

}  // namespace tick
