#pragma once

#include "tick/endian.hpp"
#include "tick/itch.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Building ITCH 5.0 messages, the exact inverse of itch_decoder.hpp.
//
// A decoder is only as trustworthy as the bytes it was tested against. The four
// gigabyte NASDAQ sample cannot ride in a repository and cannot be fetched in
// CI, so the only bytes always available are the ones this project writes
// itself. That makes the encoder part of the build rather than test scaffolding,
// and it is also what the publisher uses to put a book back on the wire.
//
// Two rules hold everywhere below.
//
// Nothing allocates. Every function writes into a caller supplied buffer and
// returns the number of bytes it wrote, which is the fixed length for that
// message type. The caller owns the buffer and its lifetime.
//
// Offsets come from tick::itch::off wherever the decoder already names them, so
// an encoder and decoder disagreement is impossible for those fields by
// construction. A test that hand builds bytes is what catches a wrong constant
// in the shared table, which is why test_itch_decode.cpp uses literal arrays
// rather than these functions for its field assertions.

namespace tick::itch {

// The file framing is a two byte big-endian length in front of each message.
inline constexpr std::size_t kFrameLen = 2;

// Offsets for fields the decoder never reads and therefore never named. Writing
// a message needs every field, not only the ones the book cares about, so the
// remainder live here and itch.hpp stays the decoder's view of the wire.
namespace woff {

// R, stock directory
inline constexpr std::size_t kDirIssueClass         = 26;
inline constexpr std::size_t kDirIssueSubType       = 27;
inline constexpr std::size_t kDirAuthenticity       = 29;
inline constexpr std::size_t kDirShortSaleThreshold = 30;
inline constexpr std::size_t kDirIpoFlag            = 31;
inline constexpr std::size_t kDirLuldTier           = 32;
inline constexpr std::size_t kDirEtpFlag            = 33;
inline constexpr std::size_t kDirEtpLeverage        = 34;
inline constexpr std::size_t kDirInverse            = 38;

// H, stock trading action
inline constexpr std::size_t kActionReserved = 20;

} // namespace woff

// ---------------------------------------------------------------------------
// Field primitives
// ---------------------------------------------------------------------------

inline void put_char(std::byte* out, std::size_t off, char c) noexcept {
    out[off] = static_cast<std::byte>(c);
}

// Alpha fields are fixed width ASCII right padded with spaces. A shorter view
// is padded, a longer one is truncated, because a ticker that does not fit is a
// caller bug and silently overrunning the field would corrupt the next one.
inline void put_alpha(std::byte* out, std::size_t off, std::string_view s,
                      std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
        out[off + i] = static_cast<std::byte>(i < s.size() ? s[i] : ' ');
    }
}

inline void put_stock(std::byte* out, std::size_t off, std::string_view sym) noexcept {
    put_alpha(out, off, sym, kStockLen);
}

// The eleven byte header every message carries. Returns kHeaderLen so a body
// writer can start from the returned offset if it prefers that to a constant.
inline std::size_t write_header(std::byte* out, char type, uint16_t locate,
                                uint16_t tracking, uint64_t timestamp) noexcept {
    put_char(out, kOffType, type);
    be_store<uint16_t>(out + kOffLocate, locate);
    be_store<uint16_t>(out + kOffTracking, tracking);
    be_store_u48(out + kOffTimestamp, timestamp);
    return kHeaderLen;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

// S, system event, twelve bytes.
inline std::size_t write_system_event(std::byte* out, uint16_t locate, uint16_t tracking,
                                      uint64_t timestamp, char event_code) noexcept {
    write_header(out, 'S', locate, tracking, timestamp);
    put_char(out, off::kEventCode, event_code);
    return message_length('S');
}

// R, stock directory, thirty nine bytes. Bundled into a struct because fourteen
// positional arguments of which nine are single characters is a transposition
// waiting to happen, and the defaults are what a plain common stock looks like.
struct StockDirectoryFields {
    std::string_view stock                = "";
    char             market_category      = 'Q';  // NASDAQ global select
    char             financial_status     = 'N';  // no deficiency
    uint32_t         round_lot_size       = 100;
    char             round_lots_only      = 'N';
    char             issue_classification = 'C';  // common stock
    std::string_view issue_sub_type       = "Z";  // two bytes on the wire
    char             authenticity         = 'P';  // live, not a test symbol
    char             short_sale_threshold = 'N';
    char             ipo_flag             = 'N';
    char             luld_tier            = '1';
    char             etp_flag             = 'N';
    uint32_t         etp_leverage         = 0;
    char             inverse_indicator    = 'N';
};

inline std::size_t write_stock_directory(std::byte* out, uint16_t locate, uint16_t tracking,
                                         uint64_t timestamp,
                                         const StockDirectoryFields& f) noexcept {
    write_header(out, 'R', locate, tracking, timestamp);
    put_stock(out, off::kDirStock, f.stock);
    put_char(out, off::kDirMarketCat, f.market_category);
    put_char(out, off::kDirFinStatus, f.financial_status);
    be_store<uint32_t>(out + off::kDirRoundLot, f.round_lot_size);
    put_char(out, off::kDirRoundLotsOnly, f.round_lots_only);
    put_char(out, woff::kDirIssueClass, f.issue_classification);
    put_alpha(out, woff::kDirIssueSubType, f.issue_sub_type, 2);
    put_char(out, woff::kDirAuthenticity, f.authenticity);
    put_char(out, woff::kDirShortSaleThreshold, f.short_sale_threshold);
    put_char(out, woff::kDirIpoFlag, f.ipo_flag);
    put_char(out, woff::kDirLuldTier, f.luld_tier);
    put_char(out, woff::kDirEtpFlag, f.etp_flag);
    be_store<uint32_t>(out + woff::kDirEtpLeverage, f.etp_leverage);
    put_char(out, woff::kDirInverse, f.inverse_indicator);
    return message_length('R');
}

// H, stock trading action, twenty five bytes. The reserved byte is written as a
// space because that is what the feed carries, and a test that asserts on the
// surrounding fields would otherwise be reading whatever the buffer held.
inline std::size_t write_trading_action(std::byte* out, uint16_t locate, uint16_t tracking,
                                        uint64_t timestamp, std::string_view stock,
                                        char trading_state,
                                        std::string_view reason = "") noexcept {
    write_header(out, 'H', locate, tracking, timestamp);
    put_stock(out, off::kActionStock, stock);
    put_char(out, off::kActionState, trading_state);
    put_char(out, woff::kActionReserved, ' ');
    put_alpha(out, off::kActionReason, reason, 4);
    return message_length('H');
}

// A, add order, thirty six bytes.
inline std::size_t write_add_order(std::byte* out, uint16_t locate, uint16_t tracking,
                                   uint64_t timestamp, uint64_t order_ref, char side,
                                   uint32_t shares, std::string_view stock,
                                   uint32_t price) noexcept {
    write_header(out, 'A', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kAddRef, order_ref);
    put_char(out, off::kAddSide, side);
    be_store<uint32_t>(out + off::kAddShares, shares);
    put_stock(out, off::kAddStock, stock);
    be_store<uint32_t>(out + off::kAddPrice, price);
    return message_length('A');
}

// F, add order with attribution, forty bytes. Byte for byte an A through the
// first thirty six, with a four byte market participant id after it.
inline std::size_t write_add_order_mpid(std::byte* out, uint16_t locate, uint16_t tracking,
                                        uint64_t timestamp, uint64_t order_ref, char side,
                                        uint32_t shares, std::string_view stock,
                                        uint32_t price, std::string_view mpid) noexcept {
    write_add_order(out, locate, tracking, timestamp, order_ref, side, shares, stock, price);
    put_char(out, kOffType, 'F');
    put_alpha(out, off::kAddMpid, mpid, 4);
    return message_length('F');
}

// E, order executed, thirty one bytes.
inline std::size_t write_order_executed(std::byte* out, uint16_t locate, uint16_t tracking,
                                        uint64_t timestamp, uint64_t order_ref,
                                        uint32_t executed_shares, uint64_t match) noexcept {
    write_header(out, 'E', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kExecRef, order_ref);
    be_store<uint32_t>(out + off::kExecShares, executed_shares);
    be_store<uint64_t>(out + off::kExecMatch, match);
    return message_length('E');
}

// C, order executed with price, thirty six bytes. The printable flag is the
// difference that matters downstream, since a non printable execution does not
// count toward consolidated volume.
inline std::size_t write_order_executed_price(std::byte* out, uint16_t locate,
                                              uint16_t tracking, uint64_t timestamp,
                                              uint64_t order_ref, uint32_t executed_shares,
                                              uint64_t match, bool printable,
                                              uint32_t price) noexcept {
    write_header(out, 'C', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kExecPxRef, order_ref);
    be_store<uint32_t>(out + off::kExecPxShares, executed_shares);
    be_store<uint64_t>(out + off::kExecPxMatch, match);
    put_char(out, off::kExecPxPrintable, printable ? 'Y' : 'N');
    be_store<uint32_t>(out + off::kExecPxPrice, price);
    return message_length('C');
}

// X, order cancel, twenty three bytes.
inline std::size_t write_order_cancel(std::byte* out, uint16_t locate, uint16_t tracking,
                                      uint64_t timestamp, uint64_t order_ref,
                                      uint32_t cancelled_shares) noexcept {
    write_header(out, 'X', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kCancelRef, order_ref);
    be_store<uint32_t>(out + off::kCancelShares, cancelled_shares);
    return message_length('X');
}

// D, order delete, nineteen bytes.
inline std::size_t write_order_delete(std::byte* out, uint16_t locate, uint16_t tracking,
                                      uint64_t timestamp, uint64_t order_ref) noexcept {
    write_header(out, 'D', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kDeleteRef, order_ref);
    return message_length('D');
}

// U, order replace, thirty five bytes. Carries no ticker, so a consumer has to
// have seen the original add to know what was replaced.
inline std::size_t write_order_replace(std::byte* out, uint16_t locate, uint16_t tracking,
                                       uint64_t timestamp, uint64_t original_ref,
                                       uint64_t new_ref, uint32_t shares,
                                       uint32_t price) noexcept {
    write_header(out, 'U', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kReplaceOldRef, original_ref);
    be_store<uint64_t>(out + off::kReplaceNewRef, new_ref);
    be_store<uint32_t>(out + off::kReplaceShares, shares);
    be_store<uint32_t>(out + off::kReplacePrice, price);
    return message_length('U');
}

// P, trade, non cross, forty four bytes. The order reference on a P is not a
// reference to anything in the book, since the order was never displayed.
inline std::size_t write_trade(std::byte* out, uint16_t locate, uint16_t tracking,
                               uint64_t timestamp, uint64_t order_ref, char side,
                               uint32_t shares, std::string_view stock, uint32_t price,
                               uint64_t match) noexcept {
    write_header(out, 'P', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kTradeRef, order_ref);
    put_char(out, off::kTradeSide, side);
    be_store<uint32_t>(out + off::kTradeShares, shares);
    put_stock(out, off::kTradeStock, stock);
    be_store<uint32_t>(out + off::kTradePrice, price);
    be_store<uint64_t>(out + off::kTradeMatch, match);
    return message_length('P');
}

// Q, cross trade, forty bytes. Shares is eight bytes wide here and four
// everywhere else, which is the one width irregularity in the set.
inline std::size_t write_cross_trade(std::byte* out, uint16_t locate, uint16_t tracking,
                                     uint64_t timestamp, uint64_t shares,
                                     std::string_view stock, uint32_t cross_price,
                                     uint64_t match, char cross_type) noexcept {
    write_header(out, 'Q', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kCrossShares, shares);
    put_stock(out, off::kCrossStock, stock);
    be_store<uint32_t>(out + off::kCrossPrice, cross_price);
    be_store<uint64_t>(out + off::kCrossMatch, match);
    put_char(out, off::kCrossType, cross_type);
    return message_length('Q');
}

// B, broken trade, nineteen bytes. Names a match number only, so a consumer has
// to have kept the trade to know what to back out.
inline std::size_t write_broken_trade(std::byte* out, uint16_t locate, uint16_t tracking,
                                      uint64_t timestamp, uint64_t match) noexcept {
    write_header(out, 'B', locate, tracking, timestamp);
    be_store<uint64_t>(out + off::kBrokenMatch, match);
    return message_length('B');
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

// Put the two byte big-endian length in front of a message that is already in
// the buffer at out + kFrameLen. Returns the total framed size.
inline std::size_t frame(std::byte* out, std::size_t body_len) noexcept {
    be_store<uint16_t>(out, static_cast<uint16_t>(body_len));
    return kFrameLen + body_len;
}

// Write one framed message. The callable receives the body pointer and returns
// the body length, so every writer above works through this without an overload
// each and without the encoder having to know the length twice.
//
//   const std::size_t n = write_framed(buf, [&](std::byte* p) {
//       return write_add_order(p, 1, 0, ts, ref, 'B', 100, "AAPL", 1234500);
//   });
template <typename BodyFn>
inline std::size_t write_framed(std::byte* out, BodyFn&& write_body) {
    const std::size_t body_len = write_body(out + kFrameLen);
    return frame(out, body_len);
}

} // namespace tick::itch
