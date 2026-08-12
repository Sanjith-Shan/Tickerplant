#include "tick/moldudp64.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Tests for the MoldUDP64 framing.
//
// The important test in this file is WireFormatMatchesHandBuiltBytes. Every
// other test here checks that the encoder and the decoder agree with each
// other, which they would also do if both had the same offset wrong. The hand
// built byte array is the only thing in the file that checks them against the
// specification rather than against themselves, and it is the test that would
// fail if the sequence number and the count were ever transposed.

namespace {

using tick::mold::PacketBuilder;
using tick::mold::PacketView;

// Turn a literal list of octets into bytes, so the test bodies can read like
// the hex dump in the specification.
std::vector<std::byte> bytes_of(std::initializer_list<int> octets) {
    std::vector<std::byte> out;
    out.reserve(octets.size());
    for (int v : octets) out.push_back(static_cast<std::byte>(v));
    return out;
}

std::span<const std::byte> as_span(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string_view as_text(std::span<const std::byte> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

// The canonical packet this file is built around, written out octet by octet.
//
//   session         'T','I','C','K','0','0','0','0','0','1'
//   sequence        0x0000000000000001
//   count           0x0002
//   block 0         length 0x0002, payload "AB"
//   block 1         length 0x0003, payload "XYZ"
//
// Twenty header bytes, four for the first block, five for the second, so
// twenty nine in total.
std::vector<std::byte> canonical_packet() {
    return bytes_of({
        0x54, 0x49, 0x43, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,  // "TICK000001"
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // sequence 1
        0x00, 0x02,                                                   // count 2
        0x00, 0x02, 0x41, 0x42,                                       // "AB"
        0x00, 0x03, 0x58, 0x59, 0x5A,                                 // "XYZ"
    });
}

} // namespace

// ---------------------------------------------------------------------------
// The wire format itself
// ---------------------------------------------------------------------------

TEST(MoldUdp64, WireFormatMatchesHandBuiltBytes) {
    const std::vector<std::byte> expected = canonical_packet();
    ASSERT_EQ(expected.size(), 29u);

    std::array<std::byte, 64> buf{};
    PacketBuilder             b(buf.data(), buf.size(), "TICK000001");
    b.reset(1);
    ASSERT_TRUE(b.try_append(as_span("AB")));
    ASSERT_TRUE(b.try_append(as_span("XYZ")));

    const std::span<const std::byte> got = b.finish();
    ASSERT_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<int>(got[i]), static_cast<int>(expected[i]))
            << "byte " << i << " differs from the hand built packet";
    }
}

TEST(MoldUdp64, DecodesHandBuiltBytes) {
    const std::vector<std::byte> pkt = canonical_packet();

    const auto view = PacketView::parse(pkt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->session(), "TICK000001");
    EXPECT_EQ(view->sequence(), 1u);
    EXPECT_EQ(view->count(), 2u);
    EXPECT_FALSE(view->is_heartbeat());
    EXPECT_FALSE(view->is_end_of_session());

    std::vector<std::string_view> payloads;
    for (std::span<const std::byte> msg : *view) payloads.push_back(as_text(msg));

    ASSERT_EQ(payloads.size(), 2u);
    EXPECT_EQ(payloads[0], "AB");
    EXPECT_EQ(payloads[1], "XYZ");
}

TEST(MoldUdp64, HeaderFieldOffsetsAreWhereTheSpecificationSaysTheyAre) {
    EXPECT_EQ(tick::mold::kOffSession, 0u);
    EXPECT_EQ(tick::mold::kOffSequence, 10u);
    EXPECT_EQ(tick::mold::kOffCount, 18u);
    EXPECT_EQ(tick::mold::kHeaderLen, 20u);
    EXPECT_EQ(tick::mold::kSessionLen, 10u);
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(MoldUdp64, HeaderRoundTrip) {
    std::array<std::byte, 64> buf{};
    PacketBuilder             b(buf.data(), buf.size(), "SESSION-42");
    b.reset(0x0102030405060708ull);
    ASSERT_TRUE(b.try_append(as_span("m")));

    const auto view = PacketView::parse(b.finish());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->sequence(), 0x0102030405060708ull);
    EXPECT_EQ(view->count(), 1u);
    EXPECT_EQ(view->session(), "SESSION-42");
}

TEST(MoldUdp64, ShortSessionIsSpacePaddedAndTrimmedBack) {
    std::array<std::byte, 64> buf{};
    PacketBuilder             b(buf.data(), buf.size(), "MOLD");
    b.reset(7);

    const std::span<const std::byte> pkt = b.finish();
    // Padding is spaces and not NULs, which is what a receiver comparing the
    // raw ten bytes against its configured session name expects.
    for (std::size_t i = 4; i < tick::mold::kSessionLen; ++i) {
        EXPECT_EQ(static_cast<char>(pkt[i]), ' ');
    }

    const auto view = PacketView::parse(pkt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->session(), "MOLD");
}

TEST(MoldUdp64, SessionLongerThanTenBytesIsTruncated) {
    std::array<std::byte, 64> buf{};
    PacketBuilder             b(buf.data(), buf.size(), "THIS-IS-FAR-TOO-LONG");
    b.reset(1);

    const auto view = PacketView::parse(b.finish());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->session(), "THIS-IS-FA");
}

TEST(MoldUdp64, MultiMessageRoundTripPreservesOrderAndSequence) {
    std::array<std::byte, tick::mold::kMaxPacketLen> buf{};
    PacketBuilder                                    b(buf.data(), buf.size(), "TICK000001");
    b.reset(1000);

    const std::array<std::string_view, 5> sent{"alpha", "b", "gamma-delta", "", "omega"};
    for (std::string_view s : sent) ASSERT_TRUE(b.try_append(as_span(s)));
    EXPECT_EQ(b.count(), 5u);

    const auto view = PacketView::parse(b.finish());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->count(), 5u);

    // The header carries the sequence of the first block. Every later block is
    // one more than the one before it, which is the property a gap detector
    // depends on.
    uint64_t    expected_seq = view->sequence();
    std::size_t i            = 0;
    for (std::span<const std::byte> msg : *view) {
        ASSERT_LT(i, sent.size());
        EXPECT_EQ(as_text(msg), sent[i]);
        EXPECT_EQ(expected_seq, 1000u + i);
        ++expected_seq;
        ++i;
    }
    EXPECT_EQ(i, sent.size());
}

TEST(MoldUdp64, BuilderRefusesAMessageThatWouldNotFit) {
    // Room for a header and one four byte block and nothing more.
    std::array<std::byte, tick::mold::kHeaderLen + 6> buf{};
    PacketBuilder                                     b(buf.data(), buf.size(), "TICK000001");
    b.reset(1);

    ASSERT_TRUE(b.try_append(as_span("abcd")));
    EXPECT_FALSE(b.try_append(as_span("e")));

    // A refused append must leave the packet exactly as it was, so the caller
    // can close this one and start the next with the message that did not fit.
    EXPECT_EQ(b.count(), 1u);
    EXPECT_EQ(b.size(), tick::mold::kHeaderLen + 6);

    const auto view = PacketView::parse(b.finish());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->count(), 1u);
}

TEST(MoldUdp64, BuilderWithABufferTooSmallForAHeaderStaysUnusable) {
    std::array<std::byte, 8> buf{};
    PacketBuilder            b(buf.data(), buf.size(), "TICK000001");
    b.reset(1);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_FALSE(b.try_append(as_span("x")));
    EXPECT_TRUE(b.finish().empty());
}

TEST(MoldUdp64, ResetClearsThePreviousPacket) {
    std::array<std::byte, 64> buf{};
    PacketBuilder             b(buf.data(), buf.size(), "TICK000001");

    b.reset(1);
    ASSERT_TRUE(b.try_append(as_span("first")));
    (void)b.finish();

    b.reset(2);
    EXPECT_EQ(b.count(), 0u);
    EXPECT_EQ(b.size(), tick::mold::kHeaderLen);

    const auto view = PacketView::parse(b.finish());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->sequence(), 2u);
    EXPECT_TRUE(view->is_heartbeat());
}

// ---------------------------------------------------------------------------
// Heartbeat, end of session, request
// ---------------------------------------------------------------------------

TEST(MoldUdp64, Heartbeat) {
    std::array<std::byte, 32> buf{};
    const std::size_t n = PacketBuilder::heartbeat(buf.data(), buf.size(), "TICK000001", 99);
    ASSERT_EQ(n, tick::mold::kHeaderLen);

    const auto view = PacketView::parse({buf.data(), n});
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->is_heartbeat());
    EXPECT_FALSE(view->is_end_of_session());
    EXPECT_EQ(view->count(), 0u);
    // A heartbeat still says where the stream is, so a receiver that has fallen
    // behind learns the size of its gap without waiting for traffic.
    EXPECT_EQ(view->sequence(), 99u);
    EXPECT_EQ(view->begin(), view->end());
}

TEST(MoldUdp64, EndOfSession) {
    std::array<std::byte, 32> buf{};
    const std::size_t n = PacketBuilder::end_of_session(buf.data(), buf.size(), "TICK000001", 500);
    ASSERT_EQ(n, tick::mold::kHeaderLen);

    // The count field is the marker, written big-endian like everything else.
    EXPECT_EQ(static_cast<int>(buf[18]), 0xFF);
    EXPECT_EQ(static_cast<int>(buf[19]), 0xFF);

    const auto view = PacketView::parse({buf.data(), n});
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->is_end_of_session());
    EXPECT_FALSE(view->is_heartbeat());
    EXPECT_EQ(view->sequence(), 500u);
    // No blocks, despite a count of 65535. The count is a marker here and not
    // a length, and an iterator that trusted it would walk off the datagram.
    EXPECT_EQ(view->begin(), view->end());
}

TEST(MoldUdp64, RequestPacket) {
    std::array<std::byte, 32> buf{};
    const std::size_t n = PacketBuilder::request(buf.data(), buf.size(), "TICK000001", 4242, 100);
    ASSERT_EQ(n, tick::mold::kHeaderLen);

    // Checked byte for byte, because a request is the one packet shape that
    // PacketView::parse deliberately rejects. A twenty byte packet claiming a
    // hundred message blocks is malformed as a downstream packet and is only
    // meaningful to the re-request server, which reads it with parse_header.
    const std::vector<std::byte> expected = bytes_of({
        0x54, 0x49, 0x43, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,  // "TICK000001"
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x92,              // sequence 4242
        0x00, 0x64,                                                   // count 100
    });
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<int>(buf[i]), static_cast<int>(expected[i])) << "byte " << i;
    }

    const auto hdr = tick::mold::parse_header({buf.data(), n});
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->sequence, 4242u);
    EXPECT_EQ(hdr->count, 100u);
    EXPECT_EQ(tick::mold::session_of(*hdr), "TICK000001");

    EXPECT_FALSE(PacketView::parse({buf.data(), n}).has_value());
}

// ---------------------------------------------------------------------------
// Hostile input
//
// These bytes are what an attacker, a broken sender, or a truncating middlebox
// would produce. Every one of them must come back as nullopt, and none of them
// may read past the end of the buffer the test handed in. Running this file
// under the address sanitizer is what proves the second half of that.
// ---------------------------------------------------------------------------

TEST(MoldUdp64, RejectsZeroLengthPacket) {
    EXPECT_FALSE(PacketView::parse({}).has_value());
}

TEST(MoldUdp64, RejectsTruncatedHeader) {
    const std::vector<std::byte> full = canonical_packet();
    for (std::size_t n = 0; n < tick::mold::kHeaderLen; ++n) {
        EXPECT_FALSE(PacketView::parse({full.data(), n}).has_value())
            << "accepted a " << n << " byte packet";
    }
}

TEST(MoldUdp64, RejectsBlockLengthRunningPastTheEnd) {
    // Count of one, a block claiming a thousand bytes, and five bytes present.
    const std::vector<std::byte> pkt = bytes_of({
        0x54, 0x49, 0x43, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x01,                                      // count 1
        0x03, 0xE8,                                      // block length 1000
        0x41, 0x42, 0x43,                                // only three bytes follow
    });
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, RejectsCountLargerThanTheBlocksPresent) {
    // Count of three, one complete block, nothing after it.
    const std::vector<std::byte> pkt = bytes_of({
        0x54, 0x49, 0x43, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x03,                                      // count 3
        0x00, 0x02, 0x41, 0x42,                          // one block, "AB"
    });
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, RejectsCountSmallerThanTheBlocksPresent) {
    // The canonical packet with its count knocked down to one. The first block
    // parses, and then four bytes are left over that nothing accounts for.
    std::vector<std::byte> pkt = canonical_packet();
    pkt[19]                    = static_cast<std::byte>(0x01);
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, RejectsATrailingByteAfterTheLastBlock) {
    std::vector<std::byte> pkt = canonical_packet();
    pkt.push_back(static_cast<std::byte>(0x00));
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, RejectsABlockLengthWithNoRoomForTheLengthItself) {
    // Count of two, the first block consumes everything, and there is one byte
    // left where a two byte length has to start.
    const std::vector<std::byte> pkt = bytes_of({
        0x54, 0x49, 0x43, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x02,                                      // count 2
        0x00, 0x02, 0x41, 0x42,                          // block 0, "AB"
        0x00,                                            // half a length
    });
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, RejectsEndOfSessionCarryingPayload) {
    std::vector<std::byte> pkt = canonical_packet();
    pkt[18]                    = static_cast<std::byte>(0xFF);
    pkt[19]                    = static_cast<std::byte>(0xFF);
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}

TEST(MoldUdp64, TruncatingAValidPacketAnywhereIsRejected) {
    // Every prefix of a good packet except the whole thing is a datagram that
    // lost its tail, and none of them may parse.
    const std::vector<std::byte> full = canonical_packet();
    for (std::size_t n = 0; n < full.size(); ++n) {
        EXPECT_FALSE(PacketView::parse({full.data(), n}).has_value())
            << "accepted a packet truncated to " << n << " bytes";
    }
    EXPECT_TRUE(PacketView::parse(full).has_value());
}

TEST(MoldUdp64, HeartbeatWithTrailingBytesIsRejected) {
    std::vector<std::byte> pkt = canonical_packet();
    pkt[18]                    = static_cast<std::byte>(0x00);
    pkt[19]                    = static_cast<std::byte>(0x00);
    EXPECT_FALSE(PacketView::parse(pkt).has_value());
}
