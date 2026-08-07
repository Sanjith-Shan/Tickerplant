#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__linux__)
#  include <time.h>
#elif defined(__APPLE__)
#  include <time.h>
#  include <mach/mach_time.h>
#endif

// Timestamping that is honest about which clock produced the number.
//
// A feed handler's whole claim is a latency distribution, so the clock is not
// plumbing, it is the instrument. Three decisions are made here and each one
// changes what the reported numbers mean.
//
// First, the wall reference is a RAW monotonic clock. CLOCK_MONOTONIC on Linux
// and the plain uptime clock on macOS are slewed by NTP, which means the kernel
// quietly stretches or compresses them so they agree with a time server. That
// is the right behaviour for a clock that answers "what time is it" and the
// wrong behaviour for a clock that answers "how long did that take". A latency
// measurement wants elapsed time, not corrected time, so this file uses
// CLOCK_MONOTONIC_RAW on Linux and CLOCK_UPTIME_RAW on macOS. The cost is that
// the number drifts against real time over hours. Over a microsecond it does
// not matter and over a benchmark run it is exactly what is wanted.
//
// Second, the hot path uses a raw cycle counter rather than calling the clock.
// clock_gettime is a vDSO call on Linux and a commpage read on macOS, so it is
// cheap by system call standards, but cheap here still means tens of
// nanoseconds. The thing being measured in this project is a decode step that
// is itself on the order of tens of nanoseconds, so timestamping with
// clock_gettime would put the instrument and the subject in the same order of
// magnitude. A counter read is a handful of cycles, which keeps the instrument
// an order of magnitude smaller than the subject, which is the only regime in
// which the measurement means anything.
//
// Third, converting counter ticks to nanoseconds requires a calibration, and a
// calibration is a claim about the platform. TscClock below states the terms of
// that claim rather than hiding it, and reports its own uncertainty so a
// results table can print it next to the latency numbers.

namespace tick {

// ---------------------------------------------------------------------------
// Wall reference
// ---------------------------------------------------------------------------

// Nanoseconds on a raw monotonic timebase. Not slewed, not settable, and with
// an arbitrary zero, so only differences are meaningful.
[[nodiscard]] inline uint64_t now_ns() noexcept {
#if defined(__linux__)
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
#elif defined(__APPLE__)
    // CLOCK_UPTIME_RAW is mach_absolute_time already scaled to nanoseconds and
    // it is the raw, unslewed timebase. The scaling is not free. Measured on
    // this project's Mac, an Apple M3 Pro on macOS 26.5.1, mach_timebase_info
    // reports numer 125 and denom 3, so the mach timebase runs at 24 MHz and
    // the kernel multiplies to reach nanoseconds. Use mach_timebase below to
    // check that on any other machine rather than assuming a one to one ratio.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    // Portable fallback. steady_clock is monotonic by contract but the standard
    // says nothing about slewing, so a platform landing here should be audited
    // before its numbers are published.
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

#if defined(__APPLE__)
// Exposed so a caller that wants the untouched mach timebase can check that
// CLOCK_UPTIME_RAW really is a pass through on this machine rather than
// assuming it. Returns numerator and denominator such that
// nanoseconds equals ticks times numer divided by denom.
inline mach_timebase_info_data_t mach_timebase() noexcept {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return tb;
}
#endif

// The kernel stamps received packets with the realtime clock, because that is
// the only clock two machines can agree on. Everything else in this project is
// on the monotonic base. This offset converts one to the other.
//
// Captured once per process and then reused. That is acceptable here because
// the offset only moves when NTP slews or steps the realtime clock, which is
// parts per million of drift over a benchmark run that lasts seconds. A step
// would matter, so a run that spans an NTP step is a run to discard rather than
// a reason to re-read the offset in the hot path. Re-reading it per packet
// would cost two clock reads per packet to chase a correction that is below the
// resolution of what is being measured.
[[nodiscard]] inline int64_t realtime_to_monotonic_offset_ns() noexcept {
    static const int64_t offset = [] {
        // Sandwiching one realtime read between two monotonic reads and taking
        // the midpoint is the right idea and it is not enough on its own,
        // because if the thread is descheduled inside that sandwich the
        // midpoint is wrong by however long it was away. On a busy machine
        // that is milliseconds, and it showed up as a flaky test that asserted
        // a kernel timestamp lands between a send and the return from recv.
        //
        // So take several samples and keep the one whose sandwich was tightest.
        // The shortest observed round trip is the sample least likely to
        // contain a scheduling gap, which is the same argument NTP uses for
        // preferring the lowest delay exchange. Sixteen samples costs about a
        // microsecond, once per process.
        int64_t  best_offset = 0;
        uint64_t best_span   = UINT64_MAX;

        for (int i = 0; i < 16; ++i) {
            const uint64_t m0 = now_ns();
#if defined(__linux__)
            timespec rt{};
            clock_gettime(CLOCK_REALTIME, &rt);
            const uint64_t r = static_cast<uint64_t>(rt.tv_sec) * 1000000000ULL +
                               static_cast<uint64_t>(rt.tv_nsec);
#else
            const uint64_t r = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
#endif
            const uint64_t m1 = now_ns();
            if (m1 < m0) continue; // a clock that went backwards is not a sample

            const uint64_t span = m1 - m0;
            if (span < best_span) {
                best_span = span;
                const uint64_t mid = m0 / 2 + m1 / 2 + (m0 % 2 + m1 % 2) / 2;
                best_offset = static_cast<int64_t>(r) - static_cast<int64_t>(mid);
            }
        }
        return best_offset;
    }();
    return offset;
}

// Convert a kernel SO_TIMESTAMP style realtime nanosecond value onto the
// monotonic base used everywhere else.
[[nodiscard]] inline uint64_t realtime_ns_to_monotonic(uint64_t realtime_ns) noexcept {
    return static_cast<uint64_t>(static_cast<int64_t>(realtime_ns) -
                                 realtime_to_monotonic_offset_ns());
}

// ---------------------------------------------------------------------------
// Cycle counter
// ---------------------------------------------------------------------------

// The cheapest timestamp the hardware offers.
//
// On x86_64 this is the time stamp counter. On a modern part it is invariant,
// meaning it ticks at a fixed rate regardless of the core's current frequency,
// so it is a clock rather than a cycle count despite the name.
//
// On arm64 this is cntvct_el0, the architectural virtual counter. Two things
// about it are worth stating plainly because they trip people up.
//
// It is not the core clock. A delta is not a count of CPU cycles and cannot be
// used to reason about instructions per cycle, whatever the name rdtsc
// suggests. Its rate is whatever cntfrq_el0 says, which is a per part property
// and is emphatically not the advertised GHz of the chip.
//
// That rate also sets the resolution, and it varies enough between parts that
// it has to be read rather than assumed. Measured on this project's Mac, an
// Apple M3 Pro on macOS 26.5.1, cntfrq_el0 reads 1000000000, so one tick is one
// nanosecond there. That is unusually fine for this register and nothing should
// be built on it, because an arm64 part whose counter runs far slower would put
// a single tick above the cost of a whole decode. This is why every interval in
// this project is timed as a batch and divided, which is correct at any counter
// rate, rather than timed per message, which is only correct at a fast one.
//
// Neither form is ordered against surrounding instructions. The processor is
// free to execute the read early or late relative to the code being measured.
// Use this form when the interval is long enough that a few tens of cycles of
// slop does not matter, which is the common case for a batch timing, and use
// rdtsc_serialized below when timing something short enough that it does.
[[nodiscard]] inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return now_ns();
#endif
}

// The same counter, fenced so that it cannot float past the work on either
// side of it.
//
// On x86 the fence is lfence, which waits for all prior loads to retire before
// the counter read issues. rdtscp is the other option and it also returns the
// core id, but it only orders what came before it, so pairing lfence with rdtsc
// is the form that brackets an interval cleanly on both ends.
//
// On arm64 the fence is isb, an instruction synchronisation barrier, which
// flushes the pipeline so the mrs cannot be hoisted above earlier work.
//
// The fence is not free. It costs tens of cycles on both architectures, which
// is the entire budget of a short measurement, so it is the wrong tool for
// timing a single message and the right tool for establishing that a loop of a
// thousand messages started and ended where the source says it did. When in
// doubt, time a batch with the unfenced form rather than a single item with the
// fenced one, because the fence perturbs what it measures.
[[nodiscard]] inline uint64_t rdtsc_serialized() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_lfence();
    const uint64_t v = __builtin_ia32_rdtsc();
    __builtin_ia32_lfence();
    return v;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("isb" ::: "memory");
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ __volatile__("isb" ::: "memory");
    return v;
#else
    return now_ns();
#endif
}

// The counter's nominal frequency in Hz if the architecture publishes one, and
// zero if it does not.
//
// arm64 publishes cntfrq_el0 and it is authoritative, so calibration there is a
// cross check rather than a necessity, and TscClock's test suite uses it as
// exactly that. x86_64 publishes nothing a user mode program can read
// portably. CPUID leaf 0x15 gives the ratio on some parts and
// is absent or wrong on others, and the model name string is a marketing
// number rather than the TSC rate, so this returns zero on x86 and the
// calibration below is the only source of truth there.
[[nodiscard]] inline uint64_t counter_frequency_hz() noexcept {
#if defined(__aarch64__)
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

// Turns counter ticks into nanoseconds, and says how much to trust the answer.
//
// Calibrating from a single pair of reads taken a fixed interval apart is the
// obvious approach and it is worse than it looks. One pair gives no way to tell
// a good measurement from one that was interrupted by a context switch, and a
// single scheduling delay lands entirely in the slope. So this takes several
// samples spread across the interval and fits a line by ordinary least squares.
// The fit gives the rate, and the spread of the residuals around the fit gives
// an honest uncertainty, reported as parts per million of the slope.
//
// What this cannot do is make a bad platform good. The fit assumes the counter
// rate was constant while it was being measured. On x86 that holds only if the
// part has an invariant TSC. Without it the TSC tracks the core clock, so
// frequency scaling, turbo, and deep C states all move the rate underneath the
// fit and the residual spread will not necessarily reveal it, because the rate
// can be locally stable and globally wrong. This is why the project's Linux
// benchmark box boots with intel_pstate=disable and the performance governor,
// and why every results table prints the calibration error alongside the
// latency figures. On arm64 the architectural counter is fixed rate by
// definition and this concern does not apply, which is one of the few places
// where the Mac is the easier measurement target.
//
// Construction sleeps, by default for 100 ms, so build one per process at
// startup and hold it. It is not something to construct inside a measurement.
class TscClock {
public:
    // interval is the total wall time spent calibrating. samples is how many
    // points the line is fitted through, so it must be at least three for the
    // residual based error estimate to mean anything.
    explicit TscClock(std::chrono::nanoseconds interval = std::chrono::milliseconds(100),
                      int samples = 8) {
        calibrate(interval, samples < 3 ? 3 : samples);
    }

    // Ticks to nanoseconds. Kept in double because a tick is a fraction of a
    // nanosecond on x86 and several nanoseconds on arm64, and integer scaling
    // that is correct for both needs a fixed point representation that buys
    // nothing at the call sites this project has, all of which are off the hot
    // path in the reporting step.
    [[nodiscard]] double cycles_to_ns(uint64_t cycles) const noexcept {
        return static_cast<double>(cycles) * ns_per_tick_;
    }

    [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }

    // Standard error of the fitted slope, expressed in parts per million of the
    // slope itself. This is the statistical uncertainty of the fit and nothing
    // more. It does not and cannot capture a counter whose rate changed in a
    // way the samples did not see.
    [[nodiscard]] double calibration_error_ppm() const noexcept { return error_ppm_; }

    // How long calibration actually took, which is not the requested interval
    // once the scheduler has had its say.
    [[nodiscard]] uint64_t calibration_span_ns() const noexcept { return span_ns_; }

    [[nodiscard]] int sample_count() const noexcept { return sample_count_; }

    // The nominal rate the architecture reports, or zero where there is none.
    // A results table that prints both this and 1/ns_per_tick lets a reader see
    // the calibration agreeing with the hardware instead of taking it on faith.
    [[nodiscard]] uint64_t nominal_frequency_hz() const noexcept {
        return counter_frequency_hz();
    }

private:
    void calibrate(std::chrono::nanoseconds interval, int samples) {
        struct Point { double mono; double tsc; };
        std::vector<Point> pts;
        pts.reserve(static_cast<std::size_t>(samples));

        const auto step = interval / (samples - 1);

        for (int i = 0; i < samples; ++i) {
            if (i > 0) std::this_thread::sleep_for(step);
            // Sandwich the counter read between two clock reads and take the
            // midpoint of the clock. This centres the counter sample inside the
            // clock interval instead of leaving it biased by the cost of one
            // clock call, which on a 100 ms fit is small but is free to remove.
            const uint64_t m0 = now_ns();
            const uint64_t t  = rdtsc_serialized();
            const uint64_t m1 = now_ns();
            pts.push_back(Point{
                (static_cast<double>(m0) + static_cast<double>(m1)) * 0.5,
                static_cast<double>(t)});
        }

        sample_count_ = samples;
        span_ns_ = static_cast<uint64_t>(pts.back().mono - pts.front().mono);

        // Shift to the first point so the doubles stay small. Raw monotonic
        // values are large enough that a naive fit loses precision in the sums.
        const double x0 = pts.front().mono;
        const double y0 = pts.front().tsc;

        double sx = 0.0, sy = 0.0;
        for (const auto& p : pts) { sx += p.mono - x0; sy += p.tsc - y0; }
        const double n    = static_cast<double>(samples);
        const double xbar = sx / n;
        const double ybar = sy / n;

        double sxx = 0.0, sxy = 0.0;
        for (const auto& p : pts) {
            const double dx = (p.mono - x0) - xbar;
            sxx += dx * dx;
            sxy += dx * ((p.tsc - y0) - ybar);
        }

        // Slope is ticks per nanosecond. A degenerate fit means the clock did
        // not move, which is a broken platform rather than a rate of zero, so
        // fall back to the architectural frequency where one exists.
        if (sxx <= 0.0 || sxy <= 0.0) {
            const uint64_t nominal = counter_frequency_hz();
            ns_per_tick_ = nominal ? 1e9 / static_cast<double>(nominal) : 1.0;
            error_ppm_   = -1.0;  // negative means the fit failed, never a real spread
            return;
        }

        const double slope = sxy / sxx;               // ticks per ns
        ns_per_tick_ = 1.0 / slope;

        // Residual standard error of the slope, then expressed in ppm.
        double ss_resid = 0.0;
        for (const auto& p : pts) {
            const double dx   = (p.mono - x0) - xbar;
            const double pred = ybar + slope * dx;
            const double r    = ((p.tsc - y0) - ybar) - (pred - ybar);
            ss_resid += r * r;
        }
        const double dof = n - 2.0;
        const double se  = (dof > 0.0) ? std::sqrt(ss_resid / dof / sxx) : 0.0;
        error_ppm_ = 1e6 * se / slope;
    }

    double   ns_per_tick_  = 1.0;
    double   error_ppm_    = 0.0;
    uint64_t span_ns_      = 0;
    int      sample_count_ = 0;
};

} // namespace tick
