#pragma once

#include "tick/clock.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <span>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <linux/net_tstamp.h>
#endif

// An RAII UDP socket that behaves the same on macOS and Linux.
//
// The class exists for two reasons. One is ownership, because a raw fd that
// escapes a scope on an error path is the classic way a long running feed
// handler runs out of descriptors overnight. The other is that every knob a
// market data receiver actually needs is a setsockopt with a platform specific
// spelling, and putting those spellings in one file keeps the strategy code in
// receive_strategy.hpp readable.
//
// Construction throws. A socket that cannot be created or a group that cannot
// be joined is a startup failure, and a failure at startup should be loud and
// carry strerror(errno) rather than return a sentinel nobody checks. Nothing on
// the receive path throws, and recv_one reports failure the way the system call
// does, with -1 and errno.
//
// ---------------------------------------------------------------------------
// Which clock the receive timestamp is on
// ---------------------------------------------------------------------------
//
// This is the part that is easy to get quietly wrong, so it is stated plainly.
//
// On Linux, SO_TIMESTAMPING with SOF_TIMESTAMPING_RX_SOFTWARE and
// SOF_TIMESTAMPING_SOFTWARE delivers the timestamp out of band, as an
// SCM_TIMESTAMPING control message attached to the datagram. It is not
// available through recv, only through recvmsg or recvmmsg with a control
// buffer. The value arrives as a struct scm_timestamping, whose first timespec
// is the software timestamp, and that timestamp is on CLOCK_REALTIME unless the
// socket asked for something else with the SOF_TIMESTAMPING_OPT_ family.
//
// On macOS the closest available facility is SO_TIMESTAMP, which attaches an
// SCM_TIMESTAMP control message holding a struct timeval. It is also on the
// realtime clock, and it has microsecond resolution rather than nanosecond.
//
// So on both platforms the kernel hands back a realtime value, while everything
// this project measures against is the raw monotonic base in clock.hpp.
// Subtracting one from the other gives a number that looks like a latency and
// is not one.
//
// The conversion is clock.hpp's realtime_ns_to_monotonic, which applies one
// offset captured once at first use. This file deliberately does not calibrate
// its own offset or read its own monotonic clock. Two offsets against two
// slightly different monotonic bases, which is what a local copy would be,
// produce two answers that disagree by however much NTP has slewed
// CLOCK_MONOTONIC away from CLOCK_MONOTONIC_RAW. Having one base in the
// process is the whole point of the warning above.
//
// Callers must not mix bases either. A kernel timestamp is converted here and
// then compared only against tick::now_ns, never against a reading from some
// other clock.

namespace tick {

namespace detail {

// clock.hpp's conversion, with the zero marker preserved. Zero means "there was
// no kernel timestamp" and must survive the conversion as zero rather than
// becoming a plausible looking number roughly one boot-age away from now.
[[nodiscard]] inline uint64_t kernel_realtime_to_mono_ns(uint64_t realtime_ns) noexcept {
    if (realtime_ns == 0) return 0;
    return realtime_ns_to_monotonic(realtime_ns);
}

// Pull the receive timestamp out of a recvmsg control buffer and return it on
// the monotonic base, or 0 when the kernel attached nothing. Shared with the
// batching strategies, which run their own recvmmsg and get the same control
// messages per datagram.
[[nodiscard]] inline uint64_t rx_timestamp_ns(const msghdr& msg) noexcept {
    // The control message walk needs a non-const msghdr, because glibc declares
    // CMSG_NXTHDR as taking one and will not accept a pointer to const. macOS
    // is more permissive and compiled the const version happily, which is
    // exactly how a header ends up being portable in theory and broken on the
    // platform that matters. Nothing here writes through the pointer.
    msghdr* m = const_cast<msghdr*>(&msg);
    for (cmsghdr* cm = CMSG_FIRSTHDR(m); cm != nullptr; cm = CMSG_NXTHDR(m, cm)) {
        if (cm->cmsg_level != SOL_SOCKET) continue;

#if defined(__linux__)
        if (cm->cmsg_type == SCM_TIMESTAMPING) {
            // The payload is struct scm_timestamping, which is three timespecs.
            // [0] is the software timestamp, [1] is deprecated, [2] is the
            // hardware one when a NIC supplied it. Only the software slot is
            // requested here, so only [0] is read.
            //
            // The layout is read as an array of timespec rather than through
            // struct scm_timestamping so that <linux/errqueue.h> does not have
            // to be included alongside the C library's own headers, which is a
            // reliable way to end up with two definitions of timespec.
            timespec ts[3]{};
            std::memcpy(ts, CMSG_DATA(cm), sizeof(ts));
            const uint64_t rt = static_cast<uint64_t>(ts[0].tv_sec) * 1000000000ull +
                                static_cast<uint64_t>(ts[0].tv_nsec);
            return kernel_realtime_to_mono_ns(rt);
        }
        if (cm->cmsg_type == SCM_TIMESTAMPNS) {
            timespec ts{};
            std::memcpy(&ts, CMSG_DATA(cm), sizeof(ts));
            const uint64_t rt = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                static_cast<uint64_t>(ts.tv_nsec);
            return kernel_realtime_to_mono_ns(rt);
        }
#endif
        if (cm->cmsg_type == SCM_TIMESTAMP) {
            // microsecond resolution, which is all macOS offers here
            timeval tv{};
            std::memcpy(&tv, CMSG_DATA(cm), sizeof(tv));
            const uint64_t rt = static_cast<uint64_t>(tv.tv_sec) * 1000000000ull +
                                static_cast<uint64_t>(tv.tv_usec) * 1000ull;
            return kernel_realtime_to_mono_ns(rt);
        }
    }
    return 0;
}

// A control buffer large enough for any timestamp control message either
// platform attaches, with room left for anything else the kernel decides to
// send along. Too small a buffer does not fail, it silently sets MSG_CTRUNC and
// drops the timestamp, which would look like "timestamping is off".
inline constexpr std::size_t kControlLen = 256;

} // namespace detail

// ---------------------------------------------------------------------------
// The socket
// ---------------------------------------------------------------------------

class UdpSocket {
public:
    UdpSocket() = default;

    ~UdpSocket() { close(); }

    UdpSocket(UdpSocket&& other) noexcept
        : fd_(other.fd_), ts_mode_(other.ts_mode_) {
        other.fd_      = -1;
        other.ts_mode_ = TsMode::None;
    }

    UdpSocket& operator=(UdpSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_            = other.fd_;
            ts_mode_       = other.ts_mode_;
            other.fd_      = -1;
            other.ts_mode_ = TsMode::None;
        }
        return *this;
    }

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bind to port and join a multicast group.
    //
    // The bind is to INADDR_ANY and not to the group address. Binding a UDP
    // socket to a multicast address is a BSD extension that filters the receive
    // path to that group, Linux allows it, and macOS is inconsistent about it
    // depending on how the group was joined. Binding to ANY and relying on
    // IP_ADD_MEMBERSHIP for the subscription works identically on both, at the
    // cost of also receiving unicast traffic sent to the same port. For a feed
    // handler on a dedicated port that cost is nothing.
    //
    // iface is the local address of the interface to join on. Empty means
    // INADDR_ANY, which lets the kernel pick using the routing table. On a
    // machine with more than one interface that pick is frequently the wrong
    // one, so production configuration should name the interface explicitly.
    static UdpSocket multicast_receiver(const std::string& group, uint16_t port,
                                        const std::string& iface = {}) {
        UdpSocket s(make_fd());
        s.set_reuse();

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s.fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw_errno("bind udp port " + std::to_string(port));
        }

        ip_mreq mreq{};
        if (::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
            throw std::runtime_error("not a valid IPv4 multicast group: " + group);
        }
        mreq.imr_interface.s_addr = parse_iface(iface);
        if (::setsockopt(s.fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            throw_errno("IP_ADD_MEMBERSHIP " + group);
        }
        return s;
    }

    // A plain bound socket, used for the unicast reply from the re-request
    // server. No group to join, otherwise identical.
    static UdpSocket unicast_receiver(uint16_t port) {
        UdpSocket s(make_fd());
        s.set_reuse();

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s.fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw_errno("bind udp port " + std::to_string(port));
        }
        return s;
    }

    // ttl of 1 keeps the traffic on the local link, which is what a test or a
    // single rack wants. loopback on is what lets a receiver on this same host
    // see the packets, and is the only reason the loopback test can run at all.
    static UdpSocket multicast_sender(const std::string& iface = {}, int ttl = 1,
                                      bool loopback = true) {
        UdpSocket s(make_fd());

        // These two options take a u_char on the BSD sockets in macOS. Linux
        // accepts either width and infers from optlen, so one spelling works on
        // both and avoids a platform branch over two setsockopt calls.
        const unsigned char ttl_val  = static_cast<unsigned char>(ttl);
        const unsigned char loop_val = loopback ? 1u : 0u;
        if (::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_val, sizeof(ttl_val)) != 0) {
            throw_errno("IP_MULTICAST_TTL");
        }
        if (::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_val, sizeof(loop_val)) != 0) {
            throw_errno("IP_MULTICAST_LOOP");
        }

        if (!iface.empty()) {
            in_addr a{};
            a.s_addr = parse_iface(iface);
            if (::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) != 0) {
                throw_errno("IP_MULTICAST_IF " + iface);
            }
        }
        return s;
    }

    [[nodiscard]] int  fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    void set_nonblocking(bool on) {
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0) throw_errno("F_GETFL");
        const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        if (::fcntl(fd_, F_SETFL, want) != 0) throw_errno("F_SETFL O_NONBLOCK");
    }

    // Ask for a receive buffer. The kernel is free to give less, and both
    // platforms do. Linux caps at net.core.rmem_max and then doubles what it
    // stored for its own bookkeeping, so getsockopt reports roughly twice the
    // request. macOS caps at kern.ipc.maxsockbuf, which defaults low enough
    // that a request for a few megabytes is silently trimmed.
    //
    // A market data receiver that drops packets under a burst is usually a
    // receiver whose socket buffer is smaller than it believes, so this never
    // asserts on the value it got. The caller reads rcvbuf() back and reports
    // the real number.
    void set_rcvbuf(int bytes) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != 0) {
            throw_errno("SO_RCVBUF");
        }
    }

    [[nodiscard]] int rcvbuf() const {
        int       value = 0;
        socklen_t len   = sizeof(value);
        if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, &len) != 0) {
            throw_errno("getsockopt SO_RCVBUF");
        }
        return value;
    }

    // SO_REUSEADDR lets a second receiver bind the same port, which is how two
    // processes both subscribe to one feed. SO_REUSEPORT is needed in addition
    // on the BSD sockets in macOS for that to work, and exists on Linux since
    // 3.9 where it additionally load balances unicast. It is requested where it
    // exists and its absence is not an error.
    // A receive timeout, which is what makes a blocking receiver terminable.
    //
    // Without it a blocking recv on a feed that has stopped waits forever, so
    // the process cannot notice the feed went quiet and cannot shut itself
    // down. A real blocking receiver has exactly this problem and solves it the
    // same way. It is not a latency mechanism and it does not fire on the hot
    // path, because on a live feed the next datagram always arrives first.
    void set_rcvtimeo(int milliseconds) {
        timeval tv{};
        tv.tv_sec  = milliseconds / 1000;
        tv.tv_usec = (milliseconds % 1000) * 1000;
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            throw std::runtime_error(std::string("SO_RCVTIMEO: ") + std::strerror(errno));
        }
    }

    void set_reuse() {
        const int on = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
            throw_errno("SO_REUSEADDR");
        }
#if defined(SO_REUSEPORT)
        // Not fatal. Some kernels compile it out.
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    }

    // Turn on kernel receive timestamping. Returns false when the platform
    // cannot do it, and a caller that gets false must timestamp in userspace
    // after recv returns and say so in its results table, because a userspace
    // timestamp includes the syscall return path and a kernel one does not.
    // Reporting the two as the same measurement would overstate the kernel path
    // and understate nothing, which is the wrong direction to be wrong in.
    bool enable_rx_timestamping() {
#if defined(__linux__)
        // Software receive timestamps, taken in the kernel as the packet is
        // handed to the socket. RX_SOFTWARE says when to take one and SOFTWARE
        // says to report it. Both are required, asking for only one silently
        // yields nothing.
        const int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0) {
            ts_mode_ = TsMode::Kernel;
            return true;
        }
        // Older kernels and some containers refuse SO_TIMESTAMPING but allow
        // the nanosecond variant of the older interface. Still a kernel
        // timestamp, still on the realtime clock.
        const int on = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) == 0) {
            ts_mode_ = TsMode::Kernel;
            return true;
        }
        ts_mode_ = TsMode::None;
        return false;
#else
        // macOS. SO_TIMESTAMP gives a struct timeval, so microseconds. That is
        // coarse next to the nanoseconds Linux reports and it is the reason the
        // headline timestamping numbers in this project have to come from
        // Linux.
        const int on = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on)) == 0) {
            ts_mode_ = TsMode::Kernel;
            return true;
        }
        ts_mode_ = TsMode::None;
        return false;
#endif
    }

    [[nodiscard]] bool has_kernel_timestamping() const noexcept {
        return ts_mode_ == TsMode::Kernel;
    }

    ssize_t send_to(std::span<const std::byte> buf, const sockaddr_in& dst) noexcept {
        return ::sendto(fd_, buf.data(), buf.size(), 0,
                        reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    }

    // One datagram. kernel_ts_ns is filled with the kernel receive timestamp on
    // the monotonic base when timestamping is on and the kernel attached one,
    // and with 0 otherwise. It is never a guess.
    //
    // recvmsg is used only when a timestamp is wanted, because the control
    // message plumbing costs something and the strategies that do not ask for a
    // timestamp should not pay for it.
    ssize_t recv_one(std::span<std::byte> buf, uint64_t* kernel_ts_ns = nullptr) noexcept {
        if (kernel_ts_ns != nullptr) *kernel_ts_ns = 0;

        if (ts_mode_ != TsMode::Kernel || kernel_ts_ns == nullptr) {
            return ::recv(fd_, buf.data(), buf.size(), 0);
        }

        iovec iov{};
        iov.iov_base = buf.data();
        iov.iov_len  = buf.size();

        alignas(cmsghdr) char control[detail::kControlLen];
        msghdr msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = control;
        msg.msg_controllen = static_cast<socklen_t>(sizeof(control));

        const ssize_t n = ::recvmsg(fd_, &msg, 0);
        if (n < 0) return n;
        *kernel_ts_ns = detail::rx_timestamp_ns(msg);
        return n;
    }

    // Build a destination address. Throws on a malformed address, because this
    // is called at configuration time and not on the send path.
    static sockaddr_in endpoint(const std::string& addr, uint16_t port) {
        sockaddr_in out{};
        out.sin_family = AF_INET;
        out.sin_port   = htons(port);
        if (::inet_pton(AF_INET, addr.c_str(), &out.sin_addr) != 1) {
            throw std::runtime_error("not a valid IPv4 address: " + addr);
        }
        return out;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        ts_mode_ = TsMode::None;
    }

private:
    enum class TsMode : uint8_t { None, Kernel };

    explicit UdpSocket(int fd) noexcept : fd_(fd) {}

    static int make_fd() {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) throw_errno("socket(AF_INET, SOCK_DGRAM)");
        return fd;
    }

    static in_addr_t parse_iface(const std::string& iface) {
        if (iface.empty()) return htonl(INADDR_ANY);
        in_addr a{};
        if (::inet_pton(AF_INET, iface.c_str(), &a) != 1) {
            throw std::runtime_error("interface must be a local IPv4 address, got: " + iface);
        }
        return a.s_addr;
    }

    [[noreturn]] static void throw_errno(const std::string& what) {
        throw std::runtime_error(what + ": " + std::strerror(errno));
    }

    int    fd_      = -1;
    TsMode ts_mode_ = TsMode::None;
};

} // namespace tick
