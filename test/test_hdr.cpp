#include "tick/hdr.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

// Tests for the histogram wrapper, and in particular for the coordinated
// omission correction, which is the part of this file that carries an argument
// rather than just an API.

namespace {

// HdrHistogram is lossy by design. Three significant digits means a recorded
// value can come back as anything within one part in a thousand of itself, so
// every assertion on a value has to allow that. A test written with EXPECT_EQ
// against a recorded value passes for small numbers and fails for large ones,
// which is a confusing way to learn how the library works.
::testing::AssertionResult WithinThreeSigFigs(int64_t expected, int64_t actual) {
    const double tolerance = static_cast<double>(expected) / 1000.0 + 1.0;
    const double diff = static_cast<double>(actual) - static_cast<double>(expected);
    if (diff < 0) {
        if (-diff <= tolerance) return ::testing::AssertionSuccess();
    } else if (diff <= tolerance) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
        << actual << " is not within three significant figures of " << expected;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A unique scratch path per test, so a parallel ctest run does not have two
// tests writing the same file.
std::string scratch_path(const char* name) {
    return std::string("tick_hdr_test_") + name + "_" +
           std::to_string(static_cast<long long>(::getpid())) + ".csv";
}

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

TEST(Histogram, StartsEmpty) {
    tick::Histogram h;
    EXPECT_EQ(h.count(), 0);
    EXPECT_EQ(h.dropped(), 0u);
}

TEST(Histogram, RecordsAndCounts) {
    tick::Histogram h;
    for (int i = 0; i < 1000; ++i) h.record(100);
    EXPECT_EQ(h.count(), 1000);
    EXPECT_EQ(h.dropped(), 0u);
    EXPECT_TRUE(WithinThreeSigFigs(100, h.min()));
    EXPECT_TRUE(WithinThreeSigFigs(100, h.max()));
    EXPECT_NEAR(h.mean(), 100.0, 1.0);
}

TEST(Histogram, KnownValuesGiveTheExpectedPercentiles) {
    // One sample at each of 1 through 1000 nanoseconds. With a uniform
    // distribution over that range the pth percentile is p*10, which makes
    // every expected value checkable by hand rather than by running the code
    // and pasting whatever it printed.
    tick::Histogram h;
    for (int64_t v = 1; v <= 1000; ++v) h.record(v);

    EXPECT_EQ(h.count(), 1000);
    EXPECT_TRUE(WithinThreeSigFigs(1, h.min()));
    EXPECT_TRUE(WithinThreeSigFigs(1000, h.max()));

    EXPECT_TRUE(WithinThreeSigFigs(500,  h.value_at_percentile(50.0)));
    EXPECT_TRUE(WithinThreeSigFigs(900,  h.value_at_percentile(90.0)));
    EXPECT_TRUE(WithinThreeSigFigs(990,  h.value_at_percentile(99.0)));
    EXPECT_TRUE(WithinThreeSigFigs(999,  h.value_at_percentile(99.9)));
    EXPECT_TRUE(WithinThreeSigFigs(1000, h.value_at_percentile(100.0)));

    // Percentiles must not go backwards. An inverted pair would mean the
    // wrapper is passing the argument in the wrong units, since the C library
    // takes a percentage rather than a fraction, and that mistake produces
    // plausible looking numbers rather than an obvious failure.
    EXPECT_LE(h.value_at_percentile(50.0), h.value_at_percentile(90.0));
    EXPECT_LE(h.value_at_percentile(90.0), h.value_at_percentile(99.0));
    EXPECT_LE(h.value_at_percentile(99.0), h.value_at_percentile(99.9));
}

TEST(Histogram, TailIsWhereItWasPut) {
    // 99 samples at 10 ns and 1 at 10000 ns. The median must stay at the body
    // of the distribution and the max must show the outlier. This is the shape
    // of a real feed handler distribution and it is the case where reporting a
    // mean would be most misleading, since the mean here is about 110 and no
    // sample was anywhere near it.
    tick::Histogram h;
    for (int i = 0; i < 99; ++i) h.record(10);
    h.record(10000);

    EXPECT_TRUE(WithinThreeSigFigs(10, h.value_at_percentile(50.0)));
    EXPECT_TRUE(WithinThreeSigFigs(10, h.value_at_percentile(90.0)));
    EXPECT_TRUE(WithinThreeSigFigs(10000, h.max()));
    EXPECT_GT(h.mean(), 100.0);
    EXPECT_LT(h.mean(), 120.0);
}

TEST(Histogram, ResetClearsEverything) {
    tick::Histogram h;
    for (int i = 0; i < 100; ++i) h.record(42);
    h.reset();
    EXPECT_EQ(h.count(), 0);
    EXPECT_EQ(h.dropped(), 0u);
}

TEST(Histogram, AddMergesTwoDistributions) {
    tick::Histogram a, b;
    for (int64_t v = 1; v <= 500; ++v) a.record(v);
    for (int64_t v = 501; v <= 1000; ++v) b.record(v);

    a.add(b);
    EXPECT_EQ(a.count(), 1000);
    EXPECT_TRUE(WithinThreeSigFigs(500,  a.value_at_percentile(50.0)));
    EXPECT_TRUE(WithinThreeSigFigs(1000, a.max()));
    // b is untouched, so per thread histograms can be merged into an aggregate
    // without destroying the per thread view.
    EXPECT_EQ(b.count(), 500);
}

TEST(Histogram, MoveLeavesTheSourceUsableEnoughToDestroy) {
    tick::Histogram a;
    for (int i = 0; i < 10; ++i) a.record(100);
    tick::Histogram b(std::move(a));
    EXPECT_EQ(b.count(), 10);
    // a is now empty and its destructor must not double free, which is the
    // whole reason this class is move only rather than copyable.
}

TEST(Histogram, ValuesAboveTheCeilingAreCountedAsDroppedAndNotClamped) {
    // A deliberately tiny range, so the drop path can be exercised. The point
    // is that an out of range sample is visible rather than silently absent,
    // because a silently absent outlier is coordinated omission by another
    // route.
    tick::Histogram h(1, 1000, 3);
    h.record(500);
    h.record(1000000);
    EXPECT_EQ(h.dropped(), 1u);
    EXPECT_EQ(h.count(), 1);
    EXPECT_TRUE(WithinThreeSigFigs(500, h.max()));
}

// ---------------------------------------------------------------------------
// Coordinated omission
// ---------------------------------------------------------------------------

TEST(Histogram, RecordCorrectedBackFillsAKnownStall) {
    // The scenario from the header comment, with numbers chosen so the answer
    // can be worked out on paper.
    //
    // A sender that intends one message every 1 ns. Ten thousand messages are
    // served in 1 ns each. Then one message takes 10000 ns because the system
    // stalled.
    //
    // Raw, that is 10001 samples, of which exactly one is large. p99 is 1.
    // Corrected, the single 10000 ns sample expands into 10000 samples of
    // 10000, 9999, 9998 and so on down to 1, because that is the queue a fixed
    // rate sender would have built during the stall.
    const int64_t interval = 1;

    tick::Histogram raw;
    tick::Histogram corrected;

    for (int i = 0; i < 10000; ++i) {
        raw.record(interval);
        corrected.record_corrected(interval, interval);
    }
    raw.record(10000);
    corrected.record_corrected(10000, interval);

    EXPECT_EQ(raw.count(), 10001);

    // The back fill adds the 9999 samples the blocked sender never took, so the
    // corrected histogram holds 10000 more entries than the raw one.
    EXPECT_EQ(corrected.count(), 20000);

    // This is the whole point, stated as an assertion. The raw p99 says the
    // system is fine. It is not fine.
    EXPECT_TRUE(WithinThreeSigFigs(1, raw.value_at_percentile(99.0)));
    EXPECT_GT(corrected.value_at_percentile(99.0), 100);

    // Both agree on the worst single observation, because the correction adds
    // samples below the outlier and never invents one above it.
    EXPECT_TRUE(WithinThreeSigFigs(10000, raw.max()));
    EXPECT_TRUE(WithinThreeSigFigs(10000, corrected.max()));

    // And the median is unmoved, because half the corrected samples are still
    // the well served ones.
    EXPECT_LT(corrected.value_at_percentile(50.0), 100);
}

TEST(Histogram, RecordCorrectedIsIdenticalToRecordWhenNothingStalled) {
    // If every sample arrives at the expected interval there is nothing to back
    // fill, so the two must agree exactly. If they did not, the correction
    // would be inflating every ordinary run and the reported numbers would be
    // pessimistic for no reason.
    tick::Histogram raw, corrected;
    for (int i = 0; i < 1000; ++i) {
        raw.record(100);
        corrected.record_corrected(100, 100);
    }
    EXPECT_EQ(raw.count(), corrected.count());
    EXPECT_EQ(raw.value_at_percentile(99.0), corrected.value_at_percentile(99.0));
    EXPECT_EQ(raw.max(), corrected.max());
}

TEST(Histogram, RecordCorrectedWithNoIntervalDegradesToPlainRecord) {
    // Zero means "no expected rate", which is the right behaviour for a
    // measurement that is not rate driven at all, such as timing a decode loop
    // with no sender in the picture.
    tick::Histogram a, b;
    a.record(5000);
    b.record_corrected(5000, 0);
    EXPECT_EQ(a.count(), b.count());
    EXPECT_EQ(a.max(), b.max());
}

// ---------------------------------------------------------------------------
// Output shape
// ---------------------------------------------------------------------------

TEST(Histogram, SummaryMentionsEveryPercentileItPromises) {
    tick::Histogram h;
    for (int64_t v = 1; v <= 1000; ++v) h.record(v);
    const std::string s = h.summary();

    EXPECT_NE(s.find("n=1000"), std::string::npos);
    EXPECT_NE(s.find("p50="),    std::string::npos);
    EXPECT_NE(s.find("p90="),    std::string::npos);
    EXPECT_NE(s.find("p99="),    std::string::npos);
    EXPECT_NE(s.find("p99.9="),  std::string::npos);
    EXPECT_NE(s.find("p99.99="), std::string::npos);
    EXPECT_NE(s.find("max="),    std::string::npos);
    // No drops in this run, so the warning must be absent rather than present
    // with a zero, since a reader scanning for the word should only find it
    // when it matters.
    EXPECT_EQ(s.find("DROPPED"), std::string::npos);
}

TEST(Histogram, SummarySaysSoWhenSamplesWereDropped) {
    tick::Histogram h(1, 1000, 3);
    h.record(500);
    h.record(999999);
    EXPECT_NE(h.summary().find("DROPPED=1"), std::string::npos);
}

TEST(Histogram, JsonHasTheExpectedKeysAndIsWellFormed) {
    tick::Histogram h;
    for (int64_t v = 1; v <= 1000; ++v) h.record(v);
    const std::string j = h.to_json("zero-copy decode");

    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(),  '}');
    EXPECT_NE(j.find("\"label\":\"zero-copy decode\""), std::string::npos);
    EXPECT_NE(j.find("\"count\":1000"),  std::string::npos);
    EXPECT_NE(j.find("\"p50_ns\":"),     std::string::npos);
    EXPECT_NE(j.find("\"p90_ns\":"),     std::string::npos);
    EXPECT_NE(j.find("\"p99_ns\":"),     std::string::npos);
    EXPECT_NE(j.find("\"p99_9_ns\":"),   std::string::npos);
    EXPECT_NE(j.find("\"p99_99_ns\":"),  std::string::npos);
    EXPECT_NE(j.find("\"max_ns\":"),     std::string::npos);
    EXPECT_NE(j.find("\"mean_ns\":"),    std::string::npos);
    EXPECT_NE(j.find("\"dropped\":0"),   std::string::npos);

    // Braces balance, which is a cheap structural check that catches a missing
    // closing brace after an edit.
    int depth = 0;
    for (char c : j) {
        if (c == '{') ++depth;
        if (c == '}') --depth;
        ASSERT_GE(depth, 0);
    }
    EXPECT_EQ(depth, 0);
}

TEST(Histogram, JsonEscapesALabelThatWouldBreakTheFile) {
    tick::Histogram h;
    h.record(1);
    const std::string j = h.to_json("a \"quoted\" label");
    // The raw quote must not appear unescaped, since one of those turns a
    // results file into something no parser will read.
    EXPECT_NE(j.find("\\\"quoted\\\""), std::string::npos);
}

TEST(Histogram, PercentilesCsvHasAHeaderAndMonotonicValues) {
    tick::Histogram h;
    for (int64_t v = 1; v <= 10000; ++v) h.record(v);

    const std::string path = scratch_path("percentiles");
    h.write_percentiles_csv(path);

    const std::string body = slurp(path);
    ASSERT_FALSE(body.empty());

    std::istringstream ss(body);
    std::string line;
    ASSERT_TRUE(std::getline(ss, line));
    EXPECT_EQ(line, "value_ns,percentile,total_count,inverted_percentile");

    int rows = 0;
    int64_t prev_value = -1;
    double  prev_pct   = -1.0;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        ++rows;
        // value,percentile,total_count,inverted
        const auto c1 = line.find(',');
        const auto c2 = line.find(',', c1 + 1);
        const auto c3 = line.find(',', c2 + 1);
        ASSERT_NE(c1, std::string::npos);
        ASSERT_NE(c2, std::string::npos);
        ASSERT_NE(c3, std::string::npos);

        const int64_t value = std::stoll(line.substr(0, c1));
        const double  pct   = std::stod(line.substr(c1 + 1, c2 - c1 - 1));

        // A percentile curve that goes backwards on either axis is not a
        // percentile curve, and would plot as nonsense.
        EXPECT_GE(value, prev_value);
        EXPECT_GE(pct, prev_pct);
        prev_value = value;
        prev_pct   = pct;
    }

    // The percentile iterator emits many rows with five ticks per half
    // distance, so a handful of rows would mean the iterator was not driven.
    EXPECT_GT(rows, 20);
    // The curve has to reach the top, otherwise the tail is cut off exactly
    // where it is interesting.
    EXPECT_GE(prev_pct, 99.9);
    EXPECT_NE(body.find("inf"), std::string::npos)
        << "the last bucket sits at percentile 100 and its inverted value must be written as inf";

    std::remove(path.c_str());
}

TEST(Histogram, PercentilesCsvOnAnEmptyHistogramStillWritesAHeader) {
    // An empty run is a result too, usually a broken one, and the file should
    // say so rather than not existing.
    tick::Histogram h;
    const std::string path = scratch_path("empty");
    h.write_percentiles_csv(path);
    const std::string body = slurp(path);
    EXPECT_NE(body.find("value_ns,percentile,total_count,inverted_percentile"),
              std::string::npos);
    std::remove(path.c_str());
}

TEST(Histogram, WritePercentilesCsvThrowsOnAnUnwritablePath) {
    tick::Histogram h;
    h.record(1);
    EXPECT_THROW(h.write_percentiles_csv("/this/directory/does/not/exist/out.csv"),
                 std::runtime_error);
}

} // namespace
