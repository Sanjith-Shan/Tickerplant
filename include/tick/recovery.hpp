#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// Retransmission, both halves of it, with no socket anywhere in the file.
//
// A gap on a redundant feed is a decision, not an event. The receiver can wait
// for a late copy on the other line, ask a retransmission server for the range,
// or give up and declare the book untrustworthy. The waiting half lives in
// sequencer.hpp because it is inseparable from sequence tracking. What lives
// here is the asking half and the answering half.
//
// RecoveryClient is the asking half. It exists as its own object rather than as
// more state inside the Sequencer for one reason. The Sequencer can only ever
// have one hole, the one at the next sequence it owes downstream, so its notion
// of an outstanding request is a single range and a single deadline. A client
// that talks to a real retransmission server has to cope with several ranges in
// flight at once, with a sixteen bit count field that cannot express a gap
// larger than 65535, and with a server that is itself under load and must not
// be hammered. Those three concerns are a policy, and policies are easier to
// argue about and to test when they are not tangled into a hot loop.
//
// RetransmitStore is the answering half, which is what the publisher side of
// this project needs. It is a fixed ring of recently published messages.
//
// Neither class knows what a payload means. Both take and return bytes.

namespace tick {

// A request for a contiguous run of sequences starting at first_sequence.
// count is sixteen bits because that is the width MoldUDP64 gives it, and
// honouring that width here rather than at the wire boundary is what forces the
// splitting logic below to exist at all.
struct RetransmitRequest {
    uint64_t first_sequence = 0;
    uint16_t count          = 0;
};

struct RecoveryConfig {
    // First timeout applied to a request. The retry schedule doubles from here.
    uint64_t initial_timeout_ns = 50'000'000;   // 50 milliseconds
    // Ceiling on the doubling. Without a cap, three or four retries put the
    // next attempt minutes away, by which point the answer is worthless because
    // the book has been rebuilt from a snapshot anyway.
    uint64_t max_timeout_ns = 1'000'000'000;    // 1 second
    // Attempts include the first send, so 3 means one send and two retries.
    uint32_t max_attempts = 3;
    // How many ranges may be in flight or queued at once. Bounded for the same
    // reason the reorder window is bounded. A feed that is dropping badly must
    // not be able to turn that into unbounded memory or into a request storm
    // aimed at the venue.
    std::size_t max_outstanding = 16;
};

struct RecoveryStats {
    uint64_t ranges_requested = 0;  // distinct ranges accepted by need()
    uint64_t requests_sent    = 0;  // wire requests handed to the caller, retries included
    uint64_t retries          = 0;  // of those, the ones that were not a first attempt
    uint64_t responses        = 0;  // on_response calls that matched something outstanding
    uint64_t sequences_filled = 0;  // sequences covered by those responses
    uint64_t ranges_abandoned = 0;  // gave up after max_attempts
    uint64_t ranges_refused   = 0;  // dropped because max_outstanding was already reached
};

// Tracks which ranges have been asked for, when to ask again, and when to stop
// asking. The caller drives it. Nothing here sends anything.
class RecoveryClient {
public:
    // The only allocation this class performs. Everything afterwards reuses
    // these slots, so a burst of gaps costs no allocator traffic on a path that
    // is already having a bad time.
    explicit RecoveryClient(RecoveryConfig cfg = {}) : cfg_(cfg) {
        if (cfg_.max_outstanding == 0) cfg_.max_outstanding = 1;
        if (cfg_.max_attempts == 0) cfg_.max_attempts = 1;
        if (cfg_.initial_timeout_ns == 0) cfg_.initial_timeout_ns = 1;
        if (cfg_.max_timeout_ns < cfg_.initial_timeout_ns) {
            cfg_.max_timeout_ns = cfg_.initial_timeout_ns;
        }
        entries_.reserve(cfg_.max_outstanding);
    }

    // Register a missing range. count is sixty four bits here and sixteen bits
    // on the wire, so a gap wider than 65535 becomes several queued requests.
    // Returns the number of wire requests the range was split into, which is
    // zero when the range was refused for being over the outstanding limit.
    std::size_t need(uint64_t first_sequence, uint64_t count) {
        if (count == 0) return 0;
        std::size_t chunks = 0;
        uint64_t    seq    = first_sequence;
        uint64_t    left   = count;
        while (left > 0) {
            if (entries_.size() >= cfg_.max_outstanding) {
                // Refusing is deliberate. The alternative is a queue that grows
                // with the size of the outage, and a queue like that is still
                // draining long after the data in it stopped mattering.
                ++stats_.ranges_refused;
                break;
            }
            const uint64_t n = left > kMaxCount ? kMaxCount : left;
            entries_.push_back(Entry{seq, static_cast<uint16_t>(n), 0, 0, 0, false});
            seq += n;
            left -= n;
            ++chunks;
            ++stats_.ranges_requested;
        }
        return chunks;
    }

    // The one request the caller should put on the wire now, if any. Expired
    // requests are retried ahead of fresh ones, because a range that has
    // already been waited on is older data and the order book cannot advance
    // past it. Returns nullopt when there is nothing due.
    std::optional<RetransmitRequest> next_request(uint64_t now_ns) {
        for (std::size_t i = 0; i < entries_.size();) {
            Entry& e = entries_[i];
            if (!e.in_flight) { ++i; continue; }
            if (now_ns < e.sent_ns + e.timeout_ns) { ++i; continue; }
            if (e.attempts >= cfg_.max_attempts) {
                ++stats_.ranges_abandoned;
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            // Doubling rather than a fixed interval because the usual reason a
            // retransmission does not come back is that the server is busy
            // serving everyone else who lost the same multicast packet.
            e.timeout_ns = e.timeout_ns > cfg_.max_timeout_ns / 2 ? cfg_.max_timeout_ns
                                                                  : e.timeout_ns * 2;
            e.sent_ns = now_ns;
            ++e.attempts;
            ++stats_.requests_sent;
            ++stats_.retries;
            return RetransmitRequest{e.first, e.count};
        }

        for (Entry& e : entries_) {
            if (e.in_flight) continue;
            e.in_flight  = true;
            e.sent_ns    = now_ns;
            e.timeout_ns = cfg_.initial_timeout_ns;
            e.attempts   = 1;
            ++stats_.requests_sent;
            return RetransmitRequest{e.first, e.count};
        }
        return std::nullopt;
    }

    // The server answered with count messages starting at first_seq. A server
    // answers from the front of the range it was asked for, so the overlap is
    // trimmed off the front of each outstanding entry and an entry that is
    // fully covered is retired. A response that lands in the middle of a range
    // is not split into two entries, because no venue protocol produces one and
    // inventing the case would be untested code on a recovery path.
    void on_response(uint64_t first_seq, uint16_t count, uint64_t now_ns) {
        (void)now_ns;  // kept in the signature so a future rate limiter has it
        if (count == 0) return;
        const uint64_t resp_end = first_seq + count;
        bool           matched  = false;

        for (std::size_t i = 0; i < entries_.size();) {
            Entry&         e     = entries_[i];
            const uint64_t e_end = e.first + e.count;
            if (resp_end <= e.first || first_seq >= e_end) { ++i; continue; }

            matched = true;
            if (resp_end >= e_end) {
                stats_.sequences_filled += e_end - (first_seq > e.first ? first_seq : e.first);
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (first_seq <= e.first) {
                stats_.sequences_filled += resp_end - e.first;
                e.count = static_cast<uint16_t>(e_end - resp_end);
                e.first = resp_end;
            }
            ++i;
        }
        if (matched) ++stats_.responses;
    }

    // Give up on everything, which is what a session reset means.
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] std::size_t          outstanding() const noexcept { return entries_.size(); }
    [[nodiscard]] const RecoveryStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const RecoveryConfig& config() const noexcept { return cfg_; }

private:
    static constexpr uint64_t kMaxCount = 65535;

    struct Entry {
        uint64_t first      = 0;
        uint16_t count      = 0;
        uint64_t sent_ns    = 0;
        uint64_t timeout_ns = 0;
        uint32_t attempts   = 0;
        bool     in_flight  = false;
    };

    RecoveryConfig     cfg_;
    std::vector<Entry> entries_;
    RecoveryStats      stats_{};
};

// ---------------------------------------------------------------------------
// Publisher side
// ---------------------------------------------------------------------------

// The last N published messages, keyed by sequence, so a re-request can be
// answered without reading anything back off disk.
//
// Be clear about what this is not. A real venue keeps the entire session, and
// a retransmission server can answer for a sequence from hours ago because it
// reads it out of the session log. This keeps a window in memory and nothing
// else. A request for something that has fallen out of the window is answered
// with a miss, and the caller is expected to tell the client to take a snapshot
// instead. The failure mode being avoided is answering with whatever bytes
// happen to live in that ring slot now, which is a different message with the
// same index, and which would be accepted downstream as genuine.
class RetransmitStore {
public:
    // Capacity is rounded up to a power of two so the sequence to slot mapping
    // is a mask. Both buffers are sized once here and never resized, so storing
    // a message on the publish path is a memcpy and two stores.
    explicit RetransmitStore(std::size_t capacity, std::size_t max_payload = 64)
        : cap_(round_up_pow2(capacity)),
          mask_(cap_ - 1),
          max_payload_(max_payload == 0 ? 1 : max_payload),
          slots_(cap_),
          bytes_(cap_ * max_payload_) {}

    // Returns false when the payload is wider than a slot. Refusing beats
    // truncating, because a truncated message handed back on a re-request looks
    // valid and is not.
    bool store(uint64_t seq, std::span<const std::byte> payload) noexcept {
        if (payload.size() > max_payload_) return false;
        const std::size_t idx = static_cast<std::size_t>(seq & mask_);
        Slot&             s   = slots_[idx];
        if (!payload.empty()) {
            std::memcpy(bytes_.data() + idx * max_payload_, payload.data(), payload.size());
        }
        s.seq     = seq;
        s.len     = static_cast<uint32_t>(payload.size());
        s.present = true;
        return true;
    }

    // Copies the stored payload for seq into out. The sequence stored in the
    // slot is compared against the one asked for, because the mapping aliases
    // every cap_ sequences and a slot that has been overwritten must read as a
    // miss rather than as the wrong message.
    bool fetch(uint64_t seq, std::span<std::byte> out, std::size_t* out_len) const noexcept {
        const std::size_t idx = static_cast<std::size_t>(seq & mask_);
        const Slot&       s   = slots_[idx];
        if (!s.present || s.seq != seq) return false;
        if (out.size() < s.len) return false;
        if (s.len != 0) std::memcpy(out.data(), bytes_.data() + idx * max_payload_, s.len);
        if (out_len) *out_len = s.len;
        return true;
    }

    [[nodiscard]] bool contains(uint64_t seq) const noexcept {
        const Slot& s = slots_[static_cast<std::size_t>(seq & mask_)];
        return s.present && s.seq == seq;
    }

    void clear() noexcept {
        for (Slot& s : slots_) s.present = false;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::size_t max_payload() const noexcept { return max_payload_; }

private:
    struct Slot {
        uint64_t seq     = 0;
        uint32_t len     = 0;
        bool     present = false;
    };

    static std::size_t round_up_pow2(std::size_t n) noexcept {
        std::size_t v = 1;
        while (v < n) v <<= 1;
        return v;
    }

    std::size_t            cap_;
    std::size_t            mask_;
    std::size_t            max_payload_;
    std::vector<Slot>      slots_;
    std::vector<std::byte> bytes_;
};

}  // namespace tick
