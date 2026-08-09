#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Proof, rather than assertion, that the hot path does not allocate.
//
// "Zero allocation on the hot path" is a sentence on a resume and a sentence in
// a README, and on its own it is worth nothing, because it is unfalsifiable as
// written. The reader has no way to check it and the author has no way to know
// it stayed true after the last refactor. A std::string that slipped into a log
// line, a std::function that captured too much, a vector that outgrew its
// reserve, a std::regex constructed once per message. Each of those compiles,
// passes every correctness test, and destroys the tail.
//
// So the claim is converted into an assertion the test suite enforces. This
// header replaces the global operator new and operator delete with counting
// versions, and AllocGuard snapshots the counter across a scope. A test wraps
// the decode loop in a guard and fails the build if the count moved.
//
// A COUNTER NOBODY HAS PROVED CAN SEE AN ALLOCATION IS WORTH NOTHING.
//
// That is the trap this file is built to avoid, and it deserves saying at
// length because it is the failure mode that turns this from evidence into
// theatre. A counting allocator that is not actually linked in, or that misses
// an overload the code happens to use, reports zero. Zero is exactly the answer
// the test is looking for. So the instrument silently agrees with the claim it
// was built to check, the test passes forever, and the whole apparatus proves
// nothing while looking rigorous. It is worse than having no counter at all,
// because it manufactures confidence.
//
// Two defences are built in. First, every replaceable form is overridden below,
// not just the two obvious ones. Second, and this is the part that matters,
// test_alloc_counter.cpp allocates on purpose and asserts the counter noticed
// BEFORE any test asserts a zero. If the instrument cannot see an elephant it
// is not allowed to testify about mice.
//
// WHICH FORMS AND WHY ALL OF THEM
//
// C++ has a family of replaceable allocation functions, not a pair. Miss one
// and the code that uses it becomes invisible.
//   operator new(size_t)                              the ordinary scalar form
//   operator new[](size_t)                            arrays
//   operator new(size_t, align_val_t)                 over aligned types, which
//                                                     is every cache line
//                                                     aligned struct in a low
//                                                     latency codebase, so this
//                                                     is the one most likely to
//                                                     be hit and most likely to
//                                                     be forgotten
//   the nothrow variants of all of the above          used by containers and by
//                                                     allocate_shared paths
//   operator delete(void*), sized, array, aligned     matching frees
//
// WHY A GLOBAL OVERRIDE AND NOT A CUSTOM ALLOCATOR
//
// A custom allocator only sees the containers that were told to use it. A
// global override sees everything, including allocations inside the standard
// library, inside a third party header, and inside code nobody thought to look
// at. Since the purpose is to catch the allocation nobody knew about, only the
// global form answers the question.
//
// COST AND THREADING
//
// The counters are process wide atomics with relaxed ordering. Relaxed is right
// because nothing synchronises on these values, they are read after the fact,
// and a stronger ordering would add a fence to every allocation in the process
// including in code under test. Relaxed increments are still not free, so this
// belongs in test and benchmark binaries rather than in a production build. The
// macro below exists so exactly one translation unit defines the operators,
// which is what makes that choice per binary.

namespace tick {
namespace alloc {

// Definitions live in whichever translation unit uses TICK_DEFINE_ALLOC_COUNTER.
// Declared inline so every other translation unit sees the same objects.
inline std::atomic<uint64_t>& allocation_count() noexcept {
    static std::atomic<uint64_t> v{0};
    return v;
}
inline std::atomic<uint64_t>& deallocation_count() noexcept {
    static std::atomic<uint64_t> v{0};
    return v;
}
inline std::atomic<uint64_t>& allocated_bytes() noexcept {
    static std::atomic<uint64_t> v{0};
    return v;
}

[[nodiscard]] inline uint64_t allocations() noexcept {
    return allocation_count().load(std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t deallocations() noexcept {
    return deallocation_count().load(std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t bytes() noexcept {
    return allocated_bytes().load(std::memory_order_relaxed);
}

inline void reset() noexcept {
    allocation_count().store(0, std::memory_order_relaxed);
    deallocation_count().store(0, std::memory_order_relaxed);
    allocated_bytes().store(0, std::memory_order_relaxed);
}

// True when a translation unit in this binary defined the operators. Without
// this a test that never linked the definitions would see a permanent zero and
// pass, which is the exact failure described above. The self check test asserts
// this before it asserts anything else.
[[nodiscard]] inline bool& counter_is_installed() noexcept {
    static bool installed = false;
    return installed;
}

} // namespace alloc

// Snapshot the counters on entry, report the delta at any point inside the
// scope. Deliberately does not reset the globals, so guards nest and so a
// benchmark's own outer accounting is not disturbed by an inner check.
class AllocGuard {
public:
    AllocGuard() noexcept
        : allocs_(alloc::allocations()),
          frees_(alloc::deallocations()),
          bytes_(alloc::bytes()) {}

    AllocGuard(const AllocGuard&)            = delete;
    AllocGuard& operator=(const AllocGuard&) = delete;

    [[nodiscard]] uint64_t allocations_in_scope() const noexcept {
        return alloc::allocations() - allocs_;
    }
    [[nodiscard]] uint64_t deallocations_in_scope() const noexcept {
        return alloc::deallocations() - frees_;
    }
    [[nodiscard]] uint64_t bytes_in_scope() const noexcept {
        return alloc::bytes() - bytes_;
    }

    // Restart the window without leaving and re entering the scope, for a loop
    // that wants a per iteration check.
    void rearm() noexcept {
        allocs_ = alloc::allocations();
        frees_  = alloc::deallocations();
        bytes_  = alloc::bytes();
    }

private:
    uint64_t allocs_;
    uint64_t frees_;
    uint64_t bytes_;
};

} // namespace tick

// Place TICK_DEFINE_ALLOC_COUNTER() at namespace scope in exactly one
// translation unit of a binary. Two definitions are a link error, which is the
// right outcome, and zero definitions leaves the counter permanently at zero,
// which is why alloc::counter_is_installed exists and is checked.
//
// The allocating operators use std::malloc, and the aligned forms use
// posix_memalign rather than C11 aligned_alloc. Both are available on Linux and
// on macOS, but aligned_alloc requires the requested size to be a multiple of
// the alignment on some implementations and posix_memalign has no such
// restriction and predates it everywhere, so posix_memalign is the portable
// choice and the size is rounded up anyway for good measure.
//
// Every delete form routes to std::free. The sized and aligned deletes ignore
// the size and alignment arguments because malloc tracks them internally. They
// still have to exist, because if operator delete(void*, size_t) is not
// replaced the compiler will call the library's version on memory this file
// allocated, and mixing allocators is undefined behaviour that shows up as a
// crash far from the cause.
//
// The operators are marked so the compiler cannot elide them. C++14 onwards
// permits an implementation to optimise away a new and delete pair whose result
// is unused, which for a counting allocator means the instrument disagrees with
// the code depending on optimisation level. The test compiles with that
// possibility in mind and uses the allocated memory so the pair cannot vanish.

#define TICK_DEFINE_ALLOC_COUNTER()                                             \
    namespace tick { namespace alloc { namespace detail {                       \
        inline void* tick_alloc(std::size_t n) noexcept {                       \
            if (n == 0) n = 1;                                                  \
            void* p = std::malloc(n);                                           \
            if (p) {                                                            \
                allocation_count().fetch_add(1, std::memory_order_relaxed);     \
                allocated_bytes().fetch_add(n, std::memory_order_relaxed);      \
            }                                                                   \
            return p;                                                           \
        }                                                                       \
        inline void* tick_alloc_aligned(std::size_t n, std::size_t a) noexcept {\
            if (n == 0) n = 1;                                                  \
            if (a < sizeof(void*)) a = sizeof(void*);                           \
            /* both backends require the size to be a multiple of the           \
               alignment, which the language does not guarantee for an over     \
               aligned array of a small element type, so round it up here */    \
            const std::size_t rounded = ((n + a - 1) / a) * a;                  \
            void* p = nullptr;                                                  \
            if (::posix_memalign(&p, a, rounded) != 0) p = nullptr;              \
            if (p) {                                                            \
                allocation_count().fetch_add(1, std::memory_order_relaxed);     \
                allocated_bytes().fetch_add(rounded, std::memory_order_relaxed);\
            }                                                                   \
            return p;                                                           \
        }                                                                       \
        inline void tick_free(void* p) noexcept {                               \
            if (p) {                                                            \
                deallocation_count().fetch_add(1, std::memory_order_relaxed);   \
                std::free(p);                                                   \
            }                                                                   \
        }                                                                       \
        /* Runs before main, so a binary that included the definitions says so  \
           and a binary that forgot them reports false rather than a silent     \
           and permanently correct looking zero. */                             \
        struct Installer { Installer() noexcept { counter_is_installed() = true; } }; \
        inline const Installer tick_alloc_installer{};                          \
    }}}                                                                         \
                                                                                \
    void* operator new(std::size_t n) {                                         \
        void* p = ::tick::alloc::detail::tick_alloc(n);                         \
        if (!p) throw std::bad_alloc();                                         \
        return p;                                                               \
    }                                                                           \
    void* operator new[](std::size_t n) {                                       \
        void* p = ::tick::alloc::detail::tick_alloc(n);                         \
        if (!p) throw std::bad_alloc();                                         \
        return p;                                                               \
    }                                                                           \
    void* operator new(std::size_t n, std::align_val_t a) {                     \
        void* p = ::tick::alloc::detail::tick_alloc_aligned(                    \
            n, static_cast<std::size_t>(a));                                    \
        if (!p) throw std::bad_alloc();                                         \
        return p;                                                               \
    }                                                                           \
    void* operator new[](std::size_t n, std::align_val_t a) {                   \
        void* p = ::tick::alloc::detail::tick_alloc_aligned(                    \
            n, static_cast<std::size_t>(a));                                    \
        if (!p) throw std::bad_alloc();                                         \
        return p;                                                               \
    }                                                                           \
    void* operator new(std::size_t n, const std::nothrow_t&) noexcept {         \
        return ::tick::alloc::detail::tick_alloc(n);                            \
    }                                                                           \
    void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {       \
        return ::tick::alloc::detail::tick_alloc(n);                            \
    }                                                                           \
    void* operator new(std::size_t n, std::align_val_t a,                       \
                       const std::nothrow_t&) noexcept {                        \
        return ::tick::alloc::detail::tick_alloc_aligned(                       \
            n, static_cast<std::size_t>(a));                                    \
    }                                                                           \
    void* operator new[](std::size_t n, std::align_val_t a,                     \
                         const std::nothrow_t&) noexcept {                      \
        return ::tick::alloc::detail::tick_alloc_aligned(                       \
            n, static_cast<std::size_t>(a));                                    \
    }                                                                           \
                                                                                \
    void operator delete(void* p) noexcept {                                    \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p) noexcept {                                  \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete(void* p, std::size_t) noexcept {                       \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p, std::size_t) noexcept {                     \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete(void* p, std::align_val_t) noexcept {                  \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p, std::align_val_t) noexcept {                \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete(void* p, std::size_t, std::align_val_t) noexcept {     \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {   \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete(void* p, const std::nothrow_t&) noexcept {             \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p, const std::nothrow_t&) noexcept {           \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete(void* p, std::align_val_t,                             \
                         const std::nothrow_t&) noexcept {                      \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    void operator delete[](void* p, std::align_val_t,                           \
                           const std::nothrow_t&) noexcept {                    \
        ::tick::alloc::detail::tick_free(p);                                    \
    }                                                                           \
    static_assert(true, "TICK_DEFINE_ALLOC_COUNTER expects a trailing semicolon")
