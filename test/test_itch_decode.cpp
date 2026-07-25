#include "tick/itch.hpp"
#include "tick/itch_decoder.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <span>
#include <string>
#include <vector>

// The decoder's correctness proof.
//
// Every message here is a hand written array of literal bytes. Nothing in this
// file is produced by itch_writer.hpp, because an encoder and a decoder that
// share a wrong offset constant agree with each other perfectly and both are
// wrong. The only way to catch that is to write the bytes out by hand from the
// specification and assert the decoder reads back what the specification says
// is in them.
//
// Field values are chosen so a transposed offset cannot produce a plausible
// answer. An order reference of 0x0102030405060708 read one byte early is
// obviously not the number it should be, where a reference of 1 read one byte
// early is still zero or one.

// The test bodies below live in the global namespace, so the directive does
// too rather than sitting inside the helper namespace where it would not reach
// them.
using namespace tick;

namespace {

// Shared header, identical in every message below so the body assertions stand
// out. Every byte is distinct and the timestamp has its top byte set, which is
// what catches a six byte field read as four.
constexpr uint16_t kLocate   = 0x0A0B;
constexpr uint16_t kTracking = 0x0C0D;
constexpr uint64_t kTs       = 0xA1B2C3D4E5F6ull;

// Deliberately a heap vector rather than a stack array. Under ASan the
// allocation has a redzone on both sides, so a decoder that reads one byte past
// the end of the message fails the test instead of quietly reading a
// neighbouring local.
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    out.reserve(v.size());
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

// ---------------------------------------------------------------------------
// The twelve messages, byte by byte
// ---------------------------------------------------------------------------

// header is: type, locate 0x0A0B, tracking 0x0C0D, timestamp 0xA1B2C3D4E5F6

std::vector<std::byte> msg_system_event() {
    return bytes({'S', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  'Q'});  // start of market hours
}

std::vector<std::byte> msg_stock_directory() {
    return bytes({'R', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock, padded to eight
                  'Q',                                      // market category
                  'N',                                      // financial status
                  0x00, 0x00, 0x00, 0x64,                   // round lot size 100
                  'N',                                      // round lots only
                  'C',                                      // issue classification
                  'Z', ' ',                                 // issue sub type
                  'P',                                      // authenticity
                  'N',                                      // short sale threshold
                  'N',                                      // ipo flag
                  '1',                                      // luld tier
                  'N',                                      // etp flag
                  0x00, 0x00, 0x00, 0x00,                   // etp leverage
                  'N'});                                    // inverse indicator
}

std::vector<std::byte> msg_trading_action() {
    return bytes({'H', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',  // stock
                  'H',                                      // trading state, halted
                  ' ',                                      // reserved
                  'T', '1', ' ', ' '});                     // reason
}

std::vector<std::byte> msg_add_order() {
    return bytes({'A', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  'B',                                              // buy
                  0x00, 0x00, 0x04, 0xD2,                           // shares 1234
                  'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',           // stock
                  0x00, 0x12, 0xD6, 0x44});                         // price 1234500
}

std::vector<std::byte> msg_add_order_mpid() {
    return bytes({'F', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  'S',                                              // sell
                  0x00, 0x00, 0x04, 0xD2,                           // shares 1234
                  'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',           // stock
                  0x00, 0x12, 0xD6, 0x44,                           // price 1234500
                  'N', 'S', 'D', 'Q'});                             // attribution
}

std::vector<std::byte> msg_order_executed() {
    return bytes({'E', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  0x00, 0x00, 0x01, 0xF4,                           // executed 500
                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}); // match number
}

std::vector<std::byte> msg_order_executed_price() {
    return bytes({'C', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  0x00, 0x00, 0x01, 0xF4,                           // executed 500
                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,   // match number
                  'Y',                                              // printable
                  0x00, 0x0F, 0x69, 0xB5});                         // price 1010101
}

std::vector<std::byte> msg_order_cancel() {
    return bytes({'X', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  0x00, 0x00, 0x01, 0x2C});                         // cancelled 300
}

std::vector<std::byte> msg_order_delete() {
    return bytes({'D', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}); // order ref
}

std::vector<std::byte> msg_order_replace() {
    return bytes({'U', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // original ref
                  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,  // new ref
                  0x00, 0x00, 0x09, 0xC4,                           // shares 2500
                  0x00, 0x0F, 0x12, 0x02});                         // price 987650
}

std::vector<std::byte> msg_trade() {
    return bytes({'P', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // order ref
                  'S',                                              // sell
                  0x00, 0x00, 0x1E, 0x61,                           // shares 7777
                  'T', 'S', 'L', 'A', ' ', ' ', ' ', ' ',           // stock
                  0x00, 0x45, 0xB2, 0xF8,                           // price 4567800
                  0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00}); // match number
}

std::vector<std::byte> msg_cross_trade() {
    return bytes({'Q', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xA1, 0x20,  // shares 500000, eight wide
                  'S', 'P', 'Y', ' ', ' ', ' ', ' ', ' ',           // stock
                  0x00, 0x21, 0xE8, 0x78,                           // cross price 2222200
                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,   // match number
                  'O'});                                            // opening cross
}

std::vector<std::byte> msg_broken_trade() {
    return bytes({'B', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}); // match number
}

// Y, reg sho restriction. A type the length table knows and the decoder has no
// semantics for, which is how the on_other path gets exercised.
std::vector<std::byte> msg_reg_sho() {
    return bytes({'Y', 0x0A, 0x0B, 0x0C, 0x0D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
                  'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
                  '0'});                                    // no restriction
}

// ---------------------------------------------------------------------------
// The recording handler
// ---------------------------------------------------------------------------

enum class Kind {
    SystemEvent,
    StockDirectory,
    TradingAction,
    Add,
    Execute,
    ExecutePrice,
    Cancel,
    Delete,
    Replace,
    Trade,
    Cross,
    Broken,
    Other,
};

// One record per handler call, holding every argument that call received. The
// point is comparability. Two decoders are equal when their vectors of these
// are equal, which is a stronger statement than any per field assertion.
struct Call {
    Kind        kind      = Kind::Other;
    uint16_t    locate    = 0;
    uint64_t    ts        = 0;
    uint64_t    ref       = 0;
    uint64_t    ref2      = 0;
    uint64_t    match     = 0;
    uint32_t    shares    = 0;
    uint32_t    price     = 0;
    uint32_t    round_lot = 0;
    char        c1        = '\0';
    char        c2        = '\0';
    bool        flag      = false;
    std::string symbol;

    bool operator==(const Call&) const = default;
};

// So a failing comparison names the field rather than dumping a hex blob.
// Marked maybe_unused because it is only reached when an assertion fails and
// the test framework goes looking for a way to print the value.
[[maybe_unused]] std::ostream& operator<<(std::ostream& os, const Call& c) {
    return os << "Call{kind=" << static_cast<int>(c.kind) << " locate=" << c.locate
              << " ts=" << c.ts << " ref=" << c.ref << " ref2=" << c.ref2
              << " match=" << c.match << " shares=" << c.shares << " price=" << c.price
              << " round_lot=" << c.round_lot << " c1=" << c.c1 << " flag=" << c.flag
              << " symbol=" << c.symbol << "}";
}

struct Recorder {
    std::vector<Call> calls;

    void on_system_event(uint64_t ts, char code) {
        Call c;
        c.kind = Kind::SystemEvent;
        c.ts   = ts;
        c.c1   = code;
        calls.push_back(c);
    }
    void on_stock_directory(uint16_t locate, uint64_t ts, std::string_view sym, uint32_t lot) {
        Call c;
        c.kind      = Kind::StockDirectory;
        c.locate    = locate;
        c.ts        = ts;
        c.symbol    = std::string(sym);
        c.round_lot = lot;
        calls.push_back(c);
    }
    void on_trading_action(uint16_t locate, uint64_t ts, char state) {
        Call c;
        c.kind   = Kind::TradingAction;
        c.locate = locate;
        c.ts     = ts;
        c.c1     = state;
        calls.push_back(c);
    }
    void on_add(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                uint32_t price, bool mpid) {
        Call c;
        c.kind   = Kind::Add;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        c.c1     = side;
        c.shares = shares;
        c.price  = price;
        c.flag   = mpid;
        calls.push_back(c);
    }
    void on_execute(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                    uint64_t match) {
        Call c;
        c.kind   = Kind::Execute;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        c.shares = shares;
        c.match  = match;
        calls.push_back(c);
    }
    void on_execute_price(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                          uint64_t match, bool printable, uint32_t price) {
        Call c;
        c.kind   = Kind::ExecutePrice;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        c.shares = shares;
        c.match  = match;
        c.flag   = printable;
        c.price  = price;
        calls.push_back(c);
    }
    void on_cancel(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
        Call c;
        c.kind   = Kind::Cancel;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        c.shares = shares;
        calls.push_back(c);
    }
    void on_delete(uint16_t locate, uint64_t ts, uint64_t ref) {
        Call c;
        c.kind   = Kind::Delete;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        calls.push_back(c);
    }
    void on_replace(uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                    uint32_t shares, uint32_t price) {
        Call c;
        c.kind   = Kind::Replace;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = old_ref;
        c.ref2   = new_ref;
        c.shares = shares;
        c.price  = price;
        calls.push_back(c);
    }
    void on_trade(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                  uint32_t price, uint64_t match) {
        Call c;
        c.kind   = Kind::Trade;
        c.locate = locate;
        c.ts     = ts;
        c.ref    = ref;
        c.c1     = side;
        c.shares = shares;
        c.price  = price;
        c.match  = match;
        calls.push_back(c);
    }
    void on_cross_trade(uint16_t locate, uint64_t ts, uint32_t shares, uint32_t price,
                        uint64_t match, char cross_type) {
        Call c;
        c.kind   = Kind::Cross;
        c.locate = locate;
        c.ts     = ts;
        c.shares = shares;
        c.price  = price;
        c.match  = match;
        c.c1     = cross_type;
        calls.push_back(c);
    }
    void on_broken_trade(uint16_t locate, uint64_t ts, uint64_t match) {
        Call c;
        c.kind   = Kind::Broken;
        c.locate = locate;
        c.ts     = ts;
        c.match  = match;
        calls.push_back(c);
    }
    void on_other(char type, uint16_t locate, uint64_t ts) {
        Call c;
        c.kind   = Kind::Other;
        c.locate = locate;
        c.ts     = ts;
        c.c1     = type;
        calls.push_back(c);
    }
};

static_assert(ItchHandler<Recorder>, "the recorder has to be a real handler");

// Decode exactly one message and hand back the single call it produced. The
// span is sized to the buffer, so the decoder gets no slack past the message.
template <typename Decoder>
Call decode_one(const std::vector<std::byte>& msg) {
    Decoder  dec;
    Recorder rec;
    std::size_t consumed = 0;
    EXPECT_EQ(dec.decode(std::span<const std::byte>(msg), rec, &consumed), DecodeResult::Ok);
    EXPECT_EQ(consumed, msg.size());
    EXPECT_EQ(rec.calls.size(), 1u);
    return rec.calls.empty() ? Call{} : rec.calls.front();
}

std::vector<std::vector<std::byte>> all_messages() {
    return {msg_system_event(),        msg_stock_directory(),      msg_trading_action(),
            msg_add_order(),           msg_add_order_mpid(),       msg_order_executed(),
            msg_order_executed_price(), msg_order_cancel(),        msg_order_delete(),
            msg_order_replace(),       msg_trade(),                msg_cross_trade(),
            msg_broken_trade(),        msg_reg_sho()};
}

} // namespace

// ---------------------------------------------------------------------------
// The length table
// ---------------------------------------------------------------------------

TEST(ItchLengths, MatchesSpecification) {
    EXPECT_EQ(itch::message_length('S'), 12u);
    EXPECT_EQ(itch::message_length('R'), 39u);
    EXPECT_EQ(itch::message_length('H'), 25u);
    EXPECT_EQ(itch::message_length('Y'), 20u);
    EXPECT_EQ(itch::message_length('L'), 26u);
    EXPECT_EQ(itch::message_length('V'), 35u);
    EXPECT_EQ(itch::message_length('W'), 12u);
    EXPECT_EQ(itch::message_length('K'), 28u);
    EXPECT_EQ(itch::message_length('J'), 35u);
    EXPECT_EQ(itch::message_length('h'), 21u);
    EXPECT_EQ(itch::message_length('A'), 36u);
    EXPECT_EQ(itch::message_length('F'), 40u);
    EXPECT_EQ(itch::message_length('E'), 31u);
    EXPECT_EQ(itch::message_length('C'), 36u);
    EXPECT_EQ(itch::message_length('X'), 23u);
    EXPECT_EQ(itch::message_length('D'), 19u);
    EXPECT_EQ(itch::message_length('U'), 35u);
    EXPECT_EQ(itch::message_length('P'), 44u);
    EXPECT_EQ(itch::message_length('Q'), 40u);
    EXPECT_EQ(itch::message_length('B'), 19u);
    EXPECT_EQ(itch::message_length('I'), 50u);
    EXPECT_EQ(itch::message_length('N'), 20u);
    EXPECT_EQ(itch::message_length('O'), 48u);

    // No message exceeds the staging buffer the copying decoder reserves.
    for (int t = 0; t < 256; ++t) {
        EXPECT_LE(itch::message_length(static_cast<char>(t)), itch::kMaxMessageLen);
    }

    EXPECT_EQ(itch::message_length('Z'), 0u);
    EXPECT_FALSE(itch::is_known_type('Z'));
    EXPECT_TRUE(itch::is_known_type('A'));
}

// Each hand built message is exactly as long as the table says it is. If this
// fails the field assertions below are meaningless.
TEST(ItchLengths, HandBuiltMessagesAreTheRightLength) {
    for (const auto& m : all_messages()) {
        const char type = static_cast<char>(m[itch::kOffType]);
        EXPECT_EQ(m.size(), itch::message_length(type)) << "type " << type;
    }
}

// ---------------------------------------------------------------------------
// Header, common to everything
// ---------------------------------------------------------------------------

TEST(ItchHeader, FieldsAtTheirOffsets) {
    const auto m = msg_add_order();
    EXPECT_EQ(itch::msg_type(m.data()), 'A');
    EXPECT_EQ(itch::locate(m.data()), kLocate);
    EXPECT_EQ(itch::tracking(m.data()), kTracking);
    EXPECT_EQ(itch::timestamp(m.data()), kTs);
}

TEST(ItchHeader, StockFieldTrimsTrailingSpaces) {
    const auto m = msg_add_order();
    EXPECT_EQ(itch::stock(m.data() + itch::off::kAddStock), "AAPL");

    // An eight character ticker keeps all eight, with nothing to trim.
    const auto full = bytes({'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'});
    EXPECT_EQ(itch::stock(full.data()), "ABCDEFGH");

    // Interior spaces are not trailing spaces and have to survive.
    const auto inner = bytes({'B', 'R', 'K', ' ', 'A', ' ', ' ', ' '});
    EXPECT_EQ(itch::stock(inner.data()), "BRK A");
}

// ---------------------------------------------------------------------------
// One test per message type, against the literal bytes above
// ---------------------------------------------------------------------------

TEST(ZeroCopyDecode, SystemEvent) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_system_event());
    EXPECT_EQ(c.kind, Kind::SystemEvent);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.c1, 'Q');
}

TEST(ZeroCopyDecode, StockDirectory) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_stock_directory());
    EXPECT_EQ(c.kind, Kind::StockDirectory);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.symbol, "AAPL");
    EXPECT_EQ(c.round_lot, 100u);
}

TEST(ZeroCopyDecode, TradingAction) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_trading_action());
    EXPECT_EQ(c.kind, Kind::TradingAction);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.c1, 'H');
}

TEST(ZeroCopyDecode, AddOrder) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_add_order());
    EXPECT_EQ(c.kind, Kind::Add);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.c1, 'B');
    EXPECT_EQ(c.shares, 1234u);
    EXPECT_EQ(c.price, 1234500u);  // one hundred twenty three dollars forty five
    EXPECT_FALSE(c.flag);
}

TEST(ZeroCopyDecode, AddOrderWithMpid) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_add_order_mpid());
    EXPECT_EQ(c.kind, Kind::Add);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.c1, 'S');
    EXPECT_EQ(c.shares, 1234u);
    EXPECT_EQ(c.price, 1234500u);
    // The only thing an F says that an A does not.
    EXPECT_TRUE(c.flag);
}

TEST(ZeroCopyDecode, OrderExecuted) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_order_executed());
    EXPECT_EQ(c.kind, Kind::Execute);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.shares, 500u);
    EXPECT_EQ(c.match, 0x1122334455667788ull);
}

TEST(ZeroCopyDecode, OrderExecutedWithPrice) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_order_executed_price());
    EXPECT_EQ(c.kind, Kind::ExecutePrice);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.shares, 500u);
    EXPECT_EQ(c.match, 0x1122334455667788ull);
    EXPECT_TRUE(c.flag);
    EXPECT_EQ(c.price, 1010101u);
}

// The printable flag is a single byte and the only thing separating an
// execution that counts toward volume from one that does not.
TEST(ZeroCopyDecode, OrderExecutedNotPrintable) {
    auto m = msg_order_executed_price();
    m[itch::off::kExecPxPrintable] = static_cast<std::byte>('N');

    ZeroCopyDecoder dec;
    Recorder        rec;
    ASSERT_EQ(dec.decode(std::span<const std::byte>(m), rec), DecodeResult::Ok);
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_FALSE(rec.calls[0].flag);
    EXPECT_EQ(rec.calls[0].price, 1010101u);
}

TEST(ZeroCopyDecode, OrderCancel) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_order_cancel());
    EXPECT_EQ(c.kind, Kind::Cancel);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.shares, 300u);
}

TEST(ZeroCopyDecode, OrderDelete) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_order_delete());
    EXPECT_EQ(c.kind, Kind::Delete);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
}

TEST(ZeroCopyDecode, OrderReplace) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_order_replace());
    EXPECT_EQ(c.kind, Kind::Replace);
    // The two references are adjacent eight byte fields, which is exactly where
    // an off by eight offset hides. They are given unrelated values for that
    // reason.
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.ref2, 0x1112131415161718ull);
    EXPECT_EQ(c.shares, 2500u);
    EXPECT_EQ(c.price, 987650u);
}

TEST(ZeroCopyDecode, Trade) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_trade());
    EXPECT_EQ(c.kind, Kind::Trade);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ref, 0x0102030405060708ull);
    EXPECT_EQ(c.c1, 'S');
    EXPECT_EQ(c.shares, 7777u);
    EXPECT_EQ(c.price, 4567800u);
    EXPECT_EQ(c.match, 0x99AABBCCDDEEFF00ull);
}

TEST(ZeroCopyDecode, CrossTrade) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_cross_trade());
    EXPECT_EQ(c.kind, Kind::Cross);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.shares, 500000u);
    EXPECT_EQ(c.price, 2222200u);
    EXPECT_EQ(c.match, 0x1122334455667788ull);
    EXPECT_EQ(c.c1, 'O');
}

// Cross shares is the one eight byte share count in the protocol and the
// handler takes a uint32. The narrowing is deliberate, so it is pinned down
// here rather than left to be discovered.
TEST(ZeroCopyDecode, CrossTradeSharesNarrowing) {
    auto m = msg_cross_trade();
    // Put a value in the high word that no real cross would carry.
    for (std::size_t i = 0; i < 8; ++i) {
        m[itch::off::kCrossShares + i] = static_cast<std::byte>(0);
    }
    m[itch::off::kCrossShares + 3] = static_cast<std::byte>(0x01);  // high word set
    m[itch::off::kCrossShares + 6] = static_cast<std::byte>(0x0B);
    m[itch::off::kCrossShares + 7] = static_cast<std::byte>(0xB8);  // low word 3000

    ZeroCopyDecoder dec;
    Recorder        rec;
    ASSERT_EQ(dec.decode(std::span<const std::byte>(m), rec), DecodeResult::Ok);
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].shares, 3000u);
}

TEST(ZeroCopyDecode, BrokenTrade) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_broken_trade());
    EXPECT_EQ(c.kind, Kind::Broken);
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
    EXPECT_EQ(c.match, 0x1122334455667788ull);
}

// A known length with no semantics goes to on_other, counted and never guessed
// at. Silently dropping it would leave a gap in the message count.
TEST(ZeroCopyDecode, KnownTypeWithoutSemantics) {
    const Call c = decode_one<ZeroCopyDecoder>(msg_reg_sho());
    EXPECT_EQ(c.kind, Kind::Other);
    EXPECT_EQ(c.c1, 'Y');
    EXPECT_EQ(c.locate, kLocate);
    EXPECT_EQ(c.ts, kTs);
}

// ---------------------------------------------------------------------------
// The two decoder oracle
// ---------------------------------------------------------------------------

// The single most valuable test in this file. Two independently written paths,
// one reading fields in place and one staging and normalising the whole
// message, have to produce the identical sequence of handler calls over the
// same bytes. A bug that both share has to be in the shared offset constants,
// which is what the literal byte tests above cover.
TEST(DecoderEquivalence, IdenticalCallSequence) {
    std::vector<std::byte> stream;
    for (const auto& m : all_messages()) stream.insert(stream.end(), m.begin(), m.end());

    ZeroCopyDecoder zc;
    CopyingDecoder  cp;
    Recorder        rec_zc;
    Recorder        rec_cp;

    std::size_t off_zc = 0;
    while (off_zc < stream.size()) {
        std::size_t n = 0;
        ASSERT_EQ(zc.decode(std::span<const std::byte>(stream).subspan(off_zc), rec_zc, &n),
                  DecodeResult::Ok);
        off_zc += n;
    }

    std::size_t off_cp = 0;
    while (off_cp < stream.size()) {
        std::size_t n = 0;
        ASSERT_EQ(cp.decode(std::span<const std::byte>(stream).subspan(off_cp), rec_cp, &n),
                  DecodeResult::Ok);
        off_cp += n;
    }

    EXPECT_EQ(off_zc, stream.size());
    EXPECT_EQ(off_cp, stream.size());
    EXPECT_EQ(rec_zc.calls.size(), all_messages().size());
    EXPECT_EQ(rec_zc.calls, rec_cp.calls);

    // The statistics have to agree too, since a downstream consumer trusts them
    // to account for every byte.
    EXPECT_EQ(zc.stats().messages, cp.stats().messages);
    EXPECT_EQ(zc.stats().bytes, cp.stats().bytes);
    EXPECT_EQ(zc.stats().bytes, stream.size());
    for (int t = 0; t < 256; ++t) {
        EXPECT_EQ(zc.stats().by_type[static_cast<std::size_t>(t)],
                  cp.stats().by_type[static_cast<std::size_t>(t)])
            << "type " << t;
    }
}

// The same stream shifted onto an odd address. Nothing about the decode may
// depend on where the buffer sits, and this is the case that faults on a
// target without unaligned loads.
TEST(DecoderEquivalence, UnalignedStream) {
    std::vector<std::byte> aligned;
    for (const auto& m : all_messages()) aligned.insert(aligned.end(), m.begin(), m.end());

    std::vector<std::byte> shifted(1, std::byte{0});
    shifted.insert(shifted.end(), aligned.begin(), aligned.end());

    auto run = [](std::span<const std::byte> s) {
        ZeroCopyDecoder dec;
        Recorder        rec;
        std::size_t     off = 0;
        while (off < s.size()) {
            std::size_t n = 0;
            if (dec.decode(s.subspan(off), rec, &n) != DecodeResult::Ok) break;
            off += n;
        }
        return rec.calls;
    };

    EXPECT_EQ(run(std::span<const std::byte>(aligned)),
              run(std::span<const std::byte>(shifted).subspan(1)));
}

// ---------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------

TEST(Normalize, HeaderOnEveryType) {
    for (const auto& m : all_messages()) {
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), static_cast<char>(m[itch::kOffType]), e);
        EXPECT_EQ(e.type, static_cast<char>(m[itch::kOffType]));
        EXPECT_EQ(e.locate, kLocate);
        EXPECT_EQ(e.tracking, kTracking);
        EXPECT_EQ(e.timestamp, kTs);
    }
}

TEST(Normalize, AddOrder) {
    const auto      m = msg_add_order();
    NormalizedEvent e{};
    CopyingDecoder::normalize(m.data(), 'A', e);
    EXPECT_EQ(e.ref, 0x0102030405060708ull);
    EXPECT_EQ(e.side, 'B');
    EXPECT_EQ(e.shares, 1234u);
    EXPECT_EQ(e.price, 1234500u);
    EXPECT_FALSE(e.has_mpid);
    // The zero-copy path never reads the ticker on an add. The copying path
    // always does, which is the cost the two decoder comparison is measuring.
    EXPECT_EQ(e.symbol(), "AAPL");
}

TEST(Normalize, AddOrderWithMpid) {
    const auto      m = msg_add_order_mpid();
    NormalizedEvent e{};
    CopyingDecoder::normalize(m.data(), 'F', e);
    EXPECT_EQ(e.ref, 0x0102030405060708ull);
    EXPECT_EQ(e.side, 'S');
    EXPECT_EQ(e.shares, 1234u);
    EXPECT_EQ(e.price, 1234500u);
    EXPECT_TRUE(e.has_mpid);
    EXPECT_EQ(e.symbol(), "AAPL");
}

TEST(Normalize, Executions) {
    {
        const auto      m = msg_order_executed();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'E', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.shares, 500u);
        EXPECT_EQ(e.match, 0x1122334455667788ull);
        EXPECT_EQ(e.price, 0u);  // an E carries no price
    }
    {
        const auto      m = msg_order_executed_price();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'C', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.shares, 500u);
        EXPECT_EQ(e.match, 0x1122334455667788ull);
        EXPECT_TRUE(e.printable);
        EXPECT_EQ(e.price, 1010101u);
    }
}

TEST(Normalize, CancelDeleteReplace) {
    {
        const auto      m = msg_order_cancel();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'X', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.shares, 300u);
    }
    {
        const auto      m = msg_order_delete();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'D', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.shares, 0u);
    }
    {
        const auto      m = msg_order_replace();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'U', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.ref2, 0x1112131415161718ull);
        EXPECT_EQ(e.shares, 2500u);
        EXPECT_EQ(e.price, 987650u);
    }
}

TEST(Normalize, TradesAndSessionMessages) {
    {
        const auto      m = msg_trade();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'P', e);
        EXPECT_EQ(e.ref, 0x0102030405060708ull);
        EXPECT_EQ(e.side, 'S');
        EXPECT_EQ(e.shares, 7777u);
        EXPECT_EQ(e.price, 4567800u);
        EXPECT_EQ(e.match, 0x99AABBCCDDEEFF00ull);
        EXPECT_EQ(e.symbol(), "TSLA");
    }
    {
        const auto      m = msg_cross_trade();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'Q', e);
        EXPECT_EQ(e.shares, 500000u);
        EXPECT_EQ(e.price, 2222200u);
        EXPECT_EQ(e.match, 0x1122334455667788ull);
        EXPECT_EQ(e.aux, 'O');
        EXPECT_EQ(e.symbol(), "SPY");
    }
    {
        const auto      m = msg_broken_trade();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'B', e);
        EXPECT_EQ(e.match, 0x1122334455667788ull);
    }
    {
        const auto      m = msg_system_event();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'S', e);
        EXPECT_EQ(e.aux, 'Q');
    }
    {
        const auto      m = msg_stock_directory();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'R', e);
        EXPECT_EQ(e.round_lot, 100u);
        EXPECT_EQ(e.aux, 'Q');  // market category
        EXPECT_EQ(e.symbol(), "AAPL");
    }
    {
        const auto      m = msg_trading_action();
        NormalizedEvent e{};
        CopyingDecoder::normalize(m.data(), 'H', e);
        EXPECT_EQ(e.aux, 'H');
        EXPECT_EQ(e.symbol(), "MSFT");
    }
}

// Normalising must not leave anything behind from the previous message. The
// copying decoder reuses one event, so a field a type does not carry has to
// come back zero rather than stale.
TEST(Normalize, ClearsPreviousEvent) {
    NormalizedEvent e{};
    const auto      trade = msg_trade();
    CopyingDecoder::normalize(trade.data(), 'P', e);
    ASSERT_NE(e.match, 0u);
    ASSERT_NE(e.stock_len, 0u);

    const auto del = msg_order_delete();
    CopyingDecoder::normalize(del.data(), 'D', e);
    EXPECT_EQ(e.match, 0u);
    EXPECT_EQ(e.shares, 0u);
    EXPECT_EQ(e.price, 0u);
    EXPECT_EQ(e.side, '\0');
    EXPECT_EQ(e.stock_len, 0u);
    EXPECT_FALSE(e.has_mpid);
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

// A message one byte short of its declared length is refused and dispatches
// nothing. Reading it anyway is how a feed handler reads off the end of a
// receive buffer.
TEST(Malformed, TruncatedDispatchesNothing) {
    for (const auto& full : all_messages()) {
        const char type = static_cast<char>(full[itch::kOffType]);

        // Sized to one byte less than the message, so there is genuinely
        // nothing there rather than a shorter view over a full buffer.
        std::vector<std::byte> short_msg(full.begin(), full.end() - 1);

        ZeroCopyDecoder zc;
        CopyingDecoder  cp;
        Recorder        rec_zc;
        Recorder        rec_cp;

        EXPECT_EQ(zc.decode(std::span<const std::byte>(short_msg), rec_zc),
                  DecodeResult::Truncated)
            << "type " << type;
        EXPECT_EQ(cp.decode(std::span<const std::byte>(short_msg), rec_cp),
                  DecodeResult::Truncated)
            << "type " << type;

        EXPECT_TRUE(rec_zc.calls.empty()) << "type " << type;
        EXPECT_TRUE(rec_cp.calls.empty()) << "type " << type;
        EXPECT_EQ(zc.stats().truncated, 1u);
        EXPECT_EQ(zc.stats().messages, 0u);
        EXPECT_EQ(cp.stats().truncated, 1u);
        EXPECT_EQ(cp.stats().messages, 0u);
    }
}

// Only the header present, which is what a torn read looks like.
TEST(Malformed, HeaderOnlyIsTruncated) {
    const auto full = msg_add_order();
    std::vector<std::byte> header(full.begin(), full.begin() + itch::kHeaderLen);

    ZeroCopyDecoder dec;
    Recorder        rec;
    EXPECT_EQ(dec.decode(std::span<const std::byte>(header), rec), DecodeResult::Truncated);
    EXPECT_TRUE(rec.calls.empty());
}

TEST(Malformed, EmptySpanIsTruncated) {
    ZeroCopyDecoder zc;
    CopyingDecoder  cp;
    Recorder        rec_zc;
    Recorder        rec_cp;

    EXPECT_EQ(zc.decode(std::span<const std::byte>(), rec_zc), DecodeResult::Truncated);
    EXPECT_EQ(cp.decode(std::span<const std::byte>(), rec_cp), DecodeResult::Truncated);
    EXPECT_TRUE(rec_zc.calls.empty());
    EXPECT_TRUE(rec_cp.calls.empty());
    EXPECT_EQ(zc.stats().truncated, 1u);
    EXPECT_EQ(cp.stats().truncated, 1u);
}

TEST(Malformed, UnknownTypeIsCountedNotGuessed) {
    // A type byte no ITCH 5.0 message uses, followed by enough bytes that only
    // the type byte can be the reason this is refused.
    auto m  = msg_add_order();
    m[itch::kOffType] = static_cast<std::byte>('Z');

    ZeroCopyDecoder zc;
    CopyingDecoder  cp;
    Recorder        rec_zc;
    Recorder        rec_cp;
    std::size_t     consumed = 12345;

    EXPECT_EQ(zc.decode(std::span<const std::byte>(m), rec_zc, &consumed),
              DecodeResult::UnknownType);
    EXPECT_EQ(cp.decode(std::span<const std::byte>(m), rec_cp), DecodeResult::UnknownType);
    EXPECT_TRUE(rec_zc.calls.empty());
    EXPECT_TRUE(rec_cp.calls.empty());
    EXPECT_EQ(zc.stats().unknown, 1u);
    EXPECT_EQ(cp.stats().unknown, 1u);
    EXPECT_EQ(zc.stats().messages, 0u);
    // Nothing was consumed, so the caller's cursor is untouched and it can
    // decide whether to resynchronise or give up.
    EXPECT_EQ(consumed, 12345u);
}

// A zero byte is a plausible thing to find in a buffer that was never filled,
// and it must not be mistaken for a message.
TEST(Malformed, ZeroByteIsUnknown) {
    const auto      m = bytes({0x00});
    ZeroCopyDecoder dec;
    Recorder        rec;
    EXPECT_EQ(dec.decode(std::span<const std::byte>(m), rec), DecodeResult::UnknownType);
    EXPECT_TRUE(rec.calls.empty());
}

// The span may be longer than the message and the decoder must stop at the
// message's own length rather than running to the end of the buffer.
TEST(Malformed, TrailingBytesAreNotConsumed) {
    auto m = msg_order_delete();
    m.insert(m.end(), 64, std::byte{0xFF});

    ZeroCopyDecoder dec;
    Recorder        rec;
    std::size_t     consumed = 0;
    ASSERT_EQ(dec.decode(std::span<const std::byte>(m), rec, &consumed), DecodeResult::Ok);
    EXPECT_EQ(consumed, itch::message_length('D'));
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].ref, 0x0102030405060708ull);
}

// Every message decoded from a buffer sized to exactly that message. Under ASan
// the allocation is followed by a redzone, so a read one byte past the end is
// a test failure rather than a silent success. This is the assertion that the
// decoder never reads past the end.
TEST(Malformed, NoReadPastTheEnd) {
    for (const auto& m : all_messages()) {
        ASSERT_EQ(m.size(), itch::message_length(static_cast<char>(m[itch::kOffType])));

        std::vector<std::byte> exact(m.begin(), m.end());
        ZeroCopyDecoder        zc;
        CopyingDecoder         cp;
        Recorder               rec_zc;
        Recorder               rec_cp;

        EXPECT_EQ(zc.decode(std::span<const std::byte>(exact), rec_zc), DecodeResult::Ok);
        EXPECT_EQ(cp.decode(std::span<const std::byte>(exact), rec_cp), DecodeResult::Ok);
        EXPECT_EQ(rec_zc.calls, rec_cp.calls);
    }
}

// The handler that ignores everything still has to satisfy the concept and
// still has to count, because the benchmark decodes into it.
TEST(NullHandlerDecode, CountsWithoutDoingAnything) {
    static_assert(ItchHandler<NullHandler>);

    std::vector<std::byte> stream;
    for (const auto& m : all_messages()) stream.insert(stream.end(), m.begin(), m.end());

    ZeroCopyDecoder dec;
    NullHandler     null;
    std::size_t     off = 0;
    while (off < stream.size()) {
        std::size_t n = 0;
        ASSERT_EQ(dec.decode(std::span<const std::byte>(stream).subspan(off), null, &n),
                  DecodeResult::Ok);
        off += n;
    }
    EXPECT_EQ(dec.stats().messages, all_messages().size());
    EXPECT_EQ(dec.stats().bytes, stream.size());

    dec.reset();
    EXPECT_EQ(dec.stats().messages, 0u);
    EXPECT_EQ(dec.stats().bytes, 0u);
}
