// tickerplant-replay
//
// Read a NASDAQ ITCH 5.0 file, decode it, rebuild the book, and report. No
// network anywhere in this tool, which is the point of it. Milestones one and
// two of the project are exactly this binary, and everything the wire path does
// later has to agree with what this produces from the same data.
//
// It is also where the three correctness oracles are run from.
//
//   --decoder both     decode the same file twice, once with the zero-copy
//                      decoder and once with the copying decoder, and assert
//                      the two books and the two volume totals are identical
//   --digest           print the book fingerprint, which is what a second run
//                      is compared against to prove replay is deterministic
//   --volume-csv       per symbol executed share volume, which is what gets
//                      reconciled against NASDAQ's published daily totals

#include "tick/book_builder.hpp"
#include "tick/box_info.hpp"
#include "tick/itch_decoder.hpp"
#include "tick/itch_file.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string file;
    std::string decoder    = "zerocopy"; // zerocopy, copying, both
    std::string side       = "map";      // map, vector
    std::string volume_csv;
    std::string json;
    std::string top_symbol;
    uint64_t    limit      = 0; // zero means the whole file
    int         depth      = 5;
    bool        digest     = false;
    bool        quiet      = false;
    std::size_t map_capacity = 1u << 22;
};

void usage() {
    std::fprintf(stderr,
                 "usage: tickerplant-replay --file <path.gz|path> [options]\n"
                 "  --decoder zerocopy|copying|both   which decoder to run (default zerocopy)\n"
                 "  --side map|vector                 price level container (default map)\n"
                 "  --limit N                         stop after N messages\n"
                 "  --top TICKER                      print top of book for one symbol\n"
                 "  --depth N                         levels to print with --top (default 5)\n"
                 "  --digest                          print the book fingerprint\n"
                 "  --volume-csv <path>               write per symbol executed volume\n"
                 "  --json <path>                     write the run summary as json\n"
                 "  --map-capacity N                  order table slots (default 4194304)\n"
                 "  --quiet                           suppress the progress line\n");
}

// Everything one pass over the file produced. Kept separate from the builder so
// two passes can be compared without holding two books.
struct PassResult {
    uint64_t    book_digest   = 0;
    uint64_t    volume_digest = 0;
    uint64_t    messages      = 0;
    uint64_t    bytes         = 0;
    double      seconds       = 0.0;
    tick::BookStats stats{};
    std::vector<std::pair<std::string, uint64_t>> per_symbol;
    std::size_t live_levels = 0;
    std::size_t order_map_capacity = 0;
    double      order_map_load = 0.0;
    uint64_t    order_map_growths = 0;
};

const char* human(double bytes, char* buf, std::size_t n) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int                u       = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        ++u;
    }
    std::snprintf(buf, n, "%.2f %s", bytes, units[u]);
    return buf;
}

template <typename Decoder, template <bool> class Side>
PassResult run_pass(const Options& opt) {
    // The builder holds a multi hundred megabyte order pool, so it lives on the
    // heap. Putting it on the stack is how you find out what the default stack
    // limit is.
    auto builder = std::make_unique<tick::BookBuilder<Side>>(opt.map_capacity);
    Decoder decoder;
    tick::ItchFile file(opt.file);

    const auto t0 = std::chrono::steady_clock::now();
    std::span<const std::byte> msg;
    uint64_t                   n        = 0;
    uint64_t                   next_tick = 5'000'000;

    while (file.next(msg)) {
        decoder.decode(msg, *builder);
        ++n;
        if (opt.limit != 0 && n >= opt.limit) break;
        if (!opt.quiet && n >= next_tick) {
            char b[32];
            const auto  now = std::chrono::steady_clock::now();
            const double el = std::chrono::duration<double>(now - t0).count();
            std::fprintf(stderr, "\r  %llu messages, %s read, %.2f M msg/s   ",
                         static_cast<unsigned long long>(n),
                         human(static_cast<double>(file.bytes_read()), b, sizeof(b)),
                         static_cast<double>(n) / el / 1e6);
            std::fflush(stderr);
            next_tick += 5'000'000;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (!opt.quiet) std::fprintf(stderr, "\r%60s\r", "");

    PassResult r;
    r.seconds       = std::chrono::duration<double>(t1 - t0).count();
    r.messages      = n;
    r.bytes         = file.bytes_read();
    r.book_digest   = builder->digest();
    r.volume_digest = builder->volume_digest();
    r.stats         = builder->stats();
    r.live_levels   = builder->live_levels();
    r.order_map_capacity = builder->order_map().capacity();
    r.order_map_load     = builder->order_map().load_factor();
    r.order_map_growths  = builder->order_map().growth_events();

    for (std::size_t locate = 0; locate < tick::SymbolTable::kCapacity; ++locate) {
        const auto& t = builder->totals(static_cast<uint16_t>(locate));
        if (t.executed_shares == 0 && t.messages == 0) continue;
        const auto& s = builder->symbols()[static_cast<uint16_t>(locate)];
        if (!s.known) continue;
        r.per_symbol.emplace_back(std::string(s.ticker()), t.executed_shares);
    }

    if (!opt.top_symbol.empty()) {
        const int locate = builder->symbols().find(opt.top_symbol);
        if (locate < 0) {
            std::fprintf(stderr, "symbol %s was never defined in this file\n",
                         opt.top_symbol.c_str());
        } else {
            const auto top = builder->top(static_cast<uint16_t>(locate));
            std::printf("\nTop of book for %s, locate %d\n", opt.top_symbol.c_str(), locate);
            std::printf("  bid %10.4f x %8u  (%u orders)\n",
                        static_cast<double>(top.bid_price) / tick::itch::kPriceScale,
                        top.bid_qty, top.bid_orders);
            std::printf("  ask %10.4f x %8u  (%u orders)\n",
                        static_cast<double>(top.ask_price) / tick::itch::kPriceScale,
                        top.ask_qty, top.ask_orders);
            if (top.two_sided()) {
                std::printf("  spread %.4f   mid %.4f   micro %.4f\n",
                            static_cast<double>(top.spread()) / tick::itch::kPriceScale,
                            static_cast<double>(top.mid()) / tick::itch::kPriceScale,
                            static_cast<double>(top.micro_price()) / tick::itch::kPriceScale);
            }

            std::vector<typename tick::BookBuilder<Side>::DepthLevel> lv(
                static_cast<std::size_t>(opt.depth));
            std::printf("\n  %-24s %s\n", "bids", "asks");
            const std::size_t nb = builder->depth(static_cast<uint16_t>(locate), true,
                                                  lv.data(), lv.size());
            std::vector<typename tick::BookBuilder<Side>::DepthLevel> la(
                static_cast<std::size_t>(opt.depth));
            const std::size_t na = builder->depth(static_cast<uint16_t>(locate), false,
                                                  la.data(), la.size());
            for (std::size_t i = 0; i < static_cast<std::size_t>(opt.depth); ++i) {
                char left[32] = "", right[32] = "";
                if (i < nb) {
                    std::snprintf(left, sizeof(left), "%10.4f x %8u",
                                  static_cast<double>(lv[i].price) / tick::itch::kPriceScale,
                                  lv[i].qty);
                }
                if (i < na) {
                    std::snprintf(right, sizeof(right), "%10.4f x %8u",
                                  static_cast<double>(la[i].price) / tick::itch::kPriceScale,
                                  la[i].qty);
                }
                std::printf("  %-24s %s\n", left, right);
            }
        }
    }

    return r;
}

void print_report(const Options& opt, const char* decoder_name, const PassResult& r) {
    char b1[32], b2[32];
    const tick::BookStats& s = r.stats;

    const tick::BoxInfo box = tick::detect_box();

    std::printf("\nTickerplant replay\n");
    std::printf("  machine         %s\n", box.one_line().c_str());
    std::printf("  file            %s\n", opt.file.c_str());
    std::printf("  decoder         %s\n", decoder_name);
    std::printf("  price levels    %s\n", opt.side.c_str());
    std::printf("  messages        %llu\n", static_cast<unsigned long long>(r.messages));
    std::printf("  bytes           %s\n", human(static_cast<double>(r.bytes), b1, sizeof(b1)));
    std::printf("  wall time       %.3f s\n", r.seconds);
    std::printf("  throughput      %.3f M msg/s, %s/s\n",
                static_cast<double>(r.messages) / r.seconds / 1e6,
                human(static_cast<double>(r.bytes) / r.seconds, b2, sizeof(b2)));

    std::printf("\n  message counts\n");
    std::printf("    A/F add          %llu  (%llu with mpid)\n",
                static_cast<unsigned long long>(s.adds),
                static_cast<unsigned long long>(s.adds_mpid));
    std::printf("    E/C execute      %llu  (%llu with price)\n",
                static_cast<unsigned long long>(s.executes),
                static_cast<unsigned long long>(s.executes_with_price));
    std::printf("    X cancel         %llu\n", static_cast<unsigned long long>(s.cancels));
    std::printf("    D delete         %llu\n", static_cast<unsigned long long>(s.deletes));
    std::printf("    U replace        %llu\n", static_cast<unsigned long long>(s.replaces));
    std::printf("    P trade          %llu\n", static_cast<unsigned long long>(s.trades));
    std::printf("    Q cross          %llu\n", static_cast<unsigned long long>(s.cross_trades));
    std::printf("    B broken         %llu\n", static_cast<unsigned long long>(s.broken_trades));
    std::printf("    R directory      %llu\n", static_cast<unsigned long long>(s.directory_entries));
    std::printf("    H trading action %llu\n", static_cast<unsigned long long>(s.trading_actions));
    std::printf("    S system event   %llu\n", static_cast<unsigned long long>(s.system_events));
    std::printf("    other            %llu\n", static_cast<unsigned long long>(s.other));

    std::printf("\n  book state\n");
    std::printf("    resting orders   %llu\n", static_cast<unsigned long long>(s.live_orders));
    std::printf("    peak resting     %llu\n", static_cast<unsigned long long>(s.peak_live_orders));
    std::printf("    live levels      %zu\n", r.live_levels);
    std::printf("    order table      %zu slots, %.1f%% full, %llu growths\n",
                r.order_map_capacity, r.order_map_load * 100.0,
                static_cast<unsigned long long>(r.order_map_growths));

    std::printf("\n  invariants (every one of these should be zero on a clean file)\n");
    std::printf("    duplicate refs   %llu\n", static_cast<unsigned long long>(s.duplicate_refs));
    std::printf("    orphan executes  %llu\n", static_cast<unsigned long long>(s.orphan_executes));
    std::printf("    orphan cancels   %llu\n", static_cast<unsigned long long>(s.orphan_cancels));
    std::printf("    orphan deletes   %llu\n", static_cast<unsigned long long>(s.orphan_deletes));
    std::printf("    orphan replaces  %llu\n", static_cast<unsigned long long>(s.orphan_replaces));
    std::printf("    overfills        %llu\n", static_cast<unsigned long long>(s.overfills));
    std::printf("    pool exhausted   %llu\n", static_cast<unsigned long long>(s.pool_exhausted));

    uint64_t total_shares = 0;
    for (const auto& [sym, v] : r.per_symbol) {
        (void)sym;
        total_shares += v;
    }
    std::printf("\n  volume\n");
    std::printf("    symbols seen     %zu\n", r.per_symbol.size());
    std::printf("    executed shares  %llu\n", static_cast<unsigned long long>(total_shares));

    if (opt.digest) {
        std::printf("\n  book digest      %016llx\n",
                    static_cast<unsigned long long>(r.book_digest));
        std::printf("  volume digest    %016llx\n",
                    static_cast<unsigned long long>(r.volume_digest));
    }
}

void write_volume_csv(const std::string& path, const PassResult& r) {
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    out << "symbol,executed_shares\n";
    for (const auto& [sym, shares] : r.per_symbol) {
        out << sym << ',' << shares << '\n';
    }
    std::fprintf(stderr, "wrote %zu rows to %s\n", r.per_symbol.size(), path.c_str());
}

void write_json(const std::string& path, const Options& opt, const char* decoder_name,
                const PassResult& r) {
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const tick::BookStats& s = r.stats;
    const tick::BoxInfo box = tick::detect_box();
    out << "{\n";
    out << "  \"box\": " << box.to_json() << ",\n";
    out << "  \"file\": \"" << opt.file << "\",\n";
    out << "  \"decoder\": \"" << decoder_name << "\",\n";
    out << "  \"side_container\": \"" << opt.side << "\",\n";
    out << "  \"messages\": " << r.messages << ",\n";
    out << "  \"bytes\": " << r.bytes << ",\n";
    out << "  \"seconds\": " << r.seconds << ",\n";
    out << "  \"msgs_per_sec\": " << (static_cast<double>(r.messages) / r.seconds) << ",\n";
    out << "  \"book_digest\": \"" << std::hex << r.book_digest << std::dec << "\",\n";
    out << "  \"volume_digest\": \"" << std::hex << r.volume_digest << std::dec << "\",\n";
    out << "  \"peak_resting_orders\": " << s.peak_live_orders << ",\n";
    out << "  \"resting_orders\": " << s.live_orders << ",\n";
    out << "  \"live_levels\": " << r.live_levels << ",\n";
    out << "  \"orphan_executes\": " << s.orphan_executes << ",\n";
    out << "  \"orphan_deletes\": " << s.orphan_deletes << ",\n";
    out << "  \"orphan_cancels\": " << s.orphan_cancels << ",\n";
    out << "  \"orphan_replaces\": " << s.orphan_replaces << ",\n";
    out << "  \"duplicate_refs\": " << s.duplicate_refs << ",\n";
    out << "  \"overfills\": " << s.overfills << ",\n";
    out << "  \"pool_exhausted\": " << s.pool_exhausted << ",\n";
    out << "  \"symbols\": " << r.per_symbol.size() << "\n";
    out << "}\n";
    std::fprintf(stderr, "wrote %s\n", path.c_str());
}

template <template <bool> class Side>
int dispatch_decoder(const Options& opt) {
    if (opt.decoder == "zerocopy") {
        PassResult r = run_pass<tick::ZeroCopyDecoder, Side>(opt);
        print_report(opt, "zero-copy", r);
        if (!opt.volume_csv.empty()) write_volume_csv(opt.volume_csv, r);
        if (!opt.json.empty()) write_json(opt.json, opt, "zero-copy", r);
        return 0;
    }
    if (opt.decoder == "copying") {
        PassResult r = run_pass<tick::CopyingDecoder, Side>(opt);
        print_report(opt, "copying", r);
        if (!opt.volume_csv.empty()) write_volume_csv(opt.volume_csv, r);
        if (!opt.json.empty()) write_json(opt.json, opt, "copying", r);
        return 0;
    }
    if (opt.decoder == "both") {
        // The two-decoder oracle. Two independent paths over the same bytes
        // have to produce the same book and the same volume. If they do not,
        // one of them is wrong and the benchmark comparing them is meaningless.
        std::fprintf(stderr, "pass 1 of 2, zero-copy\n");
        PassResult a = run_pass<tick::ZeroCopyDecoder, Side>(opt);
        std::fprintf(stderr, "pass 2 of 2, copying\n");
        PassResult b = run_pass<tick::CopyingDecoder, Side>(opt);

        print_report(opt, "zero-copy", a);
        print_report(opt, "copying", b);

        std::printf("\n  decoder comparison\n");
        std::printf("    zero-copy   %.3f M msg/s\n",
                    static_cast<double>(a.messages) / a.seconds / 1e6);
        std::printf("    copying     %.3f M msg/s\n",
                    static_cast<double>(b.messages) / b.seconds / 1e6);
        std::printf("    ratio       %.3fx\n",
                    (static_cast<double>(a.messages) / a.seconds) /
                        (static_cast<double>(b.messages) / b.seconds));
        std::printf("    book digest    %016llx vs %016llx  %s\n",
                    static_cast<unsigned long long>(a.book_digest),
                    static_cast<unsigned long long>(b.book_digest),
                    a.book_digest == b.book_digest ? "MATCH" : "DIFFER");
        std::printf("    volume digest  %016llx vs %016llx  %s\n",
                    static_cast<unsigned long long>(a.volume_digest),
                    static_cast<unsigned long long>(b.volume_digest),
                    a.volume_digest == b.volume_digest ? "MATCH" : "DIFFER");

        if (!opt.volume_csv.empty()) write_volume_csv(opt.volume_csv, a);
        if (!opt.json.empty()) write_json(opt.json, opt, "zero-copy", a);

        const bool ok = a.book_digest == b.book_digest &&
                        a.volume_digest == b.volume_digest && a.messages == b.messages;
        return ok ? 0 : 2;
    }
    std::fprintf(stderr, "unknown decoder %s\n", opt.decoder.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
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
        else if (a == "--decoder") opt.decoder = need("--decoder");
        else if (a == "--side") opt.side = need("--side");
        else if (a == "--limit") opt.limit = std::strtoull(need("--limit").c_str(), nullptr, 10);
        else if (a == "--top") opt.top_symbol = need("--top");
        else if (a == "--depth") opt.depth = std::atoi(need("--depth").c_str());
        else if (a == "--digest") opt.digest = true;
        else if (a == "--volume-csv") opt.volume_csv = need("--volume-csv");
        else if (a == "--json") opt.json = need("--json");
        else if (a == "--map-capacity")
            opt.map_capacity = std::strtoull(need("--map-capacity").c_str(), nullptr, 10);
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    if (opt.file.empty()) {
        usage();
        return 1;
    }

    try {
        if (opt.side == "map")    return dispatch_decoder<tick::MapSide>(opt);
        if (opt.side == "vector") return dispatch_decoder<tick::VectorSide>(opt);
        std::fprintf(stderr, "unknown side container %s\n", opt.side.c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
