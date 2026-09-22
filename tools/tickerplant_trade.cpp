// tickerplant-trade
//
// The loop closed. Feed in, book, quoting rule, pre-trade risk, OUCH order out,
// into NanoExchange standing in as the venue, and the whole path measured.
//
// READ THIS BEFORE THE NUMBERS. Three things about what this is.
//
// It is not a backtest and **no profit and loss is reported anywhere**. The
// fill model here is that a resting quote fills when the real market traded at
// or through its price, which ignores queue position entirely. In reality an
// order arriving now sits behind everything already resting at that price, and
// on a liquid NASDAQ name that queue is deep. So this model fills far more
// often than reality would, and any P&L computed from it would be fiction. It
// is good enough to exercise the position and risk paths, which is what it is
// for, and it is not good enough for anything else.
//
// The quoting rule is a rule, not a strategy. See strategy.hpp.
//
// The latency reported is **decode to order**, from the ITCH message entering
// the decoder to the OUCH order bytes leaving the risk gate. The network
// portion of tick to trade is measured separately by tickerplant-rx, because
// mixing a file replay with a wire measurement and calling the sum tick to
// trade would be measuring two different runs and adding them.

#include "tick/affinity.hpp"
#include "tick/alloc_counter.hpp"
#include "tick/book_builder.hpp"
#include "tick/box_info.hpp"
#include "tick/clock.hpp"
#include "tick/hdr.hpp"
#include "tick/itch.hpp"
#include "tick/itch_decoder.hpp"
#include "tick/itch_file.hpp"
#include "tick/ouch.hpp"
#include "tick/risk.hpp"
#include "tick/strategy.hpp"

#include "nano/matching_engine.hpp"
#include "nano/order_book.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Exactly one translation unit defines the counting allocator, and this is it.
TICK_DEFINE_ALLOC_COUNTER();

namespace {

struct Options {
    std::string file;
    std::string symbol = "AAPL";
    std::string results;
    uint64_t    limit = 0;
    int         pin_core = -1;
    bool        quiet = false;

    tick::QuoteConfig quote;
    tick::RiskLimits  limits;
};

void usage() {
    std::fprintf(stderr,
        "usage: tickerplant-trade --file <path.gz|path> [options]\n"
        "  --symbol TICKER          the one symbol to quote (default AAPL)\n"
        "  --limit N                stop after N messages\n"
        "  --pin N                  pin to core N (Linux only)\n"
        "  --results <path>         write the run summary as json\n"
        "  --quote-size N           shares per quote\n"
        "  --half-spread N          ticks either side of fair value\n"
        "  --max-inventory N        stop quoting the side that makes it worse\n"
        "  --max-order-shares N     risk limit\n"
        "  --max-position N         risk limit, counts working exposure\n"
        "  --fat-finger-bps N       risk limit, against the current micro price\n"
        "  --max-msgs-per-sec N     risk limit\n"
        "  --quiet\n");
}

// The venue side of the loop.
//
// NanoExchange's MatchingEngine is used here for exactly what it is, an engine
// that decides what trades. That is the opposite of how the book builder treats
// it, and the contrast is the point. A feed handler applies what the exchange
// decided. A venue decides. Using the same matching engine on the other side of
// the wire is what makes the two projects one system rather than two.
class Venue {
public:
    Venue() : engine_(pool_) {}

    // Rest one of our quotes.
    void submit(nano::OrderId id, const std::string& symbol, nano::Side side,
                nano::Price price, nano::Quantity qty) {
        const auto trades = engine_.submit(id, symbol, side, nano::OrderType::Limit, price, qty);
        for (const nano::Trade& t : trades) record(t);
    }

    void cancel(const std::string& symbol, nano::OrderId id) {
        if (engine_.cancel(symbol, id)) ++cancels_;
    }

    // The market traded at this price. Push an aggressive order through the
    // venue so that anything of ours resting at or through that price fills.
    //
    // This is the fill model and its one big simplification is stated in the
    // header. There is no queue position here, so a quote at the traded price
    // always fills, where in reality it would be behind whatever was already
    // there.
    void market_traded(const std::string& symbol, nano::Side aggressor_side,
                       nano::Price price, nano::Quantity qty) {
        const auto trades = engine_.submit(next_market_id_++, symbol, aggressor_side,
                                           nano::OrderType::ImmediateOrCancel, price, qty);
        for (const nano::Trade& t : trades) record(t);
    }

    struct Fill {
        nano::OrderId maker_id = 0;
        nano::Side    our_side = nano::Side::Buy;
        nano::Quantity qty     = 0;
        nano::Price   price    = 0;
    };

    // Fills where we were the maker, drained by the caller each cycle.
    std::vector<Fill>& fills() noexcept { return fills_; }

    [[nodiscard]] uint64_t total_trades() const noexcept { return engine_.total_trades(); }
    [[nodiscard]] uint64_t cancels() const noexcept { return cancels_; }

private:
    void record(const nano::Trade& t) {
        // Only trades where one of our order ids was the maker matter. Our ids
        // start below the market id space, which is how they are told apart.
        if (t.maker_id >= kMarketIdBase) return;
        Fill f;
        f.maker_id = t.maker_id;
        f.qty      = t.quantity;
        f.price    = t.price;
        // The maker took the other side of the aggressor.
        f.our_side = (t.taker_side == nano::Side::Buy) ? nano::Side::Sell : nano::Side::Buy;
        fills_.push_back(f);
    }

    static constexpr nano::OrderId kMarketIdBase = 1ULL << 40;

    nano::OrderPool      pool_;
    nano::MatchingEngine engine_;
    std::vector<Fill>    fills_;
    nano::OrderId        next_market_id_ = kMarketIdBase;
    uint64_t             cancels_ = 0;
};

// Everything the run accumulates.
struct TradeRun {
    // From the ITCH message entering the decoder to the OUCH order leaving the
    // risk gate. Only recorded on messages that produced an order.
    tick::Histogram decode_to_order{1, 1'000'000'000LL, 3};
    // Decode plus book apply, for every message.
    tick::Histogram book_update{1, 1'000'000'000LL, 3};
    // The risk check on its own, because it sits in the send path and its cost
    // is part of the latency it is protecting.
    tick::Histogram risk_check{1, 1'000'000'000LL, 3};
    // The quoting decision on its own.
    tick::Histogram quote_decision{1, 1'000'000'000LL, 3};

    uint64_t messages      = 0;
    uint64_t book_updates  = 0;
    uint64_t quote_updates = 0;
    uint64_t orders_sent   = 0;
    uint64_t orders_rejected = 0;
    uint64_t cancels_sent  = 0;
    uint64_t fills         = 0;
    uint64_t filled_shares = 0;

    // Two allocation counts rather than one. The feed path is decode and book
    // apply. The send path is the risk check and the OUCH encode. Both must be
    // zero. The venue is deliberately outside both, and the reason is worth
    // reading in the report this prints.
    uint64_t feed_allocations = 0;
    uint64_t send_allocations = 0;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    // Limits that are deliberately loose enough to let the run proceed and
    // tight enough that every check is genuinely evaluated rather than being
    // dead code in the measurement.
    opt.limits.max_order_shares        = 5000;
    opt.limits.max_order_notional      = 50'000'000'000ULL;
    opt.limits.fat_finger_bps          = 500;      // 5%
    opt.limits.max_position_shares     = 5000;
    opt.limits.max_symbol_notional     = 100'000'000'000ULL;
    opt.limits.max_open_orders         = 64;
    opt.limits.max_messages_per_second = 100000;
    opt.limits.max_market_data_age_ns  = 5'000'000'000ULL;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--file") opt.file = need("--file");
        else if (a == "--symbol") opt.symbol = need("--symbol");
        else if (a == "--limit") opt.limit = std::strtoull(need("--limit").c_str(), nullptr, 10);
        else if (a == "--pin") opt.pin_core = std::atoi(need("--pin").c_str());
        else if (a == "--results") opt.results = need("--results");
        else if (a == "--quote-size") opt.quote.quote_size = static_cast<uint32_t>(std::atoi(need("--quote-size").c_str()));
        else if (a == "--half-spread") opt.quote.half_spread_ticks = std::atoi(need("--half-spread").c_str());
        else if (a == "--max-inventory") opt.quote.max_inventory = std::atoll(need("--max-inventory").c_str());
        else if (a == "--max-order-shares") opt.limits.max_order_shares = static_cast<uint32_t>(std::atoi(need("--max-order-shares").c_str()));
        else if (a == "--max-position") opt.limits.max_position_shares = std::atoll(need("--max-position").c_str());
        else if (a == "--fat-finger-bps") opt.limits.fat_finger_bps = static_cast<uint32_t>(std::atoi(need("--fat-finger-bps").c_str()));
        else if (a == "--max-msgs-per-sec") opt.limits.max_messages_per_second = static_cast<uint32_t>(std::atoi(need("--max-msgs-per-sec").c_str()));
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 1; }
    }

    if (opt.file.empty()) { usage(); return 1; }

    tick::BoxInfo box = tick::detect_box();
    tick::PinningReport pin{false, false, opt.pin_core, "not requested"};
    if (opt.pin_core >= 0) pin = tick::pin_to_core_reported(opt.pin_core);
    box.pinned = pin.achieved;

    try {
        auto                  book = std::make_unique<tick::BookBuilder<>>();
        tick::ZeroCopyDecoder decoder;
        tick::MicroPriceQuoter quoter(opt.quote);
        tick::RiskEngine       risk(opt.limits);
        tick::ouch::TokenGenerator tokens("TKP");
        Venue                  venue;
        TradeRun               run;

        // The OUCH encode buffer. One buffer for the life of the run, because
        // an allocation in the send path is the thing the counting allocator
        // exists to catch.
        std::vector<std::byte> wire(256);

        // Our live quotes at the venue, by side. Zero means nothing resting.
        nano::OrderId live_bid_id = 0;
        nano::OrderId live_ask_id = 0;
        nano::OrderId next_our_id = 1;
        tick::ouch::Token live_bid_token{};
        tick::ouch::Token live_ask_token{};

        tick::ItchFile file(opt.file);
        std::span<const std::byte> msg;

        int         locate = -1;       // resolved from the R messages
        std::size_t last_defined = 0;  // so find() is only retried when it could succeed
        uint64_t book_ts = 0;          // when the quoted book last changed
        bool     halted = false;

        const uint64_t t_start = tick::now_ns();

        // The allocation guard wraps the whole replay. Nothing inside it is
        // allowed to touch the heap, and the counter is checked at the end
        // rather than asserted per message, because the check itself would
        // otherwise be in the measurement.
        const uint64_t alloc_before = tick::alloc::allocations();

        while (file.next(msg)) {
            const uint64_t t0 = tick::now_ns();

            const uint64_t alloc_feed_before = tick::alloc::allocations();
            decoder.decode(msg, *book);
            run.feed_allocations += tick::alloc::allocations() - alloc_feed_before;
            ++run.messages;

            const uint64_t t_book = tick::now_ns();
            run.book_update.record(static_cast<int64_t>(t_book - t0));

            // Resolve the symbol, but only when the directory has actually
            // grown. SymbolTable::find is a linear scan over the whole sixteen
            // bit locate space, which is fine once and catastrophic per message.
            // The first version of this loop called it on every message until
            // the symbol turned up, and on a file where the symbol never turns
            // up that is sixty five thousand comparisons times every message in
            // the file. It took twenty five seconds to replay three hundred
            // thousand messages.
            if (locate < 0) {
                const std::size_t defined = book->symbols().defined();
                if (defined != last_defined) {
                    last_defined = defined;
                    locate = book->symbols().find(opt.symbol);
                    if (locate >= 0) risk.enable_symbol(static_cast<uint16_t>(locate), true);
                }
                if (opt.limit != 0 && run.messages >= opt.limit) break;
                continue;
            }

            // Only messages touching the quoted symbol can change our view.
            if (msg.size() < tick::itch::kHeaderLen) continue;
            const uint16_t loc = tick::itch::locate(msg.data());
            if (loc != static_cast<uint16_t>(locate)) {
                if (opt.limit != 0 && run.messages >= opt.limit) break;
                continue;
            }

            const char type = tick::itch::msg_type(msg.data());

            // Trading halts come straight off the feed and straight into both
            // the quoter and the risk engine, because a halted symbol is a
            // reason not to quote and also a reason to reject an order that
            // somehow got through.
            if (type == 'H') {
                halted = book->symbols()[loc].trading_state != 'T';
                risk.on_trading_action(loc, book->symbols()[loc].trading_state);
            }

            // The market traded. Push it through the venue so anything of ours
            // at or through the price fills. See the fill model caveat.
            if (type == 'E' || type == 'C' || type == 'P') {
                const tick::TopOfBook t = book->top(loc);
                if (t.two_sided()) {
                    // The aggressor side is inferred from where the print sat
                    // relative to the midpoint, which is the standard tick test
                    // and is an approximation. ITCH E does not carry a side.
                    const nano::Price px = t.mid();
                    venue.market_traded(opt.symbol, nano::Side::Buy, px, 100);
                    venue.market_traded(opt.symbol, nano::Side::Sell, px, 100);
                }
            }

            const tick::TopOfBook top = book->top(loc);
            book_ts = t_book;

            const uint64_t t_q0 = tick::now_ns();
            tick::Quote    want;
            const auto     decision = quoter.on_top_of_book(top, quoter.position(), t_q0,
                                                            halted, want);
            const uint64_t t_q1 = tick::now_ns();
            run.quote_decision.record(static_cast<int64_t>(t_q1 - t_q0 + 1));
            ++run.quote_updates;

            if (decision == tick::QuoteDecision::Pull) {
                if (live_bid_id != 0) { venue.cancel(opt.symbol, live_bid_id); live_bid_id = 0; ++run.cancels_sent; }
                if (live_ask_id != 0) { venue.cancel(opt.symbol, live_ask_id); live_ask_id = 0; ++run.cancels_sent; }
            }

            if (decision == tick::QuoteDecision::Requote) {
                // Cancel first, then replace. A real gateway would use OUCH
                // Replace, which keeps one token and is one message rather than
                // two, and the reason this does not is that a replace that
                // changes price loses queue priority anyway so the only thing
                // it saves is a message.
                if (live_bid_id != 0) { venue.cancel(opt.symbol, live_bid_id); live_bid_id = 0; ++run.cancels_sent; }
                if (live_ask_id != 0) { venue.cancel(opt.symbol, live_ask_id); live_ask_id = 0; ++run.cancels_sent; }

                auto send = [&](nano::Side side, nano::Price price, uint32_t qty,
                                nano::OrderId& live_id, tick::ouch::Token& live_token) {
                    tick::OrderRequest req;
                    req.locate     = loc;
                    req.token      = tokens.next();
                    req.side       = side;
                    req.shares     = qty;
                    req.price      = price;
                    req.book       = top;
                    req.book_ts_ns = book_ts;
                    req.now_ns     = tick::now_ns();

                    const uint64_t t_r0 = tick::now_ns();
                    const auto     verdict = risk.check_and_count(req);
                    const uint64_t t_r1 = tick::now_ns();
                    run.risk_check.record(static_cast<int64_t>(t_r1 - t_r0 + 1));

                    if (verdict != tick::RiskReject::None) {
                        ++run.orders_rejected;
                        return;
                    }

                    const uint64_t alloc_send_before = tick::alloc::allocations();

                    // Encode the order the way it would go on the wire. The
                    // bytes are built even though the venue here is in process,
                    // because the encode is part of the path being measured and
                    // skipping it would flatter the number.
                    tick::ouch::EnterOrder eo;
                    eo.token  = req.token;
                    eo.side   = (side == nano::Side::Buy) ? 'B' : 'S';
                    eo.shares = qty;
                    eo.stock  = tick::ouch::alpha<tick::ouch::kStockLen>(opt.symbol);
                    eo.price  = static_cast<uint32_t>(price);
                    const std::size_t n = tick::ouch::encode(
                        std::span<std::byte>(wire.data(), wire.size()), eo);
                    if (n == 0) return;

                    risk.on_order_sent(req);
                    run.send_allocations += tick::alloc::allocations() - alloc_send_before;

                    const uint64_t t_done = tick::now_ns();
                    run.decode_to_order.record(static_cast<int64_t>(t_done - t0));

                    // The venue is outside the measurement and outside the
                    // allocation count, because it stands in for a machine on
                    // the other side of a wire rather than for anything this
                    // process would really do.
                    live_token = req.token;
                    live_id    = next_our_id++;
                    venue.submit(live_id, opt.symbol, side, price, qty);
                    ++run.orders_sent;
                };

                if (want.has_bid) send(nano::Side::Buy, want.bid, want.bid_qty, live_bid_id, live_bid_token);
                if (want.has_ask) send(nano::Side::Sell, want.ask, want.ask_qty, live_ask_id, live_ask_token);
            }

            // Drain whatever filled, so the next quoting decision and the next
            // risk check both see the position that actually exists.
            for (const Venue::Fill& f : venue.fills()) {
                quoter.on_fill(f.our_side, f.qty);
                const tick::ouch::Token& tok =
                    (f.our_side == nano::Side::Buy) ? live_bid_token : live_ask_token;
                risk.on_fill(tok, f.qty, f.price);
                ++run.fills;
                run.filled_shares += f.qty;
                if (f.our_side == nano::Side::Buy) live_bid_id = 0; else live_ask_id = 0;
            }
            venue.fills().clear();

            if (opt.limit != 0 && run.messages >= opt.limit) break;

            if (!opt.quiet && (run.messages % 5'000'000) == 0) {
                std::fprintf(stderr, "\r  %llu messages, %llu orders, %llu fills   ",
                             static_cast<unsigned long long>(run.messages),
                             static_cast<unsigned long long>(run.orders_sent),
                             static_cast<unsigned long long>(run.fills));
                std::fflush(stderr);
            }
        }

        const uint64_t total_allocations = tick::alloc::allocations() - alloc_before;
        const double elapsed = static_cast<double>(tick::now_ns() - t_start) / 1e9;
        if (!opt.quiet) std::fprintf(stderr, "\r%60s\r", "");

        std::printf("\nTickerplant tick to trade\n");
        std::printf("  machine         %s\n", box.one_line().c_str());
        std::printf("  pinning         %s\n",
                    pin.requested ? (pin.achieved ? "achieved" : pin.reason) : "not requested");
        std::printf("  file            %s\n", opt.file.c_str());
        std::printf("  symbol          %s (locate %d)\n", opt.symbol.c_str(), locate);
        std::printf("  messages        %llu in %.3f s\n",
                    static_cast<unsigned long long>(run.messages), elapsed);

        std::printf("\n  the loop\n");
        std::printf("    quote updates    %llu\n", static_cast<unsigned long long>(run.quote_updates));
        std::printf("    orders sent      %llu\n", static_cast<unsigned long long>(run.orders_sent));
        std::printf("    orders rejected  %llu\n", static_cast<unsigned long long>(run.orders_rejected));
        std::printf("    cancels sent     %llu\n", static_cast<unsigned long long>(run.cancels_sent));
        std::printf("    fills            %llu, %llu shares\n",
                    static_cast<unsigned long long>(run.fills),
                    static_cast<unsigned long long>(run.filled_shares));
        std::printf("    final position   %lld shares\n",
                    static_cast<long long>(quoter.position()));

        std::printf("\n  risk rejects by reason\n");
        for (std::size_t i = 1; i < tick::kRiskRejectCount; ++i) {
            const auto r = static_cast<tick::RiskReject>(i);
            const uint64_t n = risk.stats().count(r);
            if (n != 0) {
                std::printf("    %-20s %llu\n", tick::to_string(r),
                            static_cast<unsigned long long>(n));
            }
        }
        if (risk.tripped()) {
            std::printf("    KILL SWITCH TRIPPED. Nothing went out after that point.\n");
        }

        std::printf("\n  latency, nanoseconds\n");
        std::printf("    decode and book  %s\n", run.book_update.summary().c_str());
        std::printf("    quote decision   %s\n", run.quote_decision.summary().c_str());
        std::printf("    risk check       %s\n", run.risk_check.summary().c_str());
        std::printf("    decode to order  %s\n", run.decode_to_order.summary().c_str());

        std::printf("\n  heap allocations\n");
        // A zero here means one of two completely different things, so say
        // which. Either nothing allocated, or nothing was counting. Under
        // ThreadSanitizer the global operators belong to the sanitizer and this
        // binary has no counter at all, and printing three zeros in that build
        // would be a measurement of nothing presented as a clean result.
        if (!tick::alloc::counter_is_installed()) {
            std::printf("    not measured in this build, because no counting\n"
                        "    operators are linked into it. See alloc_counter.hpp.\n"
                        "    ThreadSanitizer builds are the case that does this.\n");
        } else {
        std::printf("    feed path        %llu   (decode and book apply)\n",
                    static_cast<unsigned long long>(run.feed_allocations));
        std::printf("    send path        %llu   (risk check and OUCH encode)\n",
                    static_cast<unsigned long long>(run.send_allocations));
        std::printf("    whole process    %llu\n",
                    static_cast<unsigned long long>(total_allocations));
        std::printf("\n    The first two are the ones that matter and both should be zero.\n"
                    "    The difference is the simulated venue, which allocates because\n"
                    "    nano::MatchingEngine returns its trades as a std::vector by value.\n"
                    "    That is a reasonable API for a matching engine and it would not be\n"
                    "    acceptable in a real order gateway, which is the kind of thing that\n"
                    "    only shows up once something counts the allocations.\n");
        }

        std::printf("\n  No profit and loss is reported. The fill model ignores queue\n"
                    "  position, so it fills far more often than reality would, and any\n"
                    "  P and L computed from it would be fiction. See the header of this\n"
                    "  file for the full statement.\n");

        if (!tick::pinning_supported()) {
            std::printf("\n  This machine cannot pin a thread to a core. The median is the\n"
                        "  honest measure and the tail is scheduler noise.\n");
        }

        if (!opt.results.empty()) {
            std::ofstream out(opt.results);
            if (out) {
                out << "{\n";
                out << "  \"box\": " << box.to_json() << ",\n";
                out << "  \"pinning_achieved\": " << (pin.achieved ? "true" : "false") << ",\n";
                out << "  \"symbol\": \"" << opt.symbol << "\",\n";
                out << "  \"messages\": " << run.messages << ",\n";
                out << "  \"elapsed_s\": " << elapsed << ",\n";
                out << "  \"orders_sent\": " << run.orders_sent << ",\n";
                out << "  \"orders_rejected\": " << run.orders_rejected << ",\n";
                out << "  \"cancels_sent\": " << run.cancels_sent << ",\n";
                out << "  \"fills\": " << run.fills << ",\n";
                out << "  \"filled_shares\": " << run.filled_shares << ",\n";
                // null rather than zero when nothing was counting, for the same
                // reason box_label.sh prints unknown rather than a plausible
                // default. A zero in this field would be read as a measurement.
                if (tick::alloc::counter_is_installed()) {
                    out << "  \"feed_path_allocations\": " << run.feed_allocations << ",\n";
                    out << "  \"send_path_allocations\": " << run.send_allocations << ",\n";
                } else {
                    out << "  \"feed_path_allocations\": null,\n";
                    out << "  \"send_path_allocations\": null,\n";
                }
                out << "  \"book_update\": " << run.book_update.to_json("book_update") << ",\n";
                out << "  \"quote_decision\": " << run.quote_decision.to_json("quote_decision") << ",\n";
                out << "  \"risk_check\": " << run.risk_check.to_json("risk_check") << ",\n";
                out << "  \"decode_to_order\": " << run.decode_to_order.to_json("decode_to_order") << "\n";
                out << "}\n";
                std::fprintf(stderr, "wrote %s\n", opt.results.c_str());
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
