#include "tick/udp_socket.hpp"

#include "tick/moldudp64.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// Tests for the UDP socket wrapper.
//
// The loopback multicast test is the one that matters and it is also the one
// that can legitimately fail to run. Multicast loopback needs an interface the
// kernel is willing to send a 239.0.0.0/8 datagram out of, and a sandbox, a
// container without CAP_NET_RAW equivalents, a machine with every interface
// down, or a CI runner with multicast filtered all fail that in ways that have
// nothing to do with this code. Those cases call GTEST_SKIP rather than
// failing, because a red test that means "the network is unusual here" trains
// people to ignore red tests.
//
// A skipped test is not a passed test. Anything that depends on this path
// working has to look at whether it ran.

namespace {

// Administratively scoped multicast, the block reserved for local use, so this
// cannot reach a real venue group by accident.
constexpr const char* kGroup = "239.255.42.99";

std::span<const std::byte> as_span(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// The port the receiver actually got. Binding to port 0 and asking the kernel
// afterwards is the only way to pick a port that is definitely free. A
// hardcoded high port would collide with whatever else is on the machine and
// turn this into a test that fails for reasons unrelated to the code.
uint16_t bound_port(const tick::UdpSocket& sock) {
    sockaddr_in addr{};
    socklen_t   len = sizeof(addr);
    if (::getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

// Spin on a non-blocking socket for a bounded time. Returns the byte count, or
// -1 if nothing arrived inside the budget.
ssize_t recv_within(tick::UdpSocket& sock, std::span<std::byte> buf, uint64_t* ts,
                    int milliseconds) {
    const uint64_t deadline = tick::now_ns() + static_cast<uint64_t>(milliseconds) * 1000000ull;
    while (tick::now_ns() < deadline) {
        const ssize_t n = sock.recv_one(buf, ts);
        if (n >= 0) return n;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Things that do not need the network
// ---------------------------------------------------------------------------

TEST(UdpSocket, DefaultConstructedIsInvalid) {
    tick::UdpSocket s;
    EXPECT_FALSE(s.valid());
    EXPECT_LT(s.fd(), 0);
    EXPECT_FALSE(s.has_kernel_timestamping());
}

TEST(UdpSocket, EndpointParsesAndRejects) {
    const sockaddr_in a = tick::UdpSocket::endpoint("127.0.0.1", 12345);
    EXPECT_EQ(a.sin_family, AF_INET);
    EXPECT_EQ(ntohs(a.sin_port), 12345);
    EXPECT_EQ(ntohl(a.sin_addr.s_addr), 0x7F000001u);

    EXPECT_THROW((void)tick::UdpSocket::endpoint("not-an-address", 1), std::runtime_error);
    EXPECT_THROW((void)tick::UdpSocket::endpoint("300.1.1.1", 1), std::runtime_error);
}

TEST(UdpSocket, MoveTransfersOwnershipAndLeavesTheSourceInvalid) {
    tick::UdpSocket a = tick::UdpSocket::unicast_receiver(0);
    ASSERT_TRUE(a.valid());
    const int fd = a.fd();

    tick::UdpSocket b = std::move(a);
    EXPECT_FALSE(a.valid());          // NOLINT, checking the moved-from state is the point
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.fd(), fd);

    // Move assignment closes whatever the target was holding first, which is
    // the part that leaks if it is written carelessly.
    tick::UdpSocket c = tick::UdpSocket::unicast_receiver(0);
    c                 = std::move(b);
    EXPECT_EQ(c.fd(), fd);
    EXPECT_FALSE(b.valid());          // NOLINT
}

TEST(UdpSocket, RejectsAGroupThatIsNotAnAddress) {
    EXPECT_THROW((void)tick::UdpSocket::multicast_receiver("not-a-group", 0),
                 std::runtime_error);
}

TEST(UdpSocket, ReceiveBufferIsReportedBackRatherThanAssumed) {
    tick::UdpSocket s = tick::UdpSocket::unicast_receiver(0);
    const int       before = s.rcvbuf();
    EXPECT_GT(before, 0);

    // Both kernels are free to give less than was asked for, and both do. The
    // contract is only that the request is accepted and the real value can be
    // read back, never that the two match.
    //
    // Ask for more than the socket already has rather than a fixed size. A
    // fixed 4 MB request fails on a tuned box, where net.core.rmem_default is
    // larger than the request and the kernel correctly shrinks the buffer to
    // what was asked for. That is the setter working, so a test that reads it
    // as a regression is testing the sysctl and not the code.
    s.set_rcvbuf(before * 2);
    const int after = s.rcvbuf();
    EXPECT_GT(after, 0);
    EXPECT_GE(after, before);
}

TEST(UdpSocket, NonblockingRecvReturnsEagainRatherThanHanging) {
    tick::UdpSocket s = tick::UdpSocket::unicast_receiver(0);
    s.set_nonblocking(true);

    std::array<std::byte, 64> buf{};
    errno           = 0;
    const ssize_t n = s.recv_one(buf);
    EXPECT_EQ(n, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

TEST(UdpSocket, ClocksAreConsistentAndTheZeroMarkerSurvivesConversion) {
    EXPECT_GT(tick::now_ns(), 0u);

    // Zero is the marker for "the kernel attached no timestamp". It has to come
    // back out of the conversion as zero, not as a plausible looking number one
    // boot-age away from now, or an absent timestamp starts reading as a real
    // one in the results.
    EXPECT_EQ(tick::detail::kernel_realtime_to_mono_ns(0), 0u);

    // A realtime reading converted onto the project's monotonic base lands
    // close to a monotonic reading taken at the same moment. A millisecond of
    // slack covers the clock reads and any scheduling in between without
    // making the assertion meaningless.
    const auto     rt_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const uint64_t converted = tick::detail::kernel_realtime_to_mono_ns(static_cast<uint64_t>(rt_now));
    const uint64_t mono      = tick::now_ns();
    const int64_t  delta     = static_cast<int64_t>(mono) - static_cast<int64_t>(converted);
    EXPECT_LT(delta < 0 ? -delta : delta, 1000000);
}

// ---------------------------------------------------------------------------
// The loopback multicast round trip
// ---------------------------------------------------------------------------

TEST(UdpSocket, MulticastLoopbackRoundTrip) {
    tick::UdpSocket rx;
    tick::UdpSocket tx;
    try {
        rx = tick::UdpSocket::multicast_receiver(kGroup, 0);
        tx = tick::UdpSocket::multicast_sender({}, 1, true);
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "multicast is not available in this environment: " << e.what();
    }

    const uint16_t port = bound_port(rx);
    ASSERT_NE(port, 0) << "could not learn the bound port";
    rx.set_nonblocking(true);

    const std::string payload = "tickerplant multicast loopback";
    const sockaddr_in dst     = tick::UdpSocket::endpoint(kGroup, port);

    const ssize_t sent = tx.send_to(as_span(payload), dst);
    if (sent < 0) {
        // No route for 239.0.0.0/8 usually means every interface is down or the
        // sandbox blocks multicast egress. Neither is this code being wrong.
        GTEST_SKIP() << "sendto to " << kGroup << " failed: " << std::strerror(errno);
    }
    ASSERT_EQ(static_cast<std::size_t>(sent), payload.size());

    std::array<std::byte, 2048> buf{};
    uint64_t                    ts = 0;
    const ssize_t               got = recv_within(rx, buf, &ts, 500);
    if (got < 0) {
        GTEST_SKIP() << "no multicast loopback delivery on this host";
    }

    ASSERT_EQ(static_cast<std::size_t>(got), payload.size());
    EXPECT_EQ(std::memcmp(buf.data(), payload.data(), payload.size()), 0);
}

TEST(UdpSocket, MulticastLoopbackCarriesAMoldPacket) {
    tick::UdpSocket rx;
    tick::UdpSocket tx;
    try {
        rx = tick::UdpSocket::multicast_receiver(kGroup, 0);
        tx = tick::UdpSocket::multicast_sender({}, 1, true);
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "multicast is not available in this environment: " << e.what();
    }

    const uint16_t port = bound_port(rx);
    ASSERT_NE(port, 0);
    rx.set_nonblocking(true);

    std::array<std::byte, tick::mold::kMaxPacketLen> out{};
    tick::mold::PacketBuilder                        b(out.data(), out.size(), "TICK000001");
    b.reset(77);
    ASSERT_TRUE(b.try_append(as_span("first")));
    ASSERT_TRUE(b.try_append(as_span("second")));

    const sockaddr_in dst  = tick::UdpSocket::endpoint(kGroup, port);
    const ssize_t     sent = tx.send_to(b.finish(), dst);
    if (sent < 0) {
        GTEST_SKIP() << "sendto to " << kGroup << " failed: " << std::strerror(errno);
    }

    std::array<std::byte, 2048> buf{};
    const ssize_t               got = recv_within(rx, buf, nullptr, 500);
    if (got < 0) {
        GTEST_SKIP() << "no multicast loopback delivery on this host";
    }

    const auto view = tick::mold::PacketView::parse({buf.data(), static_cast<std::size_t>(got)});
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->session(), "TICK000001");
    EXPECT_EQ(view->sequence(), 77u);
    EXPECT_EQ(view->count(), 2u);
}

// Receive timestamping is checked for internal consistency and nothing more.
//
// What can honestly be asserted is that when the socket says it enabled kernel
// timestamping, a datagram that arrives carries a non-zero timestamp, and that
// the timestamp sits on the monotonic base rather than the realtime one. What
// cannot be asserted is any accuracy claim, because on macOS SO_TIMESTAMP has
// microsecond resolution and any number derived from it is quantised to a
// microsecond.
TEST(UdpSocket, KernelTimestampIsOnTheMonotonicBaseOrIsHonestlyZero) {
    tick::UdpSocket rx;
    tick::UdpSocket tx;
    try {
        rx = tick::UdpSocket::multicast_receiver(kGroup, 0);
        tx = tick::UdpSocket::multicast_sender({}, 1, true);
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "multicast is not available in this environment: " << e.what();
    }

    const bool enabled = rx.enable_rx_timestamping();
    EXPECT_EQ(enabled, rx.has_kernel_timestamping());

    const uint16_t port = bound_port(rx);
    ASSERT_NE(port, 0);
    rx.set_nonblocking(true);

    const uint64_t    before = tick::now_ns();
    const sockaddr_in dst    = tick::UdpSocket::endpoint(kGroup, port);
    if (tx.send_to(as_span("ts"), dst) < 0) {
        GTEST_SKIP() << "sendto to " << kGroup << " failed: " << std::strerror(errno);
    }

    std::array<std::byte, 256> buf{};
    uint64_t                   ts  = 0;
    const ssize_t              got = recv_within(rx, buf, &ts, 500);
    if (got < 0) {
        GTEST_SKIP() << "no multicast loopback delivery on this host";
    }
    const uint64_t after = tick::now_ns();

    if (!enabled) {
        // A platform that cannot do it reports zero rather than a guess.
        EXPECT_EQ(ts, 0u);
        GTEST_SKIP() << "kernel receive timestamping is not available here";
    }

    ASSERT_NE(ts, 0u) << "timestamping was enabled but no timestamp was attached";

    // The kernel took the timestamp between the send and the return from recv,
    // so on the monotonic base it falls inside that window. A millisecond of
    // slack on each side absorbs the microsecond quantisation of the macOS
    // timeval and the error in the one-time clock offset.
    constexpr uint64_t kSlack = 1000000;
    EXPECT_GE(ts + kSlack, before);
    EXPECT_LE(ts, after + kSlack);
}
