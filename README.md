# Tickerplant

Low-latency market data feed handler in modern C++20.

Tickerplant decodes **NASDAQ TotalView-ITCH 5.0** off **UDP multicast**, framed
in **MoldUDP64**, arbitrates two redundant lines, recovers from sequence gaps by
re-requesting the missing range, and rebuilds the full limit order book. It runs
against a real trading day of real NASDAQ data, 268 million messages, and the
book it produces is checked three independent ways.

A tickerplant is the industry term for the system that ingests exchange feeds
and distributes them. This is that, built from the specification up.

The order book underneath it is [NanoExchange](https://github.com/Sanjith-Shan/NanoExchange),
the matching engine this project is the counterpart to. NanoExchange is the
exchange and decides what trades. Tickerplant is the participant and rebuilds
what the exchange already decided.

## Results

Full day replay of the real NASDAQ ITCH 5.0 file for **2019-12-30**.

| | |
|---|---|
| Messages decoded | **268,744,780** |
| Symbols | 8,906 |
| Peak resting orders | **1,924,078** |
| Executed shares reconstructed | **971,016,019** |
| Invariant violations across the whole day | **0** |
| Book state at the close | empty, as the data says it should be |

The book built from the wire is **bit identical** to the book built from the
file, and stays identical through **20% independent packet loss on each of the
two multicast lines**, closing 1,326 gaps with 41,040 retransmitted messages.

| Injected loss per line | Gaps | Recovered | Lost | Final state | Book |
|---|---|---|---|---|---|
| none | 0 | 0 | 0 | synced | bit identical |
| 1% | 0 | 0 | 0 | synced | bit identical |
| 10% | 357 | 357 | 0 | synced | bit identical |
| 20% | 1,326 | 1,326 | 0 | synced | bit identical |
| **30%** | 2,282 | 151 | 86,591 | **stale** | **differs** |
| 10%, recovery disabled | 348 | 0 | 10,770 | **stale** | **differs** |

The last two rows are the ones worth reading. At 30% loss on both lines the
system breaks, and that row is published rather than trimmed. The
recovery-disabled row is the control that shows the retransmission path is doing
work rather than decorating the diagram.

Full numbers, every command that produced them, and the machine they ran on are
in **[results/RESULTS.md](results/RESULTS.md)**. The engineering writeup is in
**[docs/DESIGN.md](docs/DESIGN.md)**.

### What these numbers do not say

Every figure above is a **count, a fingerprint, or an outcome**, which is
deliberate. Those do not move with what else the machine is doing.

**No timing measured on this machine is fit to quote, and the repository says so
with evidence rather than with an apology.** Every tool records the load average
when it ran and prints `LOADED, timings here are not trustworthy` when the
machine was busy. The same binary on the same file gave 5.68 M msg/s at a load
average under 3 and 0.91 M msg/s at 15, a factor of six from nothing but a
browser and a container runtime.

The sharpest illustration is in the two decoder run. The copying decoder
measured *faster* than the zero-copy decoder, 1.667 against 1.354. It cannot be.
It does strictly more work over the same bytes and loses by 7.4 times on an
in-memory buffer. All that number measures is that one pass ran while the
machine was busier than the other.

On top of that, every wire run here is **loopback multicast** with **no core
pinning**, because macOS cannot pin a thread to a core at all. There is no
network interface, no driver, and no switch in any of it, and macOS loopback
multicast delivery was measured here at about 3.4 milliseconds, which swamps
everything the code does.

So these runs prove the decoder, the sequencer, the arbitration, the gap
machine, and the recovery path are correct, and the proof is a fingerprint of
every price level of every symbol rather than a spot check. They prove nothing
about latency.

### The Linux paths do run, and they were checked

A container gives a real Linux kernel, so `recvmmsg`, `epoll`, `io_uring` and
`sched_setaffinity` are all exercised. All 373 tests pass there, **pinning is
achieved**, and `io_uring` compiled for the first time. It also found two real
defects, one of them a control message walk that did not compile on Linux at
all because glibc and macOS disagree on a `const`.

Pinned to a core, with `recvmmsg`, 800k messages at 400k per second.

| | p50 | p99 | max |
|---|---|---|---|
| wire to book | 6.06 us | 20.3 us | 113 us |
| due to book | 1 ns | **7.4 us** | 1.03 ms |

The same measurement on macOS had a due to book p99 of **20.3 milliseconds**.
The receiver could not keep up there and keeps ahead of the schedule here.

That is a floor rather than a result, because a container on a laptop has no
`isolcpus`, no `nohz_full`, no real interface, and twelve cores shared with
everything else. [docs/LINUX_SETUP.md](docs/LINUX_SETUP.md) is the boot line and
the run script for a box where the tail means something.

## Quick start

Requires CMake 3.20 or newer, a C++20 compiler, and zlib. GoogleTest, Google
Benchmark, HdrHistogram_c, and NanoExchange are fetched at configure time.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# the test suite
ctest --test-dir build --output-on-failure

# a synthetic day, for when you do not want to download four gigabytes
./build/bin/make-synthetic-itch --out /tmp/day.itch --messages 1000000 --symbols 200 --gzip
./build/bin/tickerplant-replay --file /tmp/day.itch.gz --digest --top AAAA
```

For the real thing, one trading day is three to five gigabytes compressed.

```
./scripts/fetch_itch.sh                       # 2019-12-30 by default

# decode the day, rebuild every book, print the top of AAPL
./build/bin/tickerplant-replay --file data/12302019.NASDAQ_ITCH50.gz --top AAPL --digest

# run both decoders over it and assert they agree
./build/bin/tickerplant-replay --file data/12302019.NASDAQ_ITCH50.gz --decoder both --digest
```

Put it on the wire. Two terminals, or use the script below.

```
# terminal 1, the receiver
./build/bin/tickerplant-rx --lines 2 --strategy busypoll --reorder-window 65536

# terminal 2, the publisher, with 10% loss injected on each line
./build/bin/tickerplant-pub --file data/12302019.NASDAQ_ITCH50.gz \
    --limit 1000000 --pace rate --rate 150000 --drop-a 0.10 --drop-b 0.10
```

Every experiment in the results file, in one command.

```
./scripts/run_wire_experiments.sh 1000000 150000
```

## Architecture

```
NASDAQ ITCH 5.0 sample file, one real trading day, gzipped
        |
        v
+--------------------------------------------------------------+
| tickerplant-pub    replay as MoldUDP64 over UDP multicast     |
|   line A --------------+   configurable rate, loss, reorder,  |
|   line B --------------+   and delay skew between the lines   |
|   re-request server        answers gaps from a bounded ring   |
+------------------------+--------------------------------------+
        +----------------+
        v                v
+--------------------------------------------------------------+
| tickerplant-rx                                                |
|   two sockets, two groups                                     |
|   +- A/B arbitration      first copy of sequence N wins       |
|   +- reorder window       fixed size, indexed by a mask       |
|   +- gap state machine    synced -> gap -> recovering -> stale|
|   +- recovery client      re-requests the missing range       |
+------------------------+--------------------------------------+
                         v
+--------------------------------------------------------------+
| decoder       zero-copy ITCH 5.0, no allocation, jump table   |
+------------------------+--------------------------------------+
                         v
+--------------------------------------------------------------+
| book builder  applies events onto NanoExchange PriceLevel     |
|               and MemoryPool. Applies. Does not match.        |
+------------------------+--------------------------------------+
                         v
        top of book, depth, per symbol volume, HdrHistogram
```

### The decision the whole project turns on

**A feed handler does not match. It applies.**

NanoExchange's `MatchingEngine` decides which orders trade. A book built from
ITCH must not, because the exchange has already decided and the feed is telling
you what it decided. Running an `E` message through a matching engine would
execute against whatever is at the top of the local book, which is not
necessarily the order the exchange filled, and the two books would diverge
within seconds and never converge again.

So Tickerplant reuses `nano::Order`, `nano::PriceLevel`, and `nano::MemoryPool`,
which were always about holding a book, and deliberately does not use
`nano::MatchingEngine`. `BookBuilder` exposes handlers that apply events and
nothing that matches.

## How the book is proved right

Benchmarks are worthless if the book is wrong, so there are three independent
checks and none of them is the code checking itself.

**Two decoders, one answer.** A zero-copy decoder that reads fields out of the
receive buffer where they lie, and a copying decoder that stages every message
and normalises every field whether it is wanted or not. Run over the same 268
million messages they produce identical books and identical per-symbol volume.
This exists because "zero-copy" without the slow path next to it is a claim
rather than a measurement, and getting the comparison for free is the second
reason.

**External volume reconciliation.** Executed share volume per symbol, summed the
way NASDAQ counts it, compared against Nasdaq's own published **NASDAQ Matched
Volume** from the Nasdaq Trader market share workbooks. That is the same
quantity the replay measures, which is volume matched on the Nasdaq exchange
rather than consolidated tape volume across every venue. No free per-symbol
Nasdaq-only daily file exists for this date, so the comparison is one session
against the month containing it, and the evidence is the tightness of the ratio
rather than an exact tie-out. Across the 500 most active symbols the day to
month ratio holds a p10 to p90 band of **0.0206 to 0.0578**, against a month of
21 sessions. A book that dropped, double counted, or misattributed executions
would not hold a band that tight across hundreds of independent names.

**Replay determinism.** The same file produces a bit identical book and an
identical event count on every run. The comparison is a 64 bit fingerprint over
a canonical walk of every price level of every symbol, so "identical" means
identical rather than "the top of book looked the same".

## The experiments

Six, and the ones that failed are reported with the ones that did not.

**Zero-copy versus copying decode.** Both built, both correct, measured on real
messages. With a handler that ignores its input, lazy decode is **7.4 times
faster**. With a handler that rebuilds the book, the same comparison is **2.7%**,
because the book work is 98% of the cost. Both numbers are published, because
the first one on its own would be a marketing figure. The second one also moves
with how big the book is, and is 1.96x over a window where the book peaks at
188k live orders rather than 1.27M, so the honest form of the answer names the
book size.

**The order reference table.** A flat open-addressed table with backward shift
deletion against `std::unordered_map`, driven by the real order reference trace
from the file. The flat table with a splitmix64 finaliser runs at 37.6 M ops/s
and 0.357 probes per lookup, against 17.7 M for a `std::unordered_map` that has
had `reserve` called on it. **The prediction in the header was wrong.** It argued
that near-sequential ITCH references were the best case for an identity hash,
and on real data the identity hash takes 96.4 probes per lookup and loses to an
unreserved `std::unordered_map`. It is also a crossover rather than a constant,
because on a hundred thousand message window the identity hash wins. A smaller
fixture would have confirmed the wrong answer.

**Price level containers.** A red black tree against a sorted vector, both
holding NanoExchange price levels. In isolation on AMZN, which carries 4,926
live levels at the open, the tree wins by five times. Across the whole feed,
where the average symbol has about 61 levels, the vector wins by 4%. The two
benchmarks disagree and both are correct, so the answer is reported as "it
depends on the symbol" rather than as a winner. The direct addressed array that
won NanoExchange's own shootout cannot be used here at all, because real NASDAQ
prices span eight orders of magnitude of ticks and the window would be gigabytes
per side per symbol.

**A/B line arbitration.** Win rate per line against injected loss and delay
skew. Every sequence line B wins is one line A lost, so the win rate tracking
the loss rate is the measurement that shows arbitration is working.

**Gap recovery under loss.** The table at the top. The finding worth more than
the successful rows is that the reorder window has to be sized against the
recovery round trip. At 150,000 messages a second, a 4,096 message window is 27
milliseconds of feed and the recovery timeout is 50 milliseconds, so the window
overflowed before the retransmission could arrive and the same run that recovers
everything with a 65,536 message window lost 6,615 messages and went stale.

**Coordinated omission, handled and stated out loud.** The receiver measures
every message against **when the publisher's schedule said it should have been
sent**, not when it arrived. The two distributions from the same clean run are
worth putting side by side.

| | p50 | p99 | max |
|---|---|---|---|
| wire to book, from the kernel receive timestamp | 21.2 us | 58.2 us | 14.4 ms |
| due to book, against the publisher's schedule | 1.7 us | **20.3 ms** | 37.3 ms |

Same run, same messages. Measured from arrival the p99 is 58 microseconds and
the receiver looks healthy. Measured from when the message was owed it is 20
milliseconds, because a message that sits in the socket buffer is timestamped
when it comes out rather than when it was due. That is coordinated omission made
visible, and it is why the publisher writes a schedule manifest.

Those absolute microsecond figures are loopback on a loaded laptop and are not
quotable. **The shape is the point**, and the shape is what a different
measurement start point does to the same run.

## What is deliberately not here

- **Kernel bypass.** DPDK, Solarflare `ef_vi`, and Onload are the real answer at
  this tier and none of them can be done without the hardware. Claiming a kernel
  bypass number without the card would be a fabrication. What the project does
  instead is measure the kernel path honestly and say what bypass would change.
- **Every ITCH message type.** Thirteen are implemented, which is everything
  that touches the book plus the session and directory messages. The rest are
  counted by type and reported as unimplemented rather than silently dropped.
- **A cross-venue NBBO consolidator.** One feed, done properly.
- **FPGA anything.**

## Tick to trade

The second half closes the loop. `tickerplant-trade` runs the feed into a
quoting rule, through **pre-trade risk checks**, out through an **OUCH** order
gateway into NanoExchange as the venue, and measures end to end tick to trade
latency the same way the feed path is measured.

The risk engine is the part worth reading. Position limits that count working
exposure and not only fills, maximum order size and notional, a fat finger bound
that **fails closed** when there is no two sided market to price against, an
allocation-free message rate limiter, a stale market data check, and a one way
kill switch that needs an explicit call to reset. Each check has a test that it
fires and a test that it does not fire on a legal order.

The quoting rule quotes around the micro price with linear inventory skew and
hysteretic requoting. It makes no claim to be profitable and there is no test
that asserts it is, because it exists so that the tick to trade path has a
decision at the end of it.

## Concurrency

The feed path is single threaded on purpose. Every handoff between threads is a
queue, a cache line moving between cores, and a source of jitter, and none of
that buys anything when one thread already keeps up with the feed.

What is here for the cases where threading is genuinely needed is a bounded
multi producer multi consumer queue and a seqlock for publishing top of book to
many readers, both with every memory ordering justified in a comment next to it
and both run under ThreadSanitizer. This is arm64, which has a weaker memory
model than x86, so code that is accidentally correct because of total store
ordering fails here.

Three results from that, and two of them went the wrong way.

- MPMC costs **1.6 times** the SPSC ring at one producer and one consumer, which
  is why the SPSC ring stays the default.
- Past four producers and four consumers **the mutex wins**, because a lock
  batches under contention while a lock free ring bounces cache lines.
- Packing the head and tail into one cache line instead of separating them costs
  **2.3 times**. Measuring the thing you claim to have fixed is the difference
  between a comment and a result. On two bare counters, same line gives 150.8 M
  operations per second, split by 64 bytes gives 493.3 M, and split by 128 gives
  711.2 M, because this machine has a 128 byte cache line and 64 is a convention
  rather than a guarantee.

ThreadSanitizer reports races in the seqlock, on the value `memcpy` and nowhere
else, across twelve runs with zero failed assertions. That report is correct and
it is not suppressed. The header explains why the technique is a race by the
letter of the memory model, what the standard-clean alternative costs, and that
Linux's own `seqlock_t` does the same thing.

## The sibling project

[NanoExchange](https://github.com/Sanjith-Shan/NanoExchange) is the matching
engine this is built on, and it got the same measurement harness in the same
pass. Its latency table now says which core it ran on, whether pinning was
achieved rather than requested, and what the machine's load was.

That produced one confirmation and one surprise.

Its add-limit median is **125 ns unpinned on macOS under clang and 125 ns pinned
on Linux under gcc**. Across an operating system, a compiler and a scheduling
policy it did not move, which is what the original "the median is the honest
measure" caveat predicted.

And pinning to a **shared** core turned out to be a bet rather than a win. Under
eight competing threads it improved every best-case p99 by 15 to 25 percent and
made every worst case worse by 20 to 50 percent. When the core is quiet you keep
your cache. When something else lands on it you cannot migrate away, which
unpinned you could. **Pinning removes the scheduler's ability to help as well as
its ability to hurt**, which is the argument for `isolcpus` stated as a
measurement rather than as folklore.

## Project layout

```
include/tick/     the whole library, header only
  endian.hpp        big-endian and six byte loads out of unaligned buffers
  itch.hpp          ITCH 5.0 message types, lengths, and field offsets
  itch_decoder.hpp  the two decoders and the handler concept
  itch_file.hpp     streaming reader for the gzipped sample files
  moldudp64.hpp     the framing protocol, encode and decode
  udp_socket.hpp    multicast sockets and kernel receive timestamping
  receive_strategy.hpp  the five receive paths behind one concept
  sequencer.hpp     A/B arbitration, reorder window, gap state machine
  recovery.hpp      retransmission client and the publisher's session store
  book_side.hpp     one side of one book over nano::PriceLevel
  flat_order_map.hpp  the order reference table
  book_builder.hpp  the resting order state machine
  clock.hpp         monotonic time, the cycle counter, and calibration
  hdr.hpp           HdrHistogram, and what coordinated omission actually is
  affinity.hpp      core pinning, and an honest account of what macOS cannot do
  alloc_counter.hpp proving there is no allocation on the hot path
  box_info.hpp      the machine label that goes on every results table
  mpmc_queue.hpp    bounded multi producer multi consumer queue
  seqlock.hpp       publishing top of book to many readers without blocking
  risk.hpp          pre-trade risk
  ouch.hpp          the order entry protocol
  strategy.hpp      the quoting rule
tools/            the four binaries and the synthetic day generator
test/             the suite, including the in-process wire integration test
bench/            the shootouts
scripts/          fetching data, running experiments, labelling the machine
docs/             the design writeup and the Linux benchmark setup
results/          every measured number with the command that produced it
```

## Tests

The suite covers the decoders against hand built byte vectors with known values
for every implemented message type, the endian helpers against known vectors
including deliberately unaligned buffers, the MoldUDP64 framing against a hand
written packet, the sequencer under a randomised soak with independent loss on
each line, every transition of the resting order state machine including the
illegal ones, and the risk engine one test per reject reason.

The test that matters most is the wire integration test, which runs the whole
pipeline in one process with no sockets. It generates a deterministic synthetic
day, packs it into MoldUDP64, loses and reorders datagrams on two lines with a
seeded generator, answers retransmission requests in line, and asserts the
rebuilt book is identical to the book built straight from the same messages. It
reproduces exactly, which the loopback runs do not.

## License

MIT. See [LICENSE](LICENSE).
