#pragma once

#include "tick/endian.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

// NASDAQ OUCH 4.2 on the wire, the order entry counterpart to ITCH.
//
// ITCH is what the exchange tells everyone. OUCH is what one participant tells
// the exchange and what the exchange tells that participant back. Nothing in
// OUCH is broadcast. Every message on this connection is about an order this
// process sent, which is why the decoder here is a private-stream decoder and
// not a market data decoder.
//
// WHAT THIS SPEAKS TO, AND WHAT IT HAS NEVER SPOKEN TO.
//
// This encodes and decodes OUCH 4.2 for NanoExchange, which is a local matching
// engine that lives in this author's other repository. It is not a real venue.
// No byte produced by this header has ever been sent to NASDAQ, and no byte
// produced by NASDAQ has ever been fed to this decoder. What has been exercised
// is the wire layout against the published specification and round trips
// against hand-built byte vectors in test_ouch.cpp. Read every performance or
// correctness claim in this repository with that boundary in mind.
//
// WHICH VERSION, AND WHY NOT 5.0.
//
// The layouts below are transcribed from
//   https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/Ouch4.2.pdf
// which is titled O*U*C*H Version 4.2 and dated Updated October, 2025.
//
// OUCH 5.0 exists and was read alongside it, at
//   https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf
// 5.0 replaces the fourteen byte Order Token with a four byte User Reference
// Number and moves optional order attributes into a variable length TagValue
// appendage, so 5.0 messages are no longer fixed length. 4.2 is implemented
// here because fixed length messages and a length table are what this repository
// is built around, and because a fixed width client-chosen token is the thing
// worth modelling. This is a scoping decision, not a claim that 4.2 is current.
//
// WIRE SHAPE.
//
// Every message is fixed length, packed, and big-endian. Integers are unsigned
// network byte order. Alpha fields are left justified and padded on the right
// with spaces, so a symbol shorter than eight characters is not zero
// terminated. Prices are integers in ten-thousandths of a dollar, the same
// scale ITCH uses, so a price crossing from the feed to the order path is a
// widening and nothing else.
//
// Two things in the specification are worth flagging because they trip people
// who assume OUCH looks like ITCH.
//
// One, there is no common header. ITCH gives every message the same eleven byte
// preamble. OUCH does not. Inbound messages carry no timestamp at all, and
// outbound messages carry an eight byte nanoseconds-past-midnight timestamp at
// offset one where ITCH puts a six byte one at offset five.
//
// Two, the message type byte is only unique within a direction. 'U' is Replace
// Order inbound and Replaced outbound. 'M' is Modify Order inbound and Order
// Modified outbound. A single length table indexed by the type byte would
// therefore be wrong, and there are two tables below for exactly that reason.
//
// Offsets are named constants rather than a packed struct, for the same reason
// as in itch.hpp. A packed struct needs a reinterpret_cast onto an unaligned
// buffer, which is undefined behaviour. See endian.hpp.

namespace tick::ouch {

// ---------------------------------------------------------------------------
// Field widths
// ---------------------------------------------------------------------------

// The Order Token. Fourteen bytes of alphanumeric ASCII, spaces allowed, case
// sensitive, and required to be unique for the day within one OUCH account.
inline constexpr std::size_t kTokenLen = 14;

inline constexpr std::size_t kStockLen = 8;
inline constexpr std::size_t kFirmLen  = 4;

// Price bounds quoted by the specification. The maximum representable price in
// OUCH 4.2 is $199,999.9900, which is 0x7735939C in ten-thousandths. A price of
// 0x7FFFFFFF is the market order sentinel used when entering a cross, and the
// specification also says anything at or above $200,000.00 is treated as a
// market order. These are named so a caller can reject out of range prices
// before the exchange does.
inline constexpr uint32_t kMaxLimitPrice  = 0x7735939Cu;
inline constexpr uint32_t kMarketPrice    = 0x7FFFFFFFu;

// Special Time in Force values from the specification. Time in force is a count
// of seconds the order should live.
inline constexpr uint32_t kTifImmediateOrCancel   = 0;
inline constexpr uint32_t kTifExtendedTradingClose = 99996;
inline constexpr uint32_t kTifMarketHours          = 99998;
inline constexpr uint32_t kTifSystemHours          = 99999;

// Fill an alpha field. Left justified, right padded with spaces, truncated if
// the caller hands over something too long. Space padding rather than zero
// padding is what the specification requires and it is the single easiest field
// convention to get wrong, so it lives in one place.
template <std::size_t N>
[[nodiscard]] constexpr std::array<char, N> alpha(std::string_view s) noexcept {
    std::array<char, N> a{};
    for (std::size_t i = 0; i < N; ++i) a[i] = i < s.size() ? s[i] : ' ';
    return a;
}

// Trailing spaces removed, for printing and for comparing against a symbol
// table entry. Returns a view into the caller's array.
template <std::size_t N>
[[nodiscard]] constexpr std::string_view trim(const std::array<char, N>& a) noexcept {
    std::size_t n = N;
    while (n > 0 && a[n - 1] == ' ') --n;
    return {a.data(), n};
}

// ---------------------------------------------------------------------------
// The Order Token
// ---------------------------------------------------------------------------

// Why the token is the most important field in the protocol.
//
// OUCH assigns the order an exchange side Reference Number, but only in the
// Accepted message, which arrives after the order does. The token is the
// participant's own identifier and it is chosen before the order leaves this
// process, so it is the only thing that can tie an acknowledgement, a fill, a
// cancel, or a reject back to the order that caused it. Every outbound message
// in section three of the specification carries the token, and the Executed
// message carries the token and no Reference Number at all.
//
// The token is also what makes inbound resend safe. The specification builds
// fail over around benign retransmission, and the mechanism is that an Enter
// Order with a previously used token is silently ignored. That is a very sharp
// edge. A duplicate token does not produce a reject, it produces nothing, so a
// generator that ever repeats a token turns a live order into an order that
// vanishes with no error anywhere. The generator below treats running out of
// token width as a counted fault rather than wrapping quietly.
struct Token {
    std::array<char, kTokenLen> b = alpha<kTokenLen>("");

    [[nodiscard]] static constexpr Token from(std::string_view s) noexcept {
        return Token{alpha<kTokenLen>(s)};
    }
    // The full fourteen bytes including padding, which is what goes on the wire.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {b.data(), kTokenLen};
    }
    // Trailing spaces removed, for logs.
    [[nodiscard]] constexpr std::string_view trimmed() const noexcept { return trim(b); }

    [[nodiscard]] constexpr bool operator==(const Token&) const noexcept = default;
};

// Base thirty six because the alphabet is exactly the characters the
// specification allows and they are contiguous and ascending in ASCII.
inline constexpr char kDigits36[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Tokens that are unique, monotonically increasing, and cost no allocation and
// no sprintf.
//
// The shape is a caller supplied prefix, which identifies the session or the
// strategy, followed by the counter rendered right justified and zero padded in
// base thirty six across whatever width is left. Zero padding to a fixed width
// is what makes byte-wise ordering agree with numeric ordering, so sorting a
// log by token sorts it by send order without parsing anything.
//
// The hot path is a divide loop over at most fourteen digits with no branches
// on the data and no call into the C library. sprintf on an order entry path
// would be a locale-aware, format-parsing, potentially locking call sitting
// inside tick to trade, which is exactly the kind of thing that does not show
// up in a microbenchmark and does show up in a tail latency histogram.
class TokenGenerator {
public:
    explicit TokenGenerator(std::string_view prefix = {}, uint64_t first = 0) noexcept
        : counter_(first) {
        const std::size_t n = prefix.size() < kTokenLen ? prefix.size() : kTokenLen;
        for (std::size_t i = 0; i < n; ++i) prefix_[i] = prefix[i];
        prefix_len_ = static_cast<uint8_t>(n);
        digits_     = kTokenLen - n;
        capacity_   = pow36(digits_);
    }

    [[nodiscard]] Token next() noexcept {
        Token t;
        for (std::size_t i = 0; i < prefix_len_; ++i) t.b[i] = prefix_[i];

        uint64_t v = counter_;
        // Running past the width the prefix left behind means the next token
        // repeats one already sent, and a repeated token is silently dropped by
        // NASDAQ. Counted here and reported through exhausted() so the order
        // path can stop rather than send orders into a hole.
        if (capacity_ != 0 && v >= capacity_) {
            ++overflowed_;
            v %= capacity_;
        }
        for (std::size_t i = kTokenLen; i > prefix_len_; --i) {
            t.b[i - 1] = kDigits36[v % 36];
            v /= 36;
        }
        ++counter_;
        return t;
    }

    [[nodiscard]] uint64_t    counter() const noexcept   { return counter_; }
    [[nodiscard]] std::size_t digits() const noexcept    { return digits_; }
    // Zero means the counter width is wider than a uint64 can ever exhaust.
    [[nodiscard]] uint64_t    capacity() const noexcept  { return capacity_; }
    [[nodiscard]] bool        exhausted() const noexcept { return overflowed_ != 0; }
    [[nodiscard]] uint64_t    overflows() const noexcept { return overflowed_; }

    // Thirty six to the power d, saturating to zero when the value no longer
    // fits in a uint64. Zero is used as the sentinel for unbounded because a
    // uint64 counter cannot reach 36^13 either.
    [[nodiscard]] static constexpr uint64_t pow36(std::size_t d) noexcept {
        uint64_t v = 1;
        for (std::size_t i = 0; i < d; ++i) {
            if (v > (~uint64_t{0}) / 36) return 0;
            v *= 36;
        }
        return v;
    }

private:
    std::array<char, kTokenLen> prefix_{};
    uint64_t                    counter_    = 0;
    uint64_t                    capacity_   = 0;
    uint64_t                    overflowed_ = 0;
    std::size_t                 digits_     = kTokenLen;
    uint8_t                     prefix_len_ = 0;
};

// ---------------------------------------------------------------------------
// Inbound, participant to exchange
// ---------------------------------------------------------------------------

namespace in {

enum class MsgType : char {
    EnterOrder   = 'O',
    ReplaceOrder = 'U',
    CancelOrder  = 'X',
    ModifyOrder  = 'M',
};

// Enter Order, section 2.1. Forty nine bytes.
namespace enter {
inline constexpr std::size_t kType         = 0;
inline constexpr std::size_t kToken        = 1;
inline constexpr std::size_t kSide         = 15;
inline constexpr std::size_t kShares       = 16;
inline constexpr std::size_t kStock        = 20;
inline constexpr std::size_t kPrice        = 28;
inline constexpr std::size_t kTimeInForce  = 32;
inline constexpr std::size_t kFirm         = 36;
inline constexpr std::size_t kDisplay      = 40;
inline constexpr std::size_t kCapacity     = 41;
inline constexpr std::size_t kIso          = 42;
inline constexpr std::size_t kMinQuantity  = 43;
inline constexpr std::size_t kCrossType    = 47;
inline constexpr std::size_t kCustomerType = 48;
inline constexpr std::size_t kLen          = 49;
} // namespace enter

// Replace Order, section 2.2. Forty seven bytes.
//
// Two tokens. The first names the order being replaced and must match the token
// on the Enter Order or on the last Replace in the chain. The second is a new
// day-unique token that this replacement will be known by from now on, and it
// may not collide with any token already used. So a replace consumes a token
// from the generator exactly as an entry does.
//
// There is no Stock field. A replace cannot move an order to another symbol.
namespace replace {
inline constexpr std::size_t kType             = 0;
inline constexpr std::size_t kExistingToken    = 1;
inline constexpr std::size_t kReplacementToken = 15;
inline constexpr std::size_t kShares           = 29;
inline constexpr std::size_t kPrice            = 33;
inline constexpr std::size_t kTimeInForce      = 37;
inline constexpr std::size_t kDisplay          = 41;
inline constexpr std::size_t kIso              = 42;
inline constexpr std::size_t kMinQuantity      = 43;
inline constexpr std::size_t kLen              = 47;
} // namespace replace

// Cancel Order, section 2.3. Nineteen bytes.
//
// The Shares field is not the quantity to remove. It is the new intended order
// size, meaning the maximum total that may still execute once the cancel is
// applied. Zero cancels the remaining balance. Reading it as a decrement is a
// genuine way to write a cancel that silently does nothing.
namespace cancel {
inline constexpr std::size_t kType   = 0;
inline constexpr std::size_t kToken  = 1;
inline constexpr std::size_t kShares = 15;
inline constexpr std::size_t kLen    = 19;
} // namespace cancel

// Modify Order, section 2.4. Twenty bytes. Offsets carried for the length table
// so an unhandled inbound message can still be skipped by the right amount.
namespace modify {
inline constexpr std::size_t kType   = 0;
inline constexpr std::size_t kToken  = 1;
inline constexpr std::size_t kSide   = 15;
inline constexpr std::size_t kShares = 16;
inline constexpr std::size_t kLen    = 20;
} // namespace modify

// One table per direction, because 'U' and 'M' mean different things each way.
// A 256 entry lookup built at compile time, same reasoning as itch.hpp.
struct LengthTable {
    std::array<uint8_t, 256> len{};
    constexpr LengthTable() {
        len[static_cast<uint8_t>('O')] = enter::kLen;
        len[static_cast<uint8_t>('U')] = replace::kLen;
        len[static_cast<uint8_t>('X')] = cancel::kLen;
        len[static_cast<uint8_t>('M')] = modify::kLen;
    }
};
inline constexpr LengthTable kLengths{};

[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
    return kLengths.len[static_cast<uint8_t>(type)];
}

// The widest inbound message, which is Enter Order. A caller can size a send
// buffer from this and never look at a length again.
inline constexpr std::size_t kMaxMessageLen = enter::kLen;

} // namespace in

// ---------------------------------------------------------------------------
// Outbound, exchange to participant
// ---------------------------------------------------------------------------

namespace out {

enum class MsgType : char {
    SystemEvent          = 'S',
    Accepted             = 'A',
    Replaced             = 'U',
    Canceled             = 'C',
    AiqCanceled          = 'D',
    Executed             = 'E',
    BrokenTrade          = 'B',
    ExecutedWithRefPrice = 'G',
    Rejected             = 'J',
    CancelPending        = 'P',
    CancelReject         = 'I',
    PriorityUpdate       = 'T',
    OrderModified        = 'M',
};

// System Event, section 3.1. Ten bytes.
//
// OUCH 4.2 defines only two codes, 'S' for start of day and 'E' for end of day.
// This is a much smaller set than the ITCH system event codes and the letters
// do not line up with ITCH, where 'S' is start of system hours and 'E' is end
// of system hours. Do not share an enum between the two.
namespace sysevt {
inline constexpr std::size_t kType      = 0;
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kEventCode = 9;
inline constexpr std::size_t kLen       = 10;
} // namespace sysevt

enum class EventCode : char {
    StartOfDay = 'S',
    EndOfDay   = 'E',
};

// Accepted, section 3.3. Sixty six bytes.
namespace accepted {
inline constexpr std::size_t kType        = 0;
inline constexpr std::size_t kTimestamp   = 1;
inline constexpr std::size_t kToken       = 9;
inline constexpr std::size_t kSide        = 23;
inline constexpr std::size_t kShares      = 24;
inline constexpr std::size_t kStock       = 28;
inline constexpr std::size_t kPrice       = 36;
inline constexpr std::size_t kTimeInForce = 40;
inline constexpr std::size_t kFirm        = 44;
inline constexpr std::size_t kDisplay     = 48;
inline constexpr std::size_t kReference   = 49;
inline constexpr std::size_t kCapacity    = 57;
inline constexpr std::size_t kIso         = 58;
inline constexpr std::size_t kMinQuantity = 59;
inline constexpr std::size_t kCrossType   = 63;
inline constexpr std::size_t kOrderState  = 64;
inline constexpr std::size_t kBboWeight   = 65;
inline constexpr std::size_t kLen         = 66;
} // namespace accepted

// Replaced, section 3.4. Eighty bytes, the longest message in the protocol.
//
// Identical to Accepted through offset sixty four and then carries the token of
// the order that was replaced, which is how a client walks a replace chain
// backwards. The token at offset nine is the replacement token, not the old
// one, which is the opposite of what the reading order suggests.
namespace replaced {
inline constexpr std::size_t kType          = 0;
inline constexpr std::size_t kTimestamp     = 1;
inline constexpr std::size_t kToken         = 9;   // the replacement token
inline constexpr std::size_t kSide          = 23;
inline constexpr std::size_t kShares        = 24;
inline constexpr std::size_t kStock         = 28;
inline constexpr std::size_t kPrice         = 36;
inline constexpr std::size_t kTimeInForce   = 40;
inline constexpr std::size_t kFirm          = 44;
inline constexpr std::size_t kDisplay       = 48;
inline constexpr std::size_t kReference     = 49;
inline constexpr std::size_t kCapacity      = 57;
inline constexpr std::size_t kIso           = 58;
inline constexpr std::size_t kMinQuantity   = 59;
inline constexpr std::size_t kCrossType     = 63;
inline constexpr std::size_t kOrderState    = 64;
inline constexpr std::size_t kPreviousToken = 65;
inline constexpr std::size_t kBboWeight     = 79;
inline constexpr std::size_t kLen           = 80;
} // namespace replaced

// Canceled, section 3.5. Twenty eight bytes.
//
// Decrement Shares is incremental and not cumulative, and a Canceled message
// does not mean the order is dead. A partial cancel reduces the order and
// leaves the rest working. Treating this as a terminal event is how a client
// ends up believing it is flat when it is not.
namespace canceled {
inline constexpr std::size_t kType             = 0;
inline constexpr std::size_t kTimestamp        = 1;
inline constexpr std::size_t kToken            = 9;
inline constexpr std::size_t kDecrementShares  = 23;
inline constexpr std::size_t kReason           = 27;
inline constexpr std::size_t kLen              = 28;
} // namespace canceled

// Executed, section 3.7. Forty bytes.
//
// Executed Shares is incremental. There is no Reference Number and no symbol on
// this message, so the token is the only key back to the order, which is the
// clearest argument for taking the token seriously.
namespace executed {
inline constexpr std::size_t kType          = 0;
inline constexpr std::size_t kTimestamp     = 1;
inline constexpr std::size_t kToken         = 9;
inline constexpr std::size_t kShares        = 23;
inline constexpr std::size_t kPrice         = 27;
inline constexpr std::size_t kLiquidityFlag = 31;
inline constexpr std::size_t kMatchNumber   = 32;
inline constexpr std::size_t kLen           = 40;
} // namespace executed

// Rejected, section 3.10. Twenty four bytes.
namespace rejected {
inline constexpr std::size_t kType      = 0;
inline constexpr std::size_t kTimestamp = 1;
inline constexpr std::size_t kToken     = 9;
inline constexpr std::size_t kReason    = 23;
inline constexpr std::size_t kLen       = 24;
} // namespace rejected

// Lengths of the outbound messages this build knows about but does not decode
// into fields. They are in the table so the framing layer can skip them by the
// right amount instead of losing the stream, and so an unhandled message is
// counted rather than guessed at.
inline constexpr std::size_t kAiqCanceledLen          = 38;  // section 3.6
inline constexpr std::size_t kBrokenTradeLen          = 32;  // section 3.8
inline constexpr std::size_t kExecutedWithRefPriceLen = 45;  // section 3.9
inline constexpr std::size_t kCancelPendingLen        = 23;  // section 3.11
inline constexpr std::size_t kCancelRejectLen         = 23;  // section 3.12
inline constexpr std::size_t kPriorityUpdateLen       = 36;  // section 3.13
inline constexpr std::size_t kOrderModifiedLen        = 28;  // section 3.14

struct LengthTable {
    std::array<uint8_t, 256> len{};
    constexpr LengthTable() {
        len[static_cast<uint8_t>('S')] = sysevt::kLen;
        len[static_cast<uint8_t>('A')] = accepted::kLen;
        len[static_cast<uint8_t>('U')] = replaced::kLen;
        len[static_cast<uint8_t>('C')] = canceled::kLen;
        len[static_cast<uint8_t>('D')] = kAiqCanceledLen;
        len[static_cast<uint8_t>('E')] = executed::kLen;
        len[static_cast<uint8_t>('B')] = kBrokenTradeLen;
        len[static_cast<uint8_t>('G')] = kExecutedWithRefPriceLen;
        len[static_cast<uint8_t>('J')] = rejected::kLen;
        len[static_cast<uint8_t>('P')] = kCancelPendingLen;
        len[static_cast<uint8_t>('I')] = kCancelRejectLen;
        len[static_cast<uint8_t>('T')] = kPriorityUpdateLen;
        len[static_cast<uint8_t>('M')] = kOrderModifiedLen;
    }
};
inline constexpr LengthTable kLengths{};

[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
    return kLengths.len[static_cast<uint8_t>(type)];
}
[[nodiscard]] constexpr bool is_known_type(char type) noexcept {
    return message_length(type) != 0;
}

// The longest outbound message, which is Replaced. A staging buffer of this
// size holds any of them.
inline constexpr std::size_t kMaxMessageLen = replaced::kLen;

// Order State on Accepted and Replaced. Order Dead means the exchange accepted
// the order and immediately killed it, and no further messages will arrive for
// that token. A client that waits for a Canceled after an Accepted with 'D'
// waits forever.
enum class OrderState : char {
    Live = 'L',
    Dead = 'D',
};

// A few cancel reasons from section 3.5.1, named because the order path reacts
// to them differently. The specification tells clients to expect additions and
// to tolerate any capital letter, so nothing here switches exhaustively on it.
enum class CancelReason : char {
    UserRequested       = 'U',
    ImmediateOrCancel   = 'I',
    Timeout             = 'T',
    Supervisory         = 'S',
    Regulatory          = 'D',
    SelfMatchPrevention = 'Q',
    SystemCancel        = 'Z',
    CrossCanceled       = 'C',
    Halted              = 'H',
};

// A few reject reasons from section 3.10.1.
//
// Worth noticing that NASDAQ's own reject list is mostly pre-trade risk. Fat
// Finger, Max Shares, Max Notional, Aggregate Exposure and the message rate
// restrictions are all in here. The venue runs the same family of checks this
// repository's risk.hpp runs, and a participant that does not run them locally
// simply finds out later and over the wire.
enum class RejectReason : char {
    RiskRestrictedStock      = 'a',
    RiskShortSellRestricted  = 'b',
    RiskOrderTypeRestricted  = 'c',
    NasdaqClosed             = 'C',
    RiskExceedsAdvLimit      = 'd',
    InvalidDisplayType       = 'D',
    RiskFatFinger            = 'e',
    Halted                   = 'H',
    RiskMaxSharesExceeded    = 'm',
    RiskMaxNotionalExceeded  = 'n',
    InvalidMinimumQuantity   = 'N',
    Other                    = 'O',
    RiskMarketImpact         = 'r',
    InvalidStock             = 'S',
    TestMode                 = 'T',
    RiskAggregateExposure    = 'v',
    RiskSymbolMessageRate    = 'w',
    RiskPortMessageRate      = 'x',
    InvalidPrice             = 'X',
    SharesOverSafetyLimit    = 'Z',
};

} // namespace out

// ---------------------------------------------------------------------------
// Outbound field accessors, zero copy
// ---------------------------------------------------------------------------

// Read a field where it already is in the receive buffer. A handler that only
// wants the token and the share count never pays to byteswap a match number.
// The caller is responsible for having checked the length first.
namespace field {

[[nodiscard]] inline char msg_type(const std::byte* p) noexcept {
    return static_cast<char>(p[0]);
}

// Outbound timestamps are a full eight bytes at offset one, nanoseconds past
// midnight. Unlike ITCH there is no six byte form here.
[[nodiscard]] inline uint64_t timestamp(const std::byte* p) noexcept {
    return be_load<uint64_t>(p + 1);
}

[[nodiscard]] inline Token token(const std::byte* p, std::size_t off) noexcept {
    Token t;
    std::memcpy(t.b.data(), p + off, kTokenLen);
    return t;
}

[[nodiscard]] inline std::array<char, kStockLen> stock(const std::byte* p,
                                                       std::size_t off) noexcept {
    std::array<char, kStockLen> a{};
    std::memcpy(a.data(), p + off, kStockLen);
    return a;
}

[[nodiscard]] inline std::array<char, kFirmLen> firm(const std::byte* p,
                                                     std::size_t off) noexcept {
    std::array<char, kFirmLen> a{};
    std::memcpy(a.data(), p + off, kFirmLen);
    return a;
}

} // namespace field

// ---------------------------------------------------------------------------
// Decoded message bodies
// ---------------------------------------------------------------------------

// Accepted and Replaced carry seventeen and eighteen fields. Handing those to a
// handler as loose scalars, the way the ITCH decoder does for its five field
// messages, produces a signature nobody can read and that a transposed argument
// slips straight through. So these two arrive as an owned struct built on the
// stack. The handler is still a template parameter, so there is no virtual call
// and the compiler is free to scalarise the struct away entirely when the
// handler only touches two of its members.
//
// The narrow messages stay as scalars, which keeps the common path, Executed,
// exactly as cheap as the ITCH one.

struct AcceptedMsg {
    uint64_t                    timestamp     = 0;
    Token                       token{};
    uint64_t                    reference     = 0;   // exchange assigned
    std::array<char, kStockLen> stock         = alpha<kStockLen>("");
    std::array<char, kFirmLen>  firm          = alpha<kFirmLen>("");
    uint32_t                    shares        = 0;
    uint32_t                    price         = 0;
    uint32_t                    time_in_force = 0;
    uint32_t                    min_quantity  = 0;
    char                        side          = '\0';
    char                        display       = '\0';
    char                        capacity      = '\0';
    char                        iso           = '\0';
    char                        cross_type    = '\0';
    char                        order_state   = '\0';
    char                        bbo_weight    = '\0';

    [[nodiscard]] std::string_view symbol() const noexcept { return trim(stock); }
    [[nodiscard]] bool             dead() const noexcept {
        return order_state == static_cast<char>(out::OrderState::Dead);
    }
};

struct ReplacedMsg {
    AcceptedMsg common{};          // everything through offset sixty four
    Token       previous_token{};  // the token this replacement retired
};

// ---------------------------------------------------------------------------
// Encoders, participant to exchange
// ---------------------------------------------------------------------------

// The fields of an Enter Order, in a form the caller fills in once. Defaults are
// the conservative reading of the specification where one exists, and are left
// at zero or space where the specification has no safe default. Nothing here is
// invented. Display 'Y' is Anonymous Price to Comply, Capacity 'P' is principal,
// Intermarket Sweep 'N' is not eligible, and Cross Type 'N' is the continuous
// market, all straight out of the field tables.
struct EnterOrder {
    Token                       token{};
    char                        side          = 'B';
    uint32_t                    shares        = 0;
    std::array<char, kStockLen> stock         = alpha<kStockLen>("");
    uint32_t                    price         = 0;
    uint32_t                    time_in_force = kTifImmediateOrCancel;
    std::array<char, kFirmLen>  firm          = alpha<kFirmLen>("");
    char                        display       = 'Y';
    char                        capacity      = 'P';
    char                        iso           = 'N';
    uint32_t                    min_quantity  = 0;
    char                        cross_type    = 'N';
    char                        customer_type = ' ';
};

struct ReplaceOrder {
    Token    existing_token{};
    Token    replacement_token{};
    uint32_t shares        = 0;
    uint32_t price         = 0;
    uint32_t time_in_force = kTifImmediateOrCancel;
    char     display       = 'Y';
    char     iso           = 'N';
    uint32_t min_quantity  = 0;
};

struct CancelOrder {
    Token    token{};
    // The new intended order size, not a decrement. Zero cancels the balance.
    uint32_t intended_shares = 0;
};

// Encoders write into a caller supplied buffer and allocate nothing. They
// return the number of bytes written, or zero when the buffer is too small,
// which the caller must treat as a failure to send rather than as a short send.
// Returning zero rather than throwing keeps the order path free of exceptions.

[[nodiscard]] inline std::size_t encode(std::span<std::byte> buf,
                                        const EnterOrder& o) noexcept {
    if (buf.size() < in::enter::kLen) return 0;
    std::byte* p = buf.data();
    p[in::enter::kType] = static_cast<std::byte>('O');
    std::memcpy(p + in::enter::kToken, o.token.b.data(), kTokenLen);
    p[in::enter::kSide] = static_cast<std::byte>(o.side);
    be_store<uint32_t>(p + in::enter::kShares, o.shares);
    std::memcpy(p + in::enter::kStock, o.stock.data(), kStockLen);
    be_store<uint32_t>(p + in::enter::kPrice, o.price);
    be_store<uint32_t>(p + in::enter::kTimeInForce, o.time_in_force);
    std::memcpy(p + in::enter::kFirm, o.firm.data(), kFirmLen);
    p[in::enter::kDisplay]      = static_cast<std::byte>(o.display);
    p[in::enter::kCapacity]     = static_cast<std::byte>(o.capacity);
    p[in::enter::kIso]          = static_cast<std::byte>(o.iso);
    be_store<uint32_t>(p + in::enter::kMinQuantity, o.min_quantity);
    p[in::enter::kCrossType]    = static_cast<std::byte>(o.cross_type);
    p[in::enter::kCustomerType] = static_cast<std::byte>(o.customer_type);
    return in::enter::kLen;
}

[[nodiscard]] inline std::size_t encode(std::span<std::byte> buf,
                                        const ReplaceOrder& o) noexcept {
    if (buf.size() < in::replace::kLen) return 0;
    std::byte* p = buf.data();
    p[in::replace::kType] = static_cast<std::byte>('U');
    std::memcpy(p + in::replace::kExistingToken, o.existing_token.b.data(), kTokenLen);
    std::memcpy(p + in::replace::kReplacementToken, o.replacement_token.b.data(), kTokenLen);
    be_store<uint32_t>(p + in::replace::kShares, o.shares);
    be_store<uint32_t>(p + in::replace::kPrice, o.price);
    be_store<uint32_t>(p + in::replace::kTimeInForce, o.time_in_force);
    p[in::replace::kDisplay] = static_cast<std::byte>(o.display);
    p[in::replace::kIso]     = static_cast<std::byte>(o.iso);
    be_store<uint32_t>(p + in::replace::kMinQuantity, o.min_quantity);
    return in::replace::kLen;
}

[[nodiscard]] inline std::size_t encode(std::span<std::byte> buf,
                                        const CancelOrder& o) noexcept {
    if (buf.size() < in::cancel::kLen) return 0;
    std::byte* p = buf.data();
    p[in::cancel::kType] = static_cast<std::byte>('X');
    std::memcpy(p + in::cancel::kToken, o.token.b.data(), kTokenLen);
    be_store<uint32_t>(p + in::cancel::kShares, o.intended_shares);
    return in::cancel::kLen;
}

// Decoding an inbound message is only needed by a test or by NanoExchange
// standing in for the venue, so these read back what the encoders wrote and are
// deliberately plain. They return false when the buffer is too short, and they
// never read past the length they checked.
[[nodiscard]] inline bool decode_enter_order(std::span<const std::byte> buf,
                                             EnterOrder& o) noexcept {
    if (buf.size() < in::enter::kLen ||
        field::msg_type(buf.data()) != 'O') return false;
    const std::byte* p = buf.data();
    o.token         = field::token(p, in::enter::kToken);
    o.side          = static_cast<char>(p[in::enter::kSide]);
    o.shares        = be_load<uint32_t>(p + in::enter::kShares);
    o.stock         = field::stock(p, in::enter::kStock);
    o.price         = be_load<uint32_t>(p + in::enter::kPrice);
    o.time_in_force = be_load<uint32_t>(p + in::enter::kTimeInForce);
    o.firm          = field::firm(p, in::enter::kFirm);
    o.display       = static_cast<char>(p[in::enter::kDisplay]);
    o.capacity      = static_cast<char>(p[in::enter::kCapacity]);
    o.iso           = static_cast<char>(p[in::enter::kIso]);
    o.min_quantity  = be_load<uint32_t>(p + in::enter::kMinQuantity);
    o.cross_type    = static_cast<char>(p[in::enter::kCrossType]);
    o.customer_type = static_cast<char>(p[in::enter::kCustomerType]);
    return true;
}

[[nodiscard]] inline bool decode_replace_order(std::span<const std::byte> buf,
                                               ReplaceOrder& o) noexcept {
    if (buf.size() < in::replace::kLen ||
        field::msg_type(buf.data()) != 'U') return false;
    const std::byte* p = buf.data();
    o.existing_token    = field::token(p, in::replace::kExistingToken);
    o.replacement_token = field::token(p, in::replace::kReplacementToken);
    o.shares            = be_load<uint32_t>(p + in::replace::kShares);
    o.price             = be_load<uint32_t>(p + in::replace::kPrice);
    o.time_in_force     = be_load<uint32_t>(p + in::replace::kTimeInForce);
    o.display           = static_cast<char>(p[in::replace::kDisplay]);
    o.iso               = static_cast<char>(p[in::replace::kIso]);
    o.min_quantity      = be_load<uint32_t>(p + in::replace::kMinQuantity);
    return true;
}

[[nodiscard]] inline bool decode_cancel_order(std::span<const std::byte> buf,
                                              CancelOrder& o) noexcept {
    if (buf.size() < in::cancel::kLen ||
        field::msg_type(buf.data()) != 'X') return false;
    const std::byte* p = buf.data();
    o.token           = field::token(p, in::cancel::kToken);
    o.intended_shares = be_load<uint32_t>(p + in::cancel::kShares);
    return true;
}

// ---------------------------------------------------------------------------
// Decoder, exchange to participant
// ---------------------------------------------------------------------------

// What a decode attempt did with the bytes. Deliberately its own enum in this
// namespace rather than the ITCH one, because an OUCH stream and an ITCH stream
// are different streams and sharing the type would invite sharing the stats.
enum class DecodeResult : uint8_t {
    Ok,          // one message decoded and dispatched
    Truncated,   // the buffer is shorter than this message type requires
    UnknownType, // the type byte is not an OUCH 4.2 outbound message
};

struct DecodeStats {
    uint64_t                     messages  = 0;
    uint64_t                     bytes     = 0;
    uint64_t                     unknown   = 0;
    uint64_t                     truncated = 0;
    std::array<uint64_t, 256>    by_type{};

    void clear() noexcept { *this = DecodeStats{}; }
};

// clang-format off
template <typename H>
concept OuchHandler = requires(H h, uint64_t ts, const Token& tok, uint32_t n,
                               uint32_t px, uint64_t match, char c,
                               const AcceptedMsg& a, const ReplacedMsg& r) {
    { h.on_system_event(ts, c) };
    { h.on_accepted(a) };
    { h.on_replaced(r) };
    { h.on_canceled(ts, tok, n, c) };
    { h.on_executed(ts, tok, n, px, c, match) };
    { h.on_rejected(ts, tok, c) };
    { h.on_other(c, ts) };
};
// clang-format on

// Ignores everything. Inherit and override only what a given consumer wants,
// which keeps the test handlers short. Same pattern as NullHandler in the ITCH
// decoder.
struct NullOuchHandler {
    void on_system_event(uint64_t, char) noexcept {}
    void on_accepted(const AcceptedMsg&) noexcept {}
    void on_replaced(const ReplacedMsg&) noexcept {}
    void on_canceled(uint64_t, const Token&, uint32_t, char) noexcept {}
    void on_executed(uint64_t, const Token&, uint32_t, uint32_t, char, uint64_t) noexcept {}
    void on_rejected(uint64_t, const Token&, char) noexcept {}
    void on_other(char, uint64_t) noexcept {}
};

// Dispatch is a switch on a small dense set of ASCII bytes with no virtual
// call, so the handler inlines and the fields a handler ignores are dead code.
// Cases are ordered by how often they arrive on a working session. Executed
// dominates, then Canceled, then the acknowledgements.
class OuchDecoder {
public:
    // Decode exactly one message at the front of msg. The span may be longer
    // than the message. On Ok, consumed holds this message's fixed length.
    //
    // Every path checks the declared length against the span before touching a
    // single body byte, so a truncated or hostile buffer is a counted return
    // and never an out of bounds read.
    template <OuchHandler H>
    DecodeResult decode(std::span<const std::byte> msg, H& handler,
                        std::size_t* consumed = nullptr) noexcept {
        if (msg.empty()) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        const std::byte*  p    = msg.data();
        const char        type = field::msg_type(p);
        const std::size_t len  = out::message_length(type);

        if (len == 0) {
            ++stats_.unknown;
            return DecodeResult::UnknownType;
        }
        if (msg.size() < len) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        const uint64_t ts = field::timestamp(p);

        switch (type) {
        case 'E':
            handler.on_executed(ts,
                                field::token(p, out::executed::kToken),
                                be_load<uint32_t>(p + out::executed::kShares),
                                be_load<uint32_t>(p + out::executed::kPrice),
                                static_cast<char>(p[out::executed::kLiquidityFlag]),
                                be_load<uint64_t>(p + out::executed::kMatchNumber));
            break;

        case 'C':
            handler.on_canceled(ts,
                                field::token(p, out::canceled::kToken),
                                be_load<uint32_t>(p + out::canceled::kDecrementShares),
                                static_cast<char>(p[out::canceled::kReason]));
            break;

        case 'A': {
            AcceptedMsg a;
            read_common(p, ts, a);
            a.bbo_weight = static_cast<char>(p[out::accepted::kBboWeight]);
            handler.on_accepted(a);
            break;
        }

        case 'U': {
            // Replaced is Accepted through offset sixty four plus the retired
            // token, so the shared prefix is read by the same function. The BBO
            // Weight indicator is not in that prefix. On Accepted it sits at
            // sixty five and on Replaced the previous token sits there instead
            // and the indicator moves to seventy nine, which is why it is read
            // at the call site rather than inside the shared function.
            ReplacedMsg r;
            read_common(p, ts, r.common);
            r.previous_token    = field::token(p, out::replaced::kPreviousToken);
            r.common.bbo_weight = static_cast<char>(p[out::replaced::kBboWeight]);
            handler.on_replaced(r);
            break;
        }

        case 'J':
            handler.on_rejected(ts,
                                field::token(p, out::rejected::kToken),
                                static_cast<char>(p[out::rejected::kReason]));
            break;

        case 'S':
            handler.on_system_event(ts, static_cast<char>(p[out::sysevt::kEventCode]));
            break;

        default:
            // Known length, no semantics implemented. Counted and skipped by
            // the right number of bytes, never guessed at.
            handler.on_other(type, ts);
            break;
        }

        ++stats_.messages;
        stats_.bytes += len;
        ++stats_.by_type[static_cast<uint8_t>(type)];
        if (consumed) *consumed = len;
        return DecodeResult::Ok;
    }

    [[nodiscard]] const DecodeStats& stats() const noexcept { return stats_; }
    void reset() noexcept { stats_.clear(); }

    // The fields Accepted and Replaced share, offsets nine through sixty four.
    // Exposed so a test can check field extraction without a handler in the
    // way. The caller has already checked the length.
    static void read_common(const std::byte* p, uint64_t ts, AcceptedMsg& a) noexcept {
        a.timestamp     = ts;
        a.token         = field::token(p, out::accepted::kToken);
        a.side          = static_cast<char>(p[out::accepted::kSide]);
        a.shares        = be_load<uint32_t>(p + out::accepted::kShares);
        a.stock         = field::stock(p, out::accepted::kStock);
        a.price         = be_load<uint32_t>(p + out::accepted::kPrice);
        a.time_in_force = be_load<uint32_t>(p + out::accepted::kTimeInForce);
        a.firm          = field::firm(p, out::accepted::kFirm);
        a.display       = static_cast<char>(p[out::accepted::kDisplay]);
        a.reference     = be_load<uint64_t>(p + out::accepted::kReference);
        a.capacity      = static_cast<char>(p[out::accepted::kCapacity]);
        a.iso           = static_cast<char>(p[out::accepted::kIso]);
        a.min_quantity  = be_load<uint32_t>(p + out::accepted::kMinQuantity);
        a.cross_type    = static_cast<char>(p[out::accepted::kCrossType]);
        a.order_state   = static_cast<char>(p[out::accepted::kOrderState]);
    }

private:
    DecodeStats stats_{};
};

// Encoders for the outbound messages, so NanoExchange can answer in OUCH and so
// the tests can build a message without hand placing every byte. Same contract
// as the inbound encoders, zero on a buffer that is too small.

[[nodiscard]] inline std::size_t encode_system_event(std::span<std::byte> buf,
                                                     uint64_t ts, char code) noexcept {
    if (buf.size() < out::sysevt::kLen) return 0;
    std::byte* p = buf.data();
    p[out::sysevt::kType] = static_cast<std::byte>('S');
    be_store<uint64_t>(p + out::sysevt::kTimestamp, ts);
    p[out::sysevt::kEventCode] = static_cast<std::byte>(code);
    return out::sysevt::kLen;
}

// The bytes Accepted and Replaced have in common, offsets one through sixty
// four. Written once so the two encoders cannot drift apart. The BBO Weight
// indicator is not included because it sits at a different offset in each.
inline void write_common(std::byte* p, const AcceptedMsg& a) noexcept {
    be_store<uint64_t>(p + out::accepted::kTimestamp, a.timestamp);
    std::memcpy(p + out::accepted::kToken, a.token.b.data(), kTokenLen);
    p[out::accepted::kSide] = static_cast<std::byte>(a.side);
    be_store<uint32_t>(p + out::accepted::kShares, a.shares);
    std::memcpy(p + out::accepted::kStock, a.stock.data(), kStockLen);
    be_store<uint32_t>(p + out::accepted::kPrice, a.price);
    be_store<uint32_t>(p + out::accepted::kTimeInForce, a.time_in_force);
    std::memcpy(p + out::accepted::kFirm, a.firm.data(), kFirmLen);
    p[out::accepted::kDisplay] = static_cast<std::byte>(a.display);
    be_store<uint64_t>(p + out::accepted::kReference, a.reference);
    p[out::accepted::kCapacity] = static_cast<std::byte>(a.capacity);
    p[out::accepted::kIso]      = static_cast<std::byte>(a.iso);
    be_store<uint32_t>(p + out::accepted::kMinQuantity, a.min_quantity);
    p[out::accepted::kCrossType]  = static_cast<std::byte>(a.cross_type);
    p[out::accepted::kOrderState] = static_cast<std::byte>(a.order_state);
}

[[nodiscard]] inline std::size_t encode_accepted(std::span<std::byte> buf,
                                                 const AcceptedMsg& a) noexcept {
    if (buf.size() < out::accepted::kLen) return 0;
    std::byte* p = buf.data();
    p[out::accepted::kType] = static_cast<std::byte>('A');
    write_common(p, a);
    p[out::accepted::kBboWeight] = static_cast<std::byte>(a.bbo_weight);
    return out::accepted::kLen;
}

[[nodiscard]] inline std::size_t encode_replaced(std::span<std::byte> buf,
                                                 const ReplacedMsg& r) noexcept {
    if (buf.size() < out::replaced::kLen) return 0;
    std::byte* p = buf.data();
    p[out::replaced::kType] = static_cast<std::byte>('U');
    write_common(p, r.common);
    std::memcpy(p + out::replaced::kPreviousToken, r.previous_token.b.data(), kTokenLen);
    p[out::replaced::kBboWeight] = static_cast<std::byte>(r.common.bbo_weight);
    return out::replaced::kLen;
}

[[nodiscard]] inline std::size_t encode_canceled(std::span<std::byte> buf, uint64_t ts,
                                                 const Token& tok, uint32_t decrement,
                                                 char reason) noexcept {
    if (buf.size() < out::canceled::kLen) return 0;
    std::byte* p = buf.data();
    p[out::canceled::kType] = static_cast<std::byte>('C');
    be_store<uint64_t>(p + out::canceled::kTimestamp, ts);
    std::memcpy(p + out::canceled::kToken, tok.b.data(), kTokenLen);
    be_store<uint32_t>(p + out::canceled::kDecrementShares, decrement);
    p[out::canceled::kReason] = static_cast<std::byte>(reason);
    return out::canceled::kLen;
}

[[nodiscard]] inline std::size_t encode_executed(std::span<std::byte> buf, uint64_t ts,
                                                 const Token& tok, uint32_t shares,
                                                 uint32_t price, char liquidity,
                                                 uint64_t match) noexcept {
    if (buf.size() < out::executed::kLen) return 0;
    std::byte* p = buf.data();
    p[out::executed::kType] = static_cast<std::byte>('E');
    be_store<uint64_t>(p + out::executed::kTimestamp, ts);
    std::memcpy(p + out::executed::kToken, tok.b.data(), kTokenLen);
    be_store<uint32_t>(p + out::executed::kShares, shares);
    be_store<uint32_t>(p + out::executed::kPrice, price);
    p[out::executed::kLiquidityFlag] = static_cast<std::byte>(liquidity);
    be_store<uint64_t>(p + out::executed::kMatchNumber, match);
    return out::executed::kLen;
}

[[nodiscard]] inline std::size_t encode_rejected(std::span<std::byte> buf, uint64_t ts,
                                                 const Token& tok, char reason) noexcept {
    if (buf.size() < out::rejected::kLen) return 0;
    std::byte* p = buf.data();
    p[out::rejected::kType] = static_cast<std::byte>('J');
    be_store<uint64_t>(p + out::rejected::kTimestamp, ts);
    std::memcpy(p + out::rejected::kToken, tok.b.data(), kTokenLen);
    p[out::rejected::kReason] = static_cast<std::byte>(reason);
    return out::rejected::kLen;
}

// ---------------------------------------------------------------------------
// Client order state
// ---------------------------------------------------------------------------

// The lifecycle of one order token.
//
// ReplaceSent is in this list although the brief for this file named seven
// states, because the Replace Order encoder above exists and a replace in
// flight is genuinely a distinct state. An order with a replace outstanding can
// still fill on its old terms, and treating it as merely Accepted loses that.
enum class OrderState : uint8_t {
    Unknown,          // nothing has been sent for this token
    Sent,             // Enter Order written to the wire, no acknowledgement yet
    Accepted,         // Accepted message received, order is live
    PartiallyFilled,  // at least one Executed, shares still working
    Filled,           // fully executed, terminal
    ReplaceSent,      // Replace Order written, no Replaced or Canceled yet
    CancelSent,       // Cancel Order written, no Canceled yet
    Canceled,         // fully canceled, terminal
    Rejected,         // Rejected message received, terminal, token is burnt
};

[[nodiscard]] inline const char* to_string(OrderState s) noexcept {
    switch (s) {
    case OrderState::Unknown:         return "Unknown";
    case OrderState::Sent:            return "Sent";
    case OrderState::Accepted:        return "Accepted";
    case OrderState::PartiallyFilled: return "PartiallyFilled";
    case OrderState::Filled:          return "Filled";
    case OrderState::ReplaceSent:     return "ReplaceSent";
    case OrderState::CancelSent:      return "CancelSent";
    case OrderState::Canceled:        return "Canceled";
    case OrderState::Rejected:        return "Rejected";
    }
    return "Invalid";
}

// One order token's worth of state.
//
//   Unknown ── send ──▶ [ Sent ]
//                          │ Accepted            ──▶ [ Accepted ]
//                          │ Rejected            ──▶ [ Rejected ]  terminal
//   Accepted / PartiallyFilled
//                          │ Executed partial    ──▶ [ PartiallyFilled ]
//                          │ Executed full       ──▶ [ Filled ]    terminal
//                          │ Canceled partial    ──▶ stay, leaves reduced
//                          │ Canceled full       ──▶ [ Canceled ]  terminal
//                          │ cancel sent         ──▶ [ CancelSent ]
//                          │ replace sent        ──▶ [ ReplaceSent ]
//
// ILLEGAL TRANSITIONS ARE COUNTED, NOT ASSERTED. This is the same choice the
// ITCH book builder makes and for the same reason. OUCH is explicitly built
// around benignly resending inbound messages after a connection failure, so a
// second Accepted for a token, or an Executed arriving after a Canceled that
// crossed it on the wire, are things the protocol tells you to expect. An
// assert turns a survivable protocol event into a lost trading session. A
// counter that is non-zero at the end of the day is a thing to investigate.
//
// The one place this is strict is a fill against a terminal or unknown token,
// because that is a position this process does not know it has. It is still
// counted rather than fatal, but it is counted separately so it cannot hide in
// the general total.
struct ClientOrderState {
    Token      token{};
    OrderState state = OrderState::Unknown;

    uint16_t   locate          = 0;   // symbol, keyed the same way as the feed
    char       side            = '\0';
    uint32_t   original_shares = 0;
    uint32_t   leaves_shares   = 0;   // still working at the exchange
    uint32_t   filled_shares   = 0;
    uint64_t   filled_notional = 0;   // ten-thousandths of a dollar
    uint32_t   price           = 0;
    uint64_t   reference       = 0;   // exchange assigned, arrives on Accepted
    uint64_t   last_update_ns  = 0;

    // Invariant violations, survived rather than asserted.
    uint32_t   illegal_transitions = 0;
    uint32_t   orphan_fills        = 0;  // a fill against a terminal or unknown token
    uint32_t   overfills           = 0;  // more shares executed than were working

    [[nodiscard]] bool terminal() const noexcept {
        return state == OrderState::Filled || state == OrderState::Canceled ||
               state == OrderState::Rejected;
    }
    [[nodiscard]] bool working() const noexcept {
        return state == OrderState::Sent || state == OrderState::Accepted ||
               state == OrderState::PartiallyFilled || state == OrderState::ReplaceSent ||
               state == OrderState::CancelSent;
    }
    [[nodiscard]] uint32_t average_price() const noexcept {
        return filled_shares == 0
                   ? 0
                   : static_cast<uint32_t>(filled_notional / filled_shares);
    }

    // Each of these returns true when the transition was legal. The state is
    // advanced either way, because the exchange is the authority on what
    // happened and refusing to believe it is worse than recording it.

    bool on_sent(const Token& t, uint16_t loc, char sd, uint32_t shares,
                 uint32_t px, uint64_t ts) noexcept {
        const bool legal = (state == OrderState::Unknown);
        if (!legal) ++illegal_transitions;
        token           = t;
        locate          = loc;
        side            = sd;
        original_shares = shares;
        leaves_shares   = shares;
        filled_shares   = 0;
        filled_notional = 0;
        price           = px;
        state           = OrderState::Sent;
        last_update_ns  = ts;
        return legal;
    }

    bool on_accepted(uint32_t accepted_shares, uint32_t accepted_price,
                     uint64_t ref, bool dead, uint64_t ts) noexcept {
        const bool legal = (state == OrderState::Sent);
        if (!legal) ++illegal_transitions;
        reference      = ref;
        price          = accepted_price;
        last_update_ns = ts;
        // The exchange may accept fewer shares than were entered, and it may
        // reprice. Its numbers win over the ones this process sent.
        original_shares = accepted_shares;
        leaves_shares   = accepted_shares > filled_shares
                              ? accepted_shares - filled_shares
                              : 0;
        // Order Dead means accepted and immediately killed. Nothing else will
        // arrive for this token, so it goes straight to a terminal state.
        state = dead ? OrderState::Canceled : OrderState::Accepted;
        if (dead) leaves_shares = 0;
        return legal;
    }

    bool on_replaced(const Token& new_token, uint32_t shares, uint32_t px,
                     uint64_t ref, bool dead, uint64_t ts) noexcept {
        const bool legal = (state == OrderState::ReplaceSent);
        if (!legal) ++illegal_transitions;
        token          = new_token;
        original_shares = shares + filled_shares;
        leaves_shares  = shares;
        price          = px;
        reference      = ref;
        last_update_ns = ts;
        state          = dead ? OrderState::Canceled : OrderState::Accepted;
        if (dead) leaves_shares = 0;
        return legal;
    }

    bool on_fill(uint32_t shares, uint32_t px, uint64_t ts) noexcept {
        bool legal = true;
        if (terminal() || state == OrderState::Unknown) {
            ++orphan_fills;
            legal = false;
        }
        if (shares > leaves_shares) {
            // More executed than this process believed was working. Take the
            // exchange's number for the position and count the discrepancy.
            ++overfills;
            legal         = false;
            leaves_shares = 0;
        } else {
            leaves_shares -= shares;
        }
        filled_shares   += shares;
        filled_notional += static_cast<uint64_t>(shares) * px;
        last_update_ns   = ts;
        state = (leaves_shares == 0) ? OrderState::Filled : OrderState::PartiallyFilled;
        return legal;
    }

    bool on_cancel_sent(uint64_t ts) noexcept {
        const bool legal = (state == OrderState::Accepted ||
                            state == OrderState::PartiallyFilled ||
                            state == OrderState::Sent);
        if (!legal) ++illegal_transitions;
        state          = OrderState::CancelSent;
        last_update_ns = ts;
        return legal;
    }

    bool on_replace_sent(uint64_t ts) noexcept {
        const bool legal = (state == OrderState::Accepted ||
                            state == OrderState::PartiallyFilled);
        if (!legal) ++illegal_transitions;
        state          = OrderState::ReplaceSent;
        last_update_ns = ts;
        return legal;
    }

    // Decrement Shares is incremental, and a Canceled does not have to be
    // terminal. The order is only dead once nothing is left working.
    bool on_canceled(uint32_t decrement, uint64_t ts) noexcept {
        bool legal = !terminal() && state != OrderState::Unknown;
        if (!legal) ++illegal_transitions;
        if (decrement > leaves_shares) {
            ++overfills;
            legal         = false;
            leaves_shares = 0;
        } else {
            leaves_shares -= decrement;
        }
        last_update_ns = ts;
        if (leaves_shares == 0) {
            state = (filled_shares > 0 && filled_shares == original_shares)
                        ? OrderState::Filled
                        : OrderState::Canceled;
        }
        return legal;
    }

    bool on_rejected(uint64_t ts) noexcept {
        // A reject is only meaningful for an order that was sent and never
        // acknowledged. Anything else means the two sides disagree about what
        // this token is.
        const bool legal = (state == OrderState::Sent || state == OrderState::ReplaceSent);
        if (!legal) ++illegal_transitions;
        leaves_shares  = 0;
        state          = OrderState::Rejected;
        last_update_ns = ts;
        return legal;
    }
};

} // namespace tick::ouch
