#include "tick/alloc_counter.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

// This translation unit owns the operator new and delete definitions for the
// whole test binary. Exactly one may, and if the CMake target for this suite
// later grows another file that also defines them the linker says so, which is
// the right failure.
TICK_DEFINE_ALLOC_COUNTER();

// PROVING THE INSTRUMENT BEFORE TRUSTING IT.
//
// A counter nobody has proved can see an allocation is worth nothing. It is
// worse than nothing, because it returns zero, zero is the answer the hot path
// test is hoping for, and so a completely disconnected counter produces a green
// suite and a false claim on a resume.
//
// So the order of this file is deliberate. Everything that asserts a zero comes
// after the tests that prove the counter can see a one. The first test asserts
// the definitions were actually linked in, the next few make the counter
// observe allocations of every form it claims to cover, and only then does
// anything assert that a scope allocated nothing.
//
// Read this file top to bottom and the argument is complete. Read only the zero
// assertions and you have learned nothing.

namespace {

// A sink the optimiser cannot see through. Without it the compiler is permitted
// by the standard to elide a new and delete pair whose result is unused, so the
// test would measure the optimiser rather than the allocator, and would behave
// differently at different optimisation levels.
volatile std::size_t g_sink = 0;

void consume(const void* p, std::size_t n) noexcept {
    g_sink += reinterpret_cast<std::uintptr_t>(p) + n;
}

// ---------------------------------------------------------------------------
// Part one, prove the instrument works
// ---------------------------------------------------------------------------

TEST(AllocCounterSelfCheck, TheDefinitionsAreActuallyLinkedIntoThisBinary) {
    // Without this, a binary that forgot TICK_DEFINE_ALLOC_COUNTER would report
    // a permanent and permanently correct looking zero from every test below.
    ASSERT_TRUE(tick::alloc::counter_is_installed())
        << "no translation unit in this binary defined the counting operators, "
           "so every zero this suite reports would be meaningless";
}

TEST(AllocCounterSelfCheck, ItSeesAPlainNew) {
    tick::AllocGuard g;
    int* p = new int(7);
    consume(p, sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 1u)
        << "the counter did not see a plain operator new, so it cannot be trusted";
    delete p;
    EXPECT_EQ(g.deallocations_in_scope(), 1u);
}

TEST(AllocCounterSelfCheck, ItSeesAnArrayNew) {
    tick::AllocGuard g;
    int* p = new int[64];
    consume(p, 64 * sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 1u)
        << "operator new[] is not being counted, so any array allocation on the "
           "hot path would be invisible";
    delete[] p;
    EXPECT_EQ(g.deallocations_in_scope(), 1u);
}

TEST(AllocCounterSelfCheck, ItSeesAnOverAlignedNew) {
    // This is the overload most easily forgotten and the one most likely to be
    // hit by this project, because a cache line aligned struct is the normal
    // shape of a low latency data structure and it routes to the aligned
    // operator rather than the ordinary one.
    struct alignas(64) CacheLine { char pad[64]; };

    tick::AllocGuard g;
    CacheLine* p = new CacheLine();
    consume(p, sizeof(CacheLine));
    EXPECT_EQ(g.allocations_in_scope(), 1u)
        << "the aligned operator new is not being counted";
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u)
        << "the replacement aligned allocator did not honour the alignment";
    delete p;
    EXPECT_EQ(g.deallocations_in_scope(), 1u);
}

TEST(AllocCounterSelfCheck, ItSeesAnOverAlignedArrayNew) {
    struct alignas(128) Wide { char pad[128]; };

    tick::AllocGuard g;
    Wide* p = new Wide[4];
    consume(p, 4 * sizeof(Wide));
    EXPECT_EQ(g.allocations_in_scope(), 1u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 128u, 0u);
    delete[] p;
    EXPECT_EQ(g.deallocations_in_scope(), 1u);
}

TEST(AllocCounterSelfCheck, ItSeesTheNothrowForms) {
    tick::AllocGuard g;
    int* a = new (std::nothrow) int(1);
    ASSERT_NE(a, nullptr);
    consume(a, sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 1u);

    int* b = new (std::nothrow) int[16];
    ASSERT_NE(b, nullptr);
    consume(b, 16 * sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 2u)
        << "the nothrow array form is not being counted";

    ::operator delete(a, std::nothrow);
    ::operator delete[](b, std::nothrow);
}

TEST(AllocCounterSelfCheck, ItSeesAllocationsMadeInsideTheStandardLibrary) {
    // The reason for replacing the global operators rather than writing a
    // custom allocator. A custom allocator only sees containers that were told
    // to use it, and the allocation this project is actually afraid of is the
    // one inside a library header that nobody inspected.
    tick::AllocGuard g;
    std::vector<int> v;
    v.reserve(1000);
    consume(v.data(), v.capacity());
    EXPECT_GE(g.allocations_in_scope(), 1u);

    // Long enough that no small string optimisation can absorb it.
    std::string s(4096, 'x');
    consume(s.data(), s.size());
    EXPECT_GE(g.allocations_in_scope(), 2u);

    auto up = std::make_unique<double[]>(512);
    consume(up.get(), 512 * sizeof(double));
    EXPECT_GE(g.allocations_in_scope(), 3u);
}

TEST(AllocCounterSelfCheck, ItCountsBytesAndNotJustCalls) {
    tick::AllocGuard g;
    char* p = new char[100000];
    consume(p, 100000);
    EXPECT_GE(g.bytes_in_scope(), 100000u)
        << "the byte counter is not tracking the requested size";
    delete[] p;
}

// ---------------------------------------------------------------------------
// Part two, the guard's own behaviour
// ---------------------------------------------------------------------------

TEST(AllocGuardBehaviour, GuardsNest) {
    tick::AllocGuard outer;
    int* a = new int(1);
    consume(a, sizeof(int));
    {
        tick::AllocGuard inner;
        int* b = new int(2);
        consume(b, sizeof(int));
        // The inner guard sees only what happened inside it, and the outer one
        // keeps counting through. A guard that reset the global counters would
        // break this and would silently corrupt a benchmark's own accounting.
        EXPECT_EQ(inner.allocations_in_scope(), 1u);
        delete b;
    }
    EXPECT_EQ(outer.allocations_in_scope(), 2u);
    delete a;
}

TEST(AllocGuardBehaviour, RearmRestartsTheWindow) {
    tick::AllocGuard g;
    int* a = new int(1);
    consume(a, sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 1u);

    g.rearm();
    EXPECT_EQ(g.allocations_in_scope(), 0u);

    int* b = new int(2);
    consume(b, sizeof(int));
    EXPECT_EQ(g.allocations_in_scope(), 1u);

    delete a;
    delete b;
}

// ---------------------------------------------------------------------------
// Part three, only now is a zero meaningful
// ---------------------------------------------------------------------------

TEST(AllocCounterZeroClaims, AnEmptyScopeAllocatesNothing) {
    tick::AllocGuard g;
    EXPECT_EQ(g.allocations_in_scope(), 0u);
    EXPECT_EQ(g.deallocations_in_scope(), 0u);
    EXPECT_EQ(g.bytes_in_scope(), 0u);
}

TEST(AllocCounterZeroClaims, ArithmeticAndStackWorkAllocateNothing) {
    // The shape of an assertion a hot path test makes, on work that obviously
    // cannot allocate, so that the pattern itself is verified here rather than
    // for the first time in a benchmark.
    std::array<int, 256> scratch{};

    tick::AllocGuard g;
    int acc = 0;
    for (std::size_t i = 0; i < scratch.size(); ++i) {
        scratch[i] = static_cast<int>(i * 3 + 1);
        acc += scratch[i];
    }
    consume(scratch.data(), static_cast<std::size_t>(acc));
    EXPECT_EQ(g.allocations_in_scope(), 0u);
}

TEST(AllocCounterZeroClaims, APreReservedVectorDoesNotAllocateWhileFilling) {
    // The realistic version. The reserve is outside the guard, which is exactly
    // how a feed handler is supposed to be arranged, and the fill inside it
    // must be silent. If reserve were wrong or the growth policy surprised us,
    // this fails and points at the real problem.
    std::vector<std::uint64_t> v;
    v.reserve(4096);

    tick::AllocGuard g;
    for (std::uint64_t i = 0; i < 4096; ++i) v.push_back(i);
    consume(v.data(), v.size());
    EXPECT_EQ(g.allocations_in_scope(), 0u);
}

TEST(AllocCounterZeroClaims, ExceedingAReserveDoesAllocateAndIsCaught) {
    // The negative control for the test above. If pushing past the reserve did
    // not register, the previous test would be passing for the wrong reason.
    std::vector<std::uint64_t> v;
    v.reserve(16);

    tick::AllocGuard g;
    for (std::uint64_t i = 0; i < 64; ++i) v.push_back(i);
    consume(v.data(), v.size());
    EXPECT_GT(g.allocations_in_scope(), 0u)
        << "outgrowing a reserve must be visible, otherwise the zero assertions "
           "above prove nothing";
}

// ---------------------------------------------------------------------------
// Process wide totals
// ---------------------------------------------------------------------------

TEST(AllocCounterTotals, TotalsAreProcessWideAndOnlyGoUp) {
    const uint64_t before = tick::alloc::allocations();
    int* p = new int(1);
    consume(p, sizeof(int));
    const uint64_t after = tick::alloc::allocations();
    EXPECT_GT(after, before);
    delete p;
    // Deallocations track separately, so a leak is visible as a growing gap
    // rather than being netted out to zero.
    EXPECT_GT(tick::alloc::deallocations(), 0u);
}

} // namespace
