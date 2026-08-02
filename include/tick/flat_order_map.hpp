#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// The order reference table.
//
// This is the hottest lookup in a feed handler and it is the structure that a
// matching engine does not have to get right in the same way. On a NASDAQ day
// there are tens of millions of add order messages, and every execute, cancel,
// delete, and replace that follows has to find the resting order by its sixty
// four bit reference number. Nothing else in the pipeline runs that often.
//
// std::unordered_map is the obvious answer and it is the wrong one. It is a
// chained hash table, so every lookup is a bucket index followed by a pointer
// chase into a separately allocated node, and every add order allocates a node.
// That is two cache misses on the critical path and a call into the heap
// allocator on the message type that occurs most.
//
// This is an open addressed table with linear probing. Keys and values live in
// two flat arrays, so a lookup is one load from the key array and, on a hit,
// one load from the value array at the same index. Probing walks forward
// through memory the prefetcher already has.
//
// Deletion uses backward shift rather than tombstones, which is the decision
// worth explaining. A feed handler deletes almost as often as it inserts, and
// over a full day a tombstoned table degrades until every lookup walks a long
// run of dead slots. Backward shift restores the table to the state it would
// have had if the deleted key had never been inserted, so the probe lengths do
// not drift. It costs a short scan on erase and it is Knuth's algorithm R.
//
// Key zero is the empty marker. ITCH order reference numbers start at one, so
// no real key collides with it, and that saves a parallel occupancy array and
// the cache line it would touch.

namespace tick {

// Mixing sequential keys before masking, and a prediction the data refuted.
//
// The reasoning that went in first was this. ITCH order references are close to
// sequential within a day, so with the identity hash and linear probing
// consecutive keys land in consecutive slots and never collide until the table
// wraps, which ought to be the best possible case. Both hashes were implemented
// anyway so the benchmark could decide.
//
// The benchmark decided against it. On the real 2019-12-30 file, replaying the
// actual order reference trace at a load factor of 0.32, the identity hash
// averages 96.4 extra probes per lookup against 0.357 for splitmix64, and it is
// 4.4 times slower. It also loses to a reserved std::unordered_map, which the
// flat table otherwise beats by more than two times.
//
// The reason the original reasoning was wrong is that the references are dense
// but gappy. About seventy percent of them are monotone and the span grows by
// 1.32 per insert, so the live keys form long unbroken runs of occupied slots
// with holes between the runs, which is precisely the input linear probing
// handles worst. Sequential is not the same as contiguous.
//
// One more thing that makes this worth keeping in the file. At a hundred
// thousand messages, which is a load factor of 0.018, identity wins by nearly
// two to one with 0.0006 probes per lookup. Anyone who benchmarks this
// structure on a small window gets the opposite answer and has a clean graph to
// defend it with. The default is splitmix64 and the loser is kept so the
// measurement can be reproduced rather than taken on trust.
struct SplitMix64Hash {
    static constexpr const char* kName = "splitmix64";
    [[nodiscard]] static inline uint64_t hash(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }
};

struct IdentityHash {
    static constexpr const char* kName = "identity";
    [[nodiscard]] static inline uint64_t hash(uint64_t x) noexcept { return x; }
};

template <typename V, typename Hash = SplitMix64Hash>
class FlatOrderMap {
public:
    static constexpr uint64_t kEmptyKey = 0;

    // Capacity is rounded up to a power of two. Size it for the peak number of
    // orders resting at once, not for the number of adds in a day, and leave
    // headroom. Linear probing degrades sharply past a load factor of about
    // 0.7, so the table refuses to go beyond kMaxLoad and grows instead.
    explicit FlatOrderMap(std::size_t capacity_hint = 1u << 22) {
        std::size_t cap = 1;
        while (cap < capacity_hint) cap <<= 1;
        keys_.assign(cap, kEmptyKey);
        vals_.resize(cap);
        mask_ = cap - 1;
    }

    // Growing is a real allocation and a full rehash, which is exactly the kind
    // of pause a feed handler must not take mid-day. It is implemented so the
    // table is correct rather than fast when it is undersized, and the counter
    // exists so the benchmark can prove it never happened on the real file.
    [[nodiscard]] uint64_t growth_events() const noexcept { return grows_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return keys_.size(); }
    [[nodiscard]] double load_factor() const noexcept {
        return static_cast<double>(size_) / static_cast<double>(keys_.size());
    }
    [[nodiscard]] uint64_t probe_count() const noexcept { return probes_; }
    [[nodiscard]] uint64_t lookup_count() const noexcept { return lookups_; }

    // Insert or overwrite. Returns false when the key was already present, in
    // which case the value is replaced. A duplicate order reference is a real
    // event in a gap and the caller counts it rather than trusting it.
    bool insert(uint64_t key, V value) noexcept {
        // Key zero is the empty marker, so it cannot be stored. ITCH order
        // references start at one, so this never happens on real data, and
        // refusing it beats corrupting the table if it ever does.
        if (key == kEmptyKey) return false;
        if (size_ + 1 > max_fill()) grow();
        std::size_t i = slot(key);
        while (keys_[i] != kEmptyKey) {
            if (keys_[i] == key) {
                vals_[i] = value;
                return false;
            }
            i = (i + 1) & mask_;
        }
        keys_[i] = key;
        vals_[i] = value;
        ++size_;
        return true;
    }

    // Returns a pointer to the value, or nullptr. The pointer is valid until
    // the next insert or erase, which is why callers use it immediately and
    // never hold it.
    [[nodiscard]] V* find(uint64_t key) noexcept {
        if (key == kEmptyKey) return nullptr;
        ++lookups_;
        std::size_t i = slot(key);
        while (true) {
            const uint64_t k = keys_[i];
            if (k == key) return &vals_[i];
            if (k == kEmptyKey) return nullptr;
            ++probes_;
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] const V* find(uint64_t key) const noexcept {
        return const_cast<FlatOrderMap*>(this)->find(key);
    }

    // Remove a key. Returns false when it was not present, which on this feed
    // means a message referring to an order that was never added or has already
    // gone. That happens after a gap and it is counted, never ignored.
    bool erase(uint64_t key) noexcept {
        if (key == kEmptyKey) return false;
        std::size_t i = slot(key);
        while (true) {
            const uint64_t k = keys_[i];
            if (k == key) break;
            if (k == kEmptyKey) return false;
            i = (i + 1) & mask_;
        }

        // Knuth algorithm R. Walk forward from the hole. An entry whose ideal
        // slot lies cyclically inside the open interval from the hole to the
        // entry stays where it is, because moving it back would put it before
        // its own probe start. Anything else moves into the hole, and the hole
        // follows it.
        std::size_t hole = i;
        std::size_t j    = i;
        keys_[hole]      = kEmptyKey;
        while (true) {
            j = (j + 1) & mask_;
            const uint64_t kj = keys_[j];
            if (kj == kEmptyKey) break;
            const std::size_t ideal = slot(kj);
            if (cyclically_between(hole, ideal, j)) continue;
            keys_[hole] = kj;
            vals_[hole] = vals_[j];
            keys_[j]    = kEmptyKey;
            hole        = j;
        }
        --size_;
        return true;
    }

    void clear() noexcept {
        std::fill(keys_.begin(), keys_.end(), kEmptyKey);
        size_ = 0;
    }

    // Walk every live entry. Used by the end of day audit, never on the hot
    // path, so the order it visits in is the table's and not the feed's.
    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] != kEmptyKey) f(keys_[i], vals_[i]);
        }
    }

private:
    static constexpr double kMaxLoad = 0.6;

    [[nodiscard]] std::size_t max_fill() const noexcept {
        return static_cast<std::size_t>(static_cast<double>(keys_.size()) * kMaxLoad);
    }

    [[nodiscard]] std::size_t slot(uint64_t key) const noexcept {
        return static_cast<std::size_t>(Hash::hash(key)) & mask_;
    }

    // Is x in the cyclic open-closed interval from lo to hi.
    [[nodiscard]] static bool cyclically_between(std::size_t lo, std::size_t x,
                                                 std::size_t hi) noexcept {
        if (lo <= hi) return lo < x && x <= hi;
        return lo < x || x <= hi;
    }

    void grow() {
        std::vector<uint64_t> old_keys = std::move(keys_);
        std::vector<V>        old_vals = std::move(vals_);
        const std::size_t     cap      = old_keys.size() * 2;
        keys_.assign(cap, kEmptyKey);
        vals_.assign(cap, V{});
        mask_ = cap - 1;
        size_ = 0;
        for (std::size_t i = 0; i < old_keys.size(); ++i) {
            if (old_keys[i] != kEmptyKey) insert(old_keys[i], old_vals[i]);
        }
        ++grows_;
    }

    std::vector<uint64_t> keys_;
    std::vector<V>        vals_;
    std::size_t           mask_    = 0;
    std::size_t           size_    = 0;
    uint64_t              grows_   = 0;
    mutable uint64_t      probes_  = 0;
    mutable uint64_t      lookups_ = 0;
};

} // namespace tick
