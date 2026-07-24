#pragma once

#include "tick/endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// NASDAQ TotalView-ITCH 5.0 on the wire.
//
// Every message is fixed length, packed, big-endian, and carries the same
// eleven byte header.
//
//   offset 0   message type          1 byte  ASCII
//   offset 1   stock locate          2 bytes index into the day's stock directory
//   offset 3   tracking number       2 bytes NASDAQ internal, ignored here
//   offset 5   timestamp             6 bytes nanoseconds since midnight Eastern
//   offset 11  body, type dependent
//
// The file format puts a two byte big-endian length in front of each message.
// On the wire MoldUDP64 does the same job with its own message block length.
// Both are handled outside this header, which only understands one message
// starting at byte zero.
//
// Field offsets below are transcribed from the specification published at
// nasdaqtrader.com. They are constants rather than a packed struct on purpose.
// A struct would need attribute packed and a reinterpret_cast onto an unaligned
// buffer, which is undefined behaviour. See endian.hpp for why that matters.

namespace tick::itch {

// The header, common to every message.
inline constexpr std::size_t kOffType      = 0;
inline constexpr std::size_t kOffLocate    = 1;
inline constexpr std::size_t kOffTracking  = 3;
inline constexpr std::size_t kOffTimestamp = 5;
inline constexpr std::size_t kHeaderLen    = 11;

// The ticker field is eight bytes of ASCII right padded with spaces.
inline constexpr std::size_t kStockLen = 8;

// The largest ITCH 5.0 message is the NOII at fifty bytes. A fixed buffer of
// this size holds any message, which is what lets the copying decoder stage a
// message without touching the heap.
inline constexpr std::size_t kMaxMessageLen = 50;

enum class MsgType : char {
    SystemEvent            = 'S',
    StockDirectory         = 'R',
    TradingAction          = 'H',
    RegSHO                 = 'Y',
    ParticipantPosition    = 'L',
    MwcbDeclineLevel       = 'V',
    MwcbStatus             = 'W',
    IpoQuotingPeriod       = 'K',
    LuldAuctionCollar      = 'J',
    OperationalHalt        = 'h',
    AddOrder               = 'A',
    AddOrderMpid           = 'F',
    OrderExecuted          = 'E',
    OrderExecutedPrice     = 'C',
    OrderCancel            = 'X',
    OrderDelete            = 'D',
    OrderReplace           = 'U',
    Trade                  = 'P',
    CrossTrade             = 'Q',
    BrokenTrade            = 'B',
    Noii                   = 'I',
    Rpii                   = 'N',
    DirectListingCapRaise  = 'O',
};

// Expected wire length for a message type, header included, or zero when the
// type is not one this build knows about.
//
// A table indexed by the raw byte rather than a switch. Dispatch on a message
// type is the hottest branch in the decoder, and a 256 entry lookup is one load
// with no branch misprediction. The table is built at compile time so there is
// no static initialisation order problem.
struct LengthTable {
    std::array<uint8_t, 256> len{};

    constexpr LengthTable() {
        len[static_cast<uint8_t>('S')] = 12;  // system event
        len[static_cast<uint8_t>('R')] = 39;  // stock directory
        len[static_cast<uint8_t>('H')] = 25;  // stock trading action
        len[static_cast<uint8_t>('Y')] = 20;  // reg sho restriction
        len[static_cast<uint8_t>('L')] = 26;  // market participant position
        len[static_cast<uint8_t>('V')] = 35;  // mwcb decline level
        len[static_cast<uint8_t>('W')] = 12;  // mwcb status
        len[static_cast<uint8_t>('K')] = 28;  // ipo quoting period update
        len[static_cast<uint8_t>('J')] = 35;  // luld auction collar
        len[static_cast<uint8_t>('h')] = 21;  // operational halt
        len[static_cast<uint8_t>('A')] = 36;  // add order, no mpid
        len[static_cast<uint8_t>('F')] = 40;  // add order with mpid
        len[static_cast<uint8_t>('E')] = 31;  // order executed
        len[static_cast<uint8_t>('C')] = 36;  // order executed with price
        len[static_cast<uint8_t>('X')] = 23;  // order cancel
        len[static_cast<uint8_t>('D')] = 19;  // order delete
        len[static_cast<uint8_t>('U')] = 35;  // order replace
        len[static_cast<uint8_t>('P')] = 44;  // trade, non cross
        len[static_cast<uint8_t>('Q')] = 40;  // cross trade
        len[static_cast<uint8_t>('B')] = 19;  // broken trade
        len[static_cast<uint8_t>('I')] = 50;  // net order imbalance indicator
        len[static_cast<uint8_t>('N')] = 20;  // retail price improvement
        len[static_cast<uint8_t>('O')] = 48;  // direct listing capital raise
    }
};

inline constexpr LengthTable kLengths{};

[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
    return kLengths.len[static_cast<uint8_t>(type)];
}

[[nodiscard]] constexpr bool is_known_type(char type) noexcept {
    return message_length(type) != 0;
}

// Header accessors. Every message has these three fields at the same offsets,
// so the decoder reads them once before dispatching on the type.
[[nodiscard]] inline char      msg_type(const std::byte* p) noexcept { return static_cast<char>(p[kOffType]); }
[[nodiscard]] inline uint16_t  locate(const std::byte* p)   noexcept { return be_load<uint16_t>(p + kOffLocate); }
[[nodiscard]] inline uint16_t  tracking(const std::byte* p) noexcept { return be_load<uint16_t>(p + kOffTracking); }
[[nodiscard]] inline uint64_t  timestamp(const std::byte* p) noexcept { return be_load_u48(p + kOffTimestamp); }

// An eight byte ticker with the trailing spaces removed. Returns a view into
// the caller's buffer, so it is only valid while that buffer lives. The stock
// directory handler copies it into the symbol table immediately.
[[nodiscard]] inline std::string_view stock(const std::byte* p) noexcept {
    const char* s = reinterpret_cast<const char*>(p);
    std::size_t n = kStockLen;
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s, n};
}

// ---------------------------------------------------------------------------
// Body field offsets, by message type.
//
// These are named rather than inlined at the call site because a transposed
// offset is the single easiest way to write a decoder that runs, produces
// plausible numbers, and is wrong. Naming them lets the unit tests assert
// against the same constants the decoder uses, and lets a reader check them
// against the specification without reading the decoder at all.
// ---------------------------------------------------------------------------

namespace off {

// S, system event
inline constexpr std::size_t kEventCode = 11;

// R, stock directory
inline constexpr std::size_t kDirStock        = 11;
inline constexpr std::size_t kDirMarketCat    = 19;
inline constexpr std::size_t kDirFinStatus    = 20;
inline constexpr std::size_t kDirRoundLot     = 21;
inline constexpr std::size_t kDirRoundLotsOnly = 25;

// H, stock trading action
inline constexpr std::size_t kActionStock = 11;
inline constexpr std::size_t kActionState = 19;
inline constexpr std::size_t kActionReason = 21;

// A and F, add order
inline constexpr std::size_t kAddRef    = 11;
inline constexpr std::size_t kAddSide   = 19;
inline constexpr std::size_t kAddShares = 20;
inline constexpr std::size_t kAddStock  = 24;
inline constexpr std::size_t kAddPrice  = 32;
inline constexpr std::size_t kAddMpid   = 36; // F only

// E, order executed
inline constexpr std::size_t kExecRef    = 11;
inline constexpr std::size_t kExecShares = 19;
inline constexpr std::size_t kExecMatch  = 23;

// C, order executed with price
inline constexpr std::size_t kExecPxRef       = 11;
inline constexpr std::size_t kExecPxShares    = 19;
inline constexpr std::size_t kExecPxMatch     = 23;
inline constexpr std::size_t kExecPxPrintable = 31;
inline constexpr std::size_t kExecPxPrice     = 32;

// X, order cancel
inline constexpr std::size_t kCancelRef    = 11;
inline constexpr std::size_t kCancelShares = 19;

// D, order delete
inline constexpr std::size_t kDeleteRef = 11;

// U, order replace
inline constexpr std::size_t kReplaceOldRef = 11;
inline constexpr std::size_t kReplaceNewRef = 19;
inline constexpr std::size_t kReplaceShares = 27;
inline constexpr std::size_t kReplacePrice  = 31;

// P, trade, non cross
inline constexpr std::size_t kTradeRef    = 11;
inline constexpr std::size_t kTradeSide   = 19;
inline constexpr std::size_t kTradeShares = 20;
inline constexpr std::size_t kTradeStock  = 24;
inline constexpr std::size_t kTradePrice  = 32;
inline constexpr std::size_t kTradeMatch  = 36;

// Q, cross trade
inline constexpr std::size_t kCrossShares = 11;
inline constexpr std::size_t kCrossStock  = 19;
inline constexpr std::size_t kCrossPrice  = 27;
inline constexpr std::size_t kCrossMatch  = 31;
inline constexpr std::size_t kCrossType   = 39;

// B, broken trade
inline constexpr std::size_t kBrokenMatch = 11;

} // namespace off

// System event codes. Only the ones that change session state are named.
enum class EventCode : char {
    StartOfMessages   = 'O',
    StartOfSystemHours = 'S',
    StartOfMarketHours = 'Q',
    EndOfMarketHours   = 'M',
    EndOfSystemHours   = 'E',
    EndOfMessages      = 'C',
};

// Prices are in units of one ten-thousandth of a dollar. There is no floating
// point anywhere between the wire and the book. ITCH already works in integer
// ticks and so does NanoExchange, so the conversion is a widening and nothing
// else. Dividing by this constant is for printing only.
inline constexpr int64_t kPriceScale = 10000;

// Cross trade and NOII prices in the halt and auction messages use the same
// scale. Kept as one constant so a future venue with a different scale changes
// in one place.

} // namespace tick::itch
