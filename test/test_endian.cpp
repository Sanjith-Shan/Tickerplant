#include "tick/endian.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Byte order is the kind of thing that looks obviously right and is wrong in
// one direction only. Every assertion below is against a literal byte array
// with a value worked out by hand, never against another call to the same
// code, because a load and a store that share a bug agree with each other.

namespace {

using tick::be_load;
using tick::be_load_u48;
using tick::be_store;
using tick::be_store_u48;

// Literal bytes, spelled out, so a reader can check the expected value against
// the array without running anything.
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    out.reserve(v.size());
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

} // namespace

TEST(Endian, LoadKnownVectors) {
    const auto u16 = bytes({0x12, 0x34});
    EXPECT_EQ(be_load<uint16_t>(u16.data()), 0x1234u);

    const auto u32 = bytes({0xDE, 0xAD, 0xBE, 0xEF});
    EXPECT_EQ(be_load<uint32_t>(u32.data()), 0xDEADBEEFu);

    const auto u64 = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(be_load<uint64_t>(u64.data()), 0x0102030405060708ull);

    // Every byte distinct and the most significant one large, so a swap that
    // works on a palindrome or a small value does not slip through.
    const auto asc = bytes({0x00, 0x01});
    EXPECT_EQ(be_load<uint16_t>(asc.data()), 1u);

    const auto max32 = bytes({0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(be_load<uint32_t>(max32.data()), 0xFFFFFFFFu);
}

TEST(Endian, StoreKnownVectors) {
    std::array<std::byte, 8> out{};

    be_store<uint16_t>(out.data(), 0x1234u);
    EXPECT_EQ(out[0], std::byte{0x12});
    EXPECT_EQ(out[1], std::byte{0x34});

    be_store<uint32_t>(out.data(), 0xDEADBEEFu);
    EXPECT_EQ(out[0], std::byte{0xDE});
    EXPECT_EQ(out[1], std::byte{0xAD});
    EXPECT_EQ(out[2], std::byte{0xBE});
    EXPECT_EQ(out[3], std::byte{0xEF});

    be_store<uint64_t>(out.data(), 0x0102030405060708ull);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(out[static_cast<std::size_t>(i)], std::byte{static_cast<unsigned char>(i + 1)})
            << "byte " << i;
    }
}

// The six byte timestamp is the field most likely to be shifted wrong, because
// there is no native type for it and the obvious implementation is a loop.
TEST(Endian, Timestamp48KnownVectors) {
    EXPECT_EQ(be_load_u48(bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00}).data()), 0ull);
    EXPECT_EQ(be_load_u48(bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x01}).data()), 1ull);
    EXPECT_EQ(be_load_u48(bytes({0x00, 0x00, 0x00, 0x00, 0x01, 0x00}).data()), 256ull);

    // 09:30:00 exactly, in nanoseconds since midnight. A real value from a real
    // trading day rather than a pattern.
    EXPECT_EQ(be_load_u48(bytes({0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00}).data()),
              34200000000000ull);

    // High bits set in the top byte of the field. A shift of 8 instead of 16,
    // or a load of four bytes instead of six, cannot produce this number.
    EXPECT_EQ(be_load_u48(bytes({0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6}).data()),
              0xA1B2C3D4E5F6ull);

    // The field is six bytes wide and nothing above that may leak in.
    EXPECT_EQ(be_load_u48(bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}).data()),
              281474976710655ull);
}

TEST(Endian, Timestamp48StoreKnownVectors) {
    std::array<std::byte, 6> out{};

    be_store_u48(out.data(), 34200000000000ull);
    EXPECT_EQ(out[0], std::byte{0x1F});
    EXPECT_EQ(out[1], std::byte{0x1A});
    EXPECT_EQ(out[2], std::byte{0xCE});
    EXPECT_EQ(out[3], std::byte{0xD9});
    EXPECT_EQ(out[4], std::byte{0xF0});
    EXPECT_EQ(out[5], std::byte{0x00});

    be_store_u48(out.data(), 0xA1B2C3D4E5F6ull);
    EXPECT_EQ(out[0], std::byte{0xA1});
    EXPECT_EQ(out[5], std::byte{0xF6});
}

TEST(Endian, RoundTrip) {
    std::array<std::byte, 8> buf{};

    for (uint16_t v : {uint16_t{0}, uint16_t{1}, uint16_t{0x00FF}, uint16_t{0xFF00},
                       uint16_t{0xFFFF}, uint16_t{12345}}) {
        be_store<uint16_t>(buf.data(), v);
        EXPECT_EQ(be_load<uint16_t>(buf.data()), v);
    }

    for (uint32_t v : {0u, 1u, 0x0000FFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 1234500u}) {
        be_store<uint32_t>(buf.data(), v);
        EXPECT_EQ(be_load<uint32_t>(buf.data()), v);
    }

    for (uint64_t v : {0ull, 1ull, 0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull,
                       0xFFFFFFFFFFFFFFFFull, 0x0102030405060708ull}) {
        be_store<uint64_t>(buf.data(), v);
        EXPECT_EQ(be_load<uint64_t>(buf.data()), v);
    }

    for (uint64_t v : {0ull, 1ull, 34200000000000ull, 0xA1B2C3D4E5F6ull, 281474976710655ull}) {
        be_store_u48(buf.data(), v);
        EXPECT_EQ(be_load_u48(buf.data()), v);
    }
}

// A uint64 field in an ITCH message lands on whatever byte the previous message
// ended on, so the loads have to work at any address. Offsetting inside a
// larger array is what makes the pointer genuinely unaligned rather than
// accidentally aligned, and under UBSan a cast based implementation reports
// here while the memcpy one stays quiet.
TEST(Endian, UnalignedAccess) {
    alignas(8) std::array<std::byte, 32> arena{};

    for (std::size_t off = 0; off < 8; ++off) {
        std::byte* p = arena.data() + off + 1;

        be_store<uint16_t>(p, 0xBEEFu);
        EXPECT_EQ(be_load<uint16_t>(p), 0xBEEFu) << "offset " << off + 1;

        be_store<uint32_t>(p, 0xDEADBEEFu);
        EXPECT_EQ(be_load<uint32_t>(p), 0xDEADBEEFu) << "offset " << off + 1;

        be_store<uint64_t>(p, 0x0102030405060708ull);
        EXPECT_EQ(be_load<uint64_t>(p), 0x0102030405060708ull) << "offset " << off + 1;

        be_store_u48(p, 0xA1B2C3D4E5F6ull);
        EXPECT_EQ(be_load_u48(p), 0xA1B2C3D4E5F6ull) << "offset " << off + 1;
    }
}

// The same known vector read from an odd address. The value cannot depend on
// where the bytes happen to sit.
TEST(Endian, UnalignedKnownVector) {
    std::array<std::byte, 16> arena{};
    std::byte*                p = arena.data() + 3;

    const unsigned char wire[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (std::size_t i = 0; i < 8; ++i) p[i] = static_cast<std::byte>(wire[i]);

    EXPECT_EQ(be_load<uint16_t>(p), 0x0102u);
    EXPECT_EQ(be_load<uint32_t>(p), 0x01020304u);
    EXPECT_EQ(be_load<uint64_t>(p), 0x0102030405060708ull);
    EXPECT_EQ(be_load_u48(p), 0x010203040506ull);
}

TEST(Endian, ByteSwapPrimitives) {
    EXPECT_EQ(tick::bswap16(0x1234u), 0x3412u);
    EXPECT_EQ(tick::bswap32(0xDEADBEEFu), 0xEFBEADDEu);
    EXPECT_EQ(tick::bswap64(0x0102030405060708ull), 0x0807060504030201ull);

    // constexpr, so a wrong answer is a compile error rather than a test
    // failure. Worth having because the decoder's length table is constexpr too.
    static_assert(tick::bswap16(0x00FFu) == 0xFF00u);
    static_assert(tick::bswap32(0x000000FFu) == 0xFF000000u);
    static_assert(tick::bswap64(0xFFull) == 0xFF00000000000000ull);
}
