# Measured results

Every number in this file was produced by a command in this repository, and the
command is written next to it. Nothing here is estimated, extrapolated, or
carried over from another machine.

## The machine

All numbers below, unless a row says otherwise, come from one box.

| | |
|---|---|
| CPU | Apple M3 Pro, 12 cores, 6 performance and 6 efficiency |
| OS | macOS 26.5.1 |
| Compiler | Apple clang 17.0.0, `-O3 -mcpu=native` |
| Pinning | **none. macOS cannot pin a thread to a core** |
| NIC | none in the path. Every wire run here is loopback multicast |

`thread_policy_set` with `THREAD_AFFINITY_POLICY` returns `KERN_NOT_SUPPORTED`
on this machine, so there is not even the weak cache-sharing hint older Intel
Macs offer, and `mlockall` fails, so pages can be evicted mid-run. The scheduler
can also move a thread between a performance core and an efficiency core, which
changes the clock rate underneath a measurement, and user space cannot detect it
or prevent it.

## Read this before quoting any timing from this file

**No throughput or latency figure measured on this machine is fit to go on a
resume, and this section is the evidence for saying so rather than an apology.**

Every tool here records the load average at the moment it ran, and prints
`LOADED, timings here are not trustworthy` when the machine was busy. That was
added after a sibling project shipped benchmark numbers taken at a load average
of 28 to 65 on twelve threads, and it earned its place immediately.

The same binary, on the same file, on the same day.

| Run | Load average | Throughput |
|---|---|---|
| full day replay | 2.7 at start, 20.5 by the end | 1.017 M msg/s |
| full day replay, repeat | 14.9 | 0.908 M msg/s |
| five million messages | under 3 | **5.684 M msg/s** |

A factor of six, from nothing but what else the laptop was doing. Docker, a
virtual machine, and a browser were running, because it is a working laptop and
not a benchmark box.

**The clearest single proof that these timings are noise** is in the two decoder
run. The copying decoder measured 1.667 M msg/s against the zero-copy decoder's
1.354, which is the copying path apparently winning. It cannot win. It does
strictly more work over the same bytes and the microbenchmark on an in-memory
buffer has it losing by 7.4 times. The only thing that number measures is that
the first pass ran while the machine was busier than the second.

### So what in this file is worth quoting

**Counts, digests, and outcomes are trustworthy.** They do not move with load.
Message counts, book fingerprints, the zero invariant violations across 268
million messages, allocation counts, average probe lengths, gap and recovery
counts, A/B win rates, the volume reconciliation ratios, and every bit-identical
comparison are the same on a loaded machine and an idle one. Those are the
project's real claims and they happen also to be the harder ones to produce.

**Timings are not, until they come from the Linux box.**
`docs/LINUX_SETUP.md` is the recipe, and every timing table below carries the
load average it was taken at so a reader can discount it themselves.

## The data

`data/12302019.NASDAQ_ITCH50.gz`, the real NASDAQ TotalView-ITCH 5.0 sample file
for **2019-12-30**, 3.28 GiB compressed, fetched with `./scripts/fetch_itch.sh`.
One full trading day.

---

## 1. Full day replay from the file

```
./build/bin/tickerplant-replay --file data/12302019.NASDAQ_ITCH50.gz --digest
```

| | |
|---|---|
| Messages | **268,744,780** |
| Bytes decoded | 7.68 GiB |
| Wall time | 85.1 s to 264 s depending on machine load |
| Throughput | 0.9 to 3.2 M msg/s end to end, gzip included. **See the load warning above. Do not quote this number** |
| Peak resting orders | **1,924,078** |
| Symbols seen | 8,906 |
| Executed shares | **971,016,019** |
| Book digest | `14650fb0739d0383` |

Message mix over the day.

| Type | Count |
|---|---|
| `A`/`F` add order | 118,631,456 (1,485,888 with MPID) |
| `D` delete | 114,360,997 |
| `U` replace | 21,639,067 |
| `E`/`C` execute | 5,822,741 (99,917 with price) |
| `X` cancel | 2,787,676 |
| `P` trade, hidden | 1,218,602 |
| `Q` cross | 17,836 |
| `R` stock directory | 8,906 |
| `H` trading action | 8,966 |
| other, not implemented | 4,248,527 |

**Every invariant counter is zero over all 268 million messages.** No duplicate
order references, no orphaned executions, cancels, deletes, or replaces, no
overfills, and no pool exhaustion. The book also unwinds to exactly zero resting
orders at the end of the session, which is a property of the data rather than a
thing the code does, and is therefore worth something as a check.

The peak resting order count is why the order pool is configured at four
million. That is a factor of two of measured headroom rather than a guess.

---

## 2. Correctness oracle one, two decoders

```
./build/bin/tickerplant-replay --file data/12302019.NASDAQ_ITCH50.gz --decoder both --digest
```

Two independent decode paths over the same 268,744,780 messages.

| | Zero-copy | Copying |
|---|---|---|
| Book digest | `14650fb0739d0383` | `14650fb0739d0383` |
| Volume digest | `fb1d58e36d2b04f1` | `fb1d58e36d2b04f1` |

**Both digests match, over all 268,744,780 messages.** That is the claim, and it
is a comparison of fingerprints over every price level of every symbol rather
than a spot check.

The throughput side of this run is the one discussed in the load warning above.
Two separate runs gave the zero-copy path 3.458 against copying's 3.172, and
then 1.354 against 1.667, which is the copying path apparently winning. Neither
pair means anything. The decode-only comparison on an in-memory buffer is in the
benchmark section and is the honest number for the decoder itself.

## 3. Correctness oracle two, replay determinism

The same command twice, on the same file.

| Run | Book digest | Volume digest |
|---|---|---|
| 1 | `14650fb0739d0383` | `fb1d58e36d2b04f1` |
| 2 | `14650fb0739d0383` | `fb1d58e36d2b04f1` |

## 4. Correctness oracle three, external volume reconciliation

Executed share volume per symbol, summed from `E`, printable `C`, `P` and `Q`,
compared against **Nasdaq's own published `NASDAQ Matched Volume`** from the
Nasdaq Trader monthly market share workbooks for December 2019.

```
python3 scripts/fetch_reference_volume.py --month 201912
python3 scripts/reconcile_volume.py --replay results/volume.csv \
        --reference data/reference/nasdaq_matched_volume_201912.csv
```

This is a genuinely external oracle and it measures the same quantity the replay
measures, which is volume matched on the Nasdaq exchange rather than
consolidated tape volume across every venue.

**The limitation, stated first.** No free per-symbol Nasdaq-only *daily* file
exists for 2019-12-30. The published figure is monthly, so this is one session
compared against the month containing it, and the evidence is the tightness of
the ratio across many independent symbols rather than an exact tie-out.

| | |
|---|---|
| Symbols in the replay | 8,906, carrying 971,016,019 shares |
| Matched to the reference by ticker | 6,103 |
| Only in the replay | 2,803, carrying 149,428,878 shares, **15.4% of the session** |
| Aggregate ratio over 6,078 comparable symbols | 0.041189, implying 24.3 sessions |
| **Top 500 symbols by reference volume** | **p10 0.0206, p50 0.0320, p90 0.0578** |
| Aggregate over the top 500 | 0.0423, implying 23.6 sessions |

December 2019 had 21 trading sessions. A ratio band that tight across 500
unrelated symbols is difficult to produce by accident, because a book that
dropped, double counted, or misattributed executions would not hold a consistent
ratio across hundreds of independent names.

The 15.4% that is only in the replay is almost entirely Tape B issues, which
Nasdaq does not publish in these workbooks, and includes SPY, IWM, GDX and EEM.
**That 15.4% is verified by nothing** and is reported here rather than quietly
excluded.

---

## 5. Wire replay, and the book built from the wire

Publisher and receiver as separate processes over loopback multicast, two lines
with arbitration, MoldUDP64 framing, 30 messages per packet.

```
./scripts/run_wire_experiments.sh 1000000 150000
```

The reference for every row is the book built from the same 1,000,000 messages
straight from the file, digest `769e5f73257c6484`.

| Injected loss per line | Gaps | Recovered | Messages lost | From recovery | Final state | Book |
|---|---|---|---|---|---|---|
| none | 0 | 0 | 0 | 0 | synced | **bit identical** |
| 1% each line | 0 | 0 | 0 | 0 | synced | **bit identical** |
| 10% each line | 357 | 357 | 0 | 10,770 | synced | **bit identical** |
| 20% each line | 1,326 | 1,326 | 0 | 41,040 | synced | **bit identical** |
| 30% each line | 2,282 | 151 | 86,591 | 5,059 | **stale** | differs |
| 10% each line, **recovery off** | 348 | 0 | 10,770 | 0 | **stale** | differs |

Three things in that table are worth more than the successful rows.

**At 1% loss per line, arbitration alone closed every gap.** Loss on the two
lines is independent, so the chance a sequence is lost on both is the product,
and at 1% that is one in ten thousand. The gap counter is zero because there was
nothing for the gap machine to do. That is what a redundant line pair buys and
it is why exchanges publish two.

**The recovery-off row is the control.** Same loss, same seed, and without the
retransmission path 10,770 messages are simply gone and the book is wrong. The
receiver exits non-zero and reports the stale state rather than handing back a
book that looks fine.

**At 30% the system breaks, and that row is published.** Gaps arrive faster than
a 50 ms recovery round trip can close them, the reorder window overflows, and
the receiver goes stale. A feed handler that claimed to survive 30% loss on both
lines would be lying.

### The reorder window has to be sized, and here is the measurement

The 10% row above was first run with a 4,096 message reorder window and it
failed, losing 6,615 messages and ending stale. The same run with a 65,536
message window recovered every gap.

| Reorder window | Gaps | Recovered | Lost | State |
|---|---|---|---|---|
| 4,096 | 355 | 137 | 6,615 | stale |
| 65,536 | 357 | 357 | 0 | synced |

The window has to hold more messages than arrive during one recovery round trip.
At 150,000 messages a second, 4,096 messages is 27 milliseconds of feed and the
recovery timeout is 50 milliseconds, so the window overflowed before the
retransmission could arrive. This is the kind of thing that is obvious after it
is measured and invisible before.

### A/B arbitration win rate

| Publisher skew on line B | First copy on A | First copy on B |
|---|---|---|
| none | 99.93% | 0.07% |
| none, 1% loss each line | 98.94% | 1.06% |
| none, 10% loss each line | 90.77% | 9.23% |
| 50 us | 99.91% | 0.09% |
| 500 us | 99.61% | 0.39% |

With no deliberate skew, line A wins almost everything, because the publisher
sends A first on the same thread. The win rate moving with the loss rate is the
measurement that shows arbitration is doing work, since every sequence B wins is
one A lost.

The skew rows are less interesting than they should be, and it is worth saying
why rather than presenting them as a result. Line A already wins 99.93% with no
skew at all, so delaying B further cannot move a number that is already at the
ceiling. To make skew the variable that matters, B would have to be the line
that usually wins, which means either sending B first or putting the two lines
on separate threads. **The experiment as built measures the send order of one
thread more than it measures skew**, and that is a flaw in the harness rather
than a finding about arbitration.

### One line failing completely

The case the redundancy exists for.

| | Gaps | Recovered | Lost | Book |
|---|---|---|---|---|
| 10% loss on line A only | 0 | 0 | 0 | **bit identical** |
| 10% loss on line B only | 3 | 3 | 0 | **bit identical** |

With loss confined to one line, the other carries the feed and the gap machine
has almost nothing to do. That is the whole argument for a redundant line pair
in one table, and it is also why the 1% row in the loss table shows zero gaps.

### Wire to book latency, and why the second row matters more

One million messages, 150,000 per second, no injected loss.

| Measurement | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| wire to book | 21.2 us | 31.2 us | 58.2 us | 649 us | 14.4 ms |
| due to book | 1.7 us | 64.6 us | **20.3 ms** | 33.0 ms | 37.3 ms |

Read the p99 column. Measured from the kernel receive timestamp, the p99 is 58
microseconds and the receiver looks fine. Measured from the time the publisher's
schedule says the message was due to be sent, the p99 is 20 milliseconds, three
hundred times worse. The receiver was falling behind and the first row could not
see it, because a message that sits in the socket buffer for twenty milliseconds
is timestamped when it arrives and not when it was owed.

That gap between the two rows is coordinated omission made visible. It is also
mostly macOS loopback multicast rather than this code, which is exactly why the
number that matters has to be taken on the Linux box.

---

## 6. Price level container, on real data

Full day replay, identical in every other respect.

| Container | Book digest |
|---|---|
| `std::map` | `14650fb0739d0383` |
| sorted vector | `14650fb0739d0383` |

**The digests match, which is the point of having two.** Swapping the container
did not change one level of one book across the whole day.

Which is faster on the full day is not answerable from this machine. Three
separate full day runs put the tree at 3.16, 1.02 and 0.91 M msg/s and the
vector at 2.73 and 1.81, with load averages between 3 and 20, and the ranges
overlap completely. The in-memory benchmark, which is the place to answer this,
finds the two disagree by symbol and is in the benchmark section.

The third NanoExchange container, the direct addressed array, is not in this
table because it cannot be used here at all. It costs memory proportional to the
whole price window, and real NASDAQ prices run from a few hundred ticks to over
a hundred million, so a window wide enough for the real data is gigabytes per
side per symbol. A container that won on synthetic data is unusable on real
data, and finding that out is worth more than the benchmark that it won.

---

## 6b. Microbenchmarks, on real messages

```
./build/bin/benchmarks --benchmark_out=results/bench.json --benchmark_out_format=json
python3 scripts/plot_bench.py --input results/bench.json --outdir results
```

Four million real messages taken from the **market open**, not the head of the
file. The first four million messages of the day are pre-open quoting in a
handful of ADRs, 14,348 executions in four million messages, on a live set small
enough to sit in cache. Benchmarking that would have been a real file and a fake
workload. The loader skips to the `S`/`Q` start of market hours event at
09:30:00.000075566, file message 6,696,721, and takes four million from there.
Peak live orders in the window, 1,274,357. Live price levels, 546,551.

Medians of seven repetitions. Within-run coefficient of variation was under
0.11% on almost every case.

### Decode, and the number that matters

| | `NullHandler` | `BookBuilder<MapSide>` |
|---|---|---|
| `ZeroCopyDecoder` | **502.6 M msg/s** | **4.52 M msg/s** |
| `CopyingDecoder` | 68.1 M msg/s | 4.40 M msg/s |
| ratio | **7.4x** | **1.027x** |

Both rows are true and only one of them is a fair description of the project.

With a handler that throws everything away, lazy field extraction is 7.4 times
faster than copying and normalising, which is the decoder measured on its own.
With a handler that rebuilds the book, the same comparison is 2.7%, because the
book work dominates so completely that the decoder is nearly invisible. 886
milliseconds against 910 for four million messages.

The claim the measurement supports is that **zero-copy decode costs nothing and
removes 58.7 ms of pure overhead per four million messages**. It does not
support "a seven times faster feed handler", and the second column is published
next to the first so that nobody has to find it themselves.

**And the second column moves with how big the book is**, which is worth knowing
before quoting it either. The same comparison over a 300,000 message window,
where the book peaks at 188,044 live orders rather than 1,274,357, gives
zero-copy 8.45 M msg/s against copying's 4.31, a ratio of **1.96x** rather than
1.03x. The book work is what buries the decoder, so a smaller book buries it
less. Neither ratio is wrong. A single number for "how much does zero-copy
decode buy" does not exist, and the honest form of the answer names the book
size it was measured at.

Per message type, `NullHandler`, messages per second.

| | `A` | `D` | `E` | `X` | `U` |
|---|---|---|---|---|---|
| zero-copy | 1,115 M | 1,299 M | 1,373 M | 1,366 M | 1,323 M |
| copying | 91.6 M | 142.6 M | 108.0 M | 118.0 M | 124.8 M |
| ratio | 12.2x | 9.1x | 12.7x | 11.6x | 10.6x |

The `E` and `X` buffers hold 66k and 34k messages, small enough to be cache
resident, so those two columns are optimistic next to `A` and `D`, which hold a
million each. The message count is reported as a counter on every row so this is
visible rather than buried.

Endian helpers at real unaligned offsets, nanoseconds per load: 16-bit 1.445,
32-bit 1.456, 64-bit 1.457, and the six byte ITCH timestamp 1.535. The width
does not matter, because the byteswap disappears next to the load. The full
eleven byte header with no dispatch is 1.848 ns per message.

### The order reference table, and a prediction the data refuted

The real operation trace from the same window. 4,771,441 events, 2,786,650
inserts, 540,028 lookups, 1,444,763 erases, peak live 1,341,902, final load
factor 0.320, and zero growth events in every case, so nothing is paying for a
rehash.

| Table | Mixed trace | Erase heavy | Probes per lookup |
|---|---|---|---|
| `FlatOrderMap`, splitmix64 | **37.55 M ops/s** | **40.79 M ops/s** | **0.357** |
| `FlatOrderMap`, identity hash | 8.59 M ops/s | 8.70 M ops/s | **96.357** |
| `std::unordered_map`, reserved | 17.68 M ops/s | 14.61 M ops/s | not instrumented |
| `std::unordered_map`, plain | 10.92 M ops/s | 9.42 M ops/s | not instrumented |

The flat table beats a `std::unordered_map` that has had `reserve` called on it
by 2.1 times on the mixed trace and 2.8 times on the erase heavy one. Comparing
against an unreserved map would have been an unfair fight, so both are shown.
Backward shift deletion holds up, and the flat table is actually faster on the
erase heavy slice than on the mixed one.

**The expected answer lost.** The header originally argued that near-sequential
ITCH order references are the best possible case for an identity hash with
linear probing. On the real trace the identity hash averages **96.4 extra probes
per lookup against 0.357**, is 4.4 times slower, and loses to an unreserved
`std::unordered_map`.

The reason is that the references are dense but gappy. About seventy percent are
larger than the previous one and the key span grows by 1.32 per insert, so live
keys form long unbroken runs with holes between them, which is exactly the input
linear probing handles worst. Sequential is not contiguous.

**It is a crossover rather than a constant, which is the part worth keeping.**
On a hundred thousand message window, load factor 0.018, the identity hash wins
by nearly two to one with 0.0006 probes per lookup. A smaller or synthetic
fixture would have confirmed the wrong answer and produced a clean chart to
defend it with. The header now says all of this.

### Price level containers, where the two benchmarks disagree

Isolated, on AMZN's real add and delete stream, sweeping the prewarmed depth.

| Prewarmed levels | `MapSide` | `VectorSide` |
|---|---|---|
| 0 | **23.03 M/s** | 4.55 M/s |
| 8 | 22.91 M/s | 4.55 M/s |
| 64 | 22.59 M/s | 4.43 M/s |
| 512 | 22.01 M/s | 3.74 M/s |

Full book, same four million messages, same decoder, same order table.

| Side container | Throughput | Book digest |
|---|---|---|
| `MapSide` | 5.49 M msg/s | `2d7c4ae985973db4` |
| `VectorSide` | **5.71 M msg/s** | `2d7c4ae985973db4` |

**These two tables disagree and both are right.** In isolation the tree beats
the sorted vector by five times, because AMZN carries 4,926 live price levels at
the open and the vector is moving thousands of pairs per insert. Across the
whole book the vector is 4% faster, because 546,551 live levels spread over
8,906 symbols is about 61 levels per symbol, most books are shallow, and
locality wins there.

So the answer is "it depends on the symbol", and the only useful way to report
it is to say which one was measured and on what. The depth sweep also turned out
to be nearly irrelevant, moving `MapSide` by 4%, because the real book is
already an order of magnitude deeper than the knob. That is a finding about real
feed data rather than a failed experiment, so it is published.

The identical digests across the two containers are a free correctness check.
Swapping the container did not change one level of one book.

**One caveat on the whole section.** 337,878 of the four million messages, 8.4%,
are orphans, meaning executions and deletes against orders added before the
window opened. That is an artefact of starting mid-session and it is reported as
a counter on every book row rather than hidden.

### The strongest argument for getting onto Linux

The identical configuration, zero-copy decode into `BookBuilder<MapSide>` over
the same four million messages, measured **4.52 M msg/s in one binary and 5.49
M msg/s in another**, a 21% gap, while the within-run coefficient of variation
of each was under 0.1%.

Within-run repeatability says nothing about between-run repeatability on an
unpinned machine. This is the pinning caveat with a number attached to it, and
it is the reason none of these figures should be quoted as the project's
headline until they have been taken again on an isolated core.

---

## 6c. The allocation the counting allocator found

This section is here because the defect it describes was invisible until
something counted, and because the obvious fix made things worse.

The order objects come from NanoExchange's memory pool and allocate nothing,
which made it easy to believe the whole book path was allocation free. The
counting allocator, run over the tick to trade binary, said otherwise.

| | Heap allocations in the feed path |
|---|---|
| 300,000 message replay, before | **14,209** |
| the same replay, after | **17** |

Every new price level was a `std::map` node straight out of the general purpose
heap. On the real day, with 546,551 live levels churning continuously, that is
millions of calls into the allocator on the hot path, which is precisely the
tail latency source this project exists to measure.

### Three allocators, measured

`-DTICK_LEVEL_ALLOCATOR=0|1|2|3` selects where price level nodes come from, so
this comparison is reproducible rather than asserted. Option 3 is a plain
`std::map` with no polymorphic allocator at all, which is the baseline the
others have to beat.

Five million real messages, three runs each, throughput in M msg/s, taken at a
load average under 3.

| | run 1 | run 2 | run 3 | best | Hot path allocations |
|---|---|---|---|---|---|
| 3, plain `std::map` on the heap | 3.709 | 5.684 | 5.291 | 5.684 | 14,209 per 300k messages |
| 0, the heap through a memory resource | 3.112 | 5.548 | 5.186 | 5.548 | 14,209 |
| 1, `std::pmr::unsynchronized_pool_resource` | 4.004 | 4.099 | 6.139 | 6.139 | 17 |
| 2, `tick::PoolResource` | 4.894 | 5.491 | 5.732 | 5.732 | 17 |

All four produce book digest `221d5596e62ac8d3`, so the allocator does not
change the book.

**What this table supports and what it does not.** The allocation column is a
count and is solid. The pooled options take 14,209 heap allocations down to 17,
and the 17 are the chunks taken during warmup.

The throughput columns do not separate the four. The spread within one
configuration, 3.112 to 5.548 for the same binary, is larger than any difference
between them. On this machine the defensible claim is narrow and it is this:
**pooling the level memory removes the allocations and does not cost
throughput.** A specific speedup is not available from this data and the pinned
re-run is where one would come from.

The pooled resource stayed as the default anyway, because an allocation count of
17 against 14,209 is a real property of the hot path regardless of what the
timer says, and because the thing being removed is a call into the general
purpose allocator, which is a tail latency source rather than a throughput one.
A throughput benchmark is the wrong instrument for it and the p99 on a pinned
core is the right one.

---

## 6d. Concurrency

The feed path is single threaded on purpose, so these structures are for the
cases where threading is genuinely needed and for closing a gap the portfolio
had, which was one lock free queue and nothing else.

Unpinned, load average 12 to 20 during the run, 16 byte payload, 1024 slot
rings, 200,000 items per iteration, median of 7 repetitions. The ratios are
worth more than the absolute numbers here for the usual reason.

### Throughput

| | Items per second |
|---|---|
| `nano::SPSCQueue`, one producer one consumer | **57.4 M** |
| `MpmcQueue`, one producer one consumer | 36.8 M |
| mutex around a `std::deque`, one and one | 14.5 M |
| `MpmcQueue`, 2 and 2 | 15.7 M |
| `MpmcQueue`, 4 and 4 | 8.5 M |
| `MpmcQueue`, 6 and 6 | 2.6 M |
| mutex, 4 and 4 | 10.0 M |
| mutex, 6 and 6 | 11.0 M |

**Two results here are not the ones you would hope for and both are published.**
MPMC costs about 1.6 times the SPSC ring at one producer and one consumer, which
is the number that keeps the SPSC ring the default for decode to book. And past
four of each, **the mutex wins**, because a lock batches under heavy contention.
The holder runs a burst while the hot cache lines stay on one core, where the
lock free ring has every thread bouncing the same lines between cores. That is
the same effect NanoExchange's design writeup already documented on a different
structure.

### False sharing, measured rather than asserted

The packed variant is a byte for byte copy of the queue with identical memory
orderings and identical per-slot padding. The only difference is that the head
and tail positions share one cache line.

| | Separated | Packed into one line | Cost |
|---|---|---|---|
| one producer, one consumer | 36.8 M/s | 15.8 M/s | **2.3x** |
| 2 and 2 | 15.7 M/s | 8.4 M/s | 1.9x |
| 4 and 4 | 8.5 M/s | 4.0 M/s | 2.1x |

And isolated to two counters with no queue in the way.

| | Operations per second |
|---|---|
| two counters in the same cache line | 150.8 M |
| split by 64 bytes | 493.3 M |
| split by 128 bytes | **711.2 M** |

**This machine reports `hw.cachelinesize: 128`.** Splitting at 64 recovers most
of the loss and splitting at 128 recovers more, which is a concrete
demonstration that 64 is a convention rather than a guarantee.

### Round trip latency

| | Per round trip |
|---|---|
| `MpmcQueue` | 165 ns |
| `nano::SPSCQueue` | 201 ns |
| mutex around a `std::deque` | 17.7 us |

MPMC beating SPSC on round trip reproduced across three runs. The likely reason
is the per-slot padding, since each slot here owns its cache line so a hop moves
one line, where the unpadded SPSC ring makes the consumer poll a shared position
on one line and then read a payload on another. **That is stated as a hypothesis
from the layout and not as fact**, because macOS exposes no per-core cache event
counters to confirm it.

### SeqLock against a mutex, for publishing top of book

| Writer, publishing a 40 byte top of book | SeqLock | Mutex |
|---|---|---|
| no readers | 2.60 ns | 10.3 ns |
| 1 reader | 29.0 ns | 398 ns |
| 4 readers | 44.1 ns | 148 ns |
| 8 readers | 79.6 ns | 305 ns |

| Reader | SeqLock | Retries per read | Mutex |
|---|---|---|---|
| writer publishing flat out | 261 ns | 160.7 | 48.0 ns |
| writer paced 1 in 16 | 180 ns | 133.4 | |
| writer paced 1 in 64 | 9.43 ns | 2.93 | |
| writer paced 1 in 256 | **5.09 ns** | 0.148 | 48.0 ns |

The writer side is the headline and it holds. A seqlock writer barely notices
readers and a mutex writer queues behind every one of them.

**The reader side has a case where it loses and it is in the table.** Against a
writer publishing in a tight loop with no work between updates, the reader is
starved, retries 160 times per read, and loses to the mutex. That is not what a
book thread does. Give the writer even a few hundred cycles between publishes,
still a far higher rate than any real top of book, and the reader drops to 5 ns
with 0.15 retries against the mutex's 48 ns. Reporting only the second half of
that would have been the dishonest version.

### ThreadSanitizer

`MpmcQueue`, **five runs, zero warnings**.

`SeqLock`, twelve runs, 10 to 14 reports each and **zero failed assertions in
every run**, meaning no reader ever accepted a mixed snapshot. Every report is
on exactly two lines, the writer's and the reader's `memcpy` of the value bytes,
and nothing on the sequence counter or the fences.

**That is the expected and correct report and it is not suppressed.** A seqlock
writes the value bytes while readers read them with no atomic on those bytes.
The fences order the accesses but do not make them non-racy, and the standard
defines the race on the accesses themselves, so ThreadSanitizer is right by the
letter of the model. The header says so, names the standard-clean alternative,
which is per-field relaxed atomics at the cost of the memcpy, and says what
real systems do, which is that Linux's own `seqlock_t` uses ordinary memory
under read and write barriers. A one line suppression is verified to work and is
deliberately not committed.

**An environment note worth recording.** Apple clang 17's own AddressSanitizer
and ThreadSanitizer runtimes are broken on macOS 26.5.1. Both die inside their
own initialisation in the dyld shared cache path, reproduced against a hello
world with a planted race. The macOS TSan runs above were done by running the
Apple-clang-instrumented binaries against the LLVM 23 runtimes through
`DYLD_LIBRARY_PATH`. This is why CI runs the sanitizers on Linux.

### And the two ThreadSanitizers disagree

Run on Linux under gcc 13.3, the same seqlock tests produce **zero** reports.
All five pass, no warnings, where clang's runtime reported ten to fourteen races
every time.

**That does not mean the code is race free under gcc.** It means the two
implementations instrument differently, most likely around `memcpy` and the
standalone fences, and the race is a property of the memory model rather than of
the tool. Clang is right. gcc's silence is the absence of a report, which is not
the same as the absence of a race, and a project that took the quiet answer as
the true one would be choosing its evidence.

The useful conclusion is the general one. **A clean sanitizer run is weak
evidence and a dirty one is strong evidence**, so the dirty result is the one
worth keeping and explaining.

Worth knowing for CI, too. The seqlock tests take **424 seconds** under gcc's
TSan against under a second normally, because every one of those memory accesses
is instrumented.

## 7. Timing primitives

Measured by `test_clock` and the calibration driver on this machine.

| | |
|---|---|
| `now_ns()`, `CLOCK_UPTIME_RAW` | 12.36 ns per call |
| `rdtsc()`, `cntvct_el0` | 0.36 ns per call |
| `rdtsc_serialized()`, with `isb` | 22.81 ns per call |
| TSC calibration error | 0.067 ppm over a 200 ms window, 16 samples |
| `cntfrq_el0` on this M3 Pro | 1 GHz, so one tick is one nanosecond |

The clock read costs about thirty four times the counter read, which is the
whole argument for timestamping the hot path with the cycle counter. The fenced
read costs sixty three times, which is why it brackets a batch and never a
message.

Worth recording because it is commonly got wrong. `cntfrq_el0` on this machine
is 1 GHz, not the 24 MHz often quoted for Apple Silicon. The 24 MHz figure
belongs to the mach timebase, which reports 41.7 ns per tick here. They are two
different counters.

---

## 8. Coordinated omission, demonstrated on synthetic data

From the histogram self-test. One million samples, a roughly 80 ns body, and a
50 microsecond stall once every thousand samples, at an intended interval of
100 ns.

| | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| raw | 79 ns | 110 ns | **144 ns** | 261 ns | 50,015 ns |
| corrected | 94 ns | 35,103 ns | **48,607 ns** | 49,919 ns | 50,015 ns |

Same data. The raw p99 says 144 nanoseconds and the system looks healthy. The
corrected p99 says 48.6 microseconds. The difference is the samples a fixed rate
sender would have produced during the stall, which a load generator that blocks
never takes.

The receiver does not apply this correction, and the reason is worth stating.
It measures every message against the publisher's schedule rather than against
observed send times, so its distribution is already complete and correcting it
again would count the same delay many times. The correction stays in the library
because a harness that does block needs it.

---

## 9. Linux, in a container, on the same laptop

Added 2026-09-20. Docker Desktop runs a real Linux kernel, so the Linux-only
paths can be built and exercised without renting anything. This section is what
that showed, and it is deliberately separated from everything above because the
container shares the same laptop.

Ubuntu 24.04, Linux 6.12.76-linuxkit aarch64, gcc 13.3.0, 12 cpus, no
`isolcpus`.

### What it proved

| | |
|---|---|
| Full CMake build | clean, no warnings |
| Tests | **373 of 373 pass**, three consecutive `ctest -j4` runs |
| `recvmmsg` | **available and used** |
| `epoll` plus `recvmmsg` | available |
| `io_uring` | **compiles for the first time**, with liburing |
| `kqueue` | correctly reports unavailable |
| Core pinning | **achieved.** `sched_setaffinity` succeeded and `sched_getcpu` confirmed the thread on core 2 |

The `io_uring` row closes an open item. That strategy had been written and
guarded and **never once compiled**, because there is no liburing on macOS. It
now does.

### Two real defects the container found

**A Linux-only compile error.** `CMSG_NXTHDR` is declared by glibc as taking a
non-const `msghdr*` and macOS accepts a pointer to const, so the control message
walk in `udp_socket.hpp` compiled cleanly on the development machine and did not
compile at all on the platform the project is actually for. That is the exact
failure mode a Linux box was supposed to catch late, caught early instead.

**A flaky test, and the cause rather than the symptom.** The first `ctest` run
failed one case, the one asserting a kernel receive timestamp lands between a
send and the return from `recv`. It passed on every run after. The cause was the
realtime to monotonic offset, captured once per process by sandwiching a
realtime read between two monotonic reads. If the thread is descheduled inside
that sandwich, which on a machine that has just finished an eight way parallel
build is likely, the midpoint is wrong by however long it was away. It now takes
sixteen samples and keeps the one whose sandwich was tightest, which is the
argument NTP uses for preferring the lowest delay exchange. Widening the test's
tolerance would have hidden it.

### A first pinned number

800,000 synthetic messages at 400,000 per second, two lines, `recvmmsg`,
receiver pinned to core 2 and publisher to core 6, load average 3.15.

| | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| wire to book | 6.06 us | 10.6 us | **20.3 us** | 43.9 us | 113 us |
| due to book | 1 ns | 1 ns | **7.4 us** | 513 us | 1.03 ms |

Zero gaps, zero messages lost, book correct.

Against the same measurement on macOS, where the wire to book p99 was 58.2 us
and the due to book p99 was **20.3 milliseconds**. The second column is the one
that moved, and it moved by three orders of magnitude, because on macOS the
receiver could not keep up with the schedule and here it is ahead of it ninety
percent of the time.

### Experiment one, the receive path shootout

The headline experiment from the spec, and the one that could not be run at all
on macOS, which has no `recvmmsg`, no `epoll` and no `io_uring`.

One line rather than two, because this measures the cost of getting a datagram
out of the kernel and arbitration is a different question. Receiver pinned to
core 2, publisher to core 6, 1,000,000 messages at 400,000 per second, recovery
off, best of three runs. Wire to book, nanoseconds.

| Strategy | p50 | p90 | p99 | p99.9 | max | Lost |
|---|---|---|---|---|---|---|
| blocking `recv` | 23,695 | 29,119 | 36,927 | 151,807 | 690,175 | 0 |
| busy poll | **5,087** | **8,295** | 10,959 | 24,463 | 518,143 | 0 |
| `recvmmsg` | 5,295 | 8,503 | 11,591 | **22,255** | 641,535 | 0 |
| `epoll` plus `recvmmsg` | 25,199 | 31,407 | 50,367 | 241,151 | 694,783 | 0 |
| `io_uring` | 28,319 | 33,919 | 46,079 | 140,159 | 562,175 | 0 |
| `kqueue` | not available on Linux | | | | | |

Four things in that table are worth saying out loud.

**Busy poll and `recvmmsg` are tied at the front and everything else is about
five times behind.** Busy poll wins the median by 4% and `recvmmsg` wins p99.9
by 10%, which on an unisolated machine is not a difference. They get there
differently. Busy poll never sleeps and pays a burned core for it. `recvmmsg`
sleeps and amortises the wakeup across up to 32 datagrams. **The crossover the
experiment exists to find is that at 400,000 messages a second there is enough
in the queue for batching to buy what spinning buys, without the core.**

**`epoll` does not help, it costs.** It is the same as blocking `recv` on the
median and worse at the tail. Readiness notification is a way to wait on many
descriptors at once, and this run has one socket, so the `epoll_wait` is pure
overhead ahead of a receive that was going to be ready anyway. `epoll` earns its
place at a thousand sockets, not at one.

**`io_uring` is the slowest on the median, which was not the expected answer.**
The reason is visible in the implementation. A ring still costs a submit per
batch of re-armed buffers, on top of reaping the completions, so it does two
trips where `recvmmsg` does one. Peeking the completion queue out of the shared
mapping before waiting was worth most of its latency and it still did not close
the gap. The configuration that would is `IORING_SETUP_SQPOLL`, where a kernel
thread polls the submission queue and the application never enters the kernel at
all. That needs privileges and burns a core, which makes it busy poll with extra
steps, and it is not implemented here. **`io_uring` is newer, not automatically
faster, and this is the measurement that says so.**

**Nothing was lost by any strategy**, so the comparison is between five
receivers that all kept up, which is the only way a latency comparison means
anything.

### And the bug the shootout found

`io_uring` had never been compiled before this, because macOS has none. On first
run it **segfaulted**, and the cause was a buffer ownership error that is worth
recording because it is the classic one.

The old code armed free slots at the end of `receive`, which handed the kernel
the very buffers it had just returned to the caller. The caller then read a
buffer the kernel was free to overwrite. Fixed by deferring the re-arm to the
start of the next call, so a slot is only given back once the caller is
demonstrably finished with it.

It also waited on completions with no timeout, so a receiver whose feed had
stopped waited forever and could never notice the feed had gone quiet.

Neither would have been found without a Linux kernel to run on, and both were
sitting in a header the repository had described as unreviewed.

### What this still is not

A container on the same laptop is a Linux kernel, not a benchmark box.

- No `isolcpus` and no `nohz_full`, so the core is pinned but not isolated, and
  the tail still contains whatever else the laptop is doing.
- Still loopback. No interface, no driver, no switch.
- Still a virtual machine sharing twelve cores with macOS, Docker itself, and
  everything else running.

So the figures above are a **floor rather than a result**. They say the Linux
paths work and that the receiver keeps up when the kernel has `recvmmsg` and
real pinning, and they do not replace the dedicated box in
`docs/LINUX_SETUP.md`. What they change is that the remaining gap is now a
better number rather than the difference between having one and not.

---

## 9b. Tick to trade, on the real file

```
./build/bin/tickerplant-trade --file data/12302019.NASDAQ_ITCH50.gz --symbol AAPL
```

Eight million real messages, quoting AAPL. Feed in, book, quoting rule,
pre-trade risk, OUCH encode, out to NanoExchange standing in as the venue.

| | |
|---|---|
| Orders sent | 72 |
| **Orders rejected by risk** | **926** |
| Fills | 8, for 800 shares |
| Risk check, p50 | 42 ns |
| Risk check, p99 | 751 ns |
| Decode to order, p50 | 584 ns |
| Decode to order, p99 | 8,127 ns |
| Heap allocations, feed path | 47 chunk acquisitions over 8M messages |
| Heap allocations, send path | **0** |

**The rejected count being thirteen times the sent count is the point**, not an
embarrassment. The risk engine is evaluating every order and refusing most of
them, on position limits, open order limits, and self-cross. A risk engine that
never fires has not been tested.

The 47 feed path allocations are the level memory pool taking one megabyte
chunks during warmup, which is roughly one per 170,000 messages and none of them
per message. The send path, which is the risk check and the OUCH encode,
allocates nothing at all.

The timing rows carry the same caveat as everything else measured here, and the
p50s are on an unpinned laptop.

**No profit and loss is reported anywhere**, because the fill model ignores
queue position and therefore fills far more often than reality would. That is
stated in the binary's own output every time it runs.

---

## 10. The NanoExchange retrofit, and what pinning is actually worth

The last item in the spec's definition of done. The sibling matching engine
gets the same measurement harness and republishes its numbers labelled, so its
"not pinned to an isolated core" caveat can come off honestly rather than by
being quietly dropped.

`nano/measure.hpp` is its own, not a copy of Tickerplant's. The dependency runs
the other way, Tickerplant builds on NanoExchange, and a shared header would
invert that.

### The median was right all along

| Operation | macOS, clang, **unpinned** | Linux, gcc, **pinned** |
|---|---|---|
| Add limit, p50 | 125 ns | **125 ns** |

Across an operating system, a compiler, and a scheduling policy, the median did
not move. The original caveat said the median was the honest measure and the
tail was scheduler noise. That was exactly right, and confirming an old caveat
is a better outcome than quietly replacing the number.

### Pinning on a quiet machine bought nothing

Best of five runs each, load average 3.00 on 12 cores, nanoseconds.

| Operation | unpinned p50 | pinned p50 | unpinned p99 | pinned p99 |
|---|---|---|---|---|
| Add limit | 125 | 125 | 416 | 416 |
| Add market | 167 | 167 | 500 | 500 |
| Cancel | 125 | 125 | 250 | 250 |
| Modify | 42 | 42 | 416 | 375 |

With nothing competing there is no migration to prevent, so there is nothing for
pinning to do.

### Pinning on a busy machine cut the best case and widened the spread

Eight spinning threads on other cores, five runs each, p99 in nanoseconds.

| Operation | unpinned best | pinned best | unpinned worst | pinned worst |
|---|---|---|---|---|
| Add limit | 541 | **459** | 625 | **917** |
| Add market | 625 | **542** | 750 | **1042** |
| Cancel | 541 | **416** | 666 | **834** |
| Modify | 541 | **458** | 583 | **708** |

Pinning improved every best case by 15 to 25 percent and made every worst case
worse by 20 to 50 percent, leaving the mean roughly unchanged.

**This is the most useful thing the retrofit produced and it was not the
expected result.** Pinning to a core you do not own is a bet. When that core is
quiet you keep your cache and win. When something else is scheduled there you
cannot migrate away from it, which unpinned you could, so you wait. Pinning
removes the scheduler's ability to help along with its ability to hurt.

Which is the entire argument for `isolcpus`, stated as a measurement rather than
as folklore. A pinned thread on an isolated core is not betting, because nothing
else is permitted to run there. The boot line in `docs/LINUX_SETUP.md` is no
longer something this project recommends because the literature says so. It is
something it recommends because pinning without it was measured and found to be
a coin flip.

---

## Not yet measured

These are open, and they are listed rather than filled in with something
plausible.

- **Every number on a pinned, isolated core.** The receive path shootout across
  `recv`, `recvmmsg`, `epoll` plus `recvmmsg`, busy poll, and `io_uring` needs
  Linux, and so does `SO_TIMESTAMPING` at nanosecond resolution, `isolcpus`, and
  `nohz_full`. `docs/LINUX_SETUP.md` has the exact boot line and the run script.
- **A real network.** Everything here is loopback. There is no interface, no
  driver, and no switch in any measurement in this file.

  Getting off loopback was attempted and did not work, and the attempt is worth
  recording. Two containers on a Docker bridge would put a veth pair and a
  Linux bridge in the path, which is not a wire but is a real network stack.
  The datagrams never arrived. The bridge has `multicast_snooping` on and
  `multicast_querier` off, which is the combination that silently drops
  multicast to ports the bridge has not learned a membership for, and with no
  querier it never learns any. Turning snooping off on the bridge did not fix
  it either, so something further up the Docker Desktop virtual machine's
  network path is also dropping it. Left unresolved rather than worked around,
  because the honest version of this experiment is two machines and a switch.
- **The NanoExchange retrofit.** The same harness applied to the sibling
  project so its numbers can be republished pinned.
