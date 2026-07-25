#pragma once

#include "tick/itch.hpp"

#include <array>
#include <concepts>
#include <cstring>
#include <span>
#include <string_view>

// Two decoders that produce exactly the same sequence of handler calls from the
// same bytes, and differ only in how the bytes get to the handler.
//
// ZeroCopyDecoder reads fields out of the receive buffer where they already
// are, at the moment the handler needs them. Nothing is materialised, nothing
// is copied, and a field the handler ignores is never loaded because the
// handler is a template parameter and the load is dead code.
//
// CopyingDecoder does what a straightforward implementation does. It copies the
// message off the wire into its own staging buffer, byteswaps every field of
// that message type into an owned NormalizedEvent whether the handler wants it
// or not, and dispatches from the staged object.
//
// Both exist because "zero-copy" without the slow path next to it is a claim
// rather than a measurement. With both built and both correct, the difference
// is a number, and running them over the same trading day and asserting the
// books come out identical is the strongest correctness check in the project.
//
// The handler is a template parameter and never a virtual interface. A virtual
// call per message on a feed doing millions of messages a second is a real cost
// and it also blocks inlining, which is where most of the win is.

namespace tick {

// Everything one ITCH message can say, owned and already byteswapped. This is
// what the copying decoder builds. It is also the record format for the replay
// harness, which wants an owned value rather than a pointer into a buffer that
// is about to be overwritten.
struct NormalizedEvent {
    uint64_t timestamp  = 0;   // nanoseconds since midnight Eastern
    uint64_t ref        = 0;   // order reference, or original reference for U
    uint64_t ref2       = 0;   // new order reference, U only
    uint64_t match      = 0;   // match number on executions and trades
    uint32_t shares     = 0;
    uint32_t price      = 0;   // ten-thousandths of a dollar
    uint32_t round_lot  = 0;   // R only
    uint16_t locate     = 0;
    uint16_t tracking   = 0;
    char     type       = '\0';
    char     side       = '\0'; // 'B' or 'S' where the message carries one
    char     aux        = '\0'; // event code, trading state, cross type
    uint8_t  stock_len  = 0;
    bool     has_mpid   = false;
    bool     printable  = false;
    std::array<char, itch::kStockLen> stock{};

    [[nodiscard]] std::string_view symbol() const noexcept {
        return {stock.data(), stock_len};
    }
};

// What a decoder needs from whatever is consuming the feed. The book builder
// models this, and so do the counting handler in the replay tool and the
// recording handler in the tests.
//
// Handlers take scalars rather than a struct so the zero-copy path can hand
// over exactly the fields it read and nothing else.
// clang-format off
template <typename H>
concept ItchHandler = requires(H h, uint16_t locate, uint64_t ts, uint64_t ref,
                               uint32_t shares, uint32_t price, uint64_t match,
                               char c, bool b, std::string_view sv, uint32_t lot) {
    { h.on_system_event(ts, c) };
    { h.on_stock_directory(locate, ts, sv, lot) };
    { h.on_trading_action(locate, ts, c) };
    { h.on_add(locate, ts, ref, c, shares, price, b) };
    { h.on_execute(locate, ts, ref, shares, match) };
    { h.on_execute_price(locate, ts, ref, shares, match, b, price) };
    { h.on_cancel(locate, ts, ref, shares) };
    { h.on_delete(locate, ts, ref) };
    { h.on_replace(locate, ts, ref, ref, shares, price) };
    { h.on_trade(locate, ts, ref, c, shares, price, match) };
    { h.on_cross_trade(locate, ts, shares, price, match, c) };
    { h.on_broken_trade(locate, ts, match) };
    { h.on_other(c, locate, ts) };
};
// clang-format on

// A handler that ignores everything. Inherit from it and override only the
// messages a given consumer cares about, which keeps the test handlers short.
struct NullHandler {
    void on_system_event(uint64_t, char) noexcept {}
    void on_stock_directory(uint16_t, uint64_t, std::string_view, uint32_t) noexcept {}
    void on_trading_action(uint16_t, uint64_t, char) noexcept {}
    void on_add(uint16_t, uint64_t, uint64_t, char, uint32_t, uint32_t, bool) noexcept {}
    void on_execute(uint16_t, uint64_t, uint64_t, uint32_t, uint64_t) noexcept {}
    void on_execute_price(uint16_t, uint64_t, uint64_t, uint32_t, uint64_t, bool, uint32_t) noexcept {}
    void on_cancel(uint16_t, uint64_t, uint64_t, uint32_t) noexcept {}
    void on_delete(uint16_t, uint64_t, uint64_t) noexcept {}
    void on_replace(uint16_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t) noexcept {}
    void on_trade(uint16_t, uint64_t, uint64_t, char, uint32_t, uint32_t, uint64_t) noexcept {}
    void on_cross_trade(uint16_t, uint64_t, uint32_t, uint32_t, uint64_t, char) noexcept {}
    void on_broken_trade(uint16_t, uint64_t, uint64_t) noexcept {}
    void on_other(char, uint16_t, uint64_t) noexcept {}
};

// What a decode attempt did with the bytes it was given.
enum class DecodeResult : uint8_t {
    Ok,           // one message decoded and dispatched
    Truncated,    // the buffer is shorter than this message type requires
    UnknownType,  // the type byte is not an ITCH 5.0 message this build knows
};

struct DecodeStats {
    uint64_t messages  = 0;
    uint64_t bytes     = 0;
    uint64_t unknown   = 0;
    uint64_t truncated = 0;
    std::array<uint64_t, 256> by_type{};

    void clear() noexcept { *this = DecodeStats{}; }
};

// ---------------------------------------------------------------------------
// Zero-copy
// ---------------------------------------------------------------------------

class ZeroCopyDecoder {
public:
    static constexpr std::string_view kName = "zero-copy";

    // Decode exactly one message at the front of msg. The span is allowed to be
    // longer than the message. On Ok, consumed holds the message's fixed length.
    template <ItchHandler H>
    DecodeResult decode(std::span<const std::byte> msg, H& handler,
                        std::size_t* consumed = nullptr) noexcept {
        if (msg.empty()) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        const std::byte*  p    = msg.data();
        const char        type = itch::msg_type(p);
        const std::size_t len  = itch::message_length(type);

        if (len == 0) {
            ++stats_.unknown;
            return DecodeResult::UnknownType;
        }
        if (msg.size() < len) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        const uint16_t loc = itch::locate(p);
        const uint64_t ts  = itch::timestamp(p);

        // One switch over a small dense set of ASCII bytes, which clang turns
        // into a jump table. The cases are ordered by how often they appear on
        // a real day, which costs nothing at runtime and documents where the
        // volume actually is. Add, delete, and execute are almost all of it.
        switch (type) {
        case 'A':
            handler.on_add(loc, ts,
                           be_load<uint64_t>(p + itch::off::kAddRef),
                           static_cast<char>(p[itch::off::kAddSide]),
                           be_load<uint32_t>(p + itch::off::kAddShares),
                           be_load<uint32_t>(p + itch::off::kAddPrice),
                           false);
            break;

        case 'D':
            handler.on_delete(loc, ts, be_load<uint64_t>(p + itch::off::kDeleteRef));
            break;

        case 'U':
            handler.on_replace(loc, ts,
                               be_load<uint64_t>(p + itch::off::kReplaceOldRef),
                               be_load<uint64_t>(p + itch::off::kReplaceNewRef),
                               be_load<uint32_t>(p + itch::off::kReplaceShares),
                               be_load<uint32_t>(p + itch::off::kReplacePrice));
            break;

        case 'E':
            handler.on_execute(loc, ts,
                               be_load<uint64_t>(p + itch::off::kExecRef),
                               be_load<uint32_t>(p + itch::off::kExecShares),
                               be_load<uint64_t>(p + itch::off::kExecMatch));
            break;

        case 'X':
            handler.on_cancel(loc, ts,
                              be_load<uint64_t>(p + itch::off::kCancelRef),
                              be_load<uint32_t>(p + itch::off::kCancelShares));
            break;

        case 'C':
            handler.on_execute_price(loc, ts,
                                     be_load<uint64_t>(p + itch::off::kExecPxRef),
                                     be_load<uint32_t>(p + itch::off::kExecPxShares),
                                     be_load<uint64_t>(p + itch::off::kExecPxMatch),
                                     static_cast<char>(p[itch::off::kExecPxPrintable]) == 'Y',
                                     be_load<uint32_t>(p + itch::off::kExecPxPrice));
            break;

        case 'F':
            // Identical to A through the first thirty six bytes. The four byte
            // attribution that follows is the only difference and the book does
            // not use it, so the flag is all the handler is told.
            handler.on_add(loc, ts,
                           be_load<uint64_t>(p + itch::off::kAddRef),
                           static_cast<char>(p[itch::off::kAddSide]),
                           be_load<uint32_t>(p + itch::off::kAddShares),
                           be_load<uint32_t>(p + itch::off::kAddPrice),
                           true);
            break;

        case 'P':
            handler.on_trade(loc, ts,
                             be_load<uint64_t>(p + itch::off::kTradeRef),
                             static_cast<char>(p[itch::off::kTradeSide]),
                             be_load<uint32_t>(p + itch::off::kTradeShares),
                             be_load<uint32_t>(p + itch::off::kTradePrice),
                             be_load<uint64_t>(p + itch::off::kTradeMatch));
            break;

        case 'Q':
            handler.on_cross_trade(loc, ts,
                                   // Cross trade shares is eight bytes wide,
                                   // unlike every other share count. Truncating
                                   // to uint32 is safe because a cross is
                                   // reported in shares and no single cross has
                                   // come near four billion, but the narrowing
                                   // is explicit rather than implicit.
                                   static_cast<uint32_t>(be_load<uint64_t>(p + itch::off::kCrossShares)),
                                   be_load<uint32_t>(p + itch::off::kCrossPrice),
                                   be_load<uint64_t>(p + itch::off::kCrossMatch),
                                   static_cast<char>(p[itch::off::kCrossType]));
            break;

        case 'B':
            handler.on_broken_trade(loc, ts, be_load<uint64_t>(p + itch::off::kBrokenMatch));
            break;

        case 'R':
            handler.on_stock_directory(loc, ts,
                                       itch::stock(p + itch::off::kDirStock),
                                       be_load<uint32_t>(p + itch::off::kDirRoundLot));
            break;

        case 'H':
            handler.on_trading_action(loc, ts, static_cast<char>(p[itch::off::kActionState]));
            break;

        case 'S':
            handler.on_system_event(ts, static_cast<char>(p[itch::off::kEventCode]));
            break;

        default:
            // Known length, no semantics implemented. Counted, never guessed at.
            handler.on_other(type, loc, ts);
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

private:
    DecodeStats stats_{};
};

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

// The honest slow path. Two things happen here that do not happen above.
//
// One, the message is copied out of the receive buffer into a staging array
// before any field is read. This is what a handler does when it wants to own
// its input, and it is what you get for free if you push messages onto a queue
// before decoding them.
//
// Two, every field of the message type is byteswapped into a NormalizedEvent,
// including fields the consumer never looks at. The eight byte ticker on an add
// order is the clearest example. The book keys on stock locate and never reads
// the ticker, so the zero-copy path never touches those eight bytes and this
// path copies them on every add.
//
// The staging buffer is a member rather than a local so the two decoders are
// compared on decode cost rather than on stack traffic, and so this path also
// allocates nothing.
class CopyingDecoder {
public:
    static constexpr std::string_view kName = "copying";

    template <ItchHandler H>
    DecodeResult decode(std::span<const std::byte> msg, H& handler,
                        std::size_t* consumed = nullptr) noexcept {
        if (msg.empty()) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        const char        type = itch::msg_type(msg.data());
        const std::size_t len  = itch::message_length(type);

        if (len == 0) {
            ++stats_.unknown;
            return DecodeResult::UnknownType;
        }
        if (msg.size() < len) {
            ++stats_.truncated;
            return DecodeResult::Truncated;
        }

        std::memcpy(staging_.data(), msg.data(), len);
        normalize(staging_.data(), type, event_);
        dispatch(event_, handler);

        ++stats_.messages;
        stats_.bytes += len;
        ++stats_.by_type[static_cast<uint8_t>(type)];
        if (consumed) *consumed = len;
        return DecodeResult::Ok;
    }

    // The last event decoded. The replay recorder uses this to keep an owned
    // copy of the stream without decoding it twice.
    [[nodiscard]] const NormalizedEvent& event() const noexcept { return event_; }

    [[nodiscard]] const DecodeStats& stats() const noexcept { return stats_; }
    void reset() noexcept { stats_.clear(); }

    // Byteswap every field this message type carries into an owned struct.
    // Exposed as a static so the tests can check normalisation on its own,
    // without a handler in the way.
    static void normalize(const std::byte* p, char type, NormalizedEvent& e) noexcept {
        e = NormalizedEvent{};
        e.type      = type;
        e.locate    = itch::locate(p);
        e.tracking  = itch::tracking(p);
        e.timestamp = itch::timestamp(p);

        switch (type) {
        case 'A':
        case 'F':
            e.ref      = be_load<uint64_t>(p + itch::off::kAddRef);
            e.side     = static_cast<char>(p[itch::off::kAddSide]);
            e.shares   = be_load<uint32_t>(p + itch::off::kAddShares);
            e.price    = be_load<uint32_t>(p + itch::off::kAddPrice);
            e.has_mpid = (type == 'F');
            copy_stock(p + itch::off::kAddStock, e);
            break;
        case 'E':
            e.ref    = be_load<uint64_t>(p + itch::off::kExecRef);
            e.shares = be_load<uint32_t>(p + itch::off::kExecShares);
            e.match  = be_load<uint64_t>(p + itch::off::kExecMatch);
            break;
        case 'C':
            e.ref       = be_load<uint64_t>(p + itch::off::kExecPxRef);
            e.shares    = be_load<uint32_t>(p + itch::off::kExecPxShares);
            e.match     = be_load<uint64_t>(p + itch::off::kExecPxMatch);
            e.printable = static_cast<char>(p[itch::off::kExecPxPrintable]) == 'Y';
            e.price     = be_load<uint32_t>(p + itch::off::kExecPxPrice);
            break;
        case 'X':
            e.ref    = be_load<uint64_t>(p + itch::off::kCancelRef);
            e.shares = be_load<uint32_t>(p + itch::off::kCancelShares);
            break;
        case 'D':
            e.ref = be_load<uint64_t>(p + itch::off::kDeleteRef);
            break;
        case 'U':
            e.ref    = be_load<uint64_t>(p + itch::off::kReplaceOldRef);
            e.ref2   = be_load<uint64_t>(p + itch::off::kReplaceNewRef);
            e.shares = be_load<uint32_t>(p + itch::off::kReplaceShares);
            e.price  = be_load<uint32_t>(p + itch::off::kReplacePrice);
            break;
        case 'P':
            e.ref    = be_load<uint64_t>(p + itch::off::kTradeRef);
            e.side   = static_cast<char>(p[itch::off::kTradeSide]);
            e.shares = be_load<uint32_t>(p + itch::off::kTradeShares);
            e.price  = be_load<uint32_t>(p + itch::off::kTradePrice);
            e.match  = be_load<uint64_t>(p + itch::off::kTradeMatch);
            copy_stock(p + itch::off::kTradeStock, e);
            break;
        case 'Q':
            e.shares = static_cast<uint32_t>(be_load<uint64_t>(p + itch::off::kCrossShares));
            e.price  = be_load<uint32_t>(p + itch::off::kCrossPrice);
            e.match  = be_load<uint64_t>(p + itch::off::kCrossMatch);
            e.aux    = static_cast<char>(p[itch::off::kCrossType]);
            copy_stock(p + itch::off::kCrossStock, e);
            break;
        case 'B':
            e.match = be_load<uint64_t>(p + itch::off::kBrokenMatch);
            break;
        case 'R':
            e.round_lot = be_load<uint32_t>(p + itch::off::kDirRoundLot);
            e.aux       = static_cast<char>(p[itch::off::kDirMarketCat]);
            copy_stock(p + itch::off::kDirStock, e);
            break;
        case 'H':
            e.aux = static_cast<char>(p[itch::off::kActionState]);
            copy_stock(p + itch::off::kActionStock, e);
            break;
        case 'S':
            e.aux = static_cast<char>(p[itch::off::kEventCode]);
            break;
        default:
            break;
        }
    }

    // Turn an owned event back into the same handler calls the zero-copy path
    // makes. Shared with the replay harness, which dispatches recorded events.
    template <ItchHandler H>
    static void dispatch(const NormalizedEvent& e, H& handler) noexcept {
        switch (e.type) {
        case 'A':
        case 'F':
            handler.on_add(e.locate, e.timestamp, e.ref, e.side, e.shares, e.price, e.has_mpid);
            break;
        case 'E':
            handler.on_execute(e.locate, e.timestamp, e.ref, e.shares, e.match);
            break;
        case 'C':
            handler.on_execute_price(e.locate, e.timestamp, e.ref, e.shares, e.match,
                                     e.printable, e.price);
            break;
        case 'X':
            handler.on_cancel(e.locate, e.timestamp, e.ref, e.shares);
            break;
        case 'D':
            handler.on_delete(e.locate, e.timestamp, e.ref);
            break;
        case 'U':
            handler.on_replace(e.locate, e.timestamp, e.ref, e.ref2, e.shares, e.price);
            break;
        case 'P':
            handler.on_trade(e.locate, e.timestamp, e.ref, e.side, e.shares, e.price, e.match);
            break;
        case 'Q':
            handler.on_cross_trade(e.locate, e.timestamp, e.shares, e.price, e.match, e.aux);
            break;
        case 'B':
            handler.on_broken_trade(e.locate, e.timestamp, e.match);
            break;
        case 'R':
            handler.on_stock_directory(e.locate, e.timestamp, e.symbol(), e.round_lot);
            break;
        case 'H':
            handler.on_trading_action(e.locate, e.timestamp, e.aux);
            break;
        case 'S':
            handler.on_system_event(e.timestamp, e.aux);
            break;
        default:
            handler.on_other(e.type, e.locate, e.timestamp);
            break;
        }
    }

private:
    static void copy_stock(const std::byte* p, NormalizedEvent& e) noexcept {
        std::memcpy(e.stock.data(), p, itch::kStockLen);
        std::size_t n = itch::kStockLen;
        while (n > 0 && e.stock[n - 1] == ' ') --n;
        e.stock_len = static_cast<uint8_t>(n);
    }

    std::array<std::byte, itch::kMaxMessageLen> staging_{};
    NormalizedEvent                             event_{};
    DecodeStats                                 stats_{};
};

} // namespace tick
