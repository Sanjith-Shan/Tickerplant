#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Reading big-endian integers out of a wire buffer that is not aligned.
//
// Two things about ITCH make this file necessary rather than incidental.
//
// The messages are packed with no padding and they are not aligned. A message
// starts wherever the previous one ended, so a uint64 can land on an odd byte.
// Casting a buffer offset to a pointer of the wider type and dereferencing it
// is undefined behaviour. On arm64 it usually works, on some targets it faults,
// and under UBSan it reports every time. The correct move is memcpy into a
// local of the right width, which every optimiser at -O2 and above folds into
// a single unaligned load. There is no cost to being correct here, only to
// being wrong.
//
// Everything on the wire is big-endian and this machine is not. C++23 has
// std::byteswap. This project is C++20, so the compiler builtins are wrapped
// here once and tested against known vectors rather than open-coded at each
// call site.

namespace tick {

[[nodiscard]] constexpr uint16_t bswap16(uint16_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap16(v);
#else
    return static_cast<uint16_t>((v >> 8) | (v << 8));
#endif
}

[[nodiscard]] constexpr uint32_t bswap32(uint32_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
#endif
}

[[nodiscard]] constexpr uint64_t bswap64(uint64_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32) |
           bswap32(static_cast<uint32_t>(v >> 32));
#endif
}

// True when the host already stores integers most significant byte first, in
// which case the swaps below are identity. No supported target is big-endian
// today, but saying so in code beats assuming it.
inline constexpr bool kHostIsBigEndian =
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__;
#else
    false;
#endif

// Load an unsigned integer of width sizeof(T) stored big-endian at p.
// p carries no alignment requirement.
template <typename T>
[[nodiscard]] inline T be_load(const std::byte* p) noexcept {
    static_assert(std::is_unsigned_v<T>, "be_load is for unsigned wire fields");
    T v{};
    std::memcpy(&v, p, sizeof(T));
    if constexpr (kHostIsBigEndian || sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(bswap16(static_cast<uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(bswap32(static_cast<uint32_t>(v)));
    } else {
        static_assert(sizeof(T) == 8, "unsupported width");
        return static_cast<T>(bswap64(static_cast<uint64_t>(v)));
    }
}

// Store an unsigned integer big-endian at p. Used by the publisher and by the
// tests that build byte vectors by hand.
template <typename T>
inline void be_store(std::byte* p, T v) noexcept {
    static_assert(std::is_unsigned_v<T>, "be_store is for unsigned wire fields");
    T out = v;
    if constexpr (!kHostIsBigEndian && sizeof(T) > 1) {
        if constexpr (sizeof(T) == 2) {
            out = static_cast<T>(bswap16(static_cast<uint16_t>(v)));
        } else if constexpr (sizeof(T) == 4) {
            out = static_cast<T>(bswap32(static_cast<uint32_t>(v)));
        } else {
            static_assert(sizeof(T) == 8, "unsupported width");
            out = static_cast<T>(bswap64(static_cast<uint64_t>(v)));
        }
    }
    std::memcpy(p, &out, sizeof(T));
}

// ITCH timestamps are six bytes, nanoseconds since midnight Eastern. There is
// no six-byte integer type, so the value is zero-extended into a uint64. Six
// bytes holds 2.8e14 nanoseconds, which is about 78 hours, so a trading day
// never comes close to overflowing it.
//
// Read as two loads rather than a byte loop. The compiler turns this into a
// 4-byte load, a 2-byte load, and a shift.
[[nodiscard]] inline uint64_t be_load_u48(const std::byte* p) noexcept {
    const uint64_t hi = be_load<uint32_t>(p);        // bytes 0..3
    const uint64_t lo = be_load<uint16_t>(p + 4);    // bytes 4..5
    return (hi << 16) | lo;
}

inline void be_store_u48(std::byte* p, uint64_t v) noexcept {
    be_store<uint32_t>(p, static_cast<uint32_t>(v >> 16));
    be_store<uint16_t>(p + 4, static_cast<uint16_t>(v & 0xFFFFu));
}

} // namespace tick
