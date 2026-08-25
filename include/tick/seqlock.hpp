#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>

// A seqlock for publishing one small, frequently updated value to many readers.
//
// The structure this exists for is top of book. One thread builds the book and
// writes the new best bid and offer after every message that moves it. Several
// strategy threads, a risk monitor, and a logger read it, each at its own rate,
// and none of them cares about the updates it missed. It only wants the most
// recent one. That is an unusual shape and it rules out most of the obvious
// answers. A mutex lets a reader stall the writer, which is exactly backwards
// here, since the writer is on the critical path of the feed and the readers
// are not. A queue delivers every update to every reader, which is work nobody
// asked for and unbounded memory when a reader falls behind. An atomic of the
// whole struct is either a lock in disguise, because a sixteen byte or larger
// type has no lock free atomic on most targets, or it forces the value down to
// what one machine word can hold.
//
// The seqlock inverts the usual priority. The writer never waits, never checks
// whether anyone is reading, and its cost is one counter increment, the value
// write, and one more counter store. Readers pay instead, by detecting that
// they raced and reading again. Under a writer that updates far more slowly
// than a read takes, which is the real case, a retry is rare.
//
// HOW IT WORKS. The sequence counter is odd while a write is in progress and
// even when the value is stable. A reader reads the counter, reads the value,
// reads the counter again, and accepts the value only when the first read was
// even and the two reads agree. An odd first read means it arrived mid write.
// A changed second read means a write started and possibly finished while the
// reader was copying, so what it copied may be half of one value and half of
// another.
//
// EXACTLY ONE WRITER. The class does not enforce this and it cannot, so it is
// a contract on the caller. Two writers break it in a way no reader can
// detect. Both read the counter as even, both store odd, both copy their own
// bytes over the value in some interleaved order, and both store even. A
// reader that runs entirely between the last odd store and the last even store
// sees a stable even counter that did not change across its read, so it accepts
// the value, and the value it accepts is a mixture of two writers' fields that
// no writer ever intended. Worse, the stored value stays mixed afterwards, so
// the corruption is permanent rather than transient. If a second writer is ever
// needed the fix is a mutex between the writers only, which leaves the readers
// exactly as they are, because the readers were never the problem.
//
// TEARING IS NORMAL HERE, NOT AN ERROR. A reader is expected to observe a half
// updated value. That is the whole reason the counter exists. Three things
// follow and all three are requirements rather than style.
//
//   1. T must be trivially copyable. A torn copy of a std::string or a
//      std::shared_ptr is a corrupt pointer that the reader will then follow
//      or free. A torn copy of a plain struct of integers is merely wrong
//      numbers, which the counter check then rejects. The static_assert below
//      is load bearing.
//   2. The value is copied with memcpy into a local, not field by field into
//      the caller's object. Field by field would hand the caller a partially
//      overwritten object to look at, and a caller that reads a field before
//      checking the counter has already acted on garbage. Copying into a local
//      keeps every torn byte inside this function until the counter says the
//      snapshot is good.
//   3. There is an acquire fence between reading the value and re-reading the
//      counter. See below, because this is the part that is easy to get wrong.
//
// WHY THE FENCE, AND WHY ACQUIRE ON THE FIRST LOAD IS NOT ENOUGH. An acquire
// load constrains what may move backwards past it. Nothing after an acquire
// load may be reordered to before it. It says nothing at all about what may
// move forwards or about the operations that follow it relative to each other.
// So the acquire on the first counter load does its job, which is to stop the
// value reads from being hoisted above it, and it does not touch the problem
// at the other end. The second counter load is an ordinary load of a different
// address from the value reads, with no data dependency on them, and arm64 is
// free to satisfy it early, out of order, before the value reads have returned.
// Then the check compares a counter read from before the race against a value
// read from during it and happily accepts a torn snapshot.
//
// std::atomic_thread_fence(std::memory_order_acquire) between the two is what
// forbids that. It orders the loads that precede it against the loads that
// follow it, so the second counter load cannot be satisfied until the value
// reads have been. On x86 this fence compiles to nothing at all, because the
// hardware never reorders a load with a later load, which is precisely why a
// seqlock missing this fence passes every test on an x86 laptop and returns
// torn data on this arm64 machine. This is the single most useful thing in
// this file to be able to explain.
//
// THE HONEST CAVEAT ABOUT THE C++ MEMORY MODEL. As written, the writer stores
// to the value bytes while readers load from them with no synchronisation
// between those accesses, which is the definition of a data race, and a data
// race is undefined behaviour. The fences order the accesses but they do not
// make them non racy, because the standard defines the race on the accesses
// themselves. ThreadSanitizer is correct to report it and the test suite for
// this header expects the report rather than hiding it.
//
// The standard clean spelling is to make the payload a sequence of atomics,
// either one relaxed atomic per field or an array of relaxed atomic bytes, and
// copy through those. That removes the race by definition and keeps every
// ordering property, and it costs the memcpy. A per byte loop over relaxed
// atomics does not vectorise the way a memcpy of a sixteen or thirty two byte
// struct does, and on a structure whose entire point is to be cheaper than a
// mutex that cost is not nothing.
//
// What real systems do is what is written here. The Linux kernel seqlock,
// seqlock_t, reads and writes ordinary memory under smp_rmb and smp_wmb. Every
// exchange colocated market data library that publishes a book snapshot does
// the same. The reasoning is that no real compiler invents a store to the
// value or reloads the local after the check, and the fences prevent the
// reorderings the hardware would perform. It is a deliberate step outside what
// the standard guarantees, taken with a known risk, and the right way to hold
// it is to know that it is one. That is a different thing from not knowing.

namespace tick {

template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a seqlock reader can observe a torn value, so T must survive being "
                  "copied byte by byte out of a half written state");
    static_assert(std::is_default_constructible_v<T>,
                  "load returns a T by value and builds it before filling it");

public:
    SeqLock() = default;
    explicit SeqLock(const T& initial) noexcept { store_unsynchronized(initial); }

    SeqLock(const SeqLock&)            = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    // The writer. One thread only. Never blocks and never retries.
    void store(const T& v) noexcept {
        // Relaxed. This thread is the only writer, so it is the only thread
        // that ever stores to seq_, and it is reading back a value it wrote
        // itself. There is no other thread's write to observe and therefore
        // nothing for an acquire to order against. Making it acquire would buy
        // nothing and would emit a barrier on arm64.
        const std::size_t s = seq_.load(std::memory_order_relaxed);

        // Odd. The write has begun. Relaxed on the store itself, with the
        // ordering supplied by the fence immediately below rather than by the
        // store, because what is needed here is release ordering relative to
        // the value writes that follow, and a release store orders what comes
        // before it, not what comes after. Spelling that as
        // seq_.store(s + 1, release) would be the common mistake, since it
        // would let the value writes float up above the odd counter and become
        // visible while the counter still reads even, which is the one state a
        // reader trusts.
        seq_.store(s + 1, std::memory_order_relaxed);

        // Release fence. No store before it may be reordered after any store
        // after it, so the odd counter is visible to every reader before the
        // first byte of the new value is. This is what makes the odd state
        // mean what it claims to mean.
        std::atomic_thread_fence(std::memory_order_release);

        // memcpy and not field assignment. The writer is allowed to tear here,
        // by construction, and going through memcpy keeps the compiler from
        // widening or reordering field stores in ways that would be fine for a
        // single threaded program and would extend the torn window here.
        //
        // The void* casts are not decoration. gcc's -Wclass-memaccess warns
        // about memcpy onto a class type, because for most class types that is
        // a bug. Here it is the technique, T is static_asserted trivially
        // copyable above, and casting to void* says so deliberately rather than
        // leaving a warning for a future reader to wonder about.
        std::memcpy(static_cast<void*>(&storage_), static_cast<const void*>(&v), sizeof(T));

        // Even again, and this store is a release. It pairs with the reader's
        // acquire load of seq_ and publishes every byte written above. A
        // relaxed store here would let a reader observe the even counter while
        // the value bytes were still in this core's store buffer, so the reader
        // would see a stable counter around a stale value and would have no way
        // of knowing.
        seq_.store(s + 2, std::memory_order_release);
    }

    // A reader that will not retry. Returns false when it raced with a write,
    // and out is then unspecified and must be ignored. This is the right entry
    // point for a monitor that would rather report nothing this tick than spend
    // time spinning, and for any caller that must not loop in a signal handler
    // or under a real time deadline.
    [[nodiscard]] bool try_load(T& out) const noexcept {
        // Acquire. It pairs with the writer's release store of the even
        // counter, so seeing an even s0 guarantees the value bytes that belong
        // to that even counter are visible below. It also stops the value reads
        // from being hoisted above this load, which would read the value from
        // before the counter was sampled and defeat the whole check.
        const std::size_t s0 = seq_.load(std::memory_order_acquire);

        // Odd means a write is in progress right now. There is no point reading
        // the value at all.
        if ((s0 & 1u) != 0u) return false;

        T local{};
        std::memcpy(static_cast<void*>(&local), static_cast<const void*>(&storage_), sizeof(T));

        // The fence described at length in the header comment. Without it the
        // load below may be satisfied before the copy above has been, and the
        // comparison then proves nothing. On x86 it is free. On arm64 it is the
        // difference between a correct seqlock and one that silently returns
        // torn data under load.
        std::atomic_thread_fence(std::memory_order_acquire);

        // Relaxed is sufficient here and an acquire would be noise. The fence
        // above already supplies every ordering this load needs, and this load
        // is being used as a value to compare rather than as a gate that
        // publishes anything.
        const std::size_t s1 = seq_.load(std::memory_order_relaxed);

        if (s0 != s1) return false;

        out = local;
        return true;
    }

    // A reader that retries until it gets a clean snapshot. This can spin, and
    // in the pathological case where the writer updates faster than a reader
    // can copy the value it spins forever, which is a reason to keep T small
    // and a reason try_load exists. It does not block the writer while it
    // spins, which is the property that separates this from a lock.
    [[nodiscard]] T load() const noexcept {
        T out{};
        while (!try_load(out)) {
            spin_hint();
        }
        return out;
    }

    // The raw counter, for tests and for a monitor that wants to know how often
    // the value is moving. Acquire so that a caller which reads the counter and
    // then the value sees them in that order.
    [[nodiscard]] std::size_t sequence() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    // For the constructor only, where no reader can exist yet, so no counter
    // dance is needed and the value can simply be placed.
    void store_unsynchronized(const T& v) noexcept {
        std::memcpy(static_cast<void*>(&storage_), static_cast<const void*>(&v), sizeof(T));
    }

    // A hint to the core that this is a spin wait, so it can yield its issue
    // slots rather than burning them on a load it is going to discard. It is
    // not a scheduler yield and it does not enter the kernel.
    static void spin_hint() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#endif
    }

    // The counter and the value sit on separate cache lines. They are touched
    // together on every read, so this costs a second line per read, and it buys
    // the property that matters more, which is that the writer's counter stores
    // do not invalidate the line a reader is in the middle of copying the value
    // out of. Without the separation, a reader copying a value that spans the
    // counter's line would be invalidated by the very stores it is racing, and
    // the retry rate would rise for no reason other than layout.
    alignas(64) std::atomic<std::size_t> seq_{0};

    // Raw storage rather than a T member. The bytes are written by one thread
    // and read by others with no atomic on them, so naming them as bytes says
    // what is really happening, and it removes any temptation to touch a field
    // of the live value directly instead of going through a snapshot.
    alignas(64) alignas(T) unsigned char storage_[sizeof(T)]{};
};

} // namespace tick
