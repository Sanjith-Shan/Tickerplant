#pragma once

#include "tick/udp_socket.hpp"

#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#define TICK_HAVE_KQUEUE 1
#endif

#if defined(TICK_HAVE_IO_URING)
#include <liburing.h>
#endif

// The receive path shootout.
//
// Getting a datagram from the kernel into this process is, on a feed that is
// mostly small packets, a large fraction of the total cost of handling it. How
// large depends entirely on which system call is used and how many datagrams it
// returns per call. That is a measurable claim, so this header holds five ways
// of doing it behind one interface and the benchmark runs them all over the
// same traffic.
//
// The interface is a concept and not a virtual base class. The entire point of
// the exercise is to measure what one receive costs, and an indirect call
// through a vtable in the middle of the measurement would add a cost of its own
// and block the inlining that makes the cheap strategies cheap. With a concept
// the benchmark instantiates each strategy separately, and what gets measured
// is the system call and nothing else.
//
// Availability is a compile-time constant per strategy rather than a runtime
// check, because a strategy that does not exist on this platform should be
// visible as absent in the results table rather than silently substituted. A
// strategy that is unavailable still compiles and its receive() returns -1 with
// errno set to ENOTSUP. Nothing is emulated. A recvmmsg number on macOS would
// be a number for something that is not recvmmsg.
//
// Which table is the headline. The Linux table is. epoll plus recvmmsg is what
// a real handler runs on a real venue link, and kqueue is here so that the
// development machine can run a three way comparison locally without pretending
// that comparison is the result. Any macOS numbers in this repository are
// labelled as macOS numbers.
//
// Every strategy fills RxDatagram::kernel_ts_ns from the socket's timestamping
// when the kernel supplied one, and leaves it 0 when it did not. A strategy
// never substitutes a userspace reading for a kernel one, because that would
// make a table comparing kernel timestamping against userspace timestamping
// compare nothing.
//
// Buffers are allocated once at construction. A per-call allocation on the
// receive path would dominate exactly the difference being measured.

namespace tick {

// One received datagram. data points into the strategy's own buffer and stays
// valid until the next call to receive() on that strategy.
struct RxDatagram {
    const std::byte* data          = nullptr;
    std::size_t      len           = 0;
    uint64_t         kernel_ts_ns  = 0;   // 0 means no kernel timestamp
};

// clang-format off
template <typename R>
concept ReceiveStrategy = requires(R r, RxDatagram* out, int max) {
    { r.receive(out, max) } -> std::same_as<int>;   // count received, 0 when nothing ready, -1 on error
    { R::kName }      -> std::convertible_to<std::string_view>;
    { R::kAvailable } -> std::convertible_to<bool>;
};
// clang-format on

// A datagram buffer sized for one MTU with room to spare. Jumbo frames are not
// a thing on any market data feed this targets, and a datagram longer than this
// is truncated by the kernel rather than corrupting anything.
inline constexpr std::size_t kRxBufferLen = 2048;

// ---------------------------------------------------------------------------
// 1. Blocking recv
// ---------------------------------------------------------------------------

// The naive baseline, and the thing every other row in the table is compared
// against. One system call, one datagram, and the thread sleeps in the kernel
// between packets. It costs a context switch per datagram on a quiet feed and
// it costs nothing in CPU when there is no traffic, which is why it is what a
// first implementation reaches for and why it is the right zero point.
class BlockingRecvStrategy {
public:
    static constexpr std::string_view kName      = "blocking-recv";
    static constexpr bool             kAvailable = true;

    explicit BlockingRecvStrategy(UdpSocket& sock, std::size_t buflen = kRxBufferLen)
        : sock_(&sock), buf_(buflen) {
        sock_->set_nonblocking(false);
    }

    int receive(RxDatagram* out, int max) noexcept {
        if (max < 1) return 0;
        uint64_t      ts = 0;
        const ssize_t n  = sock_->recv_one({buf_.data(), buf_.size()},
                                           sock_->has_kernel_timestamping() ? &ts : nullptr);
        if (n < 0) {
            // A blocking socket can still return EAGAIN when a receive timeout
            // is set, and EINTR on any signal. Neither is an error worth
            // failing the run over.
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        out[0] = RxDatagram{buf_.data(), static_cast<std::size_t>(n), ts};
        return 1;
    }

private:
    UdpSocket*             sock_;
    std::vector<std::byte> buf_;
};

// ---------------------------------------------------------------------------
// 2. Busy poll
// ---------------------------------------------------------------------------

// A non-blocking socket spun on until a datagram appears.
//
// What it buys is the removal of the wakeup. The thread is already running and
// already in cache when the packet lands, so there is no scheduler latency and
// no cold restart, and the tail of the latency distribution is where that shows
// up rather than the median.
//
// What it costs is a core, completely, forever, whether or not anything is
// arriving. On a sixteen core box running four feeds that is a quarter of the
// machine spent on doing nothing most of the day, plus the thermal headroom the
// neighbouring cores no longer have. It is the right answer for a latency
// sensitive handler pinned to an isolated core and the wrong answer for
// anything else, and a benchmark that shows it winning without saying what it
// spent is not telling the whole story.
//
// The spin is bounded and receive() returns 0 when the budget runs out. An
// unbounded spin inside receive() would never give the caller back control, and
// the caller is the thing that owns the decision to keep spinning.
class BusyPollStrategy {
public:
    static constexpr std::string_view kName      = "busy-poll";
    static constexpr bool             kAvailable = true;

    explicit BusyPollStrategy(UdpSocket& sock, int spin_budget = 1024,
                              std::size_t buflen = kRxBufferLen)
        : sock_(&sock), buf_(buflen), spin_budget_(spin_budget) {
        sock_->set_nonblocking(true);
    }

    int receive(RxDatagram* out, int max) noexcept {
        if (max < 1) return 0;
        const bool want_ts = sock_->has_kernel_timestamping();

        for (int spin = 0; spin < spin_budget_; ++spin) {
            uint64_t      ts = 0;
            const ssize_t n  = sock_->recv_one({buf_.data(), buf_.size()}, want_ts ? &ts : nullptr);
            if (n >= 0) {
                out[0] = RxDatagram{buf_.data(), static_cast<std::size_t>(n), ts};
                return 1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                spin_pause();
                continue;
            }
            return -1;
        }
        return 0;
    }

private:
    // Tell the core this is a spin loop. On x86 this is the PAUSE instruction,
    // which lets a hyperthreaded sibling have the pipeline and drops the power
    // draw. On arm64 YIELD is the equivalent hint. Neither changes correctness.
    static void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#endif
    }

    UdpSocket*             sock_;
    std::vector<std::byte> buf_;
    int                    spin_budget_;
};

// ---------------------------------------------------------------------------
// 3. recvmmsg
// ---------------------------------------------------------------------------

// Linux recvmmsg, which returns up to N datagrams from one system call.
//
// This is the row the whole table exists to produce. On a burst, the per
// datagram cost of entering and leaving the kernel is amortised across the
// whole batch, and the deeper the queue is when the call is made the better the
// amortisation gets. On an idle feed it degrades to exactly one datagram per
// call and behaves like blocking recv.
//
// macOS has no recvmmsg. This class still compiles there, kAvailable is false,
// and receive() returns -1 with ENOTSUP. It is not emulated with a loop over
// recv, because a loop over recv is a measurement of a loop over recv.
class RecvmmsgStrategy {
public:
    static constexpr std::string_view kName = "recvmmsg";
#if defined(__linux__)
    static constexpr bool kAvailable = true;
#else
    static constexpr bool kAvailable = false;
#endif

    static constexpr int kDefaultBatch = 32;

    explicit RecvmmsgStrategy(UdpSocket& sock, int batch = kDefaultBatch,
                              std::size_t buflen = kRxBufferLen)
        : sock_(&sock), batch_(batch < 1 ? 1 : batch), buflen_(buflen) {
        storage_.resize(static_cast<std::size_t>(batch_) * buflen_);
#if defined(__linux__)
        msgs_.resize(static_cast<std::size_t>(batch_));
        iovs_.resize(static_cast<std::size_t>(batch_));
        control_.resize(static_cast<std::size_t>(batch_) * detail::kControlLen);
#endif
    }

    RecvmmsgStrategy(const RecvmmsgStrategy&)            = delete;
    RecvmmsgStrategy& operator=(const RecvmmsgStrategy&) = delete;

    int receive(RxDatagram* out, int max) noexcept {
#if defined(__linux__)
        if (max < 1) return 0;
        const int want = max < batch_ ? max : batch_;
        rearm(want);

        const int n = ::recvmmsg(sock_->fd(), msgs_.data(), static_cast<unsigned>(want),
                                 0, nullptr);
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        for (int i = 0; i < n; ++i) {
            out[i] = RxDatagram{slot(i), static_cast<std::size_t>(msgs_[i].msg_len),
                                sock_->has_kernel_timestamping()
                                    ? detail::rx_timestamp_ns(msgs_[i].msg_hdr)
                                    : 0};
        }
        return n;
#else
        (void)out;
        (void)max;
        errno = ENOTSUP;
        return -1;
#endif
    }

    [[nodiscard]] int batch() const noexcept { return batch_; }
    [[nodiscard]] UdpSocket& socket() const noexcept { return *sock_; }
    [[nodiscard]] std::size_t buffer_len() const noexcept { return buflen_; }

private:
    [[nodiscard]] std::byte* slot(int i) noexcept {
        return storage_.data() + static_cast<std::size_t>(i) * buflen_;
    }

#if defined(__linux__)
    // The kernel writes msg_len and msg_controllen on every call, so the
    // descriptors have to be reset before each one. Resetting only the fields
    // the kernel touches, rather than rebuilding the arrays, keeps this off the
    // measured path as much as it can be.
    void rearm(int want) noexcept {
        for (int i = 0; i < want; ++i) {
            iovs_[static_cast<std::size_t>(i)].iov_base = slot(i);
            iovs_[static_cast<std::size_t>(i)].iov_len  = buflen_;

            msghdr& h      = msgs_[static_cast<std::size_t>(i)].msg_hdr;
            h              = msghdr{};
            h.msg_iov      = &iovs_[static_cast<std::size_t>(i)];
            h.msg_iovlen   = 1;
            h.msg_control  = control_.data() + static_cast<std::size_t>(i) * detail::kControlLen;
            h.msg_controllen = static_cast<socklen_t>(detail::kControlLen);
            msgs_[static_cast<std::size_t>(i)].msg_len = 0;
        }
    }

    std::vector<mmsghdr> msgs_;
    std::vector<iovec>   iovs_;
    std::vector<char>    control_;
#endif

    UdpSocket*             sock_;
    int                    batch_;
    std::size_t            buflen_;
    std::vector<std::byte> storage_;
};

// ---------------------------------------------------------------------------
// 4. epoll plus recvmmsg
// ---------------------------------------------------------------------------

// What a real handler runs. epoll_wait blocks until the socket has something,
// then recvmmsg drains whatever accumulated in one call.
//
// Against plain blocking recvmmsg this adds a system call per wakeup and buys
// the ability to wait on more than one socket at a time, which every real
// deployment needs because a feed is at least two multicast groups plus the
// recovery socket. Against busy polling it gives the core back.
//
// The socket is put in non-blocking mode so the recvmmsg after a wakeup drains
// and returns rather than blocking on the last descriptor in the batch. That
// detail is the usual bug in this pattern.
class EpollRecvmmsgStrategy {
public:
    static constexpr std::string_view kName = "epoll+recvmmsg";
#if defined(__linux__)
    static constexpr bool kAvailable = true;
#else
    static constexpr bool kAvailable = false;
#endif

    static constexpr int kDefaultBatch = 32;

    explicit EpollRecvmmsgStrategy(UdpSocket& sock, int batch = kDefaultBatch,
                                   int timeout_ms = 10, std::size_t buflen = kRxBufferLen)
        : inner_(sock, batch, buflen), sock_(&sock), timeout_ms_(timeout_ms) {
#if defined(__linux__)
        sock_->set_nonblocking(true);
        ep_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (ep_ < 0) throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));

        epoll_event ev{};
        // Level triggered on purpose. Edge triggered would require draining to
        // EAGAIN before the next wait, and a bounded batch cannot promise that,
        // so edge triggered here would quietly stall behind a deep queue.
        ev.events  = EPOLLIN;
        ev.data.fd = sock_->fd();
        if (::epoll_ctl(ep_, EPOLL_CTL_ADD, sock_->fd(), &ev) != 0) {
            const int e = errno;
            ::close(ep_);
            ep_ = -1;
            throw std::runtime_error(std::string("epoll_ctl ADD: ") + std::strerror(e));
        }
#endif
    }

    ~EpollRecvmmsgStrategy() {
#if defined(__linux__)
        if (ep_ >= 0) ::close(ep_);
#endif
    }

    EpollRecvmmsgStrategy(const EpollRecvmmsgStrategy&)            = delete;
    EpollRecvmmsgStrategy& operator=(const EpollRecvmmsgStrategy&) = delete;

    int receive(RxDatagram* out, int max) noexcept {
#if defined(__linux__)
        if (max < 1) return 0;
        epoll_event ev{};
        const int   n = ::epoll_wait(ep_, &ev, 1, timeout_ms_);
        if (n < 0) return (errno == EINTR) ? 0 : -1;
        if (n == 0) return 0;   // timed out, nothing ready
        return inner_.receive(out, max);
#else
        (void)out;
        (void)max;
        errno = ENOTSUP;
        return -1;
#endif
    }

    [[nodiscard]] int batch() const noexcept { return inner_.batch(); }
    [[nodiscard]] UdpSocket& socket() const noexcept { return *sock_; }
    [[nodiscard]] int timeout_ms() const noexcept { return timeout_ms_; }

private:
    RecvmmsgStrategy inner_;
    UdpSocket*       sock_;
    int              timeout_ms_;
#if defined(__linux__)
    int ep_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// 5. kqueue plus recv
// ---------------------------------------------------------------------------

// The BSD and macOS readiness interface. kevent reports how many bytes are
// queued on the socket, then recv takes one datagram.
//
// This strategy exists so that the development machine can run a three way
// comparison locally against blocking recv and busy poll, which is a useful
// sanity check while writing the code. It is not the headline result. There is
// no batching system call here because BSD has no recvmmsg, so the shape of
// this row is structurally different from the Linux batching rows and the two
// tables should not be read as one.
class KqueueStrategy {
public:
    static constexpr std::string_view kName = "kqueue+recv";
#if defined(TICK_HAVE_KQUEUE)
    static constexpr bool kAvailable = true;
#else
    static constexpr bool kAvailable = false;
#endif

    explicit KqueueStrategy(UdpSocket& sock, int timeout_ms = 10,
                            std::size_t buflen = kRxBufferLen)
        : sock_(&sock), buf_(buflen), timeout_ms_(timeout_ms) {
#if defined(TICK_HAVE_KQUEUE)
        sock_->set_nonblocking(true);
        kq_ = ::kqueue();
        if (kq_ < 0) throw std::runtime_error(std::string("kqueue: ") + std::strerror(errno));

        struct kevent ev {};
        EV_SET(&ev, sock_->fd(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
            const int e = errno;
            ::close(kq_);
            kq_ = -1;
            throw std::runtime_error(std::string("kevent EV_ADD: ") + std::strerror(e));
        }
#endif
    }

    ~KqueueStrategy() {
#if defined(TICK_HAVE_KQUEUE)
        if (kq_ >= 0) ::close(kq_);
#endif
    }

    KqueueStrategy(const KqueueStrategy&)            = delete;
    KqueueStrategy& operator=(const KqueueStrategy&) = delete;

    int receive(RxDatagram* out, int max) noexcept {
#if defined(TICK_HAVE_KQUEUE)
        if (max < 1) return 0;

        struct kevent ev {};
        timespec      ts{};
        ts.tv_sec  = timeout_ms_ / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms_ % 1000) * 1000000L;

        const int n = ::kevent(kq_, nullptr, 0, &ev, 1, &ts);
        if (n < 0) return (errno == EINTR) ? 0 : -1;
        if (n == 0) return 0;

        uint64_t      tstamp = 0;
        const ssize_t got    = sock_->recv_one({buf_.data(), buf_.size()},
                                               sock_->has_kernel_timestamping() ? &tstamp : nullptr);
        if (got < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        out[0] = RxDatagram{buf_.data(), static_cast<std::size_t>(got), tstamp};
        return 1;
#else
        (void)out;
        (void)max;
        errno = ENOTSUP;
        return -1;
#endif
    }

    [[nodiscard]] UdpSocket& socket() const noexcept { return *sock_; }
    [[nodiscard]] int timeout_ms() const noexcept { return timeout_ms_; }
    [[nodiscard]] std::size_t buffer_len() const noexcept { return buf_.size(); }

private:
    UdpSocket*             sock_;
    std::vector<std::byte> buf_;
    int                    timeout_ms_;
#if defined(TICK_HAVE_KQUEUE)
    int kq_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// 6. io_uring, optional
// ---------------------------------------------------------------------------

// Behind TICK_HAVE_IO_URING because liburing is not present on every machine
// and a header that includes it unconditionally makes the whole project
// unbuildable on the ones where it is not. With the guard off this class still
// exists, kAvailable is false, and receive() returns ENOTSUP, so the benchmark
// table has a row for it that honestly says it was not measured here.
//
// The interesting property is that a batch of receives is submitted once and
// completions are reaped without a system call per datagram, so the comparison
// against recvmmsg is about submission cost rather than about batching.
class IoUringStrategy {
public:
    static constexpr std::string_view kName = "io_uring";
#if defined(TICK_HAVE_IO_URING)
    static constexpr bool kAvailable = true;
#else
    static constexpr bool kAvailable = false;
#endif

    static constexpr int kDefaultDepth = 32;

    explicit IoUringStrategy(UdpSocket& sock, int depth = kDefaultDepth,
                             std::size_t buflen = kRxBufferLen)
        : sock_(&sock), depth_(depth < 1 ? 1 : depth), buflen_(buflen) {
        storage_.resize(static_cast<std::size_t>(depth_) * buflen_);
#if defined(TICK_HAVE_IO_URING)
        iovs_.resize(static_cast<std::size_t>(depth_));
        msgs_.resize(static_cast<std::size_t>(depth_));
        control_.resize(static_cast<std::size_t>(depth_) * detail::kControlLen);
        inflight_.assign(static_cast<std::size_t>(depth_), 0);
        handed_out_.assign(static_cast<std::size_t>(depth_), 0);
        if (io_uring_queue_init(static_cast<unsigned>(depth_), &ring_, 0) < 0) {
            throw std::runtime_error("io_uring_queue_init failed");
        }
        ring_up_ = true;
        arm_free_slots();
#endif
    }

    ~IoUringStrategy() {
#if defined(TICK_HAVE_IO_URING)
        if (ring_up_) io_uring_queue_exit(&ring_);
#endif
    }

    IoUringStrategy(const IoUringStrategy&)            = delete;
    IoUringStrategy& operator=(const IoUringStrategy&) = delete;

    int receive(RxDatagram* out, int max) noexcept {
#if defined(TICK_HAVE_IO_URING)
        if (max < 1) return 0;

        // Re-arm the slots handed out on the previous call, now that the caller
        // has finished with them.
        //
        // This is the ownership rule and the first version of this file got it
        // wrong. It armed slots at the end of receive, which handed the kernel
        // the very buffers it had just returned to the caller, so the caller
        // was reading a buffer the kernel was free to overwrite. The bug was
        // invisible on the development machine because this code had never once
        // been compiled there, macOS having no io_uring.
        release_handed_out();

        // Peek before waiting.
        //
        // io_uring_wait_cqe_timeout is a system call into io_uring_enter even
        // when completions are already sitting in the ring, and on a busy feed
        // they almost always are. Peeking first reads the completion queue head
        // straight out of the shared mapping, which is a load and not a
        // syscall, and that is the whole reason the ring exists. Waiting only
        // when the ring is genuinely empty was worth most of this strategy's
        // latency.
        io_uring_cqe* cqe = nullptr;
        if (io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) {
            // Nothing ready. Wait with a timeout rather than forever, because a
            // blocking wait never returns when the feed stops, so the process
            // could not notice it had gone quiet and could not shut down.
            __kernel_timespec ts{};
            ts.tv_sec  = 0;
            ts.tv_nsec = 500 * 1000; // 500 microseconds

            const int w = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (w == -ETIME || w == -EAGAIN || w == -EINTR) return 0;
            if (w < 0) {
                errno = -w;
                return -1;
            }
        }

        // Reap one at a time rather than with io_uring_for_each_cqe, so a
        // completion is only advanced past once its buffer has been recorded.
        int count = 0;
        while (count < max) {
            if (io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) break;

            const uint64_t tag = io_uring_cqe_get_data64(cqe);
            const int      res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);

            if (tag >= static_cast<uint64_t>(depth_)) continue;
            const std::size_t u = static_cast<std::size_t>(tag);

            if (res < 0) {
                // This receive failed. The slot carries nothing, so it goes
                // straight back into the pool rather than to the caller.
                inflight_[u] = 0;
                continue;
            }

            out[count] = RxDatagram{slot(static_cast<int>(u)),
                                    static_cast<std::size_t>(res),
                                    sock_->has_kernel_timestamping()
                                        ? detail::rx_timestamp_ns(msgs_[u])
                                        : 0};
            // Still counted as in flight, because the caller now owns the
            // buffer and it must not be armed again until the next call.
            handed_out_[handed_count_++] = static_cast<int>(u);
            ++count;
        }

        // Arm whatever is genuinely free, which is every slot except the ones
        // just given to the caller.
        arm_free_slots();
        return count;
#else
        (void)out;
        (void)max;
        errno = ENOTSUP;
        return -1;
#endif
    }

    [[nodiscard]] int depth() const noexcept { return depth_; }
    [[nodiscard]] UdpSocket& socket() const noexcept { return *sock_; }
    [[nodiscard]] std::size_t buffer_len() const noexcept { return buflen_; }

private:
    [[nodiscard]] std::byte* slot(int i) noexcept {
        return storage_.data() + static_cast<std::size_t>(i) * buflen_;
    }

#if defined(TICK_HAVE_IO_URING)
    // Keep the ring full. Every slot whose completion has been reaped is armed
    // again, so the kernel always has somewhere to put the next datagram and
    // the receive path never has to submit and wait in the same breath. Only
    // free slots are armed, because arming a slot whose receive is still in
    // flight would hand the kernel a buffer it is already writing into.
    // Give back the buffers the caller was handed last time.
    void release_handed_out() noexcept {
        for (int i = 0; i < handed_count_; ++i) {
            inflight_[static_cast<std::size_t>(handed_out_[i])] = 0;
        }
        handed_count_ = 0;
    }

    void arm_free_slots() noexcept {
        bool submitted = false;
        for (int i = 0; i < depth_; ++i) {
            const std::size_t u = static_cast<std::size_t>(i);
            if (inflight_[u] != 0) continue;

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr) break;

            iovs_[u].iov_base = slot(i);
            iovs_[u].iov_len  = buflen_;

            msgs_[u]                = msghdr{};
            msgs_[u].msg_iov        = &iovs_[u];
            msgs_[u].msg_iovlen     = 1;
            msgs_[u].msg_control    = control_.data() + u * detail::kControlLen;
            msgs_[u].msg_controllen = static_cast<socklen_t>(detail::kControlLen);

            io_uring_prep_recvmsg(sqe, sock_->fd(), &msgs_[u], 0);
            io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(i));
            inflight_[u] = 1;
            submitted    = true;
        }
        if (submitted) io_uring_submit(&ring_);
    }

    io_uring                  ring_{};
    bool                      ring_up_ = false;
    std::vector<iovec>        iovs_;
    std::vector<msghdr>       msgs_;
    std::vector<char>         control_;
    std::vector<unsigned char> inflight_;
    std::vector<int>          handed_out_;   // slots the caller currently owns
    int                       handed_count_ = 0;
#endif

    UdpSocket*             sock_;
    int                    depth_;
    std::size_t            buflen_;
    std::vector<std::byte> storage_;
};

// Proof that every strategy really does model the interface, checked by the
// compiler rather than by the benchmark failing to build later. The unavailable
// ones are included deliberately, because an unavailable strategy still has to
// present the same shape for the results table to have a row for it.
static_assert(ReceiveStrategy<BlockingRecvStrategy>);
static_assert(ReceiveStrategy<BusyPollStrategy>);
static_assert(ReceiveStrategy<RecvmmsgStrategy>);
static_assert(ReceiveStrategy<EpollRecvmmsgStrategy>);
static_assert(ReceiveStrategy<KqueueStrategy>);
static_assert(ReceiveStrategy<IoUringStrategy>);

} // namespace tick
