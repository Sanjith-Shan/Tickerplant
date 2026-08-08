#pragma once

#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// A thin RAII wrapper over HdrHistogram_c, and a statement of why this project
// reports two numbers where most report one.
//
// WHY A HISTOGRAM AT ALL
//
// A mean latency is not a latency. The distribution of a feed handler's
// per message cost is long tailed by construction, because a cache miss, a page
// fault, a scheduler preemption and a branch mispredict all produce outliers
// that are orders of magnitude above the body of the distribution. Averaging
// over them produces a number that describes no message that was ever
// processed. What matters is the tail, so the tail has to be recorded, and a
// list of every sample is too much memory to keep at a million messages a
// second. HDR histograms solve exactly this. They bucket values on a
// logarithmic scale with a fixed number of significant digits, so they hold a
// range from one nanosecond to a minute in a few tens of kilobytes while still
// resolving the 99.99th percentile to three digits.
//
// This wrapper exists because the C library's ownership model is manual, and a
// histogram that leaks or is double freed in the middle of a benchmark is a
// silent source of wrong numbers. RAII, move only, no copies.
//
// COORDINATED OMISSION
//
// This is the part most latency numbers get wrong, and it is worth being
// precise about because the failure is invisible in the output.
//
// Suppose a load generator is meant to send one message every microsecond. It
// sends, waits for the response, records the round trip, and loops. Now suppose
// the system under test stalls for 10 milliseconds. The generator is blocked in
// that stall. It does not send during the stall, because it is waiting. When
// the stall ends it records one bad sample, of roughly 10 ms, and resumes.
//
// The 10000 messages that a real fixed rate sender would have pushed during
// that stall were never sent, so they were never measured. Those are precisely
// the messages that would have queued behind the stall and seen latencies of
// 10 ms, 9.999 ms, 9.998 ms and so on down. The measurement apparatus stopped
// sampling exactly when latency was worst, and only because latency was worst.
// The recorded distribution therefore contains one outlier where it should
// contain ten thousand, the tail is flattened by four orders of magnitude, and
// nothing in the output says so. The p99 looks excellent. That is coordinated
// omission. The name is Gil Tene's and the failure is the default behaviour of
// almost every hand rolled benchmark loop.
//
// record_corrected is the correction. Given the interval at which a message was
// supposed to arrive, it records the observed sample and then back fills the
// samples that a sender running at that fixed rate would have produced while
// the system was stalled, each one shorter than the last by one interval, down
// to the interval itself. It reconstructs the queue that the blocked generator
// failed to build. It is an estimate rather than a measurement, because the
// samples it invents were never actually taken, but it is an estimate of
// something real, and it is far closer to the truth than silently dropping
// them.
//
// WHICH ONE THIS PROJECT REPORTS
//
// Both, side by side, in every results table. Not one or the other.
//
// The raw distribution answers "how long does this code take when it runs".
// The corrected distribution answers "what would a consumer of this feed have
// experienced". Those are different questions and a feed handler has to answer
// both. Reporting only the raw number hides stalls. Reporting only the
// corrected number makes the code look slower than it is and hides which part
// of the system is actually at fault.
//
// The gap between the two IS the information. If raw p99 and corrected p99 are
// close, the handler kept up and there were no meaningful stalls. If corrected
// p99 is much larger, something stalled, and the size of the gap is a direct
// measure of how much work piled up behind it. That comparison is the most
// useful single line in the results, and it only exists because both are kept.

namespace tick {

class Histogram {
public:
    // lowest and highest are in the unit being recorded, which is nanoseconds
    // everywhere in this project.
    //
    // The default ceiling of 60 seconds is deliberately absurd for a decode
    // step. HdrHistogram_c discards a value above the ceiling rather than
    // clamping it, and a discarded outlier is the same failure as coordinated
    // omission, so the ceiling is set where nothing real can exceed it. The
    // cost of the headroom is bucket array size, which at three significant
    // digits is tens of kilobytes and therefore not worth optimising.
    explicit Histogram(int64_t lowest = 1,
                       int64_t highest = 60'000'000'000LL,
                       int significant_digits = 3) {
        if (hdr_init(lowest, highest, significant_digits, &h_) != 0 || h_ == nullptr) {
            throw std::runtime_error("tick::Histogram failed to allocate hdr_histogram");
        }
    }

    ~Histogram() { if (h_) hdr_close(h_); }

    // Move only. Copying a histogram is almost always a bug in a benchmark,
    // because it silently splits a stream of samples into two distributions,
    // and add() covers the one legitimate case of merging per thread results.
    Histogram(const Histogram&)            = delete;
    Histogram& operator=(const Histogram&) = delete;

    Histogram(Histogram&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    Histogram& operator=(Histogram&& other) noexcept {
        if (this != &other) {
            if (h_) hdr_close(h_);
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }

    // The raw sample. Out of range values are dropped by the library and
    // counted here, so a run that overflowed its ceiling can be caught rather
    // than quietly reporting a truncated tail.
    void record(int64_t value) noexcept {
        if (!hdr_record_value(h_, value)) ++dropped_;
    }

    // The sample plus the back fill described in the header comment above.
    // expected_interval is the period at which a sample was supposed to arrive,
    // in the same unit as value. Passing zero or a negative interval makes this
    // behave exactly like record.
    void record_corrected(int64_t value, int64_t expected_interval) noexcept {
        if (!hdr_record_corrected_value(h_, value, expected_interval)) ++dropped_;
    }

    [[nodiscard]] int64_t value_at_percentile(double p) const noexcept {
        return hdr_value_at_percentile(h_, p);
    }

    [[nodiscard]] int64_t min() const noexcept { return hdr_min(h_); }
    [[nodiscard]] int64_t max() const noexcept { return hdr_max(h_); }
    [[nodiscard]] double  mean() const noexcept { return hdr_mean(h_); }
    [[nodiscard]] double  stddev() const noexcept { return hdr_stddev(h_); }
    [[nodiscard]] int64_t count() const noexcept { return h_->total_count; }

    // Samples the library refused because they were outside the configured
    // range. Any results table printing a percentile from a histogram with a
    // non zero drop count is printing a lie, so this is public and is checked.
    [[nodiscard]] uint64_t dropped() const noexcept { return dropped_; }

    void reset() noexcept { hdr_reset(h_); dropped_ = 0; }

    // Merge another histogram into this one. The intended use is combining per
    // thread histograms after a run, which is why each thread gets its own and
    // no locking happens on the hot path.
    void add(const Histogram& other) noexcept {
        dropped_ += static_cast<uint64_t>(hdr_add(h_, other.h_));
    }

    // One line, for a console run or a log.
    [[nodiscard]] std::string summary() const {
        std::string s;
        s += "n=";        s += std::to_string(count());
        s += " min=";     s += std::to_string(min());
        s += " p50=";     s += std::to_string(value_at_percentile(50.0));
        s += " p90=";     s += std::to_string(value_at_percentile(90.0));
        s += " p99=";     s += std::to_string(value_at_percentile(99.0));
        s += " p99.9=";   s += std::to_string(value_at_percentile(99.9));
        s += " p99.99=";  s += std::to_string(value_at_percentile(99.99));
        s += " max=";     s += std::to_string(max());
        if (dropped_) { s += " DROPPED="; s += std::to_string(dropped_); }
        return s;
    }

    // The same figures as a JSON object, for the results files the benchmark
    // runner writes. Emitted by hand rather than with a JSON library because
    // one object of numbers does not justify a dependency, and every field here
    // is an integer or a controlled label.
    [[nodiscard]] std::string to_json(std::string_view label) const {
        std::string s = "{\"label\":\"";
        s += json_escape(label);
        s += "\",\"count\":";    s += std::to_string(count());
        s += ",\"min_ns\":";     s += std::to_string(min());
        s += ",\"p50_ns\":";     s += std::to_string(value_at_percentile(50.0));
        s += ",\"p90_ns\":";     s += std::to_string(value_at_percentile(90.0));
        s += ",\"p99_ns\":";     s += std::to_string(value_at_percentile(99.0));
        s += ",\"p99_9_ns\":";   s += std::to_string(value_at_percentile(99.9));
        s += ",\"p99_99_ns\":";  s += std::to_string(value_at_percentile(99.99));
        s += ",\"max_ns\":";     s += std::to_string(max());
        s += ",\"mean_ns\":";    s += fixed2(mean());
        s += ",\"stddev_ns\":";  s += fixed2(stddev());
        s += ",\"dropped\":";    s += std::to_string(dropped_);
        s += "}";
        return s;
    }

    // The full percentile distribution, so a tail can be plotted rather than
    // summarised. A results table gives five percentiles and a chart of the
    // whole curve is what actually shows where the tail turns up, which is the
    // interesting part and is invisible in any fixed set of quantiles.
    //
    // The x axis column is 1/(1-percentile), the conventional scale for these
    // plots, because it turns the tail into a straight line rather than a wall
    // at the right edge. The last bucket has percentile exactly 100, where that
    // expression divides by zero, so it is written as an explicit infinity
    // rather than left to produce a platform dependent token.
    void write_percentiles_csv(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (!f) throw std::runtime_error("tick::Histogram cannot open " + path);

        std::fprintf(f, "value_ns,percentile,total_count,inverted_percentile\n");

        hdr_iter iter;
        hdr_iter_percentile_init(&iter, h_, 5 /* ticks per half distance */);
        while (hdr_iter_next(&iter)) {
            const double pct = iter.specifics.percentiles.percentile;
            const double rem = 1.0 - pct / 100.0;
            if (rem > 0.0) {
                std::fprintf(f, "%lld,%.12f,%lld,%.6f\n",
                             static_cast<long long>(iter.value),
                             pct,
                             static_cast<long long>(iter.cumulative_count),
                             1.0 / rem);
            } else {
                std::fprintf(f, "%lld,%.12f,%lld,inf\n",
                             static_cast<long long>(iter.value),
                             pct,
                             static_cast<long long>(iter.cumulative_count));
            }
        }
        std::fclose(f);
    }

    // Escape hatch for code that needs the C handle, such as a log writer.
    [[nodiscard]] hdr_histogram*       raw()       noexcept { return h_; }
    [[nodiscard]] const hdr_histogram* raw() const noexcept { return h_; }

private:
    static std::string fixed2(double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return std::string(buf);
    }

    // Labels come from benchmark configuration rather than from the wire, but a
    // label with a quote in it would produce a results file no parser can read,
    // and a silently corrupt results file is worse than a rejected one.
    static std::string json_escape(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
            }
        }
        return out;
    }

    hdr_histogram* h_       = nullptr;
    uint64_t       dropped_ = 0;
};

} // namespace tick
