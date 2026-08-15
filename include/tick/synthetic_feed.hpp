#pragma once

#include "tick/itch.hpp"
#include "tick/itch_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

// A deterministic ITCH 5.0 trading day, generated rather than downloaded.
//
// The NASDAQ sample file is four gigabytes compressed. CI cannot fetch it, a
// laptop on a plane does not have it, and a test that depends on it stops being
// a test the day NASDAQ reorganises its FTP server. A day that reproduces bit
// for bit from a seed is worth more than a sample that might drift, because
// when it fails it fails the same way on every machine.
//
// The generator is also the oracle. It knows what it emitted, so it publishes
// the expected end state, and the book builder is checked against that rather
// than against its own output. That only works if the stream is internally
// consistent, so the invariant below is absolute. An execution, a cancel, a
// delete or a replace is only ever emitted against an order reference that was
// added earlier and has not since been removed. A live order set enforces it.
//
// On the random number generator. std::mt19937_64 is specified by the standard
// down to the output sequence, so the same seed gives the same bytes on libc++,
// libstdc++ and MSVC. std::default_random_engine is implementation defined and
// would make "seed 42" mean a different day on a different box, which defeats
// the point. For the same reason the distributions here are plain modulo
// arithmetic rather than std::uniform_int_distribution, whose output is also
// implementation defined even when the engine is not. The bias from modulo is
// irrelevant when the goal is a plausible book rather than statistics.

namespace tick {

// Relative frequencies for the order activity portion of the day. Roughly the
// shape of a real session, where adds and deletes are most of the traffic and
// actual executions are a small fraction of it.
struct SyntheticMix {
    uint32_t add           = 34;
    uint32_t add_mpid      = 4;
    uint32_t execute       = 10;
    uint32_t execute_price = 3;
    uint32_t cancel        = 9;
    uint32_t del           = 30;
    uint32_t replace       = 8;
    uint32_t trade         = 2;

    [[nodiscard]] uint32_t total() const noexcept {
        return add + add_mpid + execute + execute_price + cancel + del + replace + trade;
    }
};

struct SyntheticConfig {
    uint64_t     seed     = 42;
    uint32_t     symbols  = 50;
    uint64_t     messages = 100000;  // target for the whole session, preamble included
    SyntheticMix mix{};
};

// What the generator promises the stream adds up to. A consumer that replays
// the file and disagrees with any of this has a bug, and so might the generator,
// which is why the two decoders are also checked against each other.
struct SyntheticExpectation {
    // Index is stock locate minus one. Locates start at one because zero is the
    // locate NASDAQ uses for messages that are not about a single symbol.
    std::vector<std::string> symbols;
    std::vector<uint64_t>    executed_shares;    // printable E, C, P and Q, per symbol
    std::vector<uint32_t>    resting_by_symbol;  // orders still live at the end

    std::array<uint64_t, 256> by_type{};

    uint64_t messages              = 0;
    uint64_t body_bytes            = 0;  // message bytes, framing excluded
    uint64_t framed_bytes          = 0;  // what a file of this session weighs
    uint64_t resting_orders        = 0;
    uint64_t executed_shares_total = 0;
    uint64_t nonprintable_shares   = 0;  // C with the printable flag clear
    uint64_t last_timestamp        = 0;

    [[nodiscard]] uint64_t count(char type) const noexcept {
        return by_type[static_cast<uint8_t>(type)];
    }
};

class SyntheticFeed {
public:
    explicit SyntheticFeed(const SyntheticConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
        if (cfg_.symbols == 0) throw std::invalid_argument("synthetic feed needs a symbol");
        if (cfg_.mix.total() == 0) throw std::invalid_argument("synthetic mix is all zeroes");
        build_symbols();
    }

    // Emit the whole session. The sink is called once per message with a span
    // over an internal buffer that is reused, so a sink that wants to keep the
    // bytes has to copy them. There is no framing here because the file writer
    // and the MoldUDP64 publisher frame differently.
    //
    // One shot. The expectation counts what this call emitted, so a second call
    // on the same object would double count.
    template <typename Sink>
    const SyntheticExpectation& generate(Sink&& sink) {
        session_open(sink);

        const uint64_t overhead = 4 + 4ull * cfg_.symbols;
        const uint64_t activity = cfg_.messages > overhead ? cfg_.messages - overhead : 0;
        for (uint64_t i = 0; i < activity; ++i) step(sink);

        session_close(sink);

        exp_.resting_orders = live_.size();
        for (const LiveOrder& o : live_) ++exp_.resting_by_symbol[o.locate - 1];
        exp_.last_timestamp = ts_;
        return exp_;
    }

    [[nodiscard]] const SyntheticExpectation& expected() const noexcept { return exp_; }
    [[nodiscard]] const SyntheticConfig&      config() const noexcept { return cfg_; }

private:
    struct LiveOrder {
        uint64_t ref;
        uint32_t shares;  // remaining, always a multiple of one hundred
        uint32_t price;
        uint16_t locate;
        char     side;
    };

    // --- randomness -------------------------------------------------------

    uint64_t next() noexcept { return rng_(); }
    uint64_t below(uint64_t n) noexcept { return n == 0 ? 0 : next() % n; }

    // --- symbols ----------------------------------------------------------

    // Names of three to five letters so the eight byte padded field is
    // exercised at more than one length, which is where a trailing space bug
    // hides. Every index maps to a distinct name.
    void build_symbols() {
        exp_.symbols.reserve(cfg_.symbols);
        exp_.executed_shares.assign(cfg_.symbols, 0);
        exp_.resting_by_symbol.assign(cfg_.symbols, 0);
        mid_.reserve(cfg_.symbols);

        for (uint32_t i = 0; i < cfg_.symbols; ++i) {
            const std::size_t len = 3 + (i % 3);
            std::string       name(len, 'A');
            uint32_t          v = i;
            for (std::size_t k = len; k > 0; --k) {
                name[k - 1] = static_cast<char>('A' + (v % 26));
                v /= 26;
            }
            exp_.symbols.push_back(std::move(name));

            // Opening prices spread over roughly ten to two hundred dollars, on
            // a penny grid, which is what the price field's ten-thousandths
            // means in practice.
            mid_.push_back(static_cast<uint32_t>(100000 + below(1900) * 1000));
        }
    }

    [[nodiscard]] uint16_t pick_locate() noexcept {
        return static_cast<uint16_t>(below(cfg_.symbols) + 1);
    }

    [[nodiscard]] const std::string& symbol_of(uint16_t locate) const noexcept {
        return exp_.symbols[locate - 1];
    }

    // A penny walk, clamped so the price stays inside a sane band rather than
    // wandering to zero over a million messages.
    uint32_t walk(uint16_t locate) noexcept {
        uint32_t&    m    = mid_[locate - 1];
        const uint64_t r  = below(4);
        if (r == 0 && m > 20000) m -= 100;
        if (r == 1 && m < 5000000) m += 100;
        return m;
    }

    // --- emission ---------------------------------------------------------

    template <typename Sink>
    void emit(Sink& sink, std::size_t len) {
        ++exp_.messages;
        exp_.body_bytes += len;
        exp_.framed_bytes += len + itch::kFrameLen;
        ++exp_.by_type[static_cast<uint8_t>(buf_[itch::kOffType])];
        sink(std::span<const std::byte>(buf_.data(), len));
    }

    // Timestamps only ever move forward. A consumer is entitled to assume that
    // and a sequencer downstream will assert on it.
    uint64_t tick_clock() noexcept {
        ts_ += below(2000);
        return ts_;
    }

    void at_least(uint64_t t) noexcept {
        if (ts_ < t) ts_ = t;
    }

    template <typename Sink>
    void session_open(Sink& sink) {
        at_least(kPreMarketNs);
        emit(sink, itch::write_system_event(buf_.data(), 0, 0, tick_clock(),
                                            static_cast<char>(itch::EventCode::StartOfMessages)));

        for (uint32_t i = 0; i < cfg_.symbols; ++i) {
            itch::StockDirectoryFields f{};
            f.stock = exp_.symbols[i];
            emit(sink, itch::write_stock_directory(buf_.data(),
                                                   static_cast<uint16_t>(i + 1), 0,
                                                   tick_clock(), f));
        }

        at_least(kMarketOpenNs);
        emit(sink, itch::write_system_event(buf_.data(), 0, 0, tick_clock(),
                                            static_cast<char>(itch::EventCode::StartOfMarketHours)));

        // Every symbol goes to trading, which is the H a real day opens with.
        for (uint32_t i = 0; i < cfg_.symbols; ++i) {
            emit(sink, itch::write_trading_action(buf_.data(), static_cast<uint16_t>(i + 1),
                                                  0, tick_clock(), exp_.symbols[i], 'T'));
        }

        for (uint32_t i = 0; i < cfg_.symbols; ++i) cross(sink, static_cast<uint16_t>(i + 1), 'O');
    }

    template <typename Sink>
    void session_close(Sink& sink) {
        for (uint32_t i = 0; i < cfg_.symbols; ++i) cross(sink, static_cast<uint16_t>(i + 1), 'C');

        at_least(kMarketCloseNs);
        emit(sink, itch::write_system_event(buf_.data(), 0, 0, tick_clock(),
                                            static_cast<char>(itch::EventCode::EndOfMarketHours)));
        emit(sink, itch::write_system_event(buf_.data(), 0, 0, tick_clock(),
                                            static_cast<char>(itch::EventCode::EndOfMessages)));
    }

    // Opening and closing crosses. A cross prints volume without touching the
    // continuous book, so it counts toward the symbol's shares and changes no
    // order.
    template <typename Sink>
    void cross(Sink& sink, uint16_t locate, char cross_type) {
        const uint64_t shares = 100ull * (1 + below(500));
        const uint32_t price  = mid_[locate - 1];
        exp_.executed_shares[locate - 1] += shares;
        exp_.executed_shares_total += shares;
        emit(sink, itch::write_cross_trade(buf_.data(), locate, 0, tick_clock(), shares,
                                           symbol_of(locate), price, next_match_++, cross_type));
    }

    // --- order activity ---------------------------------------------------

    enum class Action { Add, AddMpid, Execute, ExecutePrice, Cancel, Delete, Replace, Trade };

    Action pick_action() noexcept {
        const SyntheticMix& m = cfg_.mix;
        uint64_t            r = below(m.total());
        if (r < m.add) return Action::Add;
        r -= m.add;
        if (r < m.add_mpid) return Action::AddMpid;
        r -= m.add_mpid;
        if (r < m.execute) return Action::Execute;
        r -= m.execute;
        if (r < m.execute_price) return Action::ExecutePrice;
        r -= m.execute_price;
        if (r < m.cancel) return Action::Cancel;
        r -= m.cancel;
        if (r < m.del) return Action::Delete;
        r -= m.del;
        if (r < m.replace) return Action::Replace;
        return Action::Trade;
    }

    template <typename Sink>
    void step(Sink& sink) {
        Action a = pick_action();
        // Everything but an add and a trade needs an order to act on, and the
        // set is empty at the open and can empty out again later.
        const bool needs_order = a != Action::Add && a != Action::AddMpid && a != Action::Trade;
        if (needs_order && live_.empty()) a = Action::Add;

        switch (a) {
        case Action::Add:          do_add(sink, false); break;
        case Action::AddMpid:      do_add(sink, true); break;
        case Action::Execute:      do_execute(sink, false); break;
        case Action::ExecutePrice: do_execute(sink, true); break;
        case Action::Cancel:       do_cancel(sink); break;
        case Action::Delete:       do_delete(sink); break;
        case Action::Replace:      do_replace(sink); break;
        case Action::Trade:        do_trade(sink); break;
        }
    }

    template <typename Sink>
    void do_add(Sink& sink, bool with_mpid) {
        const uint16_t locate = pick_locate();
        const uint32_t mid    = walk(locate);
        const char     side   = (next() & 1) ? 'B' : 'S';
        const uint32_t shares = static_cast<uint32_t>(100 * (1 + below(20)));
        const uint32_t depth  = static_cast<uint32_t>(100 * (1 + below(10)));
        const uint32_t price  = side == 'B' ? mid - depth : mid + depth;
        const uint64_t ref    = next_ref_++;

        const std::size_t n =
            with_mpid ? itch::write_add_order_mpid(buf_.data(), locate, 0, tick_clock(), ref,
                                                   side, shares, symbol_of(locate), price, "NSDQ")
                      : itch::write_add_order(buf_.data(), locate, 0, tick_clock(), ref, side,
                                              shares, symbol_of(locate), price);
        emit(sink, n);
        live_.push_back(LiveOrder{ref, shares, price, locate, side});
    }

    std::size_t pick_live() noexcept { return static_cast<std::size_t>(below(live_.size())); }

    // Swap with the back rather than erase from the middle. The set is only a
    // pool to draw from, so its order carries no meaning and this keeps removal
    // constant time on a set that can hold a hundred thousand orders.
    void drop_live(std::size_t i) noexcept {
        live_[i] = live_.back();
        live_.pop_back();
    }

    template <typename Sink>
    void do_execute(Sink& sink, bool with_price) {
        const std::size_t i    = pick_live();
        LiveOrder&        o    = live_[i];
        const uint32_t    lots = o.shares / 100;
        const uint32_t    done = static_cast<uint32_t>(100 * (1 + below(lots)));
        const uint64_t    match = next_match_++;

        // A non printable execution moves shares out of the book but does not
        // print to the tape, so the volume oracle leaves it out. Getting this
        // wrong is a classic reconciliation bug, so the generator models it.
        const bool printable = with_price ? (below(10) != 0) : true;

        std::size_t n = 0;
        if (with_price) {
            n = itch::write_order_executed_price(buf_.data(), o.locate, 0, tick_clock(), o.ref,
                                                 done, match, printable, o.price);
        } else {
            n = itch::write_order_executed(buf_.data(), o.locate, 0, tick_clock(), o.ref, done,
                                           match);
        }
        emit(sink, n);

        if (printable) {
            exp_.executed_shares[o.locate - 1] += done;
            exp_.executed_shares_total += done;
        } else {
            exp_.nonprintable_shares += done;
        }

        // A fully executed order leaves the book with no further message, which
        // is what the real feed does and what the book builder has to handle.
        o.shares -= done;
        if (o.shares == 0) drop_live(i);
    }

    template <typename Sink>
    void do_cancel(Sink& sink) {
        const std::size_t i    = pick_live();
        LiveOrder&        o    = live_[i];
        const uint32_t    lots = o.shares / 100;
        if (lots < 2) {
            // X always leaves a remainder on the real feed. A cancel of the
            // whole order comes across as a D, so emit that instead of an X
            // that would take the order to zero.
            do_delete_at(sink, i);
            return;
        }
        const uint32_t gone = static_cast<uint32_t>(100 * (1 + below(lots - 1)));
        emit(sink, itch::write_order_cancel(buf_.data(), o.locate, 0, tick_clock(), o.ref, gone));
        o.shares -= gone;
    }

    template <typename Sink>
    void do_delete(Sink& sink) {
        do_delete_at(sink, pick_live());
    }

    template <typename Sink>
    void do_delete_at(Sink& sink, std::size_t i) {
        const LiveOrder o = live_[i];
        emit(sink, itch::write_order_delete(buf_.data(), o.locate, 0, tick_clock(), o.ref));
        drop_live(i);
    }

    template <typename Sink>
    void do_replace(Sink& sink) {
        const std::size_t i   = pick_live();
        const LiveOrder   old = live_[i];

        const uint32_t mid    = walk(old.locate);
        const uint32_t shares = static_cast<uint32_t>(100 * (1 + below(20)));
        const uint32_t depth  = static_cast<uint32_t>(100 * (1 + below(10)));
        const uint32_t price  = old.side == 'B' ? mid - depth : mid + depth;
        const uint64_t ref    = next_ref_++;

        emit(sink, itch::write_order_replace(buf_.data(), old.locate, 0, tick_clock(), old.ref,
                                             ref, shares, price));

        // A replace cancels the original and adds a new reference in one
        // message. The old reference is dead the moment this is sent, so it has
        // to leave the pool or a later E would reference a gone order.
        drop_live(i);
        live_.push_back(LiveOrder{ref, shares, price, old.locate, old.side});
    }

    template <typename Sink>
    void do_trade(Sink& sink) {
        const uint16_t locate = pick_locate();
        const uint32_t price  = walk(locate);
        const uint32_t shares = static_cast<uint32_t>(100 * (1 + below(10)));
        // A P reports a match against a non displayed order, so its reference
        // is consumed and never enters the live set.
        const uint64_t ref = next_ref_++;
        emit(sink, itch::write_trade(buf_.data(), locate, 0, tick_clock(), ref, 'B', shares,
                                     symbol_of(locate), price, next_match_++));
        exp_.executed_shares[locate - 1] += shares;
        exp_.executed_shares_total += shares;
    }

    // Nanoseconds since midnight Eastern for the session boundaries.
    static constexpr uint64_t kPreMarketNs   = 4ull * 3600 * 1000000000ull;
    static constexpr uint64_t kMarketOpenNs  = 34200ull * 1000000000ull;  // 09:30
    static constexpr uint64_t kMarketCloseNs = 57600ull * 1000000000ull;  // 16:00

    SyntheticConfig                              cfg_;
    std::mt19937_64                              rng_;
    SyntheticExpectation                         exp_{};
    std::vector<uint32_t>                        mid_;
    std::vector<LiveOrder>                       live_;
    std::array<std::byte, itch::kMaxMessageLen>  buf_{};
    uint64_t                                     ts_         = 0;
    uint64_t                                     next_ref_   = 1;
    uint64_t                                     next_match_ = 1;
};

// ---------------------------------------------------------------------------
// Writing a session to a file
// ---------------------------------------------------------------------------

// The length prefixed stream ItchFile reads back, gzipped or not. Batched into
// one buffer because a write syscall or a deflate call per nineteen byte
// message would dominate the time and tell nobody anything.
inline SyntheticExpectation write_synthetic_file(const SyntheticConfig& cfg,
                                                 const std::string& path, bool gzip) {
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::byte> out;
    out.reserve(kChunk + itch::kFrameLen + itch::kMaxMessageLen);

    gzFile      gz = nullptr;
    std::FILE*  fp = nullptr;
    if (gzip) {
        gz = gzopen(path.c_str(), "wb6");
        if (gz == nullptr) throw std::runtime_error("cannot create " + path);
    } else {
        fp = std::fopen(path.c_str(), "wb");
        if (fp == nullptr) throw std::runtime_error("cannot create " + path);
    }

    auto flush = [&](bool force) {
        if (!force && out.size() < kChunk) return;
        if (out.empty()) return;
        if (gz != nullptr) {
            const int n = gzwrite(gz, out.data(), static_cast<unsigned>(out.size()));
            if (n <= 0) throw std::runtime_error("gzwrite failed on " + path);
        } else {
            if (std::fwrite(out.data(), 1, out.size(), fp) != out.size()) {
                throw std::runtime_error("fwrite failed on " + path);
            }
        }
        out.clear();
    };

    SyntheticFeed feed(cfg);
    try {
        feed.generate([&](std::span<const std::byte> msg) {
            std::byte len[itch::kFrameLen];
            be_store<uint16_t>(len, static_cast<uint16_t>(msg.size()));
            out.insert(out.end(), len, len + itch::kFrameLen);
            out.insert(out.end(), msg.begin(), msg.end());
            flush(false);
        });
        flush(true);
    } catch (...) {
        if (gz != nullptr) gzclose(gz);
        if (fp != nullptr) std::fclose(fp);
        throw;
    }

    if (gz != nullptr) gzclose(gz);
    if (fp != nullptr) std::fclose(fp);
    return feed.expected();
}

} // namespace tick
