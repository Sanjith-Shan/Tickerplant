#include "tick/ouch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

// Tests for the OUCH 4.2 wire layer.
//
// The byte vectors below are built by hand from the field tables in the
// specification rather than by calling the encoder, because a test that encodes
// and then decodes with the same offset constants passes just as happily when
// both are wrong. Every numeric field is given a value whose bytes are all
// different from each other and from every other field's, so a decoder reading
// one field at another field's offset produces a number that cannot be mistaken
// for the right one.

using namespace tick;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    out.reserve(v.size());
    for (int b : v) out.push_back(static_cast<std::byte>(static_cast<uint8_t>(b)));
    return out;
}

// Fourteen characters, all distinct, so a token read one byte off is visible.
constexpr const char* kTok = "TOKEN12345678";   // thirteen, padded to fourteen
constexpr const char* kTok2 = "REPLACEMENT01";

// A recording handler. Inherits the do-nothing base so each test only writes
// down the callback it cares about.
struct Recorder : ouch::NullOuchHandler {
    int system_events = 0, accepted = 0, replaced = 0;
    int canceled = 0, executed = 0, rejected = 0, other = 0;

    uint64_t          ts = 0;
    char              code = '\0';
    ouch::Token       token{};
    ouch::AcceptedMsg acc{};
    ouch::ReplacedMsg rep{};
    uint32_t          shares = 0, price = 0;
    char              flag = '\0', reason = '\0';
    uint64_t          match = 0;
    char              other_type = '\0';

    void on_system_event(uint64_t t, char c) noexcept { ++system_events; ts = t; code = c; }
    void on_accepted(const ouch::AcceptedMsg& a) noexcept { ++accepted; acc = a; ts = a.timestamp; }
    void on_replaced(const ouch::ReplacedMsg& r) noexcept { ++replaced; rep = r; ts = r.common.timestamp; }
    void on_canceled(uint64_t t, const ouch::Token& k, uint32_t n, char r) noexcept {
        ++canceled; ts = t; token = k; shares = n; reason = r;
    }
    void on_executed(uint64_t t, const ouch::Token& k, uint32_t n, uint32_t px,
                     char lf, uint64_t m) noexcept {
        ++executed; ts = t; token = k; shares = n; price = px; flag = lf; match = m;
    }
    void on_rejected(uint64_t t, const ouch::Token& k, char r) noexcept {
        ++rejected; ts = t; token = k; reason = r;
    }
    void on_other(char type, uint64_t t) noexcept { ++other; other_type = type; ts = t; }
};

} // namespace

// ---------------------------------------------------------------------------
// The length tables, checked against the specification
// ---------------------------------------------------------------------------

// Lengths are transcribed from the field tables of O*U*C*H Version 4.2 at
// nasdaqtrader.com. Each one is the last field's offset plus that field's
// length. Asserting the constants directly means a future edit to an offset
// that changes a message's size cannot pass silently.
TEST(OuchLengths, InboundMatchesSpecification) {
    EXPECT_EQ(ouch::in::enter::kLen, 49u);    // 2.1, Customer Type at 48 len 1
    EXPECT_EQ(ouch::in::replace::kLen, 47u);  // 2.2, Minimum Quantity at 43 len 4
    EXPECT_EQ(ouch::in::cancel::kLen, 19u);   // 2.3, Shares at 15 len 4
    EXPECT_EQ(ouch::in::modify::kLen, 20u);   // 2.4, Shares at 16 len 4

    EXPECT_EQ(ouch::in::message_length('O'), ouch::in::enter::kLen);
    EXPECT_EQ(ouch::in::message_length('U'), ouch::in::replace::kLen);
    EXPECT_EQ(ouch::in::message_length('X'), ouch::in::cancel::kLen);
    EXPECT_EQ(ouch::in::message_length('M'), ouch::in::modify::kLen);
    EXPECT_EQ(ouch::in::message_length('A'), 0u);  // outbound only
    EXPECT_EQ(ouch::in::message_length('\0'), 0u);
}

TEST(OuchLengths, OutboundMatchesSpecification) {
    EXPECT_EQ(ouch::out::sysevt::kLen, 10u);    // 3.1
    EXPECT_EQ(ouch::out::accepted::kLen, 66u);  // 3.3, BBO Weight at 65 len 1
    EXPECT_EQ(ouch::out::replaced::kLen, 80u);  // 3.4, BBO Weight at 79 len 1
    EXPECT_EQ(ouch::out::canceled::kLen, 28u);  // 3.5, Reason at 27 len 1
    EXPECT_EQ(ouch::out::kAiqCanceledLen, 38u); // 3.6
    EXPECT_EQ(ouch::out::executed::kLen, 40u);  // 3.7, Match Number at 32 len 8
    EXPECT_EQ(ouch::out::kBrokenTradeLen, 32u); // 3.8
    EXPECT_EQ(ouch::out::kExecutedWithRefPriceLen, 45u); // 3.9
    EXPECT_EQ(ouch::out::rejected::kLen, 24u);  // 3.10, Reason at 23 len 1
    EXPECT_EQ(ouch::out::kCancelPendingLen, 23u);  // 3.11
    EXPECT_EQ(ouch::out::kCancelRejectLen, 23u);   // 3.12
    EXPECT_EQ(ouch::out::kPriorityUpdateLen, 36u); // 3.13
    EXPECT_EQ(ouch::out::kOrderModifiedLen, 28u);  // 3.14
}

// 'U' is Replace Order going out and Replaced coming back. 'M' is Modify Order
// going out and Order Modified coming back. One table indexed by the type byte
// would have to pick a side and be wrong on the other.
TEST(OuchLengths, TypeByteIsOnlyUniqueWithinADirection) {
    EXPECT_NE(ouch::in::message_length('U'), ouch::out::message_length('U'));
    EXPECT_NE(ouch::in::message_length('M'), ouch::out::message_length('M'));
    EXPECT_EQ(ouch::in::message_length('U'), 47u);
    EXPECT_EQ(ouch::out::message_length('U'), 80u);
}

TEST(OuchLengths, MaxMessageLenCoversEveryKnownMessage) {
    for (int c = 0; c < 256; ++c) {
        const char t = static_cast<char>(c);
        EXPECT_LE(ouch::in::message_length(t), ouch::in::kMaxMessageLen);
        EXPECT_LE(ouch::out::message_length(t), ouch::out::kMaxMessageLen);
    }
    EXPECT_EQ(ouch::in::kMaxMessageLen, 49u);
    EXPECT_EQ(ouch::out::kMaxMessageLen, 80u);
}

// ---------------------------------------------------------------------------
// Inbound, hand-built bytes
// ---------------------------------------------------------------------------

// Enter Order from section 2.1, built byte by byte.
//   0  'O'
//   1  token          "TOKEN12345678 "
//   15 side           'B'
//   16 shares         0x11223344
//   20 stock          "AAPL    "
//   28 price          0x55667788
//   32 time in force  0x000186A0
//   36 firm           "ABCD"
//   40 display        'Y'
//   41 capacity       'P'
//   42 iso            'N'
//   43 min quantity   0x99AABBCC
//   47 cross type     'N'
//   48 customer type  'R'
TEST(OuchInbound, EnterOrderHandBuiltBytesDecode) {
    auto b = bytes({
        'O',
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        'B',
        0x11, 0x22, 0x33, 0x44,
        'A','A','P','L',' ',' ',' ',' ',
        0x55, 0x66, 0x77, 0x88,
        0x00, 0x01, 0x86, 0xA0,
        'A','B','C','D',
        'Y',
        'P',
        'N',
        0x99, 0xAA, 0xBB, 0xCC,
        'N',
        'R',
    });
    ASSERT_EQ(b.size(), ouch::in::enter::kLen);

    ouch::EnterOrder o;
    ASSERT_TRUE(ouch::decode_enter_order(b, o));
    EXPECT_EQ(o.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(o.side, 'B');
    EXPECT_EQ(o.shares, 0x11223344u);
    EXPECT_EQ(ouch::trim(o.stock), "AAPL");
    EXPECT_EQ(o.price, 0x55667788u);
    EXPECT_EQ(o.time_in_force, 0x000186A0u);
    EXPECT_EQ(ouch::trim(o.firm), "ABCD");
    EXPECT_EQ(o.display, 'Y');
    EXPECT_EQ(o.capacity, 'P');
    EXPECT_EQ(o.iso, 'N');
    EXPECT_EQ(o.min_quantity, 0x99AABBCCu);
    EXPECT_EQ(o.cross_type, 'N');
    EXPECT_EQ(o.customer_type, 'R');
}

// Encoding the same fields must reproduce the same bytes. This is the half that
// catches an encoder writing a field at the right value and the wrong place.
TEST(OuchInbound, EnterOrderEncodesToTheSameBytes) {
    auto expected = bytes({
        'O',
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        'B',
        0x11, 0x22, 0x33, 0x44,
        'A','A','P','L',' ',' ',' ',' ',
        0x55, 0x66, 0x77, 0x88,
        0x00, 0x01, 0x86, 0xA0,
        'A','B','C','D',
        'Y', 'P', 'N',
        0x99, 0xAA, 0xBB, 0xCC,
        'N', 'R',
    });

    ouch::EnterOrder o;
    o.token         = ouch::Token::from(kTok);
    o.side          = 'B';
    o.shares        = 0x11223344u;
    o.stock         = ouch::alpha<ouch::kStockLen>("AAPL");
    o.price         = 0x55667788u;
    o.time_in_force = 0x000186A0u;
    o.firm          = ouch::alpha<ouch::kFirmLen>("ABCD");
    o.display       = 'Y';
    o.capacity      = 'P';
    o.iso           = 'N';
    o.min_quantity  = 0x99AABBCCu;
    o.cross_type    = 'N';
    o.customer_type = 'R';

    std::array<std::byte, ouch::in::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode(buf, o), ouch::in::enter::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), expected.data(), expected.size()), 0);
}

// Replace Order from section 2.2. Two tokens, and no stock field.
//   0  'U'
//   1  existing token     "TOKEN12345678 "
//   15 replacement token  "REPLACEMENT01 "
//   29 shares             0x0A0B0C0D
//   33 price              0x1E1F2021
//   37 time in force      0x0001869F
//   41 display            'N'
//   42 iso                'Y'
//   43 min quantity       0x3C3D3E3F
TEST(OuchInbound, ReplaceOrderHandBuiltBytesDecode) {
    auto b = bytes({
        'U',
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        'R','E','P','L','A','C','E','M','E','N','T','0','1',' ',
        0x0A, 0x0B, 0x0C, 0x0D,
        0x1E, 0x1F, 0x20, 0x21,
        0x00, 0x01, 0x86, 0x9F,
        'N',
        'Y',
        0x3C, 0x3D, 0x3E, 0x3F,
    });
    ASSERT_EQ(b.size(), ouch::in::replace::kLen);

    ouch::ReplaceOrder o;
    ASSERT_TRUE(ouch::decode_replace_order(b, o));
    EXPECT_EQ(o.existing_token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(o.replacement_token.trimmed(), "REPLACEMENT01");
    EXPECT_EQ(o.shares, 0x0A0B0C0Du);
    EXPECT_EQ(o.price, 0x1E1F2021u);
    EXPECT_EQ(o.time_in_force, ouch::kTifSystemHours);
    EXPECT_EQ(o.display, 'N');
    EXPECT_EQ(o.iso, 'Y');
    EXPECT_EQ(o.min_quantity, 0x3C3D3E3Fu);

    std::array<std::byte, ouch::in::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode(buf, o), ouch::in::replace::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), b.data(), b.size()), 0);
}

// Cancel Order from section 2.3. Shares is the new intended order size, not a
// decrement, and zero means cancel the balance.
TEST(OuchInbound, CancelOrderHandBuiltBytesDecode) {
    auto b = bytes({
        'X',
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        0x00, 0x00, 0x01, 0xF4,   // intended size 500
    });
    ASSERT_EQ(b.size(), ouch::in::cancel::kLen);

    ouch::CancelOrder o;
    ASSERT_TRUE(ouch::decode_cancel_order(b, o));
    EXPECT_EQ(o.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(o.intended_shares, 500u);

    std::array<std::byte, ouch::in::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode(buf, o), ouch::in::cancel::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), b.data(), b.size()), 0);
}

TEST(OuchInbound, FullCancelIsAZeroIntendedSize) {
    ouch::CancelOrder o;
    o.token           = ouch::Token::from(kTok);
    o.intended_shares = 0;

    std::array<std::byte, ouch::in::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode(buf, o), ouch::in::cancel::kLen);
    EXPECT_EQ(be_load<uint32_t>(buf.data() + ouch::in::cancel::kShares), 0u);
}

// ---------------------------------------------------------------------------
// Outbound, hand-built bytes
// ---------------------------------------------------------------------------

// System Event from section 3.1. Ten bytes, and the timestamp is a full eight
// bytes at offset one rather than the six bytes at offset five that ITCH uses.
TEST(OuchOutbound, SystemEventHandBuiltBytesDecode) {
    auto b = bytes({
        'S',
        0x00, 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F,
        'S',
    });
    ASSERT_EQ(b.size(), ouch::out::sysevt::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    std::size_t       consumed = 0;
    ASSERT_EQ(dec.decode(b, rec, &consumed), ouch::DecodeResult::Ok);
    EXPECT_EQ(consumed, ouch::out::sysevt::kLen);
    EXPECT_EQ(rec.system_events, 1);
    EXPECT_EQ(rec.ts, 0x00001A2B3C4D5E6Full);
    EXPECT_EQ(rec.code, static_cast<char>(ouch::out::EventCode::StartOfDay));
}

// Accepted from section 3.3, sixty six bytes.
//   0  'A'
//   1  timestamp      0x0102030405060708
//   9  token          "TOKEN12345678 "
//   23 side           'S'
//   24 shares         0x0000012C  (300)
//   28 stock          "MSFT    "
//   36 price          0x000186A0
//   40 time in force  0x0001869E
//   44 firm           "WXYZ"
//   48 display        'A'
//   49 reference      0x1122334455667788
//   57 capacity       'A'
//   58 iso            'y'
//   59 min quantity   0x0000006E  (110)
//   63 cross type     'O'
//   64 order state    'L'
//   65 bbo weight     '2'
TEST(OuchOutbound, AcceptedHandBuiltBytesDecode) {
    auto b = bytes({
        'A',
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        'S',
        0x00, 0x00, 0x01, 0x2C,
        'M','S','F','T',' ',' ',' ',' ',
        0x00, 0x01, 0x86, 0xA0,
        0x00, 0x01, 0x86, 0x9E,
        'W','X','Y','Z',
        'A',
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        'A',
        'y',
        0x00, 0x00, 0x00, 0x6E,
        'O',
        'L',
        '2',
    });
    ASSERT_EQ(b.size(), ouch::out::accepted::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    std::size_t       consumed = 0;
    ASSERT_EQ(dec.decode(b, rec, &consumed), ouch::DecodeResult::Ok);
    EXPECT_EQ(consumed, ouch::out::accepted::kLen);
    ASSERT_EQ(rec.accepted, 1);

    const auto& a = rec.acc;
    EXPECT_EQ(a.timestamp, 0x0102030405060708ull);
    EXPECT_EQ(a.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(a.side, 'S');
    EXPECT_EQ(a.shares, 300u);
    EXPECT_EQ(a.symbol(), "MSFT");
    EXPECT_EQ(a.price, 100000u);
    EXPECT_EQ(a.time_in_force, ouch::kTifMarketHours);
    EXPECT_EQ(ouch::trim(a.firm), "WXYZ");
    EXPECT_EQ(a.display, 'A');
    EXPECT_EQ(a.reference, 0x1122334455667788ull);
    EXPECT_EQ(a.capacity, 'A');
    EXPECT_EQ(a.iso, 'y');
    EXPECT_EQ(a.min_quantity, 110u);
    EXPECT_EQ(a.cross_type, 'O');
    EXPECT_EQ(a.order_state, 'L');
    EXPECT_EQ(a.bbo_weight, '2');
    EXPECT_FALSE(a.dead());
}

TEST(OuchOutbound, AcceptedRoundTripsThroughTheEncoder) {
    ouch::AcceptedMsg a;
    a.timestamp     = 0x0102030405060708ull;
    a.token         = ouch::Token::from(kTok);
    a.side          = 'S';
    a.shares        = 300;
    a.stock         = ouch::alpha<ouch::kStockLen>("MSFT");
    a.price         = 100000;
    a.time_in_force = ouch::kTifMarketHours;
    a.firm          = ouch::alpha<ouch::kFirmLen>("WXYZ");
    a.display       = 'A';
    a.reference     = 0x1122334455667788ull;
    a.capacity      = 'A';
    a.iso           = 'y';
    a.min_quantity  = 110;
    a.cross_type    = 'O';
    a.order_state   = 'L';
    a.bbo_weight    = '2';

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_accepted(buf, a), ouch::out::accepted::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    ASSERT_EQ(dec.decode(std::span<const std::byte>(buf).first(ouch::out::accepted::kLen),
                         rec),
              ouch::DecodeResult::Ok);
    ASSERT_EQ(rec.accepted, 1);
    EXPECT_EQ(rec.acc.timestamp, a.timestamp);
    EXPECT_EQ(rec.acc.token, a.token);
    EXPECT_EQ(rec.acc.shares, a.shares);
    EXPECT_EQ(rec.acc.price, a.price);
    EXPECT_EQ(rec.acc.reference, a.reference);
    EXPECT_EQ(rec.acc.min_quantity, a.min_quantity);
    EXPECT_EQ(rec.acc.bbo_weight, a.bbo_weight);
    EXPECT_EQ(rec.acc.symbol(), "MSFT");
}

// Order State 'D' means accepted and immediately killed, with nothing further
// to come for the token.
TEST(OuchOutbound, AcceptedOrderDeadIsTerminal) {
    ouch::AcceptedMsg a;
    a.token       = ouch::Token::from(kTok);
    a.order_state = static_cast<char>(ouch::out::OrderState::Dead);
    EXPECT_TRUE(a.dead());
    a.order_state = static_cast<char>(ouch::out::OrderState::Live);
    EXPECT_FALSE(a.dead());
}

// Replaced from section 3.4, eighty bytes. The token at offset nine is the
// REPLACEMENT token. The token that was retired sits at offset sixty five, and
// the BBO Weight indicator that lives at sixty five on Accepted moves to
// seventy nine here. Both tokens are given different text so a decoder that
// reads the wrong one is caught rather than merely producing a token.
TEST(OuchOutbound, ReplacedHandBuiltBytesDecode) {
    auto b = bytes({
        'U',
        0x00, 0x00, 0x00, 0x00, 0x0B, 0xAD, 0xC0, 0xDE,
        'R','E','P','L','A','C','E','M','E','N','T','0','1',' ',
        'B',
        0x00, 0x00, 0x00, 0xC8,   // 200 shares outstanding
        'N','V','D','A',' ',' ',' ',' ',
        0x00, 0x03, 0x0D, 0x40,   // price 200000
        0x00, 0x01, 0x86, 0x9F,   // system hours
        'F','I','R','M',
        'Y',
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39,  // reference 12345
        'P',
        'N',
        0x00, 0x00, 0x00, 0x32,   // min quantity 50
        'N',
        'L',
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        '3',
    });
    ASSERT_EQ(b.size(), ouch::out::replaced::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    std::size_t       consumed = 0;
    ASSERT_EQ(dec.decode(b, rec, &consumed), ouch::DecodeResult::Ok);
    EXPECT_EQ(consumed, ouch::out::replaced::kLen);
    ASSERT_EQ(rec.replaced, 1);
    EXPECT_EQ(rec.accepted, 0);  // a Replaced is not an Accepted

    EXPECT_EQ(rec.rep.common.timestamp, 0x000000000BADC0DEull);
    EXPECT_EQ(rec.rep.common.token.trimmed(), "REPLACEMENT01");
    EXPECT_EQ(rec.rep.previous_token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(rec.rep.common.side, 'B');
    EXPECT_EQ(rec.rep.common.shares, 200u);
    EXPECT_EQ(rec.rep.common.symbol(), "NVDA");
    EXPECT_EQ(rec.rep.common.price, 200000u);
    EXPECT_EQ(rec.rep.common.time_in_force, ouch::kTifSystemHours);
    EXPECT_EQ(ouch::trim(rec.rep.common.firm), "FIRM");
    EXPECT_EQ(rec.rep.common.reference, 12345u);
    EXPECT_EQ(rec.rep.common.min_quantity, 50u);
    EXPECT_EQ(rec.rep.common.order_state, 'L');
    // Read from seventy nine, not sixty five. Sixty five holds a 'T' here.
    EXPECT_EQ(rec.rep.common.bbo_weight, '3');
}

TEST(OuchOutbound, ReplacedRoundTripsAndKeepsTheTwoTokensApart) {
    ouch::ReplacedMsg r;
    r.common.timestamp  = 0x000000000BADC0DEull;
    r.common.token      = ouch::Token::from(kTok2);
    r.common.side       = 'B';
    r.common.shares     = 200;
    r.common.stock      = ouch::alpha<ouch::kStockLen>("NVDA");
    r.common.price      = 200000;
    r.common.bbo_weight = '3';
    r.common.order_state = 'L';
    r.previous_token    = ouch::Token::from(kTok);

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_replaced(buf, r), ouch::out::replaced::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    ASSERT_EQ(dec.decode(buf, rec), ouch::DecodeResult::Ok);
    ASSERT_EQ(rec.replaced, 1);
    EXPECT_EQ(rec.rep.common.token, ouch::Token::from(kTok2));
    EXPECT_EQ(rec.rep.previous_token, ouch::Token::from(kTok));
    EXPECT_NE(rec.rep.common.token, rec.rep.previous_token);
    EXPECT_EQ(rec.rep.common.bbo_weight, '3');
}

// Canceled from section 3.5, twenty eight bytes. Decrement Shares is
// incremental.
TEST(OuchOutbound, CanceledHandBuiltBytesDecode) {
    auto b = bytes({
        'C',
        0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        0x00, 0x00, 0x00, 0x64,   // 100 shares decremented
        'U',                      // user requested
    });
    ASSERT_EQ(b.size(), ouch::out::canceled::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    ASSERT_EQ(dec.decode(b, rec), ouch::DecodeResult::Ok);
    ASSERT_EQ(rec.canceled, 1);
    EXPECT_EQ(rec.ts, 0x00000000DEADBEEFull);
    EXPECT_EQ(rec.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(rec.shares, 100u);
    EXPECT_EQ(rec.reason, static_cast<char>(ouch::out::CancelReason::UserRequested));

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_canceled(buf, 0x00000000DEADBEEFull,
                                    ouch::Token::from(kTok), 100, 'U'),
              ouch::out::canceled::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), b.data(), b.size()), 0);
}

// Executed from section 3.7, forty bytes. There is no reference number and no
// symbol on this message, which is why the token has to be right.
TEST(OuchOutbound, ExecutedHandBuiltBytesDecode) {
    auto b = bytes({
        'E',
        0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78,
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        0x00, 0x00, 0x00, 0x96,   // 150 shares
        0x00, 0x02, 0x5D, 0xE4,   // price 155108
        'R',                      // removed liquidity
        0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
    });
    ASSERT_EQ(b.size(), ouch::out::executed::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    ASSERT_EQ(dec.decode(b, rec), ouch::DecodeResult::Ok);
    ASSERT_EQ(rec.executed, 1);
    EXPECT_EQ(rec.ts, 0x0000000012345678ull);
    EXPECT_EQ(rec.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(rec.shares, 150u);
    EXPECT_EQ(rec.price, 155108u);
    EXPECT_EQ(rec.flag, 'R');
    EXPECT_EQ(rec.match, 0x0A0B0C0D0E0F1011ull);

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_executed(buf, 0x0000000012345678ull,
                                    ouch::Token::from(kTok), 150, 155108, 'R',
                                    0x0A0B0C0D0E0F1011ull),
              ouch::out::executed::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), b.data(), b.size()), 0);
}

// Rejected from section 3.10, twenty four bytes.
TEST(OuchOutbound, RejectedHandBuiltBytesDecode) {
    auto b = bytes({
        'J',
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x10,
        'T','O','K','E','N','1','2','3','4','5','6','7','8',' ',
        'e',   // Risk: Fat Finger
    });
    ASSERT_EQ(b.size(), ouch::out::rejected::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    ASSERT_EQ(dec.decode(b, rec), ouch::DecodeResult::Ok);
    ASSERT_EQ(rec.rejected, 1);
    EXPECT_EQ(rec.ts, 10000u);
    EXPECT_EQ(rec.token.trimmed(), "TOKEN12345678");
    EXPECT_EQ(rec.reason, static_cast<char>(ouch::out::RejectReason::RiskFatFinger));

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_rejected(buf, 10000, ouch::Token::from(kTok), 'e'),
              ouch::out::rejected::kLen);
    EXPECT_EQ(std::memcmp(buf.data(), b.data(), b.size()), 0);
}

// ---------------------------------------------------------------------------
// Malformed and truncated input
// ---------------------------------------------------------------------------

// Every declared length is checked against the span before any body byte is
// touched. Truncating each known message one byte at a time and running the
// decoder over it proves no path reads past the end. Under the sanitized build
// this is the test that would fire on an out of bounds read.
TEST(OuchDecode, TruncatedMessagesNeverReadPastTheEnd) {
    ouch::AcceptedMsg a;
    a.token       = ouch::Token::from(kTok);
    a.stock       = ouch::alpha<ouch::kStockLen>("AAPL");
    a.order_state = 'L';

    ouch::ReplacedMsg r;
    r.common         = a;
    r.previous_token = ouch::Token::from(kTok2);

    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};

    // Each message is encoded into its own buffer and then offered to the
    // decoder at every length shorter than it is. Nothing may return Ok and,
    // under the sanitized build, nothing may read past the span it was given.
    for (int which = 0; which < 6; ++which) {
        buf.fill(std::byte{0});
        std::size_t len = 0;
        switch (which) {
        case 0: len = ouch::encode_system_event(buf, 0x0102030405060708ull, 'S'); break;
        case 1: len = ouch::encode_accepted(buf, a); break;
        case 2: len = ouch::encode_replaced(buf, r); break;
        case 3: len = ouch::encode_canceled(buf, 7, a.token, 25, 'U'); break;
        case 4: len = ouch::encode_executed(buf, 7, a.token, 25, 99, 'A', 3); break;
        default: len = ouch::encode_rejected(buf, 7, a.token, 'O'); break;
        }
        ASSERT_GT(len, 0u) << "case " << which;

        Recorder          rec;
        ouch::OuchDecoder dec;
        for (std::size_t n = 0; n < len; ++n) {
            const auto res = dec.decode(std::span<const std::byte>(buf).first(n), rec);
            EXPECT_NE(res, ouch::DecodeResult::Ok)
                << "case " << which << " decoded from only " << n << " of " << len;
        }
        // The full message still decodes, so the sweep above rejected short
        // buffers rather than rejecting the message itself.
        EXPECT_EQ(dec.decode(std::span<const std::byte>(buf).first(len), rec),
                  ouch::DecodeResult::Ok);
        EXPECT_EQ(dec.stats().messages, 1u);
    }
}

TEST(OuchDecode, TruncatedIsCountedAndReported) {
    std::array<std::byte, ouch::out::kMaxMessageLen> buf{};
    ASSERT_EQ(ouch::encode_executed(buf, 1, ouch::Token::from(kTok), 1, 1, 'A', 1),
              ouch::out::executed::kLen);

    Recorder          rec;
    ouch::OuchDecoder dec;
    const auto        res =
        dec.decode(std::span<const std::byte>(buf).first(ouch::out::executed::kLen - 1), rec);
    EXPECT_EQ(res, ouch::DecodeResult::Truncated);
    EXPECT_EQ(dec.stats().truncated, 1u);
    EXPECT_EQ(dec.stats().messages, 0u);
    EXPECT_EQ(rec.executed, 0);
}

TEST(OuchDecode, EmptyBufferIsTruncatedNotACrash) {
    Recorder          rec;
    ouch::OuchDecoder dec;
    EXPECT_EQ(dec.decode(std::span<const std::byte>{}, rec), ouch::DecodeResult::Truncated);
    EXPECT_EQ(dec.stats().truncated, 1u);
}

TEST(OuchDecode, UnknownTypeByteIsCountedAndNotDispatched) {
    Recorder          rec;
    ouch::OuchDecoder dec;
    for (int c : {0x00, 0x01, 0x5A, 0x71, 0xFF}) {
        auto b = bytes({c, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        if (ouch::out::is_known_type(static_cast<char>(c))) continue;
        EXPECT_EQ(dec.decode(b, rec), ouch::DecodeResult::UnknownType);
    }
    EXPECT_GT(dec.stats().unknown, 0u);
    EXPECT_EQ(dec.stats().messages, 0u);
    EXPECT_EQ(rec.accepted + rec.executed + rec.canceled + rec.rejected, 0);
}

// A message type with a known length but no field decoding must still be
// skipped by the right number of bytes, otherwise one unhandled message
// destroys the framing for everything behind it.
TEST(OuchDecode, KnownButUnhandledTypeIsSkippedByItsFullLength) {
    std::vector<std::byte> b(ouch::out::kBrokenTradeLen, std::byte{0});
    b[0] = static_cast<std::byte>('B');
    be_store<uint64_t>(b.data() + 1, 0x4242424242424242ull);

    Recorder          rec;
    ouch::OuchDecoder dec;
    std::size_t       consumed = 0;
    EXPECT_EQ(dec.decode(b, rec, &consumed), ouch::DecodeResult::Ok);
    EXPECT_EQ(consumed, ouch::out::kBrokenTradeLen);
    EXPECT_EQ(rec.other, 1);
    EXPECT_EQ(rec.other_type, 'B');
    EXPECT_EQ(rec.ts, 0x4242424242424242ull);
}

TEST(OuchEncode, BufferTooSmallReturnsZeroAndWritesNothing) {
    ouch::EnterOrder o;
    o.token = ouch::Token::from(kTok);

    std::array<std::byte, ouch::in::enter::kLen - 1> small{};
    const auto before = small;
    EXPECT_EQ(ouch::encode(small, o), 0u);
    EXPECT_EQ(std::memcmp(small.data(), before.data(), small.size()), 0);

    std::array<std::byte, ouch::out::accepted::kLen - 1> small_out{};
    ouch::AcceptedMsg a;
    EXPECT_EQ(ouch::encode_accepted(small_out, a), 0u);
    EXPECT_EQ(ouch::encode_replaced(small_out, ouch::ReplacedMsg{}), 0u);
}

TEST(OuchDecode, WrongTypeByteIsRefusedByTheInboundDecoders) {
    // A full length buffer, so the only thing that can make a decoder refuse is
    // the type byte itself rather than a short read.
    std::vector<std::byte> b(ouch::in::kMaxMessageLen, std::byte{' '});
    b[0] = static_cast<std::byte>('X');

    ouch::EnterOrder e;
    EXPECT_FALSE(ouch::decode_enter_order(b, e));
    ouch::ReplaceOrder r;
    EXPECT_FALSE(ouch::decode_replace_order(b, r));
    ouch::CancelOrder c;
    EXPECT_TRUE(ouch::decode_cancel_order(b, c));

    b[0] = static_cast<std::byte>('O');
    EXPECT_TRUE(ouch::decode_enter_order(b, e));
    EXPECT_FALSE(ouch::decode_cancel_order(b, c));
}

// ---------------------------------------------------------------------------
// Alpha fields
// ---------------------------------------------------------------------------

// Alpha fields are space padded, not zero padded, and an over long value is
// truncated rather than allowed to run into the next field.
TEST(OuchAlpha, PadsWithSpacesAndTruncates) {
    const auto s = ouch::alpha<ouch::kStockLen>("AAPL");
    EXPECT_EQ(s[3], 'L');
    EXPECT_EQ(s[4], ' ');
    EXPECT_EQ(s[7], ' ');
    EXPECT_EQ(ouch::trim(s), "AAPL");

    const auto long_one = ouch::alpha<ouch::kStockLen>("ABCDEFGHIJKL");
    EXPECT_EQ(ouch::trim(long_one), "ABCDEFGH");

    const auto empty = ouch::alpha<ouch::kFirmLen>("");
    EXPECT_EQ(ouch::trim(empty), "");
    EXPECT_EQ(empty[0], ' ');
}

TEST(OuchAlpha, DefaultTokenIsAllSpaces) {
    ouch::Token t;
    for (char c : t.b) EXPECT_EQ(c, ' ');
    EXPECT_EQ(t.view().size(), ouch::kTokenLen);
    EXPECT_EQ(t.trimmed(), "");
}

// ---------------------------------------------------------------------------
// The token generator
// ---------------------------------------------------------------------------

TEST(OuchTokenGenerator, ProducesFixedWidthTokens) {
    ouch::TokenGenerator g("TP");
    for (int i = 0; i < 1000; ++i) {
        const auto t = g.next();
        EXPECT_EQ(t.view().size(), ouch::kTokenLen);
        EXPECT_EQ(t.b[0], 'T');
        EXPECT_EQ(t.b[1], 'P');
        // No spaces inside the counter, which is what keeps the width fixed.
        for (std::size_t j = 2; j < ouch::kTokenLen; ++j) {
            const char c = t.b[j];
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'));
        }
    }
    EXPECT_EQ(g.digits(), ouch::kTokenLen - 2);
}

TEST(OuchTokenGenerator, TokensAreUniqueOverALongRun) {
    ouch::TokenGenerator            g("X");
    std::vector<std::string>        seen;
    seen.reserve(20000);
    for (int i = 0; i < 20000; ++i) seen.emplace_back(g.next().view());
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
    EXPECT_FALSE(g.exhausted());
}

// Zero padding to a fixed width is what makes the bytes sort in the same order
// as the counter, so a log sorted by token is sorted by send order.
TEST(OuchTokenGenerator, ByteOrderMatchesSendOrder) {
    ouch::TokenGenerator g("SES");
    std::string          prev(g.next().view());
    for (int i = 0; i < 5000; ++i) {
        std::string cur(g.next().view());
        EXPECT_LT(prev, cur) << "token order broke at " << i;
        prev = cur;
    }
}

TEST(OuchTokenGenerator, RendersBaseThirtySixRightJustified) {
    ouch::TokenGenerator g("", 0);
    EXPECT_EQ(std::string(g.next().view()), "00000000000000");
    EXPECT_EQ(std::string(g.next().view()), "00000000000001");
    for (int i = 2; i < 35; ++i) (void)g.next();
    EXPECT_EQ(std::string(g.next().view()), "0000000000000Z");  // counter 35
    EXPECT_EQ(std::string(g.next().view()), "00000000000010");  // counter 36
}

TEST(OuchTokenGenerator, StartValueIsHonoured) {
    // A two character prefix leaves twelve base thirty six digits, and thirty
    // six squared renders as a one followed by two zeros in that base.
    ouch::TokenGenerator g("AB", 36 * 36);
    EXPECT_EQ(std::string(g.next().view()), "AB000000000100");
    EXPECT_EQ(g.counter(), 36u * 36u + 1u);
}

// A prefix that leaves almost no room for the counter must report exhaustion
// rather than quietly repeat a token, because NASDAQ silently ignores an Enter
// Order carrying a token it has already seen. A silent repeat is an order that
// never exists and never errors.
TEST(OuchTokenGenerator, ExhaustionIsCountedNotSilent) {
    // Thirteen characters of prefix leaves one base thirty six digit, so there
    // are exactly thirty six distinct tokens.
    ouch::TokenGenerator g("PREFIX1234567", 0);
    ASSERT_EQ(g.digits(), 1u);
    ASSERT_EQ(g.capacity(), 36u);

    for (int i = 0; i < 36; ++i) (void)g.next();
    EXPECT_FALSE(g.exhausted());

    (void)g.next();  // the thirty seventh token must repeat the first
    EXPECT_TRUE(g.exhausted());
    EXPECT_EQ(g.overflows(), 1u);
}

// With no prefix the counter has all fourteen digits, which is wider than a
// uint64 can exhaust, and the generator reports that rather than a number.
TEST(OuchTokenGenerator, FullWidthCounterCannotBeExhausted) {
    ouch::TokenGenerator g;
    EXPECT_EQ(g.digits(), ouch::kTokenLen);
    EXPECT_EQ(g.capacity(), 0u);  // zero means wider than a uint64 can reach
    EXPECT_EQ(ouch::TokenGenerator::pow36(12), 4738381338321616896ull);
    EXPECT_EQ(ouch::TokenGenerator::pow36(13), 0u);
}

// ---------------------------------------------------------------------------
// The client order state machine
// ---------------------------------------------------------------------------

TEST(OuchOrderState, HappyPathToAFullFill) {
    ouch::ClientOrderState o;
    const auto             tok = ouch::Token::from(kTok);

    EXPECT_TRUE(o.on_sent(tok, 42, 'B', 500, 100000, 1));
    EXPECT_EQ(o.state, ouch::OrderState::Sent);
    EXPECT_EQ(o.leaves_shares, 500u);
    EXPECT_TRUE(o.working());
    EXPECT_FALSE(o.terminal());

    EXPECT_TRUE(o.on_accepted(500, 100000, 0xABCD, false, 2));
    EXPECT_EQ(o.state, ouch::OrderState::Accepted);
    EXPECT_EQ(o.reference, 0xABCDu);

    EXPECT_TRUE(o.on_fill(200, 100000, 3));
    EXPECT_EQ(o.state, ouch::OrderState::PartiallyFilled);
    EXPECT_EQ(o.filled_shares, 200u);
    EXPECT_EQ(o.leaves_shares, 300u);

    EXPECT_TRUE(o.on_fill(300, 100100, 4));
    EXPECT_EQ(o.state, ouch::OrderState::Filled);
    EXPECT_EQ(o.leaves_shares, 0u);
    EXPECT_EQ(o.filled_shares, 500u);
    EXPECT_TRUE(o.terminal());
    EXPECT_FALSE(o.working());
    EXPECT_EQ(o.filled_notional, 200ull * 100000 + 300ull * 100100);
    EXPECT_EQ(o.average_price(), o.filled_notional / 500);
    EXPECT_EQ(o.illegal_transitions, 0u);
}

TEST(OuchOrderState, CancelPathIsNotTerminalUntilNothingIsWorking) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 7, 'S', 400, 50000, 1));
    ASSERT_TRUE(o.on_accepted(400, 50000, 1, false, 2));
    ASSERT_TRUE(o.on_cancel_sent(3));
    EXPECT_EQ(o.state, ouch::OrderState::CancelSent);

    // A partial cancel. Some of the order is still alive, so this is not the
    // end of it.
    EXPECT_TRUE(o.on_canceled(100, 4));
    EXPECT_EQ(o.leaves_shares, 300u);
    EXPECT_FALSE(o.terminal());

    EXPECT_TRUE(o.on_canceled(300, 5));
    EXPECT_EQ(o.leaves_shares, 0u);
    EXPECT_EQ(o.state, ouch::OrderState::Canceled);
    EXPECT_TRUE(o.terminal());
    EXPECT_EQ(o.illegal_transitions, 0u);
}

TEST(OuchOrderState, AcceptedOrderDeadGoesStraightToTerminal) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    EXPECT_TRUE(o.on_accepted(100, 10000, 5, /*dead=*/true, 2));
    EXPECT_EQ(o.state, ouch::OrderState::Canceled);
    EXPECT_EQ(o.leaves_shares, 0u);
    EXPECT_TRUE(o.terminal());
}

TEST(OuchOrderState, RejectIsTerminalAndKillsWorkingShares) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    EXPECT_TRUE(o.on_rejected(2));
    EXPECT_EQ(o.state, ouch::OrderState::Rejected);
    EXPECT_EQ(o.leaves_shares, 0u);
    EXPECT_TRUE(o.terminal());
    EXPECT_EQ(o.illegal_transitions, 0u);
}

TEST(OuchOrderState, ReplaceChainCarriesTheNewToken) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 3, 'B', 500, 100000, 1));
    ASSERT_TRUE(o.on_accepted(500, 100000, 1, false, 2));
    ASSERT_TRUE(o.on_fill(100, 100000, 3));
    ASSERT_TRUE(o.on_replace_sent(4));
    EXPECT_EQ(o.state, ouch::OrderState::ReplaceSent);

    // The Replaced message reports what is left exposed, which is four hundred.
    EXPECT_TRUE(o.on_replaced(ouch::Token::from(kTok2), 400, 100500, 99, false, 5));
    EXPECT_EQ(o.token, ouch::Token::from(kTok2));
    EXPECT_EQ(o.leaves_shares, 400u);
    EXPECT_EQ(o.filled_shares, 100u);
    EXPECT_EQ(o.price, 100500u);
    EXPECT_EQ(o.state, ouch::OrderState::Accepted);
    EXPECT_EQ(o.illegal_transitions, 0u);
}

// Illegal transitions are counted, never asserted. OUCH is explicitly built
// around benignly resending inbound messages, so a second acknowledgement for a
// token is a thing the protocol tells clients to expect.
TEST(OuchOrderState, DuplicateAcceptedIsCountedNotFatal) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    ASSERT_TRUE(o.on_accepted(100, 10000, 5, false, 2));

    EXPECT_FALSE(o.on_accepted(100, 10000, 5, false, 3));
    EXPECT_EQ(o.illegal_transitions, 1u);
    // The state is still usable. The order did not disappear.
    EXPECT_EQ(o.state, ouch::OrderState::Accepted);
    EXPECT_EQ(o.leaves_shares, 100u);
}

TEST(OuchOrderState, FillAfterTerminalIsCountedAsAnOrphan) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    ASSERT_TRUE(o.on_accepted(100, 10000, 5, false, 2));
    ASSERT_TRUE(o.on_fill(100, 10000, 3));
    ASSERT_EQ(o.state, ouch::OrderState::Filled);

    // An execution that crossed the cancel on the wire. Counted separately from
    // the general illegal transition total so it cannot hide in it.
    EXPECT_FALSE(o.on_fill(50, 10000, 4));
    EXPECT_EQ(o.orphan_fills, 1u);
    // The shares are real whether or not this process expected them.
    EXPECT_EQ(o.filled_shares, 150u);
}

TEST(OuchOrderState, OverfillIsCountedAndClampsLeaves) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    ASSERT_TRUE(o.on_accepted(100, 10000, 5, false, 2));

    EXPECT_FALSE(o.on_fill(250, 10000, 3));
    EXPECT_EQ(o.overfills, 1u);
    EXPECT_EQ(o.leaves_shares, 0u);
    EXPECT_EQ(o.filled_shares, 250u);  // the exchange's number wins
    EXPECT_EQ(o.state, ouch::OrderState::Filled);
}

TEST(OuchOrderState, FillOnAnUnknownTokenIsCounted) {
    ouch::ClientOrderState o;  // never sent
    EXPECT_FALSE(o.on_fill(100, 10000, 1));
    EXPECT_EQ(o.orphan_fills, 1u);
}

TEST(OuchOrderState, CancelSentFromATerminalStateIsCounted) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    ASSERT_TRUE(o.on_rejected(2));
    EXPECT_FALSE(o.on_cancel_sent(3));
    EXPECT_EQ(o.illegal_transitions, 1u);
}

TEST(OuchOrderState, RejectAfterAcceptedIsCounted) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    ASSERT_TRUE(o.on_accepted(100, 10000, 5, false, 2));
    EXPECT_FALSE(o.on_rejected(3));
    EXPECT_EQ(o.illegal_transitions, 1u);
    EXPECT_EQ(o.state, ouch::OrderState::Rejected);
}

TEST(OuchOrderState, SendingTwiceOnOneTokenIsCounted) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 1));
    EXPECT_FALSE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 100, 10000, 2));
    EXPECT_EQ(o.illegal_transitions, 1u);
}

TEST(OuchOrderState, AcceptedMayShrinkAndRepriceTheOrder) {
    ouch::ClientOrderState o;
    ASSERT_TRUE(o.on_sent(ouch::Token::from(kTok), 1, 'B', 500, 100000, 1));
    // The exchange accepted fewer shares at a better price. Its numbers win.
    EXPECT_TRUE(o.on_accepted(300, 99900, 7, false, 2));
    EXPECT_EQ(o.original_shares, 300u);
    EXPECT_EQ(o.leaves_shares, 300u);
    EXPECT_EQ(o.price, 99900u);
}

TEST(OuchOrderState, StateNamesAreAllDistinct) {
    const ouch::OrderState all[] = {
        ouch::OrderState::Unknown,     ouch::OrderState::Sent,
        ouch::OrderState::Accepted,    ouch::OrderState::PartiallyFilled,
        ouch::OrderState::Filled,      ouch::OrderState::ReplaceSent,
        ouch::OrderState::CancelSent,  ouch::OrderState::Canceled,
        ouch::OrderState::Rejected,
    };
    std::vector<std::string> names;
    for (auto s : all) names.emplace_back(ouch::to_string(s));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

// ---------------------------------------------------------------------------
// A whole session, encoded and decoded back
// ---------------------------------------------------------------------------

// The end to end shape this file exists to support. Send an order, take the
// acknowledgement, take two fills, and end with the order state agreeing with
// what came back over the wire.
TEST(OuchSession, OrderAcknowledgementAndFillsDriveTheState) {
    ouch::TokenGenerator   gen("SES");
    const ouch::Token      tok = gen.next();
    ouch::ClientOrderState order;

    std::array<std::byte, ouch::in::kMaxMessageLen> out_buf{};
    ouch::EnterOrder                                eo;
    eo.token         = tok;
    eo.side          = 'B';
    eo.shares        = 500;
    eo.stock         = ouch::alpha<ouch::kStockLen>("AAPL");
    eo.price         = 1900000;
    eo.time_in_force = ouch::kTifSystemHours;
    ASSERT_EQ(ouch::encode(out_buf, eo), ouch::in::enter::kLen);
    ASSERT_TRUE(order.on_sent(tok, 11, 'B', 500, 1900000, 1000));

    // What the venue sends back, built through the outbound encoders and put
    // through the real decoder so nothing is taken on trust.
    struct Applier : ouch::NullOuchHandler {
        ouch::ClientOrderState* o = nullptr;
        void on_accepted(const ouch::AcceptedMsg& a) noexcept {
            o->on_accepted(a.shares, a.price, a.reference, a.dead(), a.timestamp);
        }
        void on_executed(uint64_t ts, const ouch::Token&, uint32_t n, uint32_t px,
                         char, uint64_t) noexcept {
            o->on_fill(n, px, ts);
        }
    };
    Applier applier;
    applier.o = &order;
    ouch::OuchDecoder dec;

    std::array<std::byte, ouch::out::kMaxMessageLen> in_buf{};

    ouch::AcceptedMsg acc;
    acc.timestamp   = 1100;
    acc.token       = tok;
    acc.side        = 'B';
    acc.shares      = 500;
    acc.stock       = ouch::alpha<ouch::kStockLen>("AAPL");
    acc.price       = 1900000;
    acc.reference   = 777;
    acc.order_state = 'L';
    ASSERT_EQ(ouch::encode_accepted(in_buf, acc), ouch::out::accepted::kLen);
    ASSERT_EQ(dec.decode(in_buf, applier), ouch::DecodeResult::Ok);
    EXPECT_EQ(order.state, ouch::OrderState::Accepted);
    EXPECT_EQ(order.reference, 777u);

    ASSERT_EQ(ouch::encode_executed(in_buf, 1200, tok, 200, 1900000, 'R', 1),
              ouch::out::executed::kLen);
    ASSERT_EQ(dec.decode(in_buf, applier), ouch::DecodeResult::Ok);
    EXPECT_EQ(order.state, ouch::OrderState::PartiallyFilled);
    EXPECT_EQ(order.leaves_shares, 300u);

    ASSERT_EQ(ouch::encode_executed(in_buf, 1300, tok, 300, 1899500, 'R', 2),
              ouch::out::executed::kLen);
    ASSERT_EQ(dec.decode(in_buf, applier), ouch::DecodeResult::Ok);
    EXPECT_EQ(order.state, ouch::OrderState::Filled);
    EXPECT_EQ(order.leaves_shares, 0u);
    EXPECT_EQ(order.filled_shares, 500u);

    EXPECT_EQ(order.illegal_transitions, 0u);
    EXPECT_EQ(order.orphan_fills, 0u);
    EXPECT_EQ(order.overfills, 0u);
    EXPECT_EQ(dec.stats().messages, 3u);
    EXPECT_EQ(dec.stats().truncated, 0u);
    EXPECT_EQ(dec.stats().unknown, 0u);
}
