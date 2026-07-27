#pragma once

#include "tick/endian.hpp"
#include "tick/itch.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

// Reading a NASDAQ ITCH 5.0 sample file.
//
// The file is a flat stream of length-prefixed messages. Two bytes big-endian
// say how long the next message is, then that many bytes are the message. There
// are no record separators, no padding, and no index. NASDAQ ships the file
// gzipped and a full day is three to five gigabytes compressed and roughly
// three times that raw, so it is decompressed as a stream and never held in
// memory.
//
// The buffer compacts rather than cycles. A message can straddle a refill
// boundary, and the cheapest correct answer at this size is to move the tail of
// the buffer to the front and read again. That memmove happens once per refill,
// so once per megabyte, not once per message.

namespace tick {

class ItchFile {
public:
    // A refill reads this much at a time. One megabyte is large enough that the
    // per-refill memmove and gzread overhead disappear against the work done on
    // the roughly thirty thousand messages inside it.
    static constexpr std::size_t kBufferSize = 1u << 20;

    explicit ItchFile(const std::string& path) {
        gz_ = gzopen(path.c_str(), "rb");
        if (gz_ == nullptr) {
            throw std::runtime_error("cannot open ITCH file " + path);
        }
        // zlib's own buffer sits under ours. Raising it measurably cuts the
        // inflate call count on a multi gigabyte file.
        gzbuffer(gz_, 1u << 18);
        buf_.resize(kBufferSize);
    }

    ~ItchFile() {
        if (gz_ != nullptr) gzclose(gz_);
    }

    ItchFile(const ItchFile&)            = delete;
    ItchFile& operator=(const ItchFile&) = delete;

    // Hand back the next message, without its two byte length prefix. Returns
    // false at clean end of file. The span points into the internal buffer and
    // stays valid until the next call.
    [[nodiscard]] bool next(std::span<const std::byte>& out) {
        // The length prefix and then the body have to be present together.
        if (avail() < 2 && !refill(2)) return false;

        std::size_t len = be_load<uint16_t>(data() + pos_);
        if (len == 0) {
            // A zero length frame is the end of session marker some files carry.
            return false;
        }
        if (avail() < 2 + len && !refill(2 + len)) {
            ++truncated_;
            return false;
        }

        out = std::span<const std::byte>(data() + pos_ + 2, len);
        pos_ += 2 + len;
        ++framed_;
        bytes_ += 2 + len;
        return true;
    }

    [[nodiscard]] uint64_t framed_messages() const noexcept { return framed_; }
    [[nodiscard]] uint64_t bytes_read() const noexcept { return bytes_; }
    [[nodiscard]] uint64_t truncated_frames() const noexcept { return truncated_; }

private:
    [[nodiscard]] std::byte* data() noexcept {
        return reinterpret_cast<std::byte*>(buf_.data());
    }
    [[nodiscard]] std::size_t avail() const noexcept { return end_ - pos_; }

    // Make at least want bytes available, moving the unread tail to the front
    // and reading more. Returns false when the file cannot supply that many,
    // which at the end of a clean file means the stream ended on a boundary.
    bool refill(std::size_t want) {
        if (want > buf_.size()) {
            // No ITCH message approaches a megabyte. This is a corrupt length.
            return false;
        }
        const std::size_t tail = avail();
        if (tail > 0 && pos_ > 0) {
            std::memmove(buf_.data(), buf_.data() + pos_, tail);
        }
        pos_ = 0;
        end_ = tail;

        while (end_ < want) {
            const int n = gzread(gz_, buf_.data() + end_,
                                 static_cast<unsigned>(buf_.size() - end_));
            if (n < 0) {
                int         err = 0;
                const char* msg = gzerror(gz_, &err);
                throw std::runtime_error(std::string("gzread failed: ") +
                                         (msg ? msg : "unknown"));
            }
            if (n == 0) return false; // clean end of file
            end_ += static_cast<std::size_t>(n);
        }
        return true;
    }

    gzFile                 gz_   = nullptr;
    std::vector<char>      buf_;
    std::size_t            pos_  = 0;
    std::size_t            end_  = 0;
    uint64_t               framed_ = 0;
    uint64_t               bytes_  = 0;
    uint64_t               truncated_ = 0;
};

} // namespace tick
