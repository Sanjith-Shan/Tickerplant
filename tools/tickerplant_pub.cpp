// tickerplant-pub
//
// Replay an ITCH 5.0 file onto the wire as MoldUDP64 over UDP multicast, on two
// lines, with configurable loss, reordering, and skew between the lines.
//
// This is the other half of the test rig rather than a product. A real exchange
// publishes A and B from separate hardware down separate paths, and the reason
// a receiver arbitrates them is that the two paths do not fail together. There
// is no way to reproduce that honestly on one machine, so this injects the
// failure modes deliberately and the receiver has to survive them. What can be
// claimed from that is that the arbitration and recovery logic is correct.
// What cannot be claimed is a number about a real network, and the README says
// so where it reports these runs.
//
// Two design points worth reading before the code.
//
// Loss is injected per datagram, not per message. A packet either arrives or it
// does not, and since a MoldUDP64 packet carries several messages, one lost
// datagram is a gap of several sequence numbers. Dropping individual messages
// would be a friendlier failure than the real one.
//
// Pacing is scheduled, not measured. The publisher decides in advance when
// message N should be sent and then tries to hit that time. It writes the
// schedule into a manifest file so the receiver can compute latency against
// when a message SHOULD have been sent rather than when it was. That is what
// makes the receiver's numbers free of coordinated omission, and it is the
// whole reason the manifest exists.

#include "tick/clock.hpp"
#include "tick/itch.hpp"
#include "tick/itch_file.hpp"
#include "tick/moldudp64.hpp"
#include "tick/recovery.hpp"
#include "tick/udp_socket.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Options {
    std::string file;
    std::string group_a = "239.255.42.10";
    std::string group_b = "239.255.42.11";
    uint16_t    port_a  = 26010;
    uint16_t    port_b  = 26011;
    std::string iface;
    std::string session = "TICKPLANT";
    std::string manifest = "results/run_manifest.json";

    // Pacing. "max" sends as fast as the socket accepts, "rate" sends at a
    // fixed message rate, and "natural" follows the ITCH timestamps at a speed
    // multiplier so the shape of the real day is preserved.
    std::string pace  = "max";
    double      rate  = 1'000'000.0;
    double      speed = 1.0;

    // Failure injection. Per datagram, per line, independent.
    double   drop_a  = 0.0;
    double   drop_b  = 0.0;
    double   reorder = 0.0;
    uint64_t skew_b_us = 0;
    uint64_t seed    = 20191230;

    uint64_t limit = 0;
    int      batch = 8;   // messages per packet, capped by the MTU as well
    int      ttl   = 1;
    bool     loopback = true;
    uint16_t retransmit_port = 26020;
    std::size_t store_capacity = 1u << 20;
    bool     quiet = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: tickerplant-pub --file <path.gz|path> [options]\n"
        "  --group-a A --port-a N        line A multicast group and port\n"
        "  --group-b A --port-b N        line B multicast group and port\n"
        "  --iface ADDR                  outbound interface address\n"
        "  --session NAME                MoldUDP64 session, up to 10 characters\n"
        "  --pace max|rate|natural       how fast to replay (default max)\n"
        "  --rate N                      messages per second when --pace rate\n"
        "  --speed X                     multiplier when --pace natural\n"
        "  --drop-a P --drop-b P         per datagram loss probability per line\n"
        "  --reorder P                   probability a datagram is held one place back\n"
        "  --skew-b-us N                 delay line B by this many microseconds\n"
        "  --seed N                      makes the injected failures reproducible\n"
        "  --batch N                     messages per packet (default 8)\n"
        "  --limit N                     stop after N messages\n"
        "  --retransmit-port N           port the re-request server listens on\n"
        "  --manifest <path>             where to write the run manifest\n"
        "  --ttl N --no-loopback         multicast options\n"
        "  --quiet\n");
}

// The re-request server.
//
// A receiver that finds a gap asks for the missing range here, over unicast.
// This holds a bounded ring of recently published messages, which is what a
// real venue does in memory in front of the session store it keeps on disk. A
// request for something older than the ring is answered with nothing rather
// than with the wrong message, and the counter says how often that happened.
struct RetransmitServer {
    tick::RetransmitStore store;
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> served{0};
    std::atomic<uint64_t> misses{0};
    std::string           session;
    uint16_t              port;
    std::thread           thread;
    std::atomic<bool>     running{false};

    RetransmitServer(std::size_t capacity, std::string sess, uint16_t p)
        : store(capacity), session(std::move(sess)), port(p) {}

    void start() {
        running.store(true, std::memory_order_relaxed);
        thread = std::thread([this] { serve(); });
    }

    void stop() {
        running.store(false, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }

    void serve() {
        tick::UdpSocket sock;
        try {
            sock = tick::UdpSocket::unicast_receiver(port);
            sock.set_nonblocking(true);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "retransmit server cannot start: %s\n", e.what());
            return;
        }

        std::vector<std::byte> in(tick::mold::kMaxPacketLen);
        std::vector<std::byte> out(tick::mold::kMaxPacketLen);
        std::vector<std::byte> payload(64);

        while (running.load(std::memory_order_relaxed)) {
            sockaddr_in from{};
            socklen_t   fromlen = sizeof(from);
            const ssize_t n = ::recvfrom(sock.fd(), in.data(), in.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n <= 0) {
                // Nothing waiting. A short sleep beats a spin here because the
                // recovery path is not latency critical and a burned core is.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            const auto hdr = tick::mold::parse_header(
                std::span<const std::byte>(in.data(), static_cast<std::size_t>(n)));
            if (!hdr) continue;
            requests.fetch_add(1, std::memory_order_relaxed);

            tick::mold::PacketBuilder b(out.data(), out.size(), session);
            b.reset(hdr->sequence);
            uint16_t sent = 0;
            for (uint16_t i = 0; i < hdr->count; ++i) {
                std::size_t len = 0;
                if (!store.fetch(hdr->sequence + i, payload, &len)) {
                    misses.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (!b.try_append(std::span<const std::byte>(payload.data(), len))) break;
                ++sent;
            }
            if (sent == 0) continue;

            const auto pkt = b.finish();
            ::sendto(sock.fd(), pkt.data(), pkt.size(), 0,
                     reinterpret_cast<const sockaddr*>(&from), fromlen);
            served.fetch_add(sent, std::memory_order_relaxed);
        }
    }
};

// Wait until a deadline on the monotonic clock. Sleeping is cheap but coarse
// and spinning is precise but burns a core, so this does both. The crossover is
// deliberately generous, because the publisher oversleeping is a measurement
// error in the data and the receiver is the thing being measured.
void wait_until(uint64_t deadline_ns) {
    for (;;) {
        const uint64_t now = tick::now_ns();
        if (now >= deadline_ns) return;
        const uint64_t remaining = deadline_ns - now;
        if (remaining > 200'000) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 150'000));
        } else {
            for (int i = 0; i < 64; ++i) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
                asm volatile("pause" ::: "memory");
#endif
            }
        }
    }
}

struct PendingPacket {
    std::vector<std::byte> bytes;
    uint64_t               due_ns = 0;
    bool                   line_b = false;
};

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
        else if (a == "--group-a") opt.group_a = need("--group-a");
        else if (a == "--group-b") opt.group_b = need("--group-b");
        else if (a == "--port-a") opt.port_a = static_cast<uint16_t>(std::atoi(need("--port-a").c_str()));
        else if (a == "--port-b") opt.port_b = static_cast<uint16_t>(std::atoi(need("--port-b").c_str()));
        else if (a == "--iface") opt.iface = need("--iface");
        else if (a == "--session") opt.session = need("--session");
        else if (a == "--pace") opt.pace = need("--pace");
        else if (a == "--rate") opt.rate = std::atof(need("--rate").c_str());
        else if (a == "--speed") opt.speed = std::atof(need("--speed").c_str());
        else if (a == "--drop-a") opt.drop_a = std::atof(need("--drop-a").c_str());
        else if (a == "--drop-b") opt.drop_b = std::atof(need("--drop-b").c_str());
        else if (a == "--reorder") opt.reorder = std::atof(need("--reorder").c_str());
        else if (a == "--skew-b-us") opt.skew_b_us = std::strtoull(need("--skew-b-us").c_str(), nullptr, 10);
        else if (a == "--seed") opt.seed = std::strtoull(need("--seed").c_str(), nullptr, 10);
        else if (a == "--limit") opt.limit = std::strtoull(need("--limit").c_str(), nullptr, 10);
        else if (a == "--batch") opt.batch = std::atoi(need("--batch").c_str());
        else if (a == "--ttl") opt.ttl = std::atoi(need("--ttl").c_str());
        else if (a == "--no-loopback") opt.loopback = false;
        else if (a == "--retransmit-port") opt.retransmit_port = static_cast<uint16_t>(std::atoi(need("--retransmit-port").c_str()));
        else if (a == "--manifest") opt.manifest = need("--manifest");
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 1; }
    }

    if (opt.file.empty()) { usage(); return 1; }
    if (opt.batch < 1) opt.batch = 1;
    if (opt.batch > tick::mold::kMaxCount) opt.batch = tick::mold::kMaxCount;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        tick::ItchFile  file(opt.file);
        tick::UdpSocket sock = tick::UdpSocket::multicast_sender(opt.iface, opt.ttl, opt.loopback);
        const sockaddr_in dst_a = tick::UdpSocket::endpoint(opt.group_a, opt.port_a);
        const sockaddr_in dst_b = tick::UdpSocket::endpoint(opt.group_b, opt.port_b);

        RetransmitServer retx(opt.store_capacity, opt.session, opt.retransmit_port);
        retx.start();

        std::mt19937_64                        rng(opt.seed);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        std::vector<std::byte> packet_buf(tick::mold::kMaxPacketLen);
        tick::mold::PacketBuilder builder(packet_buf.data(), packet_buf.size(), opt.session);

        // Packets held back, either because line B is skewed or because this
        // one was chosen to arrive out of order.
        std::deque<PendingPacket> delayed;

        uint64_t sequence   = 1;   // MoldUDP64 sequences are one based
        uint64_t messages   = 0;
        uint64_t packets    = 0;
        uint64_t dropped_a  = 0;
        uint64_t dropped_b  = 0;
        uint64_t reordered  = 0;
        uint64_t first_itch_ts = 0;
        bool     have_first    = false;

        // Peek the first message before anything is sent, so the manifest can
        // be written up front. The receiver reads it at startup to learn the
        // schedule, and a manifest that only appears when the run finishes is
        // no use to the thing it exists for.
        std::vector<std::byte> first_msg;
        {
            std::span<const std::byte> m0;
            if (file.next(m0)) {
                first_msg.assign(m0.begin(), m0.end());
                if (first_msg.size() >= tick::itch::kHeaderLen) {
                    first_itch_ts = tick::itch::timestamp(first_msg.data());
                    have_first    = true;
                }
            }
        }

        const uint64_t t0 = tick::now_ns();
        const double   ns_per_message = (opt.rate > 0.0) ? 1e9 / opt.rate : 0.0;

        // The manifest. This is what lets the receiver measure latency against
        // when a message was due rather than when it turned up, which is the
        // difference between a distribution that hides a stall and one that
        // shows it. Written once before the first packet and again at the end
        // with the counters filled in.
        auto write_manifest = [&](uint64_t msgs, uint64_t pkts, uint64_t da, uint64_t db,
                                  double elapsed) {
            if (opt.manifest.empty()) return;
            std::ofstream out(opt.manifest);
            if (!out) {
                std::fprintf(stderr, "cannot write manifest %s\n", opt.manifest.c_str());
                return;
            }
            out << "{\n";
            out << "  \"session\": \"" << opt.session << "\",\n";
            out << "  \"file\": \"" << opt.file << "\",\n";
            out << "  \"first_sequence\": 1,\n";
            out << "  \"messages\": " << msgs << ",\n";
            out << "  \"packets\": " << pkts << ",\n";
            out << "  \"t0_mono_ns\": " << t0 << ",\n";
            out << "  \"pace\": \"" << opt.pace << "\",\n";
            out << "  \"rate\": " << opt.rate << ",\n";
            out << "  \"speed\": " << opt.speed << ",\n";
            out << "  \"ns_per_message\": " << ns_per_message << ",\n";
            out << "  \"first_itch_timestamp_ns\": " << first_itch_ts << ",\n";
            out << "  \"batch\": " << opt.batch << ",\n";
            out << "  \"drop_a\": " << opt.drop_a << ",\n";
            out << "  \"drop_b\": " << opt.drop_b << ",\n";
            out << "  \"reorder\": " << opt.reorder << ",\n";
            out << "  \"skew_b_us\": " << opt.skew_b_us << ",\n";
            out << "  \"seed\": " << opt.seed << ",\n";
            out << "  \"dropped_packets_a\": " << da << ",\n";
            out << "  \"dropped_packets_b\": " << db << ",\n";
            out << "  \"elapsed_s\": " << elapsed << "\n";
            out << "}\n";
        };
        write_manifest(0, 0, 0, 0, 0.0);

        auto flush_delayed = [&](uint64_t now) {
            while (!delayed.empty() && delayed.front().due_ns <= now) {
                PendingPacket& p = delayed.front();
                ::sendto(sock.fd(), p.bytes.data(), p.bytes.size(), 0,
                         reinterpret_cast<const sockaddr*>(p.line_b ? &dst_b : &dst_a),
                         sizeof(sockaddr_in));
                delayed.pop_front();
            }
        };

        // One packet's worth of messages, already framed, ready to go on both
        // lines. The same bytes go to A and B, which is what makes arbitration
        // meaningful on the receiving side.
        auto send_packet = [&](std::span<const std::byte> pkt, uint64_t now) {
            ++packets;

            const bool hold = opt.reorder > 0.0 && unit(rng) < opt.reorder;

            if (unit(rng) >= opt.drop_a) {
                if (hold) {
                    delayed.push_back({std::vector<std::byte>(pkt.begin(), pkt.end()),
                                       now + 50'000, false});
                    ++reordered;
                } else {
                    ::sendto(sock.fd(), pkt.data(), pkt.size(), 0,
                             reinterpret_cast<const sockaddr*>(&dst_a), sizeof(dst_a));
                }
            } else {
                ++dropped_a;
            }

            if (unit(rng) >= opt.drop_b) {
                if (opt.skew_b_us > 0 || hold) {
                    delayed.push_back({std::vector<std::byte>(pkt.begin(), pkt.end()),
                                       now + opt.skew_b_us * 1000 + (hold ? 50'000 : 0), true});
                } else {
                    ::sendto(sock.fd(), pkt.data(), pkt.size(), 0,
                             reinterpret_cast<const sockaddr*>(&dst_b), sizeof(dst_b));
                }
            } else {
                ++dropped_b;
            }
        };

        std::span<const std::byte> msg;
        builder.reset(sequence);
        uint64_t packet_first_seq = sequence;
        uint64_t packet_first_ts  = 0;

        // When the packet that is being built is due to leave. For a fixed
        // rate that is the first message's slot in the schedule, and for
        // natural pacing it is where that message sat in the real trading day.
        // Both are computed from the first message in the packet, because that
        // is the one the receiver will measure against.
        auto packet_due = [&]() -> uint64_t {
            if (opt.pace == "rate" && ns_per_message > 0.0) {
                return t0 + static_cast<uint64_t>(
                                static_cast<double>(packet_first_seq - 1) * ns_per_message);
            }
            if (opt.pace == "natural" && have_first) {
                const uint64_t d = packet_first_ts > first_itch_ts
                                       ? packet_first_ts - first_itch_ts : 0;
                return t0 + static_cast<uint64_t>(static_cast<double>(d) / opt.speed);
            }
            return 0;
        };

        auto flush_packet = [&]() {
            if (builder.count() == 0) return;
            const uint64_t due = packet_due();
            if (due != 0) wait_until(due);
            const uint64_t now = tick::now_ns();
            flush_delayed(now);
            send_packet(builder.finish(), now);
        };

        bool used_first = false;
        auto next_message = [&](std::span<const std::byte>& out) -> bool {
            if (!used_first && !first_msg.empty()) {
                used_first = true;
                out = std::span<const std::byte>(first_msg.data(), first_msg.size());
                return true;
            }
            return file.next(out);
        };

        while (!g_stop.load(std::memory_order_relaxed) && next_message(msg)) {
            const uint64_t itch_ts = msg.size() >= tick::itch::kHeaderLen
                                         ? tick::itch::timestamp(msg.data()) : 0;

            // The retransmission store has to hold the message before it goes
            // out, because a receiver can ask for it the instant it notices the
            // gap and that can be before the publisher's next loop iteration.
            retx.store.store(sequence, msg);

            if (builder.count() == 0) packet_first_ts = itch_ts;

            if (!builder.try_append(msg)) {
                flush_packet();
                packet_first_seq = sequence;
                packet_first_ts  = itch_ts;
                builder.reset(sequence);
                if (!builder.try_append(msg)) {
                    std::fprintf(stderr, "message of %zu bytes does not fit a packet\n",
                                 msg.size());
                    break;
                }
            }

            ++sequence;
            ++messages;

            if (builder.count() >= opt.batch) {
                flush_packet();
                packet_first_seq = sequence;
                packet_first_ts  = 0;
                builder.reset(sequence);
            }

            if (opt.limit != 0 && messages >= opt.limit) break;

            if (!opt.quiet && (messages % 1000000) == 0) {
                std::fprintf(stderr, "\r  published %llu messages, %llu packets   ",
                             static_cast<unsigned long long>(messages),
                             static_cast<unsigned long long>(packets));
                std::fflush(stderr);
            }
        }

        flush_packet();

        // Everything still held back has to go out before the session ends,
        // otherwise the receiver sits in a gap that will never close and the
        // run looks like a bug in the recovery path rather than the end of the
        // file.
        while (!delayed.empty()) {
            const uint64_t now = tick::now_ns();
            flush_delayed(now);
            if (!delayed.empty()) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // End of session, twice on each line, because this one matters and it
        // is not worth a receiver hanging because a single datagram was lost.
        std::vector<std::byte> eos(tick::mold::kHeaderLen);
        const std::size_t      eos_len = tick::mold::PacketBuilder::end_of_session(
            eos.data(), eos.size(), opt.session, sequence);
        for (int i = 0; i < 2; ++i) {
            ::sendto(sock.fd(), eos.data(), eos_len, 0,
                     reinterpret_cast<const sockaddr*>(&dst_a), sizeof(dst_a));
            ::sendto(sock.fd(), eos.data(), eos_len, 0,
                     reinterpret_cast<const sockaddr*>(&dst_b), sizeof(dst_b));
        }

        const uint64_t t1 = tick::now_ns();
        const double   elapsed = static_cast<double>(t1 - t0) / 1e9;

        if (!opt.quiet) std::fprintf(stderr, "\r%60s\r", "");
        std::printf("\nTickerplant publisher\n");
        std::printf("  file            %s\n", opt.file.c_str());
        std::printf("  session         %s\n", opt.session.c_str());
        std::printf("  line A          %s:%u\n", opt.group_a.c_str(), opt.port_a);
        std::printf("  line B          %s:%u  skew %llu us\n", opt.group_b.c_str(), opt.port_b,
                    static_cast<unsigned long long>(opt.skew_b_us));
        std::printf("  pacing          %s", opt.pace.c_str());
        if (opt.pace == "rate") std::printf(" at %.0f msg/s", opt.rate);
        if (opt.pace == "natural") std::printf(" at %.2fx", opt.speed);
        std::printf("\n");
        std::printf("  messages        %llu in %llu packets\n",
                    static_cast<unsigned long long>(messages),
                    static_cast<unsigned long long>(packets));
        std::printf("  elapsed         %.3f s, %.3f M msg/s\n", elapsed,
                    static_cast<double>(messages) / elapsed / 1e6);
        std::printf("  injected loss   A %llu packets (%.3f%%), B %llu packets (%.3f%%)\n",
                    static_cast<unsigned long long>(dropped_a),
                    packets ? 100.0 * static_cast<double>(dropped_a) / static_cast<double>(packets) : 0.0,
                    static_cast<unsigned long long>(dropped_b),
                    packets ? 100.0 * static_cast<double>(dropped_b) / static_cast<double>(packets) : 0.0);
        std::printf("  reordered       %llu packets\n", static_cast<unsigned long long>(reordered));
        std::printf("  retransmit      %llu requests, %llu messages served, %llu misses\n",
                    static_cast<unsigned long long>(retx.requests.load()),
                    static_cast<unsigned long long>(retx.served.load()),
                    static_cast<unsigned long long>(retx.misses.load()));

        // Rewrite the manifest now that the counters are known. The schedule
        // fields are unchanged, so a receiver that read the first version is
        // still measuring against the same times.
        write_manifest(messages, packets, dropped_a, dropped_b, elapsed);
        if (!opt.manifest.empty()) std::fprintf(stderr, "wrote %s\n", opt.manifest.c_str());

        // Keep answering re-requests for a moment after the data ends, because
        // a receiver that lost the last packet asks for it after everything
        // else has stopped.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        retx.stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
