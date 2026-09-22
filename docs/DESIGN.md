# Design

What was built, what was measured, and what was rejected.

This is the engineering writeup. The numbers live in
[../results/RESULTS.md](../results/RESULTS.md) with the command that produced
each one, and this file is about why the code looks the way it does.

---

## 1. The decision the whole project turns on

A feed handler does not match. It applies.

That sentence is the difference between a feed handler and an order book demo,
and getting it wrong is the failure mode that looks like it works for about
thirty seconds.

NanoExchange, the sibling project, is a matching engine. Its `MatchingEngine`
takes an aggressive order, walks the opposite side of the book, and decides
which resting orders trade. That is what an exchange does.

Tickerplant is on the other side of the wire. When an `E` message arrives saying
that order reference 4,812,339 executed 100 shares, the exchange has already
decided. The feed is a record of a decision, not a request for one. Running that
message through a matching engine would execute against whatever happens to be
at the top of the local book, which is not necessarily the order the exchange
filled, and from that moment the two books are different books. There is no
recovery from it, because nothing in the feed ever says "here is the whole book
again", and the error compounds on every subsequent message.

So the reuse is careful. `nano::Order`, `nano::PriceLevel`, and
`nano::MemoryPool` come across unchanged, because they were always about holding
a book rather than about running an auction. `nano::MatchingEngine` does not,
and `BookBuilder` exposes `on_add`, `on_execute`, `on_cancel`, `on_delete`,
`on_replace` and nothing that could be mistaken for matching.

This is the question an interviewer asks about this project, and it is a good
answer to "what surprised you".

## 2. The wire

NASDAQ TotalView-ITCH 5.0. About twenty message types, fixed length, packed,
big-endian, with an eleven byte common header. The specification is public and
it is dense, and turning the prose into correct classes is most of milestone
one.

Two things in it will bite, and both were planned for rather than discovered.

**The messages are not aligned.** A message starts wherever the previous one
ended, so a `uint64` order reference routinely lands on an odd byte. Casting a
buffer offset to a `uint64_t*` and dereferencing it is undefined behaviour. On
arm64 it usually works. On some targets it faults. Under UBSan it reports every
single time. The correct move is `memcpy` into a local of the right width, which
every optimiser at `-O2` and above folds into a single unaligned load, so there
is no cost to being correct and a real cost to being wrong. Everything in
`endian.hpp` goes through `memcpy`, and the decoder tests run against
deliberately offset buffers to keep it that way.

Measured, at real unaligned offsets on this machine, the width does not matter.
A 16 bit load is 1.445 nanoseconds, a 64 bit load is 1.457. The byteswap
disappears next to the load.

**Everything is big-endian and the timestamp is six bytes.** There is no six
byte integer type and `std::byteswap` is C++23, so the compiler builtins are
wrapped once in `endian.hpp` and tested against known vectors rather than
open-coded at twenty call sites. The timestamp is read as two loads, a four byte
and a two byte, zero extended into a `uint64`. Six bytes holds about 78 hours of
nanoseconds, so a trading day never comes close to overflowing it.

**No floating point anywhere between the wire and the book.** ITCH works in
integer ten-thousandths of a dollar and NanoExchange works in integer ticks, so
the conversion is a widening and nothing else. Prices are compared, never
summed with rounding, and a division only ever appears when something is about
to be printed.

### Dispatch

The message type is one ASCII byte at offset zero. Dispatch is a `switch` over a
small dense set, which clang turns into a jump table, and the cases are ordered
by how often the type appears on a real day. That ordering costs nothing at
runtime and documents where the volume actually is, which on 2019-12-30 is 44%
add, 43% delete, 8% replace, and 2% execute.

The message length table is a 256 entry `constexpr` array indexed by the raw
type byte rather than a switch, so validating a length is one load with no
branch. It is also the thing that makes an unknown type safe. A type this build
does not know has a length of zero, and a length of zero is refused rather than
guessed at.

## 3. The two decoders, and the measurement that deflated the headline

There are two decoders that produce exactly the same sequence of handler calls
from the same bytes.

`ZeroCopyDecoder` reads fields out of the receive buffer where they already are,
at the moment the handler needs them. The handler is a template parameter, so a
field the handler ignores is dead code and is never loaded. The eight byte
ticker on an add order is the clearest case. The book keys on stock locate, the
ticker is never read, and those eight bytes are never touched.

`CopyingDecoder` does what a straightforward implementation does. It copies the
message into a staging buffer, byteswaps every field of that message type into
an owned `NormalizedEvent` whether anyone wants it or not, and dispatches from
the staged object. This is what you get for free if you push messages onto a
queue before decoding them.

The second one exists for two reasons. Without the slow path, "zero-copy" is a
claim rather than a measurement. And two independent decode paths over the same
268 million messages producing identical books is the strongest correctness
check in the project.

Now the measurement, which did not say what the first version of this section
said it did.

| | `NullHandler` | `BookBuilder` |
|---|---|---|
| zero-copy | 502.6 M msg/s | 4.52 M msg/s |
| copying | 68.1 M msg/s | 4.40 M msg/s |
| ratio | **7.4x** | **1.027x** |

With a handler that throws its input away, lazy decode is 7.4 times faster. With
a handler that rebuilds the book, it is 2.7% faster, because the book work is
98% of the cost. 886 milliseconds against 910, for four million messages.

Both rows are true and only one of them describes the system. The claim the
measurement supports is that **zero-copy decode costs nothing and removes 58.7
milliseconds of pure overhead per four million messages**. It does not support
"a seven times faster feed handler", and reporting only the first column would
have been a marketing figure dressed as a benchmark.

The other thing the second column says is where the work actually is, which is
the order reference table and the price level containers, and that is where the
next two sections went.

## 4. The order reference table, and a prediction the data refuted

This is the hottest lookup in a feed handler and it is the structure a matching
engine does not have to get right in the same way. On 2019-12-30 there are 118
million add order messages, and every execute, cancel, delete, and replace that
follows has to find the resting order by its 64 bit reference. Nothing else in
the pipeline runs that often.

`std::unordered_map` is the obvious answer and it is the wrong one. It is a
chained hash table, so every lookup is a bucket index followed by a pointer
chase into a separately allocated node, and every add order allocates a node.
That is two cache misses on the critical path and a call into the heap allocator
on the most frequent message type.

`FlatOrderMap` is open addressed with linear probing. Keys and values live in
two flat arrays, so a lookup is one load from the key array and, on a hit, one
load from the value array at the same index.

**Deletion uses backward shift rather than tombstones, and that is the decision
worth defending.** A feed handler deletes almost as often as it inserts, 114
million deletes against 118 million adds on this day. A tombstoned table
degrades over a session until every lookup walks a long run of dead slots.
Backward shift restores the table to the state it would have had if the deleted
key had never been inserted, so probe lengths do not drift. It costs a short
scan on erase, and Knuth's algorithm R is the correct form of it. The measured
result is that the flat table is **faster on an erase heavy slice than on the
mixed trace**, 40.8 M ops/s against 37.6, which is the shape you want.

Against `std::unordered_map` on the real trace, 2.1 times on the mixed trace and
2.8 times on the erase heavy one, and that comparison is against a map that has
had `reserve` called on it. Comparing against an unreserved map would have been
an unfair fight and an interviewer would say so, so both are in the table.

### The part that was wrong

The header originally argued that near-sequential ITCH order references are the
best possible case for an identity hash with linear probing, because consecutive
keys land in consecutive slots and never collide until the table wraps. It was a
clean argument and it was wrong.

On the real trace at a load factor of 0.32, the identity hash averages **96.4
extra probes per lookup against 0.357** for splitmix64, runs 4.4 times slower,
and loses to an **unreserved** `std::unordered_map`.

The mechanism is that dense is not contiguous. About seventy percent of
references are larger than the previous one and the key span grows by 1.32 per
insert, so the live keys form long unbroken runs of occupied slots with holes
between the runs, which is precisely the input linear probing handles worst. The
new references minted by `U` replace messages reach into a higher numeric range
and alias modulo the table size straight back on top of the live low-numbered
run.

**And it is a crossover, not a constant.** On a hundred thousand message window,
load factor 0.018, the identity hash wins by nearly two to one at 0.0006 probes
per lookup. Anyone who benchmarks this structure on a toy window gets the
opposite answer and a clean chart to defend it with.

Both hashes stay in the repository, the default is splitmix64, and the header
now carries the refutation rather than the prediction. This is the most
useful result in the benchmark suite and it only exists because the benchmark
ran on real data at a realistic load factor.

## 5. The book

### Why it is not NanoExchange's price containers

NanoExchange has three interchangeable price level containers behind a concept
and a benchmark that picks between them. None of them could be used here, for
two separate reasons.

The first is mundane. They were written for a matching engine, which only ever
asks for the best level and for one level by price, so none of them can be
iterated. A feed handler needs to walk the book for a depth snapshot and for the
digest that proves two runs produced the same state.

The second is sharper and is a real finding. **The direct addressed array
container, which won NanoExchange's own shootout, cannot be used on real market
data at all.** It indexes by the offset of a price from a fixed base, so it
costs memory proportional to the whole price window rather than to the number of
live levels. Real NASDAQ prices on this day run from a few hundred ticks to over
a hundred million. A window wide enough for the real data is gigabytes per side
per symbol, and a per-symbol window would need rebasing on every halt and
reopen. A structure that wins on a synthetic book with a bounded price range is
unusable on the real thing, and finding that out is worth more than the
benchmark it won.

So `book_side.hpp` holds the levels and `nano::PriceLevel`, `nano::Order`, and
`nano::MemoryPool` do the work underneath. The intrusive list threading orders
through a level is NanoExchange's, unchanged, and the test that it survives a
sorted vector moving the level out from under it is in `test_book_side.cpp`.

### And the two benchmarks disagree

Two containers, for the same reason NanoExchange had three.

Isolated on AMZN's real add and delete stream, the red black tree beats the
sorted vector by **five times**, 23.0 M events/s against 4.55. AMZN carries
4,926 live price levels at the open and the vector is moving thousands of pairs
per insert.

Across the whole feed, the vector is **4% faster**, 5.71 M msg/s against 5.49.
546,551 live levels across 8,906 symbols is about 61 levels per symbol, most
books are shallow, and locality wins there.

Both measurements are correct and they point opposite ways. The honest
conclusion is that it depends on the symbol, and the only useful way to report
it is to say which one was measured and on what. The depth sweep built to
explore this turned out to be nearly a no-op, because prewarming 512 extra
levels is noise on top of a book that is already 4,900 deep, and that is itself
the finding. **A feed handler book is two orders of magnitude deeper than a
matching engine benchmark assumes.**

The two containers produce identical digests over the same four million
messages, which is a free correctness cross-check that came out of having two.

### The resting order state machine

Where the invariants live.

```
          A/F --> [ RESTING qty=N ]
                       |
        E/C/X (partial)|--> [ RESTING qty=N-k ]   k < N
        E/C/X (full)   |--> [ GONE ]              k == N
        D              |--> [ GONE ]
        U              |--> [ GONE ] and a new reference is added
```

Every transition has a test and so does every illegal transition, because the
illegal ones are the ones that actually happen. An execution against a reference
that is not resting is what a sequence gap looks like from inside the book
builder. The correct behaviour is to count it and carry on. Crashing loses the
rest of the day. Silently creating the order invents liquidity that does not
exist.

The counters are `duplicate_refs`, `orphan_executes`, `orphan_cancels`,
`orphan_deletes`, `orphan_replaces`, `overfills`, and `pool_exhausted`. On a
clean replay of the whole file **every one of them is zero across 268 million
messages**. On a wire replay with injected loss they are not, and their size is
the measure of what the gap cost.

Three details that are easy to get wrong and are worth naming.

`E` carries no price, so the notional uses the resting order's display price.
`C` carries its own price because the print happened away from the display
price, and a **non printable** `C` is one leg of something reported elsewhere,
so counting it would double count the day's volume. It still reduces the book.

`U` deletes the old reference and adds a new one, and the old reference is never
reused. The side and the symbol come from the order being replaced because the
message does not carry them, which is exactly why an orphaned replace cannot be
recovered from and is counted rather than guessed at.

`P` and `Q` never touch the book. `P` is a trade against hidden liquidity that
never rested visibly, and `Q` is an auction. Both count toward volume and
nothing else, and there is a test that asserts the book digest is unchanged
across them.

### The symbol table is an array

ITCH gives every instrument a 16 bit stock locate in the `R` messages at the
start of the day, and every later message carries the locate rather than the
ticker. That is the exchange doing the interning for you, which is why nothing
on the hot path ever compares a string. The table is a flat 65,536 entry array
indexed by the locate, and a NASDAQ day defines about nine thousand of them, so
most of it is empty. The alternative saves a couple of megabytes and costs a
hash on the one lookup per message that has to happen regardless.

## 6. The wire, for real

### MoldUDP64

Twenty byte header carrying a ten byte session, an eight byte sequence number
for the first message in the packet, and a two byte count. Then that many
length-prefixed message blocks. Count zero is a heartbeat and count 0xFFFF is
end of session.

`PacketView::parse` is the one place in the project where the input is genuinely
untrusted, and it is written that way. A truncated packet, a block length that
runs past the end of the datagram, or a count that does not match the blocks
present all return nothing rather than reading out of bounds. The test builds a
hostile packet by hand for each case.

One API detail that fell out of writing it. A request packet is twenty bytes
with a count of, say, 100, which is malformed as a downstream packet because
there are no blocks behind it. Rather than weakening `parse` to accept both and
losing the validation, there is a separate `parse_header` for the re-request
server. Both behaviours are tested.

### A and B, and why arbitration is not a stage

A real exchange publishes the same feed on two multicast groups from separate
hardware down separate paths, because the two paths do not fail together. The
receiver takes whichever copy of sequence N arrives first and discards the
second.

The thing worth saying about the implementation is that **arbitration is not a
separate buffering stage**. It falls out of sequence tracking. A message whose
sequence is below what the sequencer expects is a duplicate, whichever line it
came from, and the counter that records which line won is the only code that
exists specifically for arbitration. Everything else is the sequencer doing its
job.

Measured on a clean loopback run, line A wins 99.93% because the publisher sends
A first on the same thread. Under 10% loss per line, A wins 90.77%. The win rate
tracking the loss rate is the evidence that arbitration is doing work, because
every sequence B wins is one A lost.

### The reorder window

Fixed capacity, allocated once, indexed by `sequence & mask` so a lookup is a
mask rather than a search. Each slot carries the sequence it holds as well as
the payload, because modular indexing aliases and a slot has to prove it holds
the sequence you asked for rather than one from an earlier lap of the ring.

It is bounded on purpose. An unbounded buffer turns one lost packet into
unbounded memory growth, which is a worse failure than a book you know is bad.

### The gap state machine

```
synced --(a sequence above expected arrives)--> gap
gap --(the hole fills within the grace period)--> synced
gap --(grace expires, or the window fills)--> recovering, and a request goes out
recovering --(the range arrives)--> synced
recovering --(timeout, attempts left)--> recovering, retry with backoff
recovering --(attempts exhausted)--> stale
anything --(the window would overflow)--> stale
```

`stale` is one way. The only exit is an explicit reset, and that is deliberate,
because pretending a book is fine after unrecovered loss is the exact failure
this whole file exists to prevent. `tickerplant-rx` exits non-zero when it ends
stale.

A stale feed keeps arbitrating and keeps delivering in order. It just never
claims to be synced again. The alternative is sitting forever on messages it
already has because the state machine has run out of things to say.

### Two bugs the soak test found

Both were found by a randomised soak that generates two hundred thousand
sequences, drops a configurable share on each line independently, jitters
arrival order, and asserts the sink saw a strictly increasing contiguous run.
Neither would have been found by a hand written case.

**A second hole under an answered request.** When a retransmission filled one
hole and draining the window exposed a second one, the sequencer stayed in
`recovering` against an already-satisfied request and sat there until the window
overflowed. The fix is to record what the outstanding request covers and drop
back to `gap` with a fresh grace clock the moment the expected sequence passes
it.

**The grace period outliving the window.** At a high enough message rate the
window overflows before the grace period expires, so the feed went stale without
ever having asked anyone for the data, which is the worst of both choices. The
fix is a second trigger. A window three quarters full asks immediately, whatever
the clock says.

The second bug is the interesting one because the underlying constraint is a
real design rule. **The window has to hold more messages than arrive during one
recovery round trip**, and the valve is a backstop rather than a licence to
misconfigure.

### Which produced the most useful result in the project

The 10% loss run was first done with a 4,096 message window and it failed.

| Reorder window | Gaps | Recovered | Lost | State | Book |
|---|---|---|---|---|---|
| 4,096 | 355 | 137 | 6,615 | stale | differs |
| 65,536 | 357 | 357 | 0 | synced | **bit identical** |

At 150,000 messages a second, 4,096 messages is 27 milliseconds of feed and the
recovery timeout is 50 milliseconds. The window overflowed before the
retransmission could arrive. Same code, same seed, same loss, and the only
difference is a number that had been picked because it looked reasonable.

This is the kind of thing that is obvious once measured and invisible before,
and it is the answer to "tell me about a bug you found".

### Recovery

The client caps a request at 65,535 messages because the count field is sixteen
bits, splits anything larger, and backs off exponentially between retries. The
publisher side is a bounded ring of recently published messages. A real venue
keeps the whole session on disk behind an in-memory window, and a request for
something older than the ring here is answered with **nothing rather than with
the wrong message**, with a counter for how often that happened.

## 7. Measurement

The measurement layer is the contribution. The sibling project shipped latency
numbers that were honest and unpinned, and a trading firm reads an unpinned p99
as "has not measured latency properly yet". So the methodology gets the same
scrutiny as the code.

### Which clock, and why the cycle counter

Measured on this machine, per call.

| | |
|---|---|
| `now_ns()`, `CLOCK_UPTIME_RAW` | 12.36 ns |
| `rdtsc()`, `cntvct_el0` on arm64 | 0.36 ns |
| `rdtsc_serialized()`, with `isb` | 22.81 ns |

The clock read costs thirty four times the counter read. When the thing being
measured is itself tens of nanoseconds, that is the whole argument for the
cycle counter on the hot path. The fenced read costs sixty three times, so it
brackets a batch and never a message.

The monotonic clock is the **raw** one, `CLOCK_MONOTONIC_RAW` on Linux and
`CLOCK_UPTIME_RAW` on macOS, because it is not slewed by NTP. A latency
measurement wants elapsed time, not corrected time.

Calibration of the counter against the clock is least squares over sixteen
samples across a 200 millisecond window rather than a single pair, and it
reports its own error, which came out at 0.067 ppm here. On a machine where the
invariant counter is not guaranteed or frequency scaling is on, that calibration
is only as good as the platform, which is why the Linux benchmark box sets
`intel_pstate=disable`.

One thing commonly got wrong and worth recording. `cntfrq_el0` on this M3 Pro is
1 GHz, so one tick is one nanosecond. The 24 MHz figure usually quoted for Apple
Silicon belongs to the mach timebase, which reports 41.7 ns per tick here. They
are two different counters and mixing them silently produces numbers that are
wrong by a factor of forty.

### Kernel receive timestamps

On Linux, `SO_TIMESTAMPING` puts the timestamp in a control message, so the
receive syscall and the wakeup are inside the measurement rather than outside
it. The value arrives on the realtime clock, so it has to be converted to the
monotonic base with one offset captured at startup, and the two clocks drift far
too slowly relative to microseconds for that to matter. Getting this wrong by
comparing a realtime timestamp against a monotonic one produces a latency
measurement that is off by the entire difference between the two epochs, which
is the kind of error that looks like a broken feed.

macOS has `SO_TIMESTAMP`, which is a `timeval`, so microsecond resolution
against Linux's nanoseconds. That is recorded in the results rather than papered
over, and where the platform supplies nothing the receiver takes a userspace
timestamp and **says in its output which one it used**.

### HdrHistogram, and coordinated omission

Percentiles come from HdrHistogram_c, which is bounded memory with correct high
percentiles. A vector of samples would give the same answer and allocate
proportional to the number of messages, which on a full day is hundreds of
millions of samples and is not a thing to do on the machine you are measuring.
There is no reason to report a mean.

Coordinated omission is the part worth explaining properly, because most
candidates cannot and the ones who can get asked about it with respect.

A load generator that blocks when the system stalls stops sampling exactly when
latency is worst, so the recorded distribution silently omits the bad period.
The usual correction back-fills the samples a fixed rate sender would have taken
during the stall. Here is what that looks like on synthetic data with an 80
nanosecond body and a 50 microsecond stall once every thousand samples, at an
intended interval of 100 nanoseconds.

| | p50 | p99 | max |
|---|---|---|---|
| raw | 79 ns | **144 ns** | 50,015 ns |
| corrected | 94 ns | **48,607 ns** | 50,015 ns |

Same data. The raw p99 says the system is fine.

**And the receiver does not apply that correction, deliberately.** The publisher
writes a schedule manifest before it sends anything, saying when each message
was due, and the receiver measures every message against that rather than
against when it arrived. The publisher is a separate process sending UDP and
never blocks, so every message that was due produces a sample and the
distribution is already complete. Applying the correction on top would count the
same delay many times over.

The first version of this receiver did exactly that, and it was not merely
redundant. `hdr_record_corrected_value` costs one loop iteration per expected
interval inside the sample, so a millisecond sample at a 2.5 microsecond
interval back-fills four hundred entries, which slowed the receiver down, which
made the next sample larger. The feedback loop was clearly visible in the
numbers, and a run that should have recorded two million samples recorded
twenty four billion. `Histogram::record_corrected` stays in the library and is
tested, because a harness that does block needs it. It does not belong in that
loop.

Here is what measuring against the schedule buys, on one clean run.

| | p50 | p99 | max |
|---|---|---|---|
| wire to book, from the kernel timestamp | 21.2 us | 58.2 us | 14.4 ms |
| due to book, against the schedule | 1.7 us | **20.3 ms** | 37.3 ms |

Three hundred times worse at the p99, from the same messages. A datagram that
sits in the socket buffer for twenty milliseconds gets timestamped when it comes
out rather than when it was owed, so the first row cannot see the receiver
falling behind and the second row is all it can see.

### Pinning, and what this machine cannot do

The development machine is an Apple M3 Pro and it is worth being precise about
what that rules out, because the temptation is to describe an advisory hint as
pinning.

- `pin_to_core` returns false. `thread_policy_set` with
  `THREAD_AFFINITY_POLICY` returns `KERN_NOT_SUPPORTED` on this machine, so
  there is not even the weak L2-sharing hint older Intel Macs offer.
- `mlockall` fails, so pages can be evicted mid-run and page faults are live in
  the tail.
- `set_realtime_priority` does work through `THREAD_TIME_CONSTRAINT_POLICY`, but
  that is a deadline hint intended for audio. It is not `SCHED_FIFO` and it does
  not stop migration, so it is not described as realtime scheduling anywhere.
- There are six performance cores and six efficiency cores, the split is
  invisible to user space, and a thread that migrates from one to the other did
  not merely lose its cache, it changed clock rate. This is the single biggest
  reason a macOS tail cannot be trusted.
- There is no scaling governor to read, so `box_info` reports `unknown` rather
  than guessing.

`current_core()` returns -1 rather than 0 on macOS, deliberately, so that
"unknown" can never be mistaken for "core zero" in a results file.

The clearest single argument for moving to Linux is not any of the above. It is
that the identical configuration measured **4.52 M msg/s in one binary and 5.49
M msg/s in another**, a 21% gap, while the within-run variation of each was
under 0.1%. Repeatability inside a run says nothing about repeatability across
runs here.

`docs/LINUX_SETUP.md` has the boot line, `isolcpus=2,3 nohz_full=2,3
rcu_nocbs=2,3 intel_pstate=disable idle=poll`, and the script that produces the
pinned table.

### Zero allocation, proving the instrument works, and the defect it found

The hot path allocates nothing. Every buffer, the order pool, the reorder
window, the order table, and the histograms are allocated once at construction.

That is asserted by a counting allocator that overrides every form of `operator
new` and `operator delete`, including the sized and aligned and nothrow ones,
because a missed overload is how a counting allocator quietly reports zero while
the code is allocating.

The test that matters is not the one asserting zero. It is the one that
allocates deliberately and asserts the counter noticed. A counter nobody has
proved can see an allocation is worth nothing.

**And then it found something.** The first run of the tick to trade binary
reported 14,209 heap allocations in the feed path over a three hundred thousand
message replay.

The order objects come from NanoExchange's pool and allocate nothing, which is
exactly why this was easy to miss. The pool made it feel as though the book path
was clean. It was not. Every new price level was a `std::map` node straight out
of the general purpose heap, and on the real day, with 546,551 live levels
churning continuously, that is millions of allocator calls on the hot path. That
is the tail latency source this whole project is about, sitting in the middle of
it.

The obvious fix was `std::pmr::unsynchronized_pool_resource`. It worked, 14,209
allocations down to 17, and it cost throughput. It is a general purpose
structure that copes with any size and any alignment and keeps per-size pools
with their own bookkeeping.

A book needs one thing. Price level nodes are all the same size, because they
are all nodes of the same map, and the feed creates and destroys them
continuously. So `tick::PoolResource` is a free list per size class carved out
of one megabyte chunks and nothing else, and a new level costs a pop off a free
list.

| | Allocations | Best of three runs |
|---|---|---|
| the general purpose heap | 14,209 per 300k messages | 1.502 M msg/s |
| `std::pmr::unsynchronized_pool_resource` | 17 | 1.332 M msg/s |
| `tick::PoolResource` | 17 | 1.672 M msg/s |

All three produce the same book digest. The run to run spread on this unpinned
machine is wider than the difference between the configurations, so the
defensible claim is narrow and is the one made in the results file. The custom
pool removes the allocations without costing throughput, and the standard one
removes them and costs some.

All three remain selectable with `-DTICK_LEVEL_ALLOCATOR`, because a comparison
somebody can rerun is worth more than a paragraph saying which won.

## 8. Concurrency

The feed path is single threaded on purpose. One thread takes datagrams off the
socket, sequences them, decodes them, and applies them to the book, because
every handoff between threads is a queue, a cache line moving between cores, and
a source of jitter, and none of that buys anything when the work is already
fast enough to keep up with the feed.

What is here for the cases where threading is genuinely needed is a bounded
multi producer multi consumer queue and a seqlock, both with their memory
ordering written down and both exercised under ThreadSanitizer. The SPSC queue
in NanoExchange is strictly faster and remains the right default. MPMC earns its
cost only when there really are several producers or several consumers, and
saying that in the header is worth more than the code.

The seqlock is the right structure for publishing top of book to many readers,
because a reader never blocks the writer and the writer never waits for a
reader. It is also the one place in the project with a genuine subtlety worth
discussing, which is that a reader can observe a torn value mid-write, so the
value has to be read with a `memcpy` into a local with an acquire fence between
reading the value and re-reading the sequence, and why acquire on the first
sequence load alone is not enough.

This is arm64, which has a weaker memory model than x86. Code that is
accidentally correct on x86 because of total store ordering fails here, which
makes it a better machine to get this right on than an Intel one. The generated
code confirms the reasoning rather than the comments asserting it. The queue
emits `ldapr` and `stlr` and no `dmb` at all, which is what the relaxed compare
and exchange argument predicts, and the seqlock emits the two `dmb` barriers
where the fences are.

### What the measurements said, including the parts that went the wrong way

The MPMC queue costs about **1.6 times** the SPSC ring at one producer and one
consumer. That is the number that keeps the SPSC ring the default, and it is why
the header says so before it says anything else.

Past four producers and four consumers, **the mutex wins**, 10.0 M items per
second against the lock free ring's 8.5. A lock batches under heavy contention,
because the holder runs a burst while the hot cache lines stay on one core,
where the lock free ring has every thread bouncing the same lines between cores.
That is the same effect NanoExchange's design writeup already found on a
different structure, and finding it twice on different code is more convincing
than finding it once.

**False sharing, measured rather than asserted.** The packed variant of the
queue is a byte for byte copy with identical orderings and identical per-slot
padding, differing only in the head and tail sharing one cache line. It is
**2.3 times slower** at one producer and one consumer. Isolated down to two bare
counters, same line gives 150.8 M operations per second, split by 64 bytes gives
493.3 M, and split by 128 gives 711.2 M. This machine reports a 128 byte cache
line, so 64 recovers most of the loss and not all of it, which is a concrete
demonstration that 64 is a convention rather than a guarantee.

**The seqlock has a case where it loses and it is published.** The writer side
is what it is sold on and it holds, 2.6 nanoseconds to publish with no readers
against a mutex's 10.3, and 29 nanoseconds against 398 with one reader, because
the mutex writer queues behind every reader and the seqlock writer does not.
The reader side against a writer publishing in a tight loop with no work between
updates is starved, retrying 160 times per read and losing to the mutex, 261
nanoseconds against 48. That is the pathological case and it is not what a book
thread does. Give the writer a few hundred cycles between publishes, still far
faster than any real top of book, and the reader is 5 nanoseconds with 0.15
retries. Both are in the results file, because reporting only the second would
have been the dishonest version.

### The ThreadSanitizer report that is meant to be there

`MpmcQueue` runs clean, five runs, zero warnings.

`SeqLock` reports races, twelve runs, ten to fourteen reports each, and **zero
failed assertions in every run**, meaning no reader ever accepted a mixed
snapshot. Every report lands on exactly two lines, the writer's and the reader's
`memcpy` of the value bytes. Nothing on the sequence counter, nothing on the
fences.

Run on Linux under gcc instead, the same tests report **nothing at all**. That is
not a contradiction to resolve in gcc's favour. The two runtimes instrument
differently around `memcpy` and standalone fences, and the race is a property of
the memory model rather than of the tool, so clang is right and gcc's silence is
merely silence. A clean sanitizer run is weak evidence and a dirty one is strong
evidence, which means the dirty result is the one worth keeping.

That is correct and it is not suppressed. A seqlock writes the value bytes while
readers read them with no atomic on those bytes. The fences order the accesses
and do not make them non-racy, and the standard defines the race on the accesses
themselves, so ThreadSanitizer is right by the letter of the model. The
standard-clean alternative is per-field relaxed atomics, which costs the memcpy
that makes the technique worth using, and what real systems do is exactly this,
including Linux's own `seqlock_t`, which uses ordinary memory under read and
write barriers. A one line suppression is verified to work and is deliberately
left out of the repository, because a green sanitizer run bought by hiding the
one report that explains the design is worth less than the report.

## 9. Tick to trade

The second half exists because a feed handler with nothing at the end of it
cannot be measured end to end. The path is feed, book, quoting rule, pre-trade
risk, OUCH gateway, and NanoExchange standing in as the venue.

**The risk engine is the part worth reading.** Almost no student project has
one, and every trading firm cares about it. Position limits that count working
exposure and not only filled position, maximum order size and notional, a fat
finger bound against the current micro price, an allocation-free message rate
limiter, a stale market data check, and a kill switch.

Two decisions in it are the ones to defend.

**The fat finger check fails closed.** It needs a reference price and the
reference is the current top of book. When there is no two sided market there is
no reference, and the order is rejected rather than passed. Failing open there
is how firms lose money.

**The kill switch is one way.** Once tripped nothing goes out until a human
calls an explicit reset, which is a separate function so it cannot happen by
accident as part of some other recovery path.

What is deliberately not implemented is named in the header rather than left to
be discovered, which is short sale locate, credit and margin, the full scope of
SEC 15c3-5, and cross-venue aggregate exposure.

The quoting rule quotes around the micro price rather than the mid, because the
mid ignores size and when the bid is ten times the ask the next trade is far
more likely to happen at the ask. It skews linearly against inventory, because a
rule without that accumulates a position until it is only a bet on direction.
And it requotes hysteretically, because cancelling and replacing for a one tick
move costs two messages and loses queue position, and queue position at a price
is usually worth more than the tick.

**It makes no claim to be profitable and there is no test that asserts it is.**
It exists so the tick to trade path has a decision at the end of it. Everything
interesting in this project is upstream of it, and saying that plainly is the
point.

## 10. What a production system would do differently

Worth knowing, and worth being able to say without having built it.

**Kernel bypass.** Solarflare `ef_vi`, Onload, or DPDK, which take the kernel
out of the receive path entirely and put packets into user space memory the
application already owns. That is the real answer at this tier and it is worth
single digit microseconds at the p99 against the kernel path. It cannot be done
here because it needs the card, and **claiming a kernel bypass number without
the hardware would be a fabrication**. What is free is understanding why you
would want it, what it costs, which is a dedicated NIC, a burned core, and a
much smaller pool of people who can debug it, and having that answer ready.

**Hardware timestamping.** `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_RX_HARDWARE`
on a NIC with a PHC moves the start of the measurement from the kernel to the
wire. Software timestamps are taken after the interrupt, so everything before
that is invisible.

**FPGA decode.** The ITCH decode and the book update in gate arrays, with
software only on the slow path. Different project, different decade.

**More of the specification.** Thirteen message types are implemented, which is
everything that touches the book plus the session and directory messages. A
production handler implements all of them, handles the NOII and auction messages
for the open and close, and backs out broken trades from the volume, which this
one counts and reports rather than correcting.

**A real session store.** The retransmission ring here holds a bounded window in
memory. A venue keeps the whole session.

## 11. Honest limitations

Collected in one place rather than scattered, because they are the first things
an interviewer should be told rather than the last things they find.

- **No timing in this repository is fit to quote.** Every number is unpinned,
  every wire run is loopback, and the machine is a working laptop with a browser
  and a container runtime on it. The same binary on the same file measured 5.68
  M msg/s at a load average under 3 and 0.91 at 15. In one run the copying
  decoder measured faster than the zero-copy decoder, which cannot be true and
  is purely the load moving underneath the two passes. Every tool records the
  load average and prints a warning when the machine was busy, which is a habit
  taken from a sibling project that shipped benchmark numbers at a load average
  of 28 to 65 before anyone noticed. What is quotable from this machine is
  counts, fingerprints, and outcomes, which do not move.
- **macOS loopback multicast delivery was measured at about 3.4 milliseconds
  from `sendto` returning to the kernel receive timestamp.** That swamps
  everything the code does, so local wire runs are a correctness harness and not
  a latency measurement, and they are described that way everywhere.
- **The volume reconciliation is session against month.** No free per-symbol
  Nasdaq-only daily file exists for this date. The evidence is the tightness of
  the ratio across hundreds of symbols, not an exact tie-out.
- **15.4% of the replayed volume is in Tape B issues Nasdaq does not publish in
  those workbooks**, including SPY and IWM, and that share is verified by
  nothing.
- **AddressSanitizer does not run on macOS here, and now runs on Linux.** The
  ASan runtime hangs during its own initialisation on macOS 26.5.1, confirmed
  against a hello world, and so does ThreadSanitizer's. That was a real hole in
  the verification for a while. It is closed. **All 373 tests pass under
  `-fsanitize=address,undefined` in a Linux container**, which is the coverage
  the guard page test was standing in for. The guard page test stays, because a
  million messages decoded against a `PROT_NONE` page is a different check from
  a redzone and catches a read one byte past a buffer that happens to be
  followed by more of the same allocation.
- **The `io_uring` receive strategy is no longer unreviewed, and it was
  broken.** It had never been compiled, because macOS has no liburing. On its
  first run in a Linux container it segfaulted, from a buffer ownership error
  that armed free slots at the end of `receive` and so handed the kernel the
  very buffers it had just given the caller. It also waited on completions
  without a timeout, so a receiver whose feed stopped waited forever. Both are
  fixed and it now runs clean, and it is the slowest strategy in the shootout.
- **The benchmark window starts at the market open, not the head of the file**,
  which means 8.4% of its messages are orphans referring to orders added before
  the window. That is reported as a counter on every row. Benchmarking from the
  head of the file would have measured pre-open quoting in a handful of ADRs on
  a live set small enough to sit in cache, which is a real file and a fake
  workload.

---

## 12. Notes for anyone building on this

Three things that are not defects and will still cost an afternoon if nobody
says them out loud.

**The order pool is a compile-time capacity.** `TICK_ORDER_POOL_CAPACITY`
defaults to four million, against a measured peak of 1,924,078 resting orders on
2019-12-30. That headroom is comfortable for this session and is not a
guarantee for another one. A different trading day, and particularly a more
active one, should have the peak that `tickerplant-replay` reports checked
against the capacity before the run is trusted.

**`SymbolTable::find` is a linear scan over the whole 16 bit locate space and is
meant to be called once at startup.** Calling it per message took a 300,000
message replay from 0.25 seconds to 25 seconds, a hundredfold, which was a real
defect in `tickerplant_trade.cpp` and is fixed. The lookup that belongs on the
hot path is the locate itself, which is already an index.

**`hdr_record_corrected_value` is deliberately not used.** It costs one
iteration per expected interval inside the sample, and measuring against the
publisher's schedule already removes coordinated omission. Doing both double
counts the correction and creates a feedback loop that was plainly visible in
the numbers. The reasoning sits next to the sink in `tools/tickerplant_rx.cpp`.

**Building on Linux from a Mac needs one flag.** A container gives a real kernel,
so `recvmmsg`, `epoll`, `io_uring`, `sched_setaffinity` and the sanitizers all
work in one. `io_uring_queue_init` fails without
`--security-opt seccomp=unconfined`, because Docker's default seccomp profile
blocks the io_uring syscalls. What a container still cannot give is `isolcpus`,
since the guest's isolated core is a vCPU thread the host schedules, so an
isolated run inside a nested VM looks isolated and is not. That needs the real
box in `docs/LINUX_SETUP.md`.
