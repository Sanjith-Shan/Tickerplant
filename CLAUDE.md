# Tickerplant working notes

Read this before changing anything. It is the operating manual for the
repository, not a summary of it. The README is the summary and
`docs/DESIGN.md` is the reasoning.

## What this project is for

It is a portfolio artifact for trading-firm software engineering internship
applications. That is not a reason to cut corners, it is the reason not to.
Every claim in here will be read by someone who builds feed handlers for a
living, and the fastest way to lose that reader is one number they can poke a
hole in.

## The rules, in order of how badly breaking them hurts

**1. Never write a number that was not measured on the machine it says it was
measured on.** No estimates, no extrapolations, no "roughly". If a figure goes
into the README, `docs/DESIGN.md`, or a comment, there is a row for it in
`results/RESULTS.md` with the exact command that produced it and the machine
label. A number without provenance gets deleted rather than defended.

**2. Label the machine, every time.** CPU, kernel, compiler, whether pinning was
*achieved* rather than requested, and whether the run was on loopback.
`box_info.hpp` and `scripts/box_label.sh` exist so this is automatic. Anything
they cannot determine comes out as `unknown` and never as a plausible guess.

**3. Publish the results that are unflattering.** The 30% loss row where the
system goes stale, the identity hash losing to `std::unordered_map`, the
zero-copy decoder being 2.7% rather than 7.4x once a real handler is attached,
the standard pooling resource being slower than the heap. Those are in the
README and the design writeup on purpose. They are the reason the flattering
numbers are believable.

**4. macOS cannot pin a thread to a core.** Not weakly, not approximately. Do
not describe `THREAD_AFFINITY_POLICY` as pinning, do not describe
`THREAD_TIME_CONSTRAINT_POLICY` as realtime scheduling. Every macOS number says
"not pinned" and the pinned numbers come from the Linux box in
`docs/LINUX_SETUP.md`.

**5. Loopback multicast is not a network.** A local wire run measures the decode
and book path and proves the arbitration and recovery logic. It does not measure
a NIC, a driver, or a switch, and saying otherwise ends the conversation this
project exists to start.

**6. No kernel bypass claims.** DPDK, `ef_vi`, and Onload need hardware this
project does not have. Understanding them is free. A number from them would be a
fabrication.

## Style

Follow what is already there. The comments explain *why*, not *what*, and each
header opens with a block comment giving the design and the tradeoff. Read
`include/tick/endian.hpp` or `include/tick/flat_order_map.hpp` for the register.

No em dashes anywhere. Avoid semicolons and colons inside prose comments and
prose documentation, and rewrite the sentence instead. Colons in code, in tables
and in structured headings are fine.

## The three correctness oracles, and do not break them

Everything else leans on these, so a change that makes any of them stop running
is a change that has to be reverted.

1. **Two decoders.** `tickerplant-replay --decoder both` must exit zero. The
   zero-copy and copying paths have to produce the same book digest and the same
   volume digest over the same input.
2. **Replay determinism.** The same file gives the same digest on every run. CI
   gates on this.
3. **External volume reconciliation.** `scripts/reconcile_volume.py` against
   Nasdaq's published matched volume. It is session against month and the script
   says so in its own output every time it runs. Do not let anyone shorten that
   caveat.

And the fourth, which is not on that list only because it is a test rather than
a tool. `test_wire_integration.cpp` runs the whole pipeline in one process with
seeded loss and reordering and asserts the rebuilt book is identical to the
book built straight from the messages. If that test ever needs to be relaxed to
pass, something is wrong with the code and not with the test.

## Where things are

| Path | What |
| --- | --- |
| `include/tick/` | the entire library, header only |
| `tools/` | the five binaries |
| `test/` | one binary, `ctest --test-dir build` |
| `bench/` | Google Benchmark, real ITCH messages from the market open |
| `scripts/` | data fetching, the wire experiments, the machine label |
| `docs/DESIGN.md` | the reasoning. This is the document that gets read |
| `results/RESULTS.md` | every number with its command and its machine |
| `data/` | nothing committed. `./scripts/fetch_itch.sh` |

## Things that will bite a future session

**The order pool is a compile-time capacity.** `TICK_ORDER_POOL_CAPACITY`,
default four million, against a measured peak of 1,924,078 resting orders on the
real day. If a different day is used, check the peak that `tickerplant-replay`
reports before assuming it fits.

**`SymbolTable::find` is a linear scan over the whole 16 bit locate space.** It
is meant to be called once at startup. Calling it per message took a 300,000
message replay from 0.25 seconds to 25 seconds, which is a real bug that was in
`tickerplant_trade.cpp` and is fixed. Do not reintroduce it.

**The reorder window has to hold more messages than arrive during one recovery
round trip.** At 150,000 messages a second a 4,096 message window is 27
milliseconds of feed against a 50 millisecond recovery timeout, and it overflows
and goes stale. This is measured in `results/RESULTS.md` and it is the best
answer in the repository to "tell me about a bug you found".

**`hdr_record_corrected_value` costs one iteration per expected interval inside
the sample.** The receiver does not use it, and the reason is in the sink in
`tools/tickerplant_rx.cpp`. Do not put it back. Measuring against the
publisher's schedule already removes coordinated omission, and correcting on top
of that both double counts and creates a feedback loop that was clearly visible
in the numbers.

**AddressSanitizer and ThreadSanitizer do not run on macOS here.** Both runtimes
hang during their own initialisation on macOS 26.5, confirmed against a hello
world. Use UBSan with `-fno-sanitize-recover=all` and hardened libc++ locally.

**Use a Linux container for anything the Mac cannot do**, which is more than it
first appears. Docker Desktop is a real Linux kernel, so `recvmmsg`, `epoll`,
`io_uring`, `sched_setaffinity` and the sanitizers all work there:

```
docker run --rm --security-opt seccomp=unconfined -v "$PWD/..":/work \
    -w /work/Tickerplant tickbox:latest bash -c \
    'cmake -B build-linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux -j8'
```

`--security-opt seccomp=unconfined` is needed or `io_uring_queue_init` fails,
because Docker's default seccomp profile blocks the io_uring syscalls. What a
container still cannot give you is `isolcpus`, since the guest's isolated core
is still a vCPU thread that macOS schedules, so an isolated run inside a nested
VM would look isolated and not be. That one needs real hardware.

## What is still open

- Everything on a pinned, isolated core. `docs/LINUX_SETUP.md` is the recipe.
  The receive path shootout across `recv`, `recvmmsg`, `epoll`, busy poll and
  `io_uring` cannot be run on macOS at all.
- Two machines and a switch, so that "wire to book" means a wire.
- The NanoExchange retrofit, applying this measurement harness to the sibling
  project so its numbers can be republished pinned.
