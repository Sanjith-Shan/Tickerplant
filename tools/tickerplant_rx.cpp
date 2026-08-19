// tickerplant-rx
//
// Receive the feed off UDP multicast, arbitrate the two lines, close gaps,
// decode, rebuild the book, and measure the whole path.
//
// The measurement is the part worth reading, because the rest of the pipeline
// is only worth what the numbers describing it are worth.
//
// TWO CLOCKS, TWO LATENCIES. Every message produces up to two samples.
//
//   wire to book      from when the kernel timestamped the datagram to when the
//                     book has applied the message. This is what the machine
//                     did. On Linux with SO_TIMESTAMPING the start point is a
//                     kernel timestamp, so the receive syscall and the wakeup
//                     are inside the measurement rather than outside it. Where
//                     the platform cannot supply one, a userspace timestamp is
//                     taken instead and the results say which was used.
//
//   due to book       from when the publisher's schedule said the message
//                     should have been sent to when the book applied it. This
//                     is the one that is free of coordinated omission. The
//                     schedule comes from the publisher's run manifest rather
//                     than from anything observed, so a stall in the receiver
//                     cannot hide by also stalling the sender.
//
// The difference between those two distributions is the information. If they
// agree, the run kept up. If the second one is far worse in the tail, the
// receiver fell behind and the first distribution was flattering it.
//
// WHAT A RUN ON THIS MACHINE DOES AND DOES NOT PROVE. A loopback multicast run
// on macOS measures the decode and book path and proves the arbitration and
// recovery logic is correct. It does not measure a network interface, a driver,
// or a switch, and macOS cannot pin a thread to a core at all, so its tail is
// scheduler noise. Every table this binary prints carries the machine label and
// says whether pinning was achieved, not whether it was requested.

#include "tick/affinity.hpp"
#include "tick/book_builder.hpp"
#include "tick/box_info.hpp"
#include "tick/clock.hpp"
#include "tick/hdr.hpp"
#include "tick/itch.hpp"
#include "tick/itch_decoder.hpp"
#include "tick/moldudp64.hpp"
#include "tick/receive_strategy.hpp"
#include "tick/recovery.hpp"
#include "tick/sequencer.hpp"
#include "tick/udp_socket.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Options {
    std::string group_a = "239.255.42.10";
    std::string group_b = "239.255.42.11";
    uint16_t    port_a  = 26010;
    uint16_t    port_b  = 26011;
    std::string iface;
    int         lines   = 2;

    std::string strategy = "busypoll";
    std::string manifest = "results/run_manifest.json";
    std::string results;           // where to write the run summary
    std::string percentiles_dir;   // where to write the percentile CSVs

    std::string retransmit_host = "127.0.0.1";
    uint16_t    retransmit_port = 26020;
    bool        recovery = true;

    int      pin_core = -1;
    int      rcvbuf   = 8 * 1024 * 1024;
    uint64_t idle_exit_ms = 3000;  // stop this long after the feed goes quiet
    uint64_t limit = 0;
    bool     quiet = false;
    std::size_t reorder_window = 4096;
    std::string digest_file;
};

void usage() {
    std::fprintf(stderr,
        "usage: tickerplant-rx [options]\n"
        "  --group-a A --port-a N     line A multicast group and port\n"
        "  --group-b A --port-b N     line B multicast group and port\n"
        "  --lines 1|2                receive one line or arbitrate two (default 2)\n"
        "  --strategy NAME            blocking, busypoll, recvmmsg, epoll, kqueue, iouring\n"
        "  --manifest <path>          the publisher's run manifest, for the due times\n"
        "  --results <path>           write the run summary as json\n"
        "  --percentiles <dir>        write percentile csv files for charting\n"
        "  --retransmit-host H --retransmit-port N   the re-request server\n"
        "  --no-recovery              detect gaps but never ask for a retransmission\n"
        "  --pin N                    pin this thread to core N (Linux only)\n"
        "  --rcvbuf N                 socket receive buffer request in bytes\n"
        "  --reorder-window N         messages held while a gap is open\n"
        "  --idle-exit-ms N           stop after this long with no traffic\n"
        "  --limit N                  stop after N messages\n"
        "  --digest <path>            write the final book digest, for run to run comparison\n"
        "  --quiet\n");
}

// The publisher's schedule. Parsed with a small hand written scanner rather
// than a JSON library, because pulling in a dependency to read eight numbers
// out of a file this program wrote itself is not a good trade.
struct Manifest {
    bool        present = false;
    std::string session;
    std::string pace = "max";
    double      ns_per_message = 0.0;
    double      speed = 1.0;
    uint64_t    t0_mono_ns = 0;
    uint64_t    first_itch_ts = 0;
    uint64_t    messages = 0;

    [[nodiscard]] bool has_schedule() const noexcept {
        return present && (pace == "rate" || pace == "natural") && t0_mono_ns != 0;
    }

    // When message `seq` carrying ITCH timestamp `itch_ts` was due to be sent.
    [[nodiscard]] uint64_t due_ns(uint64_t seq, uint64_t itch_ts) const noexcept {
        if (pace == "rate") {
            return t0_mono_ns + static_cast<uint64_t>(static_cast<double>(seq - 1) * ns_per_message);
        }
        const uint64_t d = itch_ts > first_itch_ts ? itch_ts - first_itch_ts : 0;
        return t0_mono_ns + static_cast<uint64_t>(static_cast<double>(d) / speed);
    }
};

Manifest load_manifest(const std::string& path) {
    Manifest m;
    std::ifstream in(path);
    if (!in) return m;

    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    auto number = [&](const char* key, double& out) {
        const std::string k = std::string("\"") + key + "\":";
        const auto        pos = text.find(k);
        if (pos == std::string::npos) return;
        out = std::strtod(text.c_str() + pos + k.size(), nullptr);
    };
    auto string_field = [&](const char* key, std::string& out) {
        const std::string k = std::string("\"") + key + "\": \"";
        const auto        pos = text.find(k);
        if (pos == std::string::npos) return;
        const auto start = pos + k.size();
        const auto end   = text.find('"', start);
        if (end == std::string::npos) return;
        out = text.substr(start, end - start);
    };

    double d = 0;
    string_field("session", m.session);
    string_field("pace", m.pace);
    number("ns_per_message", m.ns_per_message);
    number("speed", m.speed);
    d = 0; number("t0_mono_ns", d); m.t0_mono_ns = static_cast<uint64_t>(d);
    d = 0; number("first_itch_timestamp_ns", d); m.first_itch_ts = static_cast<uint64_t>(d);
    d = 0; number("messages", d); m.messages = static_cast<uint64_t>(d);
    m.present = true;
    return m;
}

// Everything the receiver accumulates. Kept in one struct so the reporting code
// does not reach into half a dozen objects.
struct RunState {
    std::unique_ptr<tick::BookBuilder<>> book;
    tick::ZeroCopyDecoder                decoder;

    tick::Histogram wire_to_book{1, 60'000'000'000LL, 3};
    tick::Histogram due_to_book{1, 60'000'000'000LL, 3};

    uint64_t datagrams       = 0;
    uint64_t bad_packets     = 0;
    uint64_t heartbeats      = 0;
    uint64_t end_of_session  = 0;
    uint64_t messages        = 0;
    uint64_t kernel_stamped  = 0;
    uint64_t retransmit_sent = 0;
    uint64_t retransmit_msgs = 0;

    // Arrival time per sequence, indexed modulo the reorder window. A message
    // that waited in the reorder buffer while a gap was open really did take
    // that long to reach the book, so the sample has to start when it arrived
    // and not when the sequencer let it through.
    std::vector<uint64_t> arrival_ns;
    std::size_t           window_mask = 0;

    void note_arrival(uint64_t seq, uint64_t ns) noexcept {
        arrival_ns[seq & window_mask] = ns;
    }
    [[nodiscard]] uint64_t arrival_of(uint64_t seq) const noexcept {
        return arrival_ns[seq & window_mask];
    }
};

const char* state_name(tick::FeedState s) {
    switch (s) {
    case tick::FeedState::Synced:     return "synced";
    case tick::FeedState::Gap:        return "gap";
    case tick::FeedState::Recovering: return "recovering";
    case tick::FeedState::Stale:      return "STALE";
    }
    return "?";
}

// Run the receive loop with one concrete strategy type. The strategy is a
// template parameter so there is no virtual call between the syscall and the
// book, which matters because the syscall cost is the thing being compared.
// Constructing a strategy for the shape of this run.
//
// The busy-poll strategy spins inside its own receive call, which is exactly
// right when it owns the thread and exactly wrong when two lines share one.
// Spinning a thousand times on an empty line A is a thousand system calls that
// line B waits behind, and on a two line run that alone is enough to fall far
// enough behind that the kernel starts dropping datagrams. With two lines the
// spinning belongs in the outer loop, which alternates, so the strategy is
// built with a budget of one and becomes a plain non blocking drain.
template <typename S>
S make_strategy(tick::UdpSocket& sock, bool two_lines) {
    if constexpr (std::is_same_v<S, tick::BusyPollStrategy>) {
        return S(sock, two_lines ? 1 : 1024);
    } else {
        return S(sock);
    }
}

// Construct in place rather than moving one in. Several strategies own a file
// descriptor of their own, an epoll instance or an io_uring ring, and are not
// movable for good reason, so optional::emplace has to build the object where
// it will live.
template <typename S>
void emplace_strategy(std::optional<S>& slot, tick::UdpSocket& sock, bool two_lines) {
    if constexpr (std::is_same_v<S, tick::BusyPollStrategy>) {
        slot.emplace(sock, two_lines ? 1 : 1024);
    } else {
        slot.emplace(sock);
    }
}

template <typename StrategyA, typename StrategyB>
int run_loop(const Options& opt, Manifest& manifest, RunState& st,
             tick::Sequencer& seq, tick::UdpSocket& sock_a, tick::UdpSocket& sock_b,
             tick::UdpSocket* retransmit_sock, const sockaddr_in& retransmit_dst) {
    const bool two = opt.lines == 2;
    StrategyA  rx_a = make_strategy<StrategyA>(sock_a, two);

    // Only build the second line's strategy when there is a second line. With
    // one line sock_b is a default constructed socket holding no descriptor,
    // and some strategies configure their socket in the constructor, so
    // building one on it fails before the run starts.
    std::optional<StrategyB> rx_b;
    if (two) emplace_strategy<StrategyB>(rx_b, sock_b, two);

    constexpr int          kBatch = 64;
    tick::RxDatagram       batch[kBatch];
    std::vector<std::byte> request(tick::mold::kHeaderLen);
    std::vector<std::byte> retx_buf(tick::mold::kMaxPacketLen);

    // The sink. Everything that gets here is in sequence order, which is the
    // sequencer's job, so this only has to decode and apply.
    auto sink = [&](uint64_t sequence, std::span<const std::byte> payload) {
        const uint64_t arrived = st.arrival_of(sequence);
        const uint64_t itch_ts = payload.size() >= tick::itch::kHeaderLen
                                     ? tick::itch::timestamp(payload.data()) : 0;

        st.decoder.decode(payload, *st.book);
        ++st.messages;

        const uint64_t done = tick::now_ns();
        if (arrived != 0 && done > arrived) {
            st.wire_to_book.record(static_cast<int64_t>(done - arrived));
        }
        if (manifest.has_schedule()) {
            // Coordinated omission, and why there is nothing to correct here.
            //
            // The usual correction back fills the samples a fixed rate load
            // generator would have taken during a stall, because a generator
            // that blocks stops sampling exactly when latency is worst. This
            // receiver is not that. The publisher is a separate process sending
            // UDP and it never waits for anything, so every message that was
            // due produces a sample, and each sample is measured against the
            // time the schedule says the message should have left rather than
            // the time it actually left. The distribution is already complete.
            //
            // Applying hdr_record_corrected_value on top of that would count
            // the same delay many times over. It also costs one loop iteration
            // per expected interval inside the sample, so on a run that has
            // genuinely fallen behind it turns a millisecond sample into a
            // hundred thousand back filled entries, which slows the receiver
            // down, which makes the next sample larger. The first version of
            // this file did exactly that and the feedback loop was clearly
            // visible in the numbers.
            //
            // Histogram::record_corrected stays in the library and is tested,
            // because a harness that does block needs it. It does not belong
            // in this loop.
            const uint64_t due = manifest.due_ns(sequence, itch_ts);
            if (done > due) {
                st.due_to_book.record(static_cast<int64_t>(done - due));
            } else {
                // The message reached the book before it was due, which happens
                // when the publisher is ahead of its own schedule. Zero is the
                // honest floor rather than a negative number.
                st.due_to_book.record(1);
            }
        }
    };

    // One datagram in, zero or more messages out.
    // The manifest is written by the publisher when its run starts, which is
    // after this process has already joined the groups and begun polling. So it
    // is read again on the first datagram. Reading it only at startup picks up
    // whatever the previous run left behind, and measuring against a schedule
    // from a different run produces numbers that are not merely wrong but
    // wrong by minutes.
    bool manifest_reloaded = false;

    auto handle = [&](const tick::RxDatagram& dg, tick::LineId line, uint64_t now) {
        ++st.datagrams;
        if (!manifest_reloaded) {
            manifest_reloaded = true;
            const Manifest fresh = load_manifest(opt.manifest);
            if (fresh.present) manifest = fresh;
        }
        const auto view = tick::mold::PacketView::parse(std::span<const std::byte>(dg.data, dg.len));
        if (!view) {
            ++st.bad_packets;
            return;
        }
        if (view->is_end_of_session()) {
            ++st.end_of_session;
            return;
        }
        if (view->is_heartbeat()) {
            ++st.heartbeats;
            return;
        }
        if (dg.kernel_ts_ns != 0) ++st.kernel_stamped;

        const uint64_t stamp = dg.kernel_ts_ns != 0 ? dg.kernel_ts_ns : now;
        uint64_t       s     = view->sequence();
        for (std::span<const std::byte> m : *view) {
            st.note_arrival(s, stamp);
            seq.on_message(line, s, m, now, sink);
            ++s;
        }
    };

    uint64_t last_traffic = tick::now_ns();

    while (!g_stop.load(std::memory_order_relaxed)) {
        bool got_any = false;

        // Drain each line until it is empty before moving to the other, with a
        // cap so one busy line cannot hold the other off indefinitely. Taking
        // one datagram per outer iteration leaves the second line waiting
        // behind whatever the first strategy does when it finds nothing.
        constexpr int kDrainCap = 4096;

        if constexpr (StrategyA::kAvailable) {
            for (int drained = 0; drained < kDrainCap;) {
                const int n = rx_a.receive(batch, kBatch);
                if (n <= 0) break;
                const uint64_t now = tick::now_ns();
                for (int i = 0; i < n; ++i) handle(batch[i], tick::LineId::A, now);
                drained += n;
                got_any = true;
            }
        }

        if (two && rx_b.has_value()) {
            if constexpr (StrategyB::kAvailable) {
                for (int drained = 0; drained < kDrainCap;) {
                    const int n = rx_b->receive(batch, kBatch);
                    if (n <= 0) break;
                    const uint64_t now = tick::now_ns();
                    for (int i = 0; i < n; ++i) handle(batch[i], tick::LineId::B, now);
                    drained += n;
                    got_any = true;
                }
            }
        }

        const uint64_t now = tick::now_ns();

        // Retransmission replies come back on the same unicast socket the
        // requests went out on.
        if (retransmit_sock != nullptr) {
            for (;;) {
                const ssize_t n = ::recv(retransmit_sock->fd(), retx_buf.data(), retx_buf.size(),
                                         MSG_DONTWAIT);
                if (n <= 0) break;
                const auto view = tick::mold::PacketView::parse(
                    std::span<const std::byte>(retx_buf.data(), static_cast<std::size_t>(n)));
                if (!view) continue;
                uint64_t s = view->sequence();
                for (std::span<const std::byte> m : *view) {
                    st.note_arrival(s, now);
                    seq.on_retransmit(s, m, now, sink);
                    ++st.retransmit_msgs;
                    ++s;
                }
                got_any = true;
            }
        }

        // Drive the gap state machine forward even when nothing arrived, since
        // that is what turns a grace period expiring into a request.
        if (auto req = seq.poll(now, sink)) {
            if (retransmit_sock != nullptr && opt.recovery) {
                const std::size_t len = tick::mold::PacketBuilder::request(
                    request.data(), request.size(),
                    manifest.session.empty() ? "TICKPLANT" : manifest.session,
                    req->first_sequence, req->count);
                ::sendto(retransmit_sock->fd(), request.data(), len, 0,
                         reinterpret_cast<const sockaddr*>(&retransmit_dst), sizeof(retransmit_dst));
                ++st.retransmit_sent;
            }
        }

        if (got_any) {
            last_traffic = now;
        } else if (st.messages > 0 && opt.idle_exit_ms != 0 &&
                   now - last_traffic > opt.idle_exit_ms * 1'000'000ULL) {
            break;
        }

        if (opt.limit != 0 && st.messages >= opt.limit) break;

        if (!opt.quiet && st.datagrams != 0 && (st.datagrams % 200000) == 0) {
            std::fprintf(stderr, "\r  %llu msgs, %llu datagrams, state %s, expected %llu   ",
                         static_cast<unsigned long long>(st.messages),
                         static_cast<unsigned long long>(st.datagrams),
                         state_name(seq.state()),
                         static_cast<unsigned long long>(seq.expected()));
            std::fflush(stderr);
        }
    }
    if (!opt.quiet) std::fprintf(stderr, "\r%78s\r", "");
    return 0;
}

void report(const Options& opt, const Manifest& manifest, const RunState& st,
            const tick::Sequencer& seq, const tick::BoxInfo& box,
            const tick::PinningReport& pin, const char* strategy_name, double elapsed) {
    const auto& ss = seq.stats();
    const auto& bs = st.book->stats();

    std::printf("\nTickerplant receiver\n");
    std::printf("  machine         %s\n", box.one_line().c_str());
    std::printf("  pinning         %s\n",
                pin.requested ? (pin.achieved ? "achieved" : pin.reason) : "not requested");
    std::printf("  strategy        %s\n", strategy_name);
    std::printf("  lines           %d\n", opt.lines);
    std::printf("  timestamps      %s\n",
                st.kernel_stamped > 0 ? "kernel receive timestamps"
                                      : "userspace, the platform gave no kernel timestamp");
    std::printf("  elapsed         %.3f s\n", elapsed);

    std::printf("\n  wire\n");
    std::printf("    datagrams        %llu\n", static_cast<unsigned long long>(st.datagrams));
    std::printf("    malformed        %llu\n", static_cast<unsigned long long>(st.bad_packets));
    std::printf("    heartbeats       %llu\n", static_cast<unsigned long long>(st.heartbeats));
    std::printf("    messages         %llu  (%.3f M msg/s)\n",
                static_cast<unsigned long long>(st.messages),
                static_cast<double>(st.messages) / elapsed / 1e6);

    std::printf("\n  sequencing\n");
    std::printf("    state            %s\n", state_name(seq.state()));
    std::printf("    accepted         %llu\n", static_cast<unsigned long long>(ss.accepted));
    std::printf("    duplicates       %llu\n", static_cast<unsigned long long>(ss.duplicates));
    std::printf("    out of order     %llu  (high water %llu)\n",
                static_cast<unsigned long long>(ss.out_of_order),
                static_cast<unsigned long long>(ss.window_high_water));
    std::printf("    gaps detected    %llu\n", static_cast<unsigned long long>(ss.gaps_detected));
    std::printf("    gaps recovered   %llu\n", static_cast<unsigned long long>(ss.gaps_recovered));
    std::printf("    messages lost    %llu\n", static_cast<unsigned long long>(ss.messages_dropped));
    std::printf("    from recovery    %llu\n", static_cast<unsigned long long>(ss.from_recovery));
    std::printf("    requests sent    %llu\n", static_cast<unsigned long long>(st.retransmit_sent));

    const uint64_t decided = ss.wins_a + ss.wins_b;
    std::printf("\n  A/B arbitration\n");
    std::printf("    first on A       %llu  (%.2f%%)\n",
                static_cast<unsigned long long>(ss.wins_a),
                decided ? 100.0 * static_cast<double>(ss.wins_a) / static_cast<double>(decided) : 0.0);
    std::printf("    first on B       %llu  (%.2f%%)\n",
                static_cast<unsigned long long>(ss.wins_b),
                decided ? 100.0 * static_cast<double>(ss.wins_b) / static_cast<double>(decided) : 0.0);

    std::printf("\n  book\n");
    std::printf("    resting orders   %llu  (peak %llu)\n",
                static_cast<unsigned long long>(bs.live_orders),
                static_cast<unsigned long long>(bs.peak_live_orders));
    std::printf("    orphan events    %llu executes, %llu cancels, %llu deletes, %llu replaces\n",
                static_cast<unsigned long long>(bs.orphan_executes),
                static_cast<unsigned long long>(bs.orphan_cancels),
                static_cast<unsigned long long>(bs.orphan_deletes),
                static_cast<unsigned long long>(bs.orphan_replaces));
    std::printf("    book digest      %016llx\n",
                static_cast<unsigned long long>(st.book->digest()));

    std::printf("\n  latency, nanoseconds\n");
    std::printf("    wire to book     %s\n", st.wire_to_book.summary().c_str());
    if (manifest.has_schedule()) {
        std::printf("    due to book      %s\n", st.due_to_book.summary().c_str());
        std::printf("\n    Due to book is measured against the publisher's schedule, so it\n"
                    "    is already free of coordinated omission. See the comment in the\n"
                    "    sink for why no further correction is applied.\n");
    } else {
        std::printf("    due to book      not measured, the publisher ran unpaced\n");
    }

    if (!tick::pinning_supported()) {
        std::printf("\n  This machine cannot pin a thread to a core, so the median is the\n"
                    "  honest measure here and the tail is operating system scheduling noise.\n"
                    "  The pinned numbers in the README come from the Linux box.\n");
    }
}

void write_results(const Options& opt, const Manifest& manifest, const RunState& st,
                   const tick::Sequencer& seq, const tick::BoxInfo& box,
                   const tick::PinningReport& pin, const char* strategy_name, double elapsed) {
    if (opt.results.empty()) return;
    std::ofstream out(opt.results);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", opt.results.c_str());
        return;
    }
    const auto& ss = seq.stats();
    out << "{\n";
    out << "  \"box\": " << box.to_json() << ",\n";
    out << "  \"pinning_requested\": " << (pin.requested ? "true" : "false") << ",\n";
    out << "  \"pinning_achieved\": " << (pin.achieved ? "true" : "false") << ",\n";
    out << "  \"pinning_note\": \"" << (pin.reason ? pin.reason : "") << "\",\n";
    out << "  \"strategy\": \"" << strategy_name << "\",\n";
    out << "  \"lines\": " << opt.lines << ",\n";
    out << "  \"kernel_timestamps\": " << (st.kernel_stamped > 0 ? "true" : "false") << ",\n";
    out << "  \"elapsed_s\": " << elapsed << ",\n";
    out << "  \"datagrams\": " << st.datagrams << ",\n";
    out << "  \"messages\": " << st.messages << ",\n";
    out << "  \"msgs_per_sec\": " << (static_cast<double>(st.messages) / elapsed) << ",\n";
    out << "  \"duplicates\": " << ss.duplicates << ",\n";
    out << "  \"out_of_order\": " << ss.out_of_order << ",\n";
    out << "  \"gaps_detected\": " << ss.gaps_detected << ",\n";
    out << "  \"gaps_recovered\": " << ss.gaps_recovered << ",\n";
    out << "  \"messages_dropped\": " << ss.messages_dropped << ",\n";
    out << "  \"from_recovery\": " << ss.from_recovery << ",\n";
    out << "  \"wins_a\": " << ss.wins_a << ",\n";
    out << "  \"wins_b\": " << ss.wins_b << ",\n";
    out << "  \"book_digest\": \"" << std::hex << st.book->digest() << std::dec << "\",\n";
    out << "  \"wire_to_book\": " << st.wire_to_book.to_json("wire_to_book") << ",\n";
    if (manifest.has_schedule()) {
        out << "  \"due_to_book\": " << st.due_to_book.to_json("due_to_book") << ",\n";
    }
    out << "  \"final_state\": \"" << state_name(seq.state()) << "\"\n";
    out << "}\n";
    std::fprintf(stderr, "wrote %s\n", opt.results.c_str());
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
        if (a == "--group-a") opt.group_a = need("--group-a");
        else if (a == "--group-b") opt.group_b = need("--group-b");
        else if (a == "--port-a") opt.port_a = static_cast<uint16_t>(std::atoi(need("--port-a").c_str()));
        else if (a == "--port-b") opt.port_b = static_cast<uint16_t>(std::atoi(need("--port-b").c_str()));
        else if (a == "--iface") opt.iface = need("--iface");
        else if (a == "--lines") opt.lines = std::atoi(need("--lines").c_str());
        else if (a == "--strategy") opt.strategy = need("--strategy");
        else if (a == "--manifest") opt.manifest = need("--manifest");
        else if (a == "--results") opt.results = need("--results");
        else if (a == "--percentiles") opt.percentiles_dir = need("--percentiles");
        else if (a == "--retransmit-host") opt.retransmit_host = need("--retransmit-host");
        else if (a == "--retransmit-port") opt.retransmit_port = static_cast<uint16_t>(std::atoi(need("--retransmit-port").c_str()));
        else if (a == "--no-recovery") opt.recovery = false;
        else if (a == "--pin") opt.pin_core = std::atoi(need("--pin").c_str());
        else if (a == "--rcvbuf") opt.rcvbuf = std::atoi(need("--rcvbuf").c_str());
        else if (a == "--reorder-window") opt.reorder_window = std::strtoull(need("--reorder-window").c_str(), nullptr, 10);
        else if (a == "--idle-exit-ms") opt.idle_exit_ms = std::strtoull(need("--idle-exit-ms").c_str(), nullptr, 10);
        else if (a == "--limit") opt.limit = std::strtoull(need("--limit").c_str(), nullptr, 10);
        else if (a == "--digest") opt.digest_file = need("--digest");
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 1; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Round the reorder window up to a power of two so the arrival table can be
    // indexed with a mask rather than a division.
    std::size_t window = 1;
    while (window < opt.reorder_window) window <<= 1;

    tick::BoxInfo box = tick::detect_box();
    tick::PinningReport pin{false, false, opt.pin_core, "not requested"};
    if (opt.pin_core >= 0) {
        pin = tick::pin_to_core_reported(opt.pin_core);
        if (!pin.achieved) {
            std::fprintf(stderr, "could not pin to core %d: %s\n", opt.pin_core, pin.reason);
        }
    }
    // The machine line has to agree with the pinning line. Reporting "pinning
    // achieved" above a label that still says "not pinned" is the kind of
    // internal contradiction that makes a reader stop believing the rest.
    box.pinned = pin.achieved;

    try {
        tick::UdpSocket sock_a = tick::UdpSocket::multicast_receiver(opt.group_a, opt.port_a, opt.iface);
        sock_a.set_rcvbuf(opt.rcvbuf);
        sock_a.enable_rx_timestamping();

        tick::UdpSocket sock_b;
        if (opt.lines == 2) {
            sock_b = tick::UdpSocket::multicast_receiver(opt.group_b, opt.port_b, opt.iface);
            sock_b.set_rcvbuf(opt.rcvbuf);
            sock_b.enable_rx_timestamping();
        }

        // Two lines means neither socket may block, because a blocking read on
        // one line starves the other. One line may block, and the blocking
        // strategy is in the comparison precisely to show what that costs.
        if (opt.lines == 2 || opt.strategy != "blocking") {
            sock_a.set_nonblocking(true);
            if (sock_b.valid()) sock_b.set_nonblocking(true);
        } else {
            // One line, blocking receive. The socket stays blocking, which is
            // the whole point of measuring this strategy, and it gets a receive
            // timeout so the process can still notice the feed has stopped.
            sock_a.set_rcvtimeo(200);
        }

        std::unique_ptr<tick::UdpSocket> retransmit_sock;
        sockaddr_in                      retransmit_dst{};
        if (opt.recovery) {
            // Bind an ephemeral port so the re-request server can reply to us.
            retransmit_sock = std::make_unique<tick::UdpSocket>(tick::UdpSocket::unicast_receiver(0));
            retransmit_sock->set_nonblocking(true);
            retransmit_dst = tick::UdpSocket::endpoint(opt.retransmit_host, opt.retransmit_port);
        }

        Manifest manifest = load_manifest(opt.manifest);
        if (!manifest.present && !opt.quiet) {
            std::fprintf(stderr,
                         "no manifest at %s, so latency against the publisher's schedule\n"
                         "cannot be measured and only wire to book will be reported\n",
                         opt.manifest.c_str());
        }

        tick::SequencerConfig scfg;
        scfg.reorder_window = window;
        tick::Sequencer seq(scfg);
        seq.reset(1);

        RunState st;
        st.book = std::make_unique<tick::BookBuilder<>>();
        st.arrival_ns.assign(window, 0);
        st.window_mask = window - 1;

        const char* strategy_name = opt.strategy.c_str();
        const uint64_t t0 = tick::now_ns();

        auto go = [&](auto tagA, auto tagB) {
            using A = typename decltype(tagA)::type;
            using B = typename decltype(tagB)::type;
            if constexpr (!A::kAvailable) {
                std::fprintf(stderr,
                             "the %s receive strategy is not available on this platform\n",
                             std::string(A::kName).c_str());
                return 2;
            } else {
                return run_loop<A, B>(opt, manifest, st, seq, sock_a, sock_b,
                                      retransmit_sock.get(), retransmit_dst);
            }
        };

        int rc = 0;
        if (opt.strategy == "blocking") {
            struct TA { using type = tick::BlockingRecvStrategy; };
            rc = go(TA{}, TA{});
        } else if (opt.strategy == "busypoll") {
            struct TA { using type = tick::BusyPollStrategy; };
            rc = go(TA{}, TA{});
        } else if (opt.strategy == "recvmmsg") {
            struct TA { using type = tick::RecvmmsgStrategy; };
            rc = go(TA{}, TA{});
        } else if (opt.strategy == "epoll") {
            struct TA { using type = tick::EpollRecvmmsgStrategy; };
            rc = go(TA{}, TA{});
        } else if (opt.strategy == "kqueue") {
            struct TA { using type = tick::KqueueStrategy; };
            rc = go(TA{}, TA{});
        } else if (opt.strategy == "iouring") {
            struct TA { using type = tick::IoUringStrategy; };
            rc = go(TA{}, TA{});
        } else {
            std::fprintf(stderr, "unknown strategy %s\n", opt.strategy.c_str());
            return 1;
        }
        if (rc != 0) return rc;

        const double elapsed = static_cast<double>(tick::now_ns() - t0) / 1e9;

        report(opt, manifest, st, seq, box, pin, strategy_name, elapsed);
        write_results(opt, manifest, st, seq, box, pin, strategy_name, elapsed);

        if (!opt.percentiles_dir.empty()) {
            st.wire_to_book.write_percentiles_csv(opt.percentiles_dir + "/wire_to_book.csv");
            if (manifest.has_schedule()) {
                st.due_to_book.write_percentiles_csv(opt.percentiles_dir + "/due_to_book.csv");
            }
        }

        if (!opt.digest_file.empty()) {
            std::ofstream d(opt.digest_file);
            if (d) {
                d << std::hex << st.book->digest() << '\n' << st.book->volume_digest() << '\n';
            }
        }

        // A run that ended in the stale state rebuilt a book that cannot be
        // trusted, and it must not exit zero. The whole point of the gap state
        // machine is that unrecovered loss is loud.
        return seq.state() == tick::FeedState::Stale ? 3 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
