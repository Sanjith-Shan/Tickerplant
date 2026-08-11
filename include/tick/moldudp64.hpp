#pragma once

#include "tick/endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

// NASDAQ MoldUDP64, the framing that carries ITCH over multicast.
//
// One UDP datagram holds a twenty byte header and then zero or more message
// blocks. Each block is a two byte big-endian length followed by that many
// bytes of payload, which for this project is one ITCH message.
//
//   offset 0    session           10 bytes ASCII, right padded with spaces
//   offset 10   sequence number    8 bytes, the sequence of the FIRST block
//   offset 18   message count      2 bytes
//   offset 20   message blocks
//
// The sequence number is the sequence of the first block, not of the packet.
// The second block in a packet is sequence + 1, and so on. A receiver that
// tracks the next sequence it expects can therefore detect a gap from the
// header alone, before it looks at a single message, which is the whole reason
// the protocol is shaped this way.
//
// Two counts are special. Zero means a heartbeat, which carries no blocks and
// exists so a receiver can tell a quiet feed from a dead one. 0xFFFF means end
// of session and also carries no blocks. A heartbeat still carries a sequence
// number, so a receiver that missed the tail of the day still learns how far
// behind it is.
//
// A request packet sent to the re-request server is the same twenty bytes with
// a different meaning for the last two fields. Sequence is the first message
// wanted and count is how many. There are no blocks after the header.
//
// This header is the one place in the project where the input is genuinely
// untrusted. Everything else reads a file that was downloaded and checksummed
// or a buffer this process filled itself. These bytes came off a network, from
// a sender this process cannot authenticate, on a path that can truncate,
// duplicate, or corrupt them. PacketView::parse therefore validates every
// length against the real size of the datagram before anything dereferences a
// pointer, and returns nullopt rather than trusting the header's own count.
//
// Nothing here allocates. Decoding borrows the caller's receive buffer and
// encoding writes into a buffer the caller owns.

namespace tick::mold {

inline constexpr std::size_t kHeaderLen  = 20;
inline constexpr std::size_t kSessionLen = 10;

// Field offsets inside the header. Named for the same reason the ITCH offsets
// are named, so a test can assert against the same constants the code uses.
inline constexpr std::size_t kOffSession  = 0;
inline constexpr std::size_t kOffSequence = 10;
inline constexpr std::size_t kOffCount    = 18;

// The two byte length that prefixes every message block.
inline constexpr std::size_t kBlockLenBytes = 2;

inline constexpr uint16_t kEndOfSession = 0xFFFF;

// The largest count a packet carrying real messages may use. 0xFFFF is spoken
// for by end of session, so an encoder that filled a packet all the way to
// 65535 blocks would emit something a conforming receiver reads as the end of
// the day. The builder stops one short.
inline constexpr uint16_t kMaxCount = 0xFFFE;

// A safe payload ceiling for one UDP datagram on a 1500 byte MTU path. 1500
// less 20 bytes of IPv4 header and 8 of UDP leaves 1472, and the margin below
// that covers a tunnel or a VLAN tag in the path without fragmenting. A
// fragmented market data datagram is a datagram that is lost whenever any one
// of its fragments is, so the ceiling is deliberately conservative.
inline constexpr std::size_t kMaxPacketLen = 1400;

struct Header {
    std::array<char, kSessionLen> session{};
    uint64_t                      sequence = 0;
    uint16_t                      count    = 0;
};

namespace detail {

// Write the twenty byte header and nothing else. Returns kHeaderLen, or zero
// when the caller's buffer cannot hold a header at all. Shared by the builder
// and by the heartbeat, end of session, and request helpers, which differ only
// in what they put in the last two fields.
inline std::size_t write_header(std::byte* buf, std::size_t cap, std::string_view session,
                                uint64_t sequence, uint16_t count) noexcept {
    if (buf == nullptr || cap < kHeaderLen) return 0;

    const std::size_t n = session.size() < kSessionLen ? session.size() : kSessionLen;
    for (std::size_t i = 0; i < n; ++i) {
        buf[kOffSession + i] = static_cast<std::byte>(session[i]);
    }
    // Space padded rather than NUL padded. The specification says ASCII spaces
    // and a receiver comparing session identifiers byte for byte would reject
    // a NUL padded one.
    for (std::size_t i = n; i < kSessionLen; ++i) {
        buf[kOffSession + i] = static_cast<std::byte>(' ');
    }

    be_store<uint64_t>(buf + kOffSequence, sequence);
    be_store<uint16_t>(buf + kOffCount, count);
    return kHeaderLen;
}

// Drop the trailing spaces from a fixed width ASCII field.
[[nodiscard]] inline std::string_view trim_right(const char* p, std::size_t n) noexcept {
    while (n > 0 && p[n - 1] == ' ') --n;
    return {p, n};
}

} // namespace detail

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Read just the twenty byte header, with no expectation about what follows.
//
// This exists for the re-request server, which receives request packets whose
// header is the same twenty bytes but whose count is a number of messages
// wanted rather than a number of blocks present. PacketView::parse would reject
// every such packet, correctly, because as a downstream packet it is malformed.
// Trying to use one function for both would mean weakening the validation that
// makes parse worth having.
[[nodiscard]] inline std::optional<Header> parse_header(std::span<const std::byte> pkt) noexcept {
    if (pkt.size() < kHeaderLen) return std::nullopt;
    Header h;
    std::memcpy(h.session.data(), pkt.data() + kOffSession, kSessionLen);
    h.sequence = be_load<uint64_t>(pkt.data() + kOffSequence);
    h.count    = be_load<uint16_t>(pkt.data() + kOffCount);
    return h;
}

// The session out of a Header, padding removed.
[[nodiscard]] inline std::string_view session_of(const Header& h) noexcept {
    return detail::trim_right(h.session.data(), kSessionLen);
}

// A validated view over a datagram the caller still owns. Copies nothing, so it
// is only usable while the receive buffer it was parsed from is intact.
class PacketView {
public:
    // Walks the message blocks. Dereferencing hands back a span over one ITCH
    // message, pointing into the caller's buffer.
    //
    // Tagged as an input iterator rather than a forward iterator because
    // operator* returns a span by value and not a reference to a stored object.
    // That is exactly what std::input_iterator describes, and claiming forward
    // here would be a claim the type does not meet. A range-for needs nothing
    // more than this.
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = std::span<const std::byte>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = void;
        using reference         = value_type;

        iterator() noexcept = default;

        [[nodiscard]] reference operator*() const noexcept {
            const uint16_t len = be_load<uint16_t>(base_ + off_);
            return {base_ + off_ + kBlockLenBytes, len};
        }

        iterator& operator++() noexcept {
            const uint16_t len = be_load<uint16_t>(base_ + off_);
            off_ += kBlockLenBytes + len;
            ++index_;
            return *this;
        }

        iterator operator++(int) noexcept {
            iterator prev = *this;
            ++*this;
            return prev;
        }

        // Compared on block index rather than on offset. parse already proved
        // the walk lands exactly on the end of the datagram, so counting blocks
        // is both sufficient and cheaper than carrying an end pointer.
        [[nodiscard]] bool operator==(const iterator& other) const noexcept {
            return index_ == other.index_;
        }
        [[nodiscard]] bool operator!=(const iterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        friend class PacketView;
        iterator(const std::byte* base, std::size_t off, uint32_t index) noexcept
            : base_(base), off_(off), index_(index) {}

        const std::byte* base_  = nullptr;
        std::size_t      off_   = 0;
        uint32_t         index_ = 0;
    };

    // Validate a datagram. Returns nullopt for anything that does not describe
    // itself consistently, and never reads past pkt.
    //
    // Rejected: a datagram shorter than a header, a block length that runs past
    // the end, a count that promises more blocks than the bytes can hold, and a
    // count that leaves bytes over at the end. The last one matters as much as
    // the others. Trailing bytes mean the sender and this reader disagree about
    // the framing, and a reader that shrugs and processes the blocks it did
    // understand is a reader that will hand a truncated book to a strategy.
    [[nodiscard]] static std::optional<PacketView> parse(std::span<const std::byte> pkt) noexcept {
        if (pkt.size() < kHeaderLen) return std::nullopt;

        const std::byte* p = pkt.data();

        PacketView v;
        v.pkt_ = pkt;
        std::memcpy(v.hdr_.session.data(), p + kOffSession, kSessionLen);
        v.hdr_.sequence = be_load<uint64_t>(p + kOffSequence);
        v.hdr_.count    = be_load<uint16_t>(p + kOffCount);

        if (v.hdr_.count == kEndOfSession) {
            // End of session carries no blocks. Anything after the header means
            // the count field was not what the sender thought it was.
            if (pkt.size() != kHeaderLen) return std::nullopt;
            v.blocks_ = 0;
            return v;
        }

        std::size_t off = kHeaderLen;
        for (uint32_t i = 0; i < v.hdr_.count; ++i) {
            // Subtraction rather than addition on purpose. off is an invariant
            // no greater than pkt.size() at the top of every iteration, so
            // pkt.size() - off cannot wrap, while off + len could.
            if (pkt.size() - off < kBlockLenBytes) return std::nullopt;
            const uint16_t len = be_load<uint16_t>(p + off);
            off += kBlockLenBytes;
            if (pkt.size() - off < len) return std::nullopt;
            off += len;
        }
        if (off != pkt.size()) return std::nullopt;

        v.blocks_ = v.hdr_.count;
        return v;
    }

    [[nodiscard]] const Header& header() const noexcept { return hdr_; }
    [[nodiscard]] uint64_t sequence() const noexcept { return hdr_.sequence; }
    [[nodiscard]] uint16_t count() const noexcept { return hdr_.count; }

    [[nodiscard]] bool is_heartbeat() const noexcept { return hdr_.count == 0; }
    [[nodiscard]] bool is_end_of_session() const noexcept { return hdr_.count == kEndOfSession; }

    // The session with its padding removed. The view points into the caller's
    // datagram rather than into this object's own copy of the field, so it
    // survives copying the PacketView and dies with the receive buffer, which
    // is the same lifetime every other view in this project has.
    [[nodiscard]] std::string_view session() const noexcept {
        return detail::trim_right(reinterpret_cast<const char*>(pkt_.data() + kOffSession),
                                  kSessionLen);
    }

    // The whole datagram, header included. Useful to a recorder that wants to
    // write the raw bytes back out.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return pkt_; }

    [[nodiscard]] iterator begin() const noexcept {
        return iterator(pkt_.data(), kHeaderLen, 0);
    }
    [[nodiscard]] iterator end() const noexcept {
        return iterator(pkt_.data(), 0, blocks_);
    }

private:
    PacketView() noexcept = default;

    std::span<const std::byte> pkt_{};
    Header                     hdr_{};
    // The number of blocks actually present, which differs from hdr_.count for
    // an end of session packet where the count is a marker and not a length.
    uint32_t                   blocks_ = 0;
};

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Fills a caller-owned buffer. The builder never owns memory and never
// allocates, so a publisher can hand it the same stack buffer on every packet
// and the send path stays free of heap traffic.
class PacketBuilder {
public:
    PacketBuilder(std::byte* buf, std::size_t cap, std::string_view session) noexcept
        : buf_(buf), cap_(cap) {
        const std::size_t n = session.size() < kSessionLen ? session.size() : kSessionLen;
        for (std::size_t i = 0; i < n; ++i) session_[i] = session[i];
        for (std::size_t i = n; i < kSessionLen; ++i) session_[i] = ' ';
        reset(0);
    }

    // Start a new packet whose first message will carry this sequence number.
    // The header is rewritten in full rather than patched, so a buffer reused
    // after a heartbeat or an end of session packet cannot keep a stale count.
    void reset(uint64_t first_sequence) noexcept {
        size_  = detail::write_header(buf_, cap_, {session_.data(), kSessionLen},
                                      first_sequence, 0);
        count_ = 0;
    }

    // Append one message block. Returns false when the message would not fit or
    // when the count is already at its ceiling, and in both cases the packet is
    // left exactly as it was so the caller can finish it and start another.
    [[nodiscard]] bool try_append(std::span<const std::byte> msg) noexcept {
        if (size_ == 0) return false;                 // buffer too small for a header
        if (count_ >= kMaxCount) return false;
        if (msg.size() > 0xFFFFu) return false;       // will not fit a two byte length
        const std::size_t need = kBlockLenBytes + msg.size();
        if (cap_ - size_ < need) return false;

        be_store<uint16_t>(buf_ + size_, static_cast<uint16_t>(msg.size()));
        if (!msg.empty()) {
            std::memcpy(buf_ + size_ + kBlockLenBytes, msg.data(), msg.size());
        }
        size_ += need;
        ++count_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] uint16_t count() const noexcept { return count_; }

    // How many more payload bytes would fit, block length prefix included. A
    // publisher batching messages uses this to decide whether to flush.
    [[nodiscard]] std::size_t remaining() const noexcept {
        return size_ == 0 ? 0 : cap_ - size_;
    }

    // Stamp the count into the header and hand back the finished packet. The
    // count is written here rather than on every append because a packet under
    // construction has no valid count until it is closed, and writing it once
    // keeps the append path to a store and a memcpy.
    [[nodiscard]] std::span<const std::byte> finish() noexcept {
        if (size_ == 0) return {};
        be_store<uint16_t>(buf_ + kOffCount, count_);
        return {buf_, size_};
    }

    // A heartbeat. next_seq is the sequence the next real message will carry,
    // so a receiver that has fallen behind learns the gap size without waiting
    // for traffic to resume.
    static std::size_t heartbeat(std::byte* buf, std::size_t cap, std::string_view session,
                                 uint64_t next_seq) noexcept {
        return detail::write_header(buf, cap, session, next_seq, 0);
    }

    static std::size_t end_of_session(std::byte* buf, std::size_t cap, std::string_view session,
                                      uint64_t next_seq) noexcept {
        return detail::write_header(buf, cap, session, next_seq, kEndOfSession);
    }

    // A re-request, sent by a receiver over unicast to the recovery server.
    // Identical bytes to a downstream header, read differently by the far side.
    static std::size_t request(std::byte* buf, std::size_t cap, std::string_view session,
                               uint64_t first_seq, uint16_t count) noexcept {
        return detail::write_header(buf, cap, session, first_seq, count);
    }

private:
    std::byte*                    buf_  = nullptr;
    std::size_t                   cap_  = 0;
    std::size_t                   size_ = 0;   // zero means the buffer is unusable
    uint16_t                      count_ = 0;
    std::array<char, kSessionLen> session_{};
};

} // namespace tick::mold
