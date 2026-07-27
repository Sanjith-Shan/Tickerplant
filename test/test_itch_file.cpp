#include "tick/itch.hpp"
#include "tick/itch_decoder.hpp"
#include "tick/itch_file.hpp"
#include "tick/synthetic_feed.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// Reading a length prefixed ITCH file back.
//
// The generator is the oracle. It says how many messages it wrote, of which
// types, and what the volumes add up to, and the file reader plus the decoder
// have to arrive at the same numbers from the bytes alone. Nothing here is
// checked against the reader's own output.

using namespace tick;

namespace {

// A file that removes itself, so a failing test does not leave megabytes in the
// temporary directory and a rerun is not affected by a stale file.
class ScopedFile {
public:
    explicit ScopedFile(const std::string& name)
        : path_((std::filesystem::temp_directory_path() / name).string()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ~ScopedFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ScopedFile(const ScopedFile&)            = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

struct ReadBack {
    uint64_t                  framed    = 0;
    uint64_t                  bytes     = 0;
    uint64_t                  truncated = 0;
    DecodeStats               stats{};
};

// Read every message out of the file and decode it. A decode that is not Ok
// fails the read, because a file this project wrote cannot contain a message
// this project cannot decode.
ReadBack read_and_decode(const std::string& path) {
    ItchFile        file(path);
    ZeroCopyDecoder dec;
    NullHandler     handler;

    std::span<const std::byte> msg;
    while (file.next(msg)) {
        const DecodeResult r = dec.decode(msg, handler);
        EXPECT_EQ(r, DecodeResult::Ok);
        if (r != DecodeResult::Ok) break;
    }

    ReadBack out;
    out.framed    = file.framed_messages();
    out.bytes     = file.bytes_read();
    out.truncated = file.truncated_frames();
    out.stats     = dec.stats();
    return out;
}

void expect_matches_oracle(const ReadBack& got, const SyntheticExpectation& exp) {
    EXPECT_EQ(got.framed, exp.messages);
    EXPECT_EQ(got.stats.messages, exp.messages);
    EXPECT_EQ(got.bytes, exp.framed_bytes);
    EXPECT_EQ(got.stats.bytes, exp.body_bytes);
    EXPECT_EQ(got.truncated, 0u);
    EXPECT_EQ(got.stats.unknown, 0u);

    for (int t = 0; t < 256; ++t) {
        const auto i = static_cast<std::size_t>(t);
        EXPECT_EQ(got.stats.by_type[i], exp.by_type[i])
            << "type " << static_cast<char>(t) << " (" << t << ")";
    }
}

// Generate a session into memory, remembering where every message body starts.
// That is what lets the read back be compared message by message rather than
// only by count.
struct Captured {
    std::vector<std::byte>   stream;  // framed, exactly as it goes to disk
    std::vector<std::size_t> body_off;
    std::vector<std::size_t> body_len;
    SyntheticExpectation     exp;
};

Captured capture(const SyntheticConfig& cfg) {
    Captured      out;
    SyntheticFeed feed(cfg);
    out.exp = feed.generate([&](std::span<const std::byte> msg) {
        std::byte len[2];
        be_store<uint16_t>(len, static_cast<uint16_t>(msg.size()));
        out.stream.insert(out.stream.end(), len, len + 2);
        out.body_off.push_back(out.stream.size());
        out.body_len.push_back(msg.size());
        out.stream.insert(out.stream.end(), msg.begin(), msg.end());
    });
    return out;
}

void write_raw(const std::string& path, const std::vector<std::byte>& stream) {
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());
    f.write(reinterpret_cast<const char*>(stream.data()),
            static_cast<std::streamsize>(stream.size()));
    f.close();
    ASSERT_TRUE(f.good());
}

} // namespace

// ---------------------------------------------------------------------------
// The session the generator produces
// ---------------------------------------------------------------------------

TEST(SyntheticFeed, SessionIsWellFormed) {
    SyntheticConfig cfg{};
    cfg.seed     = 7;
    cfg.symbols  = 12;
    cfg.messages = 20000;

    const Captured cap = capture(cfg);
    const auto&    exp = cap.exp;

    // Four system events, one stock directory and one trading action per
    // symbol, and an opening and closing cross per symbol.
    EXPECT_EQ(exp.count('S'), 4u);
    EXPECT_EQ(exp.count('R'), cfg.symbols);
    EXPECT_EQ(exp.count('H'), cfg.symbols);
    EXPECT_EQ(exp.count('Q'), 2u * cfg.symbols);

    // The order activity actually exercises every path the book has.
    EXPECT_GT(exp.count('A'), 0u);
    EXPECT_GT(exp.count('F'), 0u);
    EXPECT_GT(exp.count('E'), 0u);
    EXPECT_GT(exp.count('C'), 0u);
    EXPECT_GT(exp.count('X'), 0u);
    EXPECT_GT(exp.count('D'), 0u);
    EXPECT_GT(exp.count('U'), 0u);
    EXPECT_GT(exp.count('P'), 0u);

    uint64_t sum = 0;
    for (uint64_t n : exp.by_type) sum += n;
    EXPECT_EQ(sum, exp.messages);
    EXPECT_EQ(exp.messages, cfg.messages);

    EXPECT_EQ(exp.symbols.size(), cfg.symbols);
    EXPECT_EQ(exp.executed_shares.size(), cfg.symbols);

    uint64_t per_symbol = 0;
    for (uint64_t v : exp.executed_shares) per_symbol += v;
    EXPECT_EQ(per_symbol, exp.executed_shares_total);
    EXPECT_GT(exp.executed_shares_total, 0u);
}

// The whole reason for a seeded generator. Same seed, same bytes, on any
// machine and on any day.
TEST(SyntheticFeed, IsReproducibleFromTheSeed) {
    SyntheticConfig cfg{};
    cfg.seed     = 99;
    cfg.symbols  = 5;
    cfg.messages = 5000;

    const Captured a = capture(cfg);
    const Captured b = capture(cfg);
    EXPECT_EQ(a.stream, b.stream);
    EXPECT_EQ(a.exp.executed_shares, b.exp.executed_shares);

    SyntheticConfig other = cfg;
    other.seed            = 100;
    const Captured c      = capture(other);
    EXPECT_NE(a.stream, c.stream);
    // A different day is still the same shape, since the mix is unchanged.
    EXPECT_EQ(a.exp.messages, c.exp.messages);
}

// Timestamps only move forward. A sequencer downstream assumes it and a book
// that reorders on time would be silently wrong if this ever broke.
TEST(SyntheticFeed, TimestampsNeverGoBackwards) {
    SyntheticConfig cfg{};
    cfg.seed     = 3;
    cfg.symbols  = 8;
    cfg.messages = 30000;

    SyntheticFeed feed(cfg);
    uint64_t      last  = 0;
    uint64_t      seen  = 0;
    bool          ok    = true;
    feed.generate([&](std::span<const std::byte> msg) {
        const uint64_t ts = itch::timestamp(msg.data());
        if (ts < last) ok = false;
        last = ts;
        ++seen;
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen, cfg.messages);
}

// Every message the generator emits is a message the decoder accepts, at the
// length the table says. This is the encoder and decoder meeting in the middle.
TEST(SyntheticFeed, EveryMessageDecodes) {
    SyntheticConfig cfg{};
    cfg.seed     = 11;
    cfg.symbols  = 6;
    cfg.messages = 10000;

    SyntheticFeed   feed(cfg);
    ZeroCopyDecoder zc;
    CopyingDecoder  cp;
    NullHandler     handler;
    bool            all_ok = true;

    feed.generate([&](std::span<const std::byte> msg) {
        std::size_t n_zc = 0;
        std::size_t n_cp = 0;
        if (zc.decode(msg, handler, &n_zc) != DecodeResult::Ok) all_ok = false;
        if (cp.decode(msg, handler, &n_cp) != DecodeResult::Ok) all_ok = false;
        if (n_zc != msg.size() || n_cp != msg.size()) all_ok = false;
    });

    EXPECT_TRUE(all_ok);
    EXPECT_EQ(zc.stats().messages, cfg.messages);
    EXPECT_EQ(cp.stats().messages, cfg.messages);
    EXPECT_EQ(zc.stats().unknown, 0u);
    EXPECT_EQ(zc.stats().truncated, 0u);
}

// ---------------------------------------------------------------------------
// The file reader
// ---------------------------------------------------------------------------

TEST(ItchFileRead, PlainFileMatchesTheOracle) {
    ScopedFile      out("tickerplant_test_plain.itch");
    SyntheticConfig cfg{};
    cfg.seed     = 42;
    cfg.symbols  = 20;
    cfg.messages = 50000;

    const SyntheticExpectation exp = write_synthetic_file(cfg, out.path(), false);
    ASSERT_TRUE(std::filesystem::exists(out.path()));
    EXPECT_EQ(std::filesystem::file_size(out.path()), exp.framed_bytes);

    expect_matches_oracle(read_and_decode(out.path()), exp);
}

// Same session, gzipped. ItchFile goes through gzopen either way, so this also
// checks that a plain file read through zlib is handled, which is what lets one
// code path serve both.
TEST(ItchFileRead, GzippedFileMatchesTheOracle) {
    ScopedFile      out("tickerplant_test_gzip.itch.gz");
    SyntheticConfig cfg{};
    cfg.seed     = 42;
    cfg.symbols  = 20;
    cfg.messages = 50000;

    const SyntheticExpectation exp = write_synthetic_file(cfg, out.path(), true);
    ASSERT_TRUE(std::filesystem::exists(out.path()));
    // Compressed, so the file on disk is smaller than what comes out of it.
    EXPECT_LT(std::filesystem::file_size(out.path()), exp.framed_bytes);

    expect_matches_oracle(read_and_decode(out.path()), exp);
}

TEST(ItchFileRead, PlainAndGzippedAgreeMessageForMessage) {
    ScopedFile      plain("tickerplant_test_pair.itch");
    ScopedFile      gz("tickerplant_test_pair.itch.gz");
    SyntheticConfig cfg{};
    cfg.seed     = 5;
    cfg.symbols  = 10;
    cfg.messages = 20000;

    const SyntheticExpectation a = write_synthetic_file(cfg, plain.path(), false);
    const SyntheticExpectation b = write_synthetic_file(cfg, gz.path(), true);
    EXPECT_EQ(a.messages, b.messages);

    ItchFile fa(plain.path());
    ItchFile fb(gz.path());

    std::span<const std::byte> ma;
    std::span<const std::byte> mb;
    uint64_t                   n = 0;
    while (fa.next(ma)) {
        ASSERT_TRUE(fb.next(mb));
        ASSERT_EQ(ma.size(), mb.size()) << "message " << n;
        ASSERT_EQ(std::memcmp(ma.data(), mb.data(), ma.size()), 0) << "message " << n;
        ++n;
    }
    EXPECT_FALSE(fb.next(mb));
    EXPECT_EQ(n, a.messages);
}

TEST(ItchFileRead, MissingFileThrows) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "tickerplant_no_such_file.itch").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_THROW(ItchFile{path}, std::runtime_error);
}

// ---------------------------------------------------------------------------
// The refill boundary
// ---------------------------------------------------------------------------

// The buffer is a megabyte and a message lands wherever the previous one ended,
// so messages straddle the refill boundary constantly. This writes enough to
// force several refills, checks that a straddle actually happens rather than
// assuming it, and then compares every message byte for byte against what the
// generator emitted. Nothing lost, nothing duplicated, nothing torn.
TEST(ItchFileRead, MessagesStraddleTheRefillBoundary) {
    ScopedFile      out("tickerplant_test_straddle.itch");
    SyntheticConfig cfg{};
    cfg.seed     = 2024;
    cfg.symbols  = 30;
    cfg.messages = 300000;

    const Captured cap = capture(cfg);
    ASSERT_GT(cap.stream.size(), 3 * ItchFile::kBufferSize)
        << "not enough data to force several refills";

    // A message straddles when its framed bytes span a buffer sized boundary.
    // The reader compacts, so the boundaries drift, but a stream this long with
    // messages this small cannot avoid crossing every multiple of the buffer
    // size.
    std::size_t straddles = 0;
    for (std::size_t i = 0; i < cap.body_off.size(); ++i) {
        const std::size_t start = cap.body_off[i] - 2;  // include the length prefix
        const std::size_t end   = cap.body_off[i] + cap.body_len[i];
        if (start / ItchFile::kBufferSize != (end - 1) / ItchFile::kBufferSize) ++straddles;
    }
    EXPECT_GE(straddles, 3u);

    write_raw(out.path(), cap.stream);

    ItchFile                   file(out.path());
    std::span<const std::byte> msg;
    std::size_t                i = 0;
    while (file.next(msg)) {
        ASSERT_LT(i, cap.body_len.size()) << "reader produced more messages than were written";
        ASSERT_EQ(msg.size(), cap.body_len[i]) << "message " << i;
        ASSERT_EQ(std::memcmp(msg.data(), cap.stream.data() + cap.body_off[i], msg.size()), 0)
            << "message " << i << " came back with different bytes";
        ++i;
    }

    EXPECT_EQ(i, cap.body_len.size());
    EXPECT_EQ(file.framed_messages(), cap.exp.messages);
    EXPECT_EQ(file.bytes_read(), cap.exp.framed_bytes);
    EXPECT_EQ(file.truncated_frames(), 0u);
}

// A file cut off mid message is reported rather than silently returning a short
// message. A truncated capture is a real thing and the reader has to say so.
TEST(ItchFileRead, TruncatedTailIsReported) {
    ScopedFile      out("tickerplant_test_cut.itch");
    SyntheticConfig cfg{};
    cfg.seed     = 17;
    cfg.symbols  = 4;
    cfg.messages = 2000;

    Captured cap = capture(cfg);
    ASSERT_GE(cap.body_len.size(), 2u);

    // Drop the last few bytes of the final message, leaving its length prefix
    // promising more than the file holds.
    cap.stream.resize(cap.stream.size() - 3);
    write_raw(out.path(), cap.stream);

    ItchFile                   file(out.path());
    std::span<const std::byte> msg;
    uint64_t                   n = 0;
    while (file.next(msg)) ++n;

    EXPECT_EQ(n, cap.exp.messages - 1);
    EXPECT_EQ(file.truncated_frames(), 1u);
}

// A zero length frame is the end of session marker some captures carry. Reading
// past it would hand the decoder whatever follows.
TEST(ItchFileRead, ZeroLengthFrameEndsTheStream) {
    ScopedFile      out("tickerplant_test_zero.itch");
    SyntheticConfig cfg{};
    cfg.seed     = 21;
    cfg.symbols  = 3;
    cfg.messages = 1000;

    Captured cap = capture(cfg);
    // Two zero bytes, then a message that must never be reached.
    const std::size_t stop = cap.body_off[10] - 2;
    cap.stream[stop]       = std::byte{0};
    cap.stream[stop + 1]   = std::byte{0};
    write_raw(out.path(), cap.stream);

    ItchFile                   file(out.path());
    std::span<const std::byte> msg;
    uint64_t                   n = 0;
    while (file.next(msg)) ++n;

    EXPECT_EQ(n, 10u);
    EXPECT_EQ(file.framed_messages(), 10u);
}
