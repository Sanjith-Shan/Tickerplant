#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

// A bounded multi producer multi consumer queue.
//
// This is the Dmitry Vyukov design. A ring of slots, each slot carrying its own
// sequence counter, with the producer position and the consumer position held
// as two separate atomics that live on their own cache lines. A push is one
// relaxed read of the producer position, one acquire read of the slot sequence,
// one compare and exchange to claim the position, the payload write, and one
// release store of the slot sequence. A pop is the mirror image. Nothing here
// ever waits on another thread, so a producer that the scheduler stops in the
// middle of a push blocks no other producer and no consumer working a different
// slot, which is the property a lock cannot offer.
//
// WHEN THIS IS THE RIGHT ANSWER, AND WHEN IT IS NOT. The single producer single
// consumer ring costs less per operation and it is the right default. Its push
// is a relaxed load, a plain store, and a release store, with no atomic read
// modify write anywhere, so it costs no contended position and no retry loop.
// This queue pays for generality in three places. Every push and every pop runs
// a compare and exchange on a position that every thread of that role is
// hammering, so the line holding it moves between cores under load. Every push
// and pop touches the slot sequence as well as the payload, which is a second
// shared line per item. And padding every slot out to a cache line multiplies
// the memory footprint of the ring, which pushes the working set out of L2
// sooner.
//
// bench/bench_concurrency.cpp measures both against each other rather than
// asserting the ordering, and the result is not uniform. On streaming
// throughput with one producer and one consumer the SPSC ring wins clearly,
// which is the case that matters for decode to book. On a single item round
// trip the two are much closer and this queue can come out ahead, and the
// likely reason is a property of the padding and not of MPMC. Each slot here
// owns its cache line, so a hop moves one line between the two cores, while an
// unpadded ring makes the consumer poll a shared position on one line and then
// read a payload on another. That is a hypothesis drawn from the layout and not
// from a counter readout, because this platform has no per core cache event
// counters to confirm it with.
//
// So the rule for this feed handler is simple. Decode to book is one thread to
// one thread and stays on the SPSC ring. This queue earns its cost only where
// there genuinely are several producers or several consumers, for example a
// fan out of book updates to a pool of strategy workers, where the alternative
// is not an SPSC ring but N of them plus the logic to choose between them.
//
// FALSE SHARING, CONCRETELY. A cache line is the unit the coherence protocol
// moves, not a variable. Put the producer position and the consumer position in
// one line and a producer's compare and exchange on its own position takes that
// line exclusive, which invalidates the copy the consumer core holds. The
// consumer then reads its own position, an entirely different variable that the
// producer never touched, and takes a coherence miss to pull the line back. The
// line ping pongs between the two cores once per item and the two threads spend
// their time on the interconnect rather than on the queue. Nothing about the
// program is wrong and no variable is shared. Only the line is. Separating the
// two atomics onto their own lines costs bytes and removes the traffic entirely,
// and bench/bench_concurrency.cpp measures the packed layout against this one
// rather than asserting the effect.
//
// ARM64 IS NOT X86, AND THIS MATTERS HERE. x86 gives total store ordering, so a
// load is never reordered with a later load and a store is never reordered with
// a later store, and a great deal of lock free code written and tested only on
// x86 is accidentally correct. On arm64 loads and stores are reordered freely
// unless an ordering is requested, and an acquire or release here compiles to a
// real ldar or stlr rather than to a plain load or store that happened to be
// ordered anyway. Every ordering below is chosen for the weak model, and a
// relaxed where an acquire belongs would still pass every x86 test run and fail
// on this machine.

namespace tick {

// Sixty four bytes is the line size on x86 and on most arm64 parts and it is
// the separation this queue uses. Some arm64 implementations report a wider
// line, and on those sixty four is a floor rather than a guarantee, because two
// atomics sixty four bytes apart still land in one line of a wider cache. This
// constant is the single place to raise it, and the counter pair case in
// bench/bench_concurrency.cpp measures sixty four against a wider separation on
// whatever machine the suite is run on rather than leaving it to be assumed.
// Read hw.cachelinesize on macOS or /sys/devices/system/cpu/cpu0/cache on Linux
// before deciding.
inline constexpr std::size_t kCacheLineSize = 64;

template <typename T, std::size_t Capacity>
class MpmcQueue {
public:
    // Capacity must be a power of two, and the reason is not only that the
    // remainder becomes a bitwise and.
    //
    // The two positions are monotonically increasing std::size_t counters that
    // are never reset, so after enough items they wrap through zero. The slot a
    // position names is position & mask, and a slot is handed back to the
    // producer of the next lap by storing position + Capacity into its
    // sequence. Both of those stay consistent across the wrap at two to the
    // sixty four only when Capacity divides two to the sixty four exactly,
    // which is to say only when it is a power of two. With Capacity equal to
    // one hundred, position % Capacity jumps discontinuously at the wrap, the
    // sequence a slot is holding no longer equals the position of the producer
    // that wants it, and every subsequent push reports the queue full forever.
    // That is a silent correctness failure and not a performance one.
    static_assert(Capacity >= 2, "a ring of one slot cannot hold an item and a free marker");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    // Slots are constructed once and reused for the life of the queue, so the
    // ring always holds Capacity live T objects and a push is an assignment
    // rather than a construction. That is what keeps the hot path free of
    // placement new and of the destructor call a slot based variant would need,
    // and it is why T is constrained here rather than anywhere else.
    static_assert(std::is_default_constructible_v<T>, "slots are constructed up front");
    static_assert(std::is_nothrow_destructible_v<T>, "a throwing destructor has nowhere to go");

    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask     = Capacity - 1;

    MpmcQueue() noexcept {
        // The initial sequence of slot i is i, which is exactly the position of
        // the first producer that will ever use it. Relaxed is right because
        // the constructor is not concurrent with anything. A thread only sees
        // this queue at all after whatever published the pointer to it, and
        // that publication carries the ordering.
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    // There is deliberately no blocking push and no blocking pop, not even a
    // spinning one. A feed handler thread that waits on a queue has stopped
    // draining the socket, and the next thing that happens is a kernel receive
    // buffer overflow and a gap in the sequence numbers. A full queue is an
    // event the caller has to handle and count, never something to sleep on.
    [[nodiscard]] bool try_push(const T& item) noexcept { return push_impl(item); }
    [[nodiscard]] bool try_push(T&& item) noexcept { return push_impl(std::move(item)); }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        // Relaxed. This is a starting guess at the consumer position and
        // nothing is inferred from it. If it is stale the slot sequence check
        // below disagrees and the loop reloads. Making it acquire would order
        // this thread against a store nobody performed, since the position
        // itself publishes no data. The data is published by the slot sequence.
        std::size_t pos = head_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = slots_[pos & kMask];

            // Acquire, and this is the load that makes the queue correct. It
            // pairs with the producer's release store of pos + 1 after it wrote
            // the payload. Seeing pos + 1 here therefore guarantees that the
            // payload write happens before the read below. Relaxed here is the
            // classic arm64 bug. The sequence would arrive, the payload read
            // would be satisfied from a stale line, and the consumer would
            // return the previous lap's value. On x86 the same code passes
            // because the hardware will not reorder those two loads.
            const std::size_t seq  = slot.seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                // The slot is full and holds the item for this position. Claim
                // the position.
                //
                // Relaxed on both the success and the failure path. The claim
                // publishes nothing to anyone, it only decides which consumer
                // owns this position, and a compare and exchange is atomic
                // whatever its ordering. Ordering against the payload read that
                // follows is already supplied by the acquire above, which no
                // later memory operation may be reordered before. acq_rel here
                // would emit a barrier on arm64 for a guarantee nothing uses.
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    out = std::move(slot.value);
                    // Release, marking the slot free for the producer one lap
                    // ahead, which is the producer standing at pos + Capacity.
                    // It pairs with that producer's acquire load of this same
                    // sequence and guarantees the read above completes before
                    // that producer is allowed to overwrite the payload. Drop
                    // this to relaxed and the producer may begin writing while
                    // this consumer is still reading, which is a torn item and
                    // a real data race, not a theoretical one.
                    slot.seq.store(pos + Capacity, std::memory_order_release);
                    return true;
                }
                // compare_exchange_weak wrote the current head_ into pos, so
                // the loop retries at the position another consumer left us.
                // The weak form is right because the retry is the loop body
                // itself, so a spurious failure costs one more iteration and
                // the strong form would add a hidden inner loop on arm64.
            } else if (diff < 0) {
                // The slot sequence is still pos, meaning no producer has
                // filled this position yet. The queue is empty as far as this
                // consumer can tell.
                return false;
            } else {
                // Another consumer already took this position and moved on.
                // Relaxed for the same reason as the first load.
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Approximate. Both positions are read without any mutual ordering, so the
    // answer is a snapshot of two moments and can be stale the instant it is
    // returned. It exists for logging and for tests, never for a decision on
    // the hot path, where the only honest question is whether try_push or
    // try_pop succeeded.
    [[nodiscard]] std::size_t size() const noexcept {
        // Acquire on both so a single threaded caller, which is the only caller
        // that can draw a conclusion from this, sees the values published by
        // the last push and pop rather than a cached line.
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return t > h ? t - h : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // One ring slot. The sequence counter is aligned to a cache line, which
    // also rounds the whole slot up to a multiple of one, so two slots never
    // share a line. Without that, a producer writing slot i and a consumer
    // reading slot i + 1 would contend on one line even though they are working
    // on different items and the queue is behaving perfectly. That is the same
    // false sharing as on the two positions, one level down, and it is the
    // reason this ring costs more memory per element than the SPSC one.
    struct Slot {
        alignas(kCacheLineSize) std::atomic<std::size_t> seq;
        T value{};
    };

    template <typename U>
    [[nodiscard]] bool push_impl(U&& item) noexcept {
        // Relaxed, for the same reason as in try_pop. A stale producer position
        // costs an extra iteration and can never cost correctness, because the
        // slot sequence is the thing that is actually checked.
        std::size_t pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = slots_[pos & kMask];

            // Acquire. It pairs with the release store a consumer performed
            // when it freed this slot one lap ago. Seeing seq equal to pos
            // therefore guarantees that the consumer's read of the old payload
            // happens before the write below, so the write cannot race with it.
            // This is the ordering people leave out because on x86 the store
            // that follows will not be reordered ahead of this load anyway.
            const std::size_t seq  = slot.seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Relaxed compare and exchange, as in try_pop. It decides
                // ownership of the position and publishes no data.
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    slot.value = std::forward<U>(item);
                    // Release. This is the store that publishes the payload.
                    // The consumer that acquires this value is guaranteed to
                    // see the assignment above. Everything else in this
                    // function exists to get to this one store correctly.
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // The sequence is behind pos, so this slot still holds an item
                // from the previous lap that no consumer has taken. Full.
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    // The two positions, each alone on its line. See the header comment. Note
    // that head_ is contended only among consumers and tail_ only among
    // producers, so packing them together would create contention between two
    // groups of threads that otherwise have no reason to interact at all.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};  // next to pop
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};  // next to push

    // The ring itself starts on its own line so the first slot does not share
    // one with tail_.
    alignas(kCacheLineSize) Slot slots_[Capacity];
};

} // namespace tick
