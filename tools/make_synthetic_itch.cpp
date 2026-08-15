// Write a synthetic ITCH 5.0 day to a file.
//
//   make_synthetic_itch --out day.itch --messages 1000000 --symbols 200 --seed 42 [--gzip]
//
// The summary goes to stderr rather than stdout so a script can redirect the
// two apart, and it carries the expected per symbol volumes because those are
// what a replay is checked against. The generator is the oracle, so this is the
// only place those numbers come from.

#include "tick/synthetic_feed.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --out <path> [--messages N] [--symbols N] [--seed N] [--gzip]\n",
                 argv0);
}

// strtoull with the errors the flags actually hit, which is a missing value or
// a word where a number should be.
bool parse_u64(const char* s, uint64_t& out) {
    if (s == nullptr || *s == '\0') return false;
    char*             end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = static_cast<uint64_t>(v);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    tick::SyntheticConfig cfg{};
    std::string           out_path;
    bool                  gzip = false;

    for (int i = 1; i < argc; ++i) {
        const char* a    = argv[i];
        const bool  more = (i + 1) < argc;
        uint64_t    v    = 0;

        if (std::strcmp(a, "--out") == 0 && more) {
            out_path = argv[++i];
        } else if (std::strcmp(a, "--messages") == 0 && more && parse_u64(argv[++i], v)) {
            cfg.messages = v;
        } else if (std::strcmp(a, "--symbols") == 0 && more && parse_u64(argv[++i], v)) {
            cfg.symbols = static_cast<uint32_t>(v);
        } else if (std::strcmp(a, "--seed") == 0 && more && parse_u64(argv[++i], v)) {
            cfg.seed = v;
        } else if (std::strcmp(a, "--gzip") == 0) {
            gzip = true;
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (out_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    try {
        const tick::SyntheticExpectation exp = tick::write_synthetic_file(cfg, out_path, gzip);

        std::fprintf(stderr, "wrote %s (%s)\n", out_path.c_str(), gzip ? "gzip" : "plain");
        std::fprintf(stderr, "seed %llu symbols %u\n",
                     static_cast<unsigned long long>(cfg.seed), cfg.symbols);
        std::fprintf(stderr, "messages %llu body_bytes %llu framed_bytes %llu\n",
                     static_cast<unsigned long long>(exp.messages),
                     static_cast<unsigned long long>(exp.body_bytes),
                     static_cast<unsigned long long>(exp.framed_bytes));
        std::fprintf(stderr, "resting_orders %llu executed_shares %llu nonprintable %llu\n",
                     static_cast<unsigned long long>(exp.resting_orders),
                     static_cast<unsigned long long>(exp.executed_shares_total),
                     static_cast<unsigned long long>(exp.nonprintable_shares));

        std::fprintf(stderr, "by_type");
        for (int t = 0; t < 256; ++t) {
            if (exp.by_type[static_cast<std::size_t>(t)] != 0) {
                std::fprintf(stderr, " %c=%llu", t,
                             static_cast<unsigned long long>(exp.by_type[static_cast<std::size_t>(t)]));
            }
        }
        std::fprintf(stderr, "\n");

        for (std::size_t i = 0; i < exp.symbols.size(); ++i) {
            std::fprintf(stderr, "symbol %zu %s volume %llu resting %u\n", i + 1,
                         exp.symbols[i].c_str(),
                         static_cast<unsigned long long>(exp.executed_shares[i]),
                         exp.resting_by_symbol[i]);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}
