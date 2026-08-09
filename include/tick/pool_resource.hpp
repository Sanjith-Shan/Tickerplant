#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <vector>

// A segregated free list behind the standard memory resource interface.
//
// This exists because of two measurements taken in that order.
//
// First, the counting allocator showed that the book was allocating. The order
// objects come from NanoExchange's pool and allocate nothing, which made it
// easy to believe the whole path was allocation free, and it was not. Every new
// price level was a std::map node out of the general purpose heap, 14,209 of
// them in a three hundred thousand message replay, which on a real day is
// millions of calls into the allocator on the hot path.
//
// Second, the obvious fix made it worse. Swapping in
// std::pmr::unsynchronized_pool_resource removed the allocations, 14,209 down
// to 17, and cost about two thirds of the throughput. It is a general purpose
// structure that has to cope with any size and any alignment, and it is not
// what a book needs.
//
// A book needs one thing. The levels it creates and destroys are all the same
// size, because they are all nodes of the same map, and the feed creates and
// destroys them continuously. So a free list per size class, carved out of
// large chunks, gives back a pointer in a few instructions and never returns
// anything upstream during a session.
//
// Not thread safe, deliberately. The feed path is one thread. A resource that
// took a lock here would be paying for a guarantee nothing in this project
// needs, and saying that is better than a mutex nobody reads.

namespace tick {

class PoolResource final : public std::pmr::memory_resource {
public:
    // Blocks larger than this go straight upstream. Nothing a book allocates
    // comes close, and the fallback exists so the resource is correct for any
    // caller rather than only for the one it was written for.
    static constexpr std::size_t kMaxBlock    = 256;
    static constexpr std::size_t kGranularity = 16;
    static constexpr std::size_t kClasses     = kMaxBlock / kGranularity;

    explicit PoolResource(std::size_t chunk_bytes = 1u << 20,
                          std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(upstream), chunk_bytes_(chunk_bytes) {}

    ~PoolResource() override { release(); }

    PoolResource(const PoolResource&)            = delete;
    PoolResource& operator=(const PoolResource&) = delete;

    // Hand every chunk back upstream. Called by the destructor, and useful in a
    // test that wants to prove the resource actually owns what it handed out.
    void release() noexcept {
        for (const Chunk& c : chunks_) {
            upstream_->deallocate(c.base, c.bytes, alignof(std::max_align_t));
        }
        chunks_.clear();
        for (auto& head : free_) head = nullptr;
        carve_       = nullptr;
        carve_left_  = 0;
    }

    [[nodiscard]] std::size_t chunks() const noexcept { return chunks_.size(); }
    [[nodiscard]] std::size_t bytes_from_upstream() const noexcept { return upstream_bytes_; }
    [[nodiscard]] uint64_t    reused() const noexcept { return reused_; }
    [[nodiscard]] uint64_t    carved() const noexcept { return carved_; }
    [[nodiscard]] uint64_t    oversized() const noexcept { return oversized_; }

private:
    struct Chunk {
        void*       base  = nullptr;
        std::size_t bytes = 0;
    };

    // A free block stores the next pointer inside its own storage, which is why
    // the smallest size class has to be at least pointer sized.
    struct FreeNode {
        FreeNode* next;
    };
    static_assert(kGranularity >= sizeof(FreeNode),
                  "a size class must hold the free list link");

    [[nodiscard]] static std::size_t class_of(std::size_t bytes) noexcept {
        return (bytes + kGranularity - 1) / kGranularity - 1;
    }

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        if (bytes == 0) bytes = 1;
        // Anything over the ceiling, or wanting more alignment than a chunk
        // guarantees, is not this resource's business.
        if (bytes > kMaxBlock || align > alignof(std::max_align_t)) {
            ++oversized_;
            return upstream_->allocate(bytes, align);
        }

        const std::size_t cls  = class_of(bytes);
        const std::size_t size = (cls + 1) * kGranularity;

        if (FreeNode* n = free_[cls]) {
            free_[cls] = n->next;
            ++reused_;
            return n;
        }

        if (carve_left_ < size) new_chunk();
        void* p = carve_;
        carve_ = static_cast<std::byte*>(carve_) + size;
        carve_left_ -= size;
        ++carved_;
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        if (bytes == 0) bytes = 1;
        if (bytes > kMaxBlock || align > alignof(std::max_align_t)) {
            upstream_->deallocate(p, bytes, align);
            return;
        }
        const std::size_t cls = class_of(bytes);
        auto* n = static_cast<FreeNode*>(p);
        n->next = free_[cls];
        free_[cls] = n;
    }

    // Two resources are interchangeable only if they are the same object, which
    // is the correct answer for a resource that owns its own chunks.
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    void new_chunk() {
        void* base = upstream_->allocate(chunk_bytes_, alignof(std::max_align_t));
        chunks_.push_back(Chunk{base, chunk_bytes_});
        upstream_bytes_ += chunk_bytes_;
        carve_      = base;
        carve_left_ = chunk_bytes_;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t                chunk_bytes_;

    std::vector<Chunk>                 chunks_;
    std::array<FreeNode*, kClasses>    free_{};
    void*                              carve_      = nullptr;
    std::size_t                        carve_left_ = 0;

    std::size_t upstream_bytes_ = 0;
    uint64_t    reused_    = 0;
    uint64_t    carved_    = 0;
    uint64_t    oversized_ = 0;
};

} // namespace tick
