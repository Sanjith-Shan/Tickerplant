#pragma once

#include <cerrno>

#if defined(__linux__)
#  include <sched.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  include <pthread.h>
#elif defined(__APPLE__)
#  include <cstdint>
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#  include <mach/thread_act.h>
#  include <mach/thread_policy.h>
#  include <pthread.h>
#  include <sys/mman.h>
#  include <sys/sysctl.h>
#endif

// Thread placement, and a refusal to pretend it happened when it did not.
//
// The single most damaging thing this repository could contain is a latency
// table with a tight p99 and the word "pinned" next to it, measured on a Mac.
// It would be false, an interviewer at any of the firms this project is aimed
// at would know it was false within one question, and everything else in the
// repository would then be suspect. So this header's job is not really to pin
// threads. Its job is to report, truthfully and in a form a results table can
// print, what the operating system actually agreed to do.
//
// WHY PINNING MATTERS TO THE NUMBERS
//
// An unpinned thread migrates between cores. Every migration is a cold L1 and
// L2 on the new core, a set of remote cache line fetches, and on a multi socket
// box possibly a different NUMA node for memory the thread allocated earlier.
// Migrations are not rare and they are not uniform, so they land in the tail.
// A p99 measured on a migrating thread is measuring the scheduler at least as
// much as it is measuring the code. That is why the tail number is the one that
// moves when pinning is turned on, and why an unpinned p99 is a weaker claim
// than a pinned one rather than merely a different one.
//
// WHAT LINUX GIVES AND WHAT MACOS DOES NOT
//
// On Linux, sched_setaffinity restricts a thread to a CPU mask and the kernel
// honours it. Combined with isolcpus and nohz_full on the boot line, that core
// runs essentially nothing else and the tail collapses. That is a real pin.
//
// On macOS there is no way to pin a thread to a specific core. None. The
// closest facility is thread_policy_set with THREAD_AFFINITY_POLICY, and it is
// worth being exact about what that is, because its name invites the wrong
// conclusion. It takes an integer tag, not a core number. It expresses a wish
// that threads sharing a tag be scheduled so as to share an L2 cache, and that
// threads with different tags be kept apart. It is advisory. The scheduler may
// ignore it entirely, it says nothing about which core is chosen, and it does
// not prevent migration. On Apple Silicon it is additionally not implemented,
// because the scheduler owns the performance and efficiency core split and does
// not expose it. Measured rather than assumed, macos_affinity_hint below
// returned KERN_NOT_SUPPORTED, which is 46, on the Apple M3 Pro running macOS
// 26.5.1 that this was written on. So on that machine it is not even a weak
// hint, it is a refusal.
//
// Therefore pin_to_core returns false on macOS. Always. Not "best effort true".
// The Mac is a correctness and development machine for this project and the
// pinned numbers come from the Linux box, and the results table says which is
// which by printing the PinningReport rather than the request.

namespace tick {

// What was asked for and what was actually achieved. Every results table prints
// this rather than the configuration, because the configuration is a wish.
struct PinningReport {
    bool        requested = false;
    bool        achieved  = false;
    int         core      = -1;
    const char* reason    = "not attempted";
};

// Restrict the calling thread to one core. True only if the operating system
// actually restricted it.
[[nodiscard]] inline bool pin_to_core(int core) noexcept {
#if defined(__linux__)
    if (core < 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(core), &set);
    // pthread_setaffinity_np rather than sched_setaffinity with pid 0, because
    // the pthread form is unambiguous about applying to this thread and not to
    // the whole process, which matters once there is a receive thread and a
    // book thread.
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false;
#endif
}

// Attempt a pin and describe the outcome. This is the entry point benchmarks
// should use, because it carries the reason forward into the results file.
[[nodiscard]] inline PinningReport pin_to_core_reported(int core) noexcept {
    PinningReport r;
    r.requested = true;
    r.core      = core;
#if defined(__linux__)
    r.achieved = pin_to_core(core);
    r.reason   = r.achieved ? "sched affinity set"
                            : "sched_setaffinity refused, core may be offline or outside the cpuset";
#elif defined(__APPLE__)
    r.achieved = false;
    r.core     = -1;
    r.reason   = "macOS has no per core thread pinning, THREAD_AFFINITY_POLICY is an L2 sharing hint only";
#else
    r.achieved = false;
    r.core     = -1;
    r.reason   = "no pinning facility known for this platform";
#endif
    return r;
}

#if defined(__APPLE__)
// The macOS hint, offered for completeness and never counted as a pin.
//
// Threads given the same non zero tag are preferred to share an L2. Threads
// given tag zero are explicitly unhinted. It returns the kernel status so a
// caller can see for itself that on Apple Silicon this does not succeed, rather
// than reading that claim here and taking it on trust.
inline kern_return_t macos_affinity_hint(int tag) noexcept {
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = tag;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_AFFINITY_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_AFFINITY_POLICY_COUNT);
}
#endif

// Which core the calling thread is on right now, or -1 where the platform will
// not say.
//
// On Linux this is a snapshot and nothing more. By the time the caller reads
// the return value the thread may have moved, unless it is pinned. It is useful
// for confirming that a pin took effect and useless for anything else.
[[nodiscard]] inline int current_core() noexcept {
#if defined(__linux__)
    const int c = sched_getcpu();
    return c < 0 ? -1 : c;
#else
    // macOS exposes no supported way for a thread to learn its current core.
    // Returning -1 rather than 0 so that "unknown" cannot be mistaken for
    // "core zero" in a results file.
    return -1;
#endif
}

// Ask the scheduler to stop preempting this thread for ordinary work.
//
// On Linux this is SCHED_FIFO at a mid priority. It needs CAP_SYS_NICE or root,
// so it fails for an unprivileged run and the caller has to cope with false.
// Mid priority rather than the maximum on purpose, because a runaway SCHED_FIFO
// thread at priority 99 can lock a core away from the kernel's own threads and
// wedge the box.
//
// On macOS the equivalent is the time constraint policy, which is a deadline
// scheduler interface built for audio. It is a genuine facility and not a
// placebo, so it is called here and its real result is returned. What it is not
// is SCHED_FIFO. It expresses a period, a compute budget and a deadline, the
// kernel may still demote a thread that consistently overruns its budget, and
// it does not stop migration between cores. Treat a true from this on macOS as
// "the scheduler was told the thread is latency sensitive" and never as "the
// thread cannot be preempted".
[[nodiscard]] inline bool set_realtime_priority() noexcept {
#if defined(__linux__)
    sched_param p{};
    p.sched_priority = 50;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
#elif defined(__APPLE__)
    // The units are mach absolute time ticks, which are not nanoseconds. The
    // timebase is queried rather than assumed, because on the Apple M3 Pro this
    // project develops on it reports numer 125 and denom 3, so a mach tick is
    // about 41.7 ns and hard coding nanoseconds would ask for a period forty
    // times too long. The policy below is a 1 ms period with a 500 us budget
    // and a 1 ms deadline. A caller with a different duty cycle should set its
    // own policy rather than reuse these numbers.
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const double ticks_per_ms = 1000000.0 * static_cast<double>(tb.denom) /
                                static_cast<double>(tb.numer);

    thread_time_constraint_policy_data_t policy;
    policy.period      = static_cast<uint32_t>(ticks_per_ms);
    policy.computation = static_cast<uint32_t>(ticks_per_ms * 0.5);
    policy.constraint  = static_cast<uint32_t>(ticks_per_ms);
    policy.preemptible = 0;

    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
    return false;
#endif
}

// Keep this process's pages resident so a page fault cannot appear in the tail.
//
// On Linux mlockall with MCL_CURRENT and MCL_FUTURE locks what is mapped now
// and everything mapped later. It needs CAP_IPC_LOCK or a raised RLIMIT_MEMLOCK
// and returns false without them. This is a real fix for a real tail artefact,
// because a minor fault costs microseconds and lands in exactly the percentile
// this project cares about.
//
// On macOS mlockall is declared and is not usefully implemented. It is called
// here so the return value is the truth of what happened on this machine rather
// than an assumption written into a comment, but the expected answer is false
// and a benchmark on macOS should assume its pages can be evicted. This is one
// of the reasons the pinned tail numbers come from Linux.
[[nodiscard]] inline bool lock_memory() noexcept {
#if defined(__linux__)
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#elif defined(__APPLE__)
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
    return false;
#endif
}

// True on the one platform where a pinned measurement is possible at all.
// Benchmarks use this to decide whether to label their output as pinned,
// instead of deciding from an uname string at the reporting layer where the
// decision is easy to get wrong.
[[nodiscard]] inline constexpr bool pinning_supported() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

} // namespace tick
