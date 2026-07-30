#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Stock locate to ticker.
//
// ITCH gives every instrument a sixteen bit stock locate at the start of the
// day, in the R stock directory messages, and every later message carries the
// locate rather than the ticker. That is the exchange doing the interning for
// you, and it is why nothing on the hot path ever compares strings.
//
// So this is a flat array indexed by the locate, not a hash map. Sixty five
// thousand entries of a small struct is a few megabytes, it is allocated once,
// and the lookup is a bounds check and an index. A NASDAQ day defines about
// nine thousand locates, so most of the array is empty and that is fine. The
// alternative saves a couple of megabytes and costs a hash on the one lookup
// per message that has to happen no matter what.
//
// The ticker is stored inline as eight bytes rather than as a std::string,
// because a string here would put a heap allocation and a pointer chase behind
// a field that is only ever printed.

namespace tick {

struct SymbolInfo {
    std::array<char, 8> name{};
    uint8_t             name_len  = 0;
    uint32_t            round_lot = 0;
    char                trading_state = 'T'; // T trading, H halted, Q quote only
    bool                known     = false;

    [[nodiscard]] std::string_view ticker() const noexcept {
        return {name.data(), name_len};
    }
};

class SymbolTable {
public:
    // The locate field is sixteen bits, so this is the whole space.
    static constexpr std::size_t kCapacity = 1u << 16;

    SymbolTable() : entries_(kCapacity) {}

    void define(uint16_t locate, std::string_view ticker, uint32_t round_lot) noexcept {
        SymbolInfo& e = entries_[locate];
        const std::size_t n = ticker.size() < e.name.size() ? ticker.size() : e.name.size();
        for (std::size_t i = 0; i < n; ++i) e.name[i] = ticker[i];
        e.name_len  = static_cast<uint8_t>(n);
        e.round_lot = round_lot;
        if (!e.known) ++defined_;
        e.known = true;
    }

    void set_trading_state(uint16_t locate, char state) noexcept {
        entries_[locate].trading_state = state;
    }

    [[nodiscard]] const SymbolInfo& operator[](uint16_t locate) const noexcept {
        return entries_[locate];
    }

    [[nodiscard]] std::string_view ticker(uint16_t locate) const noexcept {
        return entries_[locate].ticker();
    }

    // Reverse lookup, for command line tools that take a ticker. Linear, and
    // deliberately so, because it runs once at startup and never on the feed.
    [[nodiscard]] int find(std::string_view ticker) const noexcept {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].known && entries_[i].ticker() == ticker) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] std::size_t defined() const noexcept { return defined_; }

private:
    std::vector<SymbolInfo> entries_;
    std::size_t             defined_ = 0;
};

} // namespace tick
