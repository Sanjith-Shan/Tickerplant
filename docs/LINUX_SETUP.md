# The benchmark box

Every latency figure in this repository today was taken on an Apple M3 Pro
laptop, unpinned, over loopback multicast. Those runs prove the pipeline is
correct. They do not produce a latency number worth putting in front of a
trading firm, and the reason is not modesty. macOS cannot pin a thread to a
core at all, it has no `recvmmsg`, no `epoll`, no `io_uring`, no
`SO_TIMESTAMPING` at nanosecond resolution, no `isolcpus`, and no scaling
governor to read, and its loopback multicast path was measured here at about
3.4 milliseconds, which swamps everything the code does.

This file is what to do about that. It is written so the run can be reproduced
by someone who is not the author, which is the only useful standard for a
methodology note.

## The machine

A cheap dedicated or bare metal instance is enough. Hetzner, OVH, or an EC2
`c6i` or `c7i`.

**Do not use a shared vCPU instance.** A noisy neighbour is precisely the thing
being measured out, and a shared vCPU tail is a measurement of somebody else's
workload.

Record the CPU model, the kernel version, the NIC, and the microcode revision.
`scripts/box_label.sh` emits all of it as JSON and the benchmark binaries embed
it in their results, so a number cannot get separated from the machine it came
from.

## Boot parameters

Add to the kernel command line, then reboot.

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 intel_pstate=disable idle=poll
```

What each one does, because a results table that lists these without explaining
them is quoting an incantation.

| | |
|---|---|
| `isolcpus=2,3` | keeps the scheduler from putting anything else on cores 2 and 3 |
| `nohz_full=2,3` | stops the periodic timer tick on those cores when one task is runnable, which removes a microsecond scale interruption at up to 1000 Hz |
| `rcu_nocbs=2,3` | moves RCU callback processing off those cores, otherwise `nohz_full` does not get you a quiet core |
| `intel_pstate=disable` | stops the frequency changing underneath the measurement, which matters for the cycle counter calibration as much as for the timings |
| `idle=poll` | keeps the core out of deep C states, so a packet arriving does not pay an exit latency |

On AMD use `amd_pstate=disable` instead.

Verify after the reboot rather than trusting it.

```
cat /proc/cmdline
cat /sys/devices/system/cpu/isolated          # should print 2-3
cat /sys/devices/system/cpu/nohz_full         # should print 2-3
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
./scripts/box_label.sh
```

`box_label.sh` reads all of this and prints `unknown` for anything it cannot
determine rather than a plausible guess.

## Before a run

```
# frequency scaling off, where the driver still allows it
sudo cpupower frequency-set -g performance

# turbo off, so the clock does not change between the warmup and the run
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# hyperthreading off. A sibling thread sharing the core is another workload
echo off | sudo tee /sys/devices/system/cpu/smt/control

# a bigger socket receive buffer, because the default will drop a real feed
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.rmem_default=134217728
sudo sysctl -w net.core.netdev_max_backlog=250000

# stop the irqbalance daemon moving interrupts onto the isolated cores
sudo systemctl stop irqbalance
```

Every one of those changes the number. Whichever ones were applied belong in the
results file next to the number, which is what `box_label.sh` is for.

## Running it

Pin the receiver to an isolated core and the publisher well away from it.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# the receive path shootout, one strategy per run, receiver pinned to core 2
for s in blocking busypoll recvmmsg epoll iouring; do
  taskset -c 2 ./build/bin/tickerplant-rx --lines 1 --strategy $s --pin 2 \
      --limit 5000000 --results results/linux/rx_$s.json &
  sleep 1
  taskset -c 6 ./build/bin/tickerplant-pub --file data/12302019.NASDAQ_ITCH50.gz \
      --limit 5000000 --pace rate --rate 500000 --quiet
  wait
done
```

The shootout uses one line rather than two, because it is measuring the cost of
getting a datagram out of the kernel and arbitration is a different experiment.
The two line runs are `scripts/run_wire_experiments.sh`.

`io_uring` needs liburing at configure time. Without it the strategy reports
itself unavailable and the results table says so rather than leaving a gap that
looks like a zero.

## The pinning experiment

This is the one that exists specifically to repair the weakest claim in the
portfolio, which is the sibling project's unpinned p99. Run the identical
benchmark four ways and publish all four.

| Run | `taskset` | Core |
|---|---|---|
| unpinned, shared core | none | any |
| pinned, shared core | `taskset -c 1` | a normal core |
| pinned, isolated core | `taskset -c 2` | an `isolcpus` core |
| pinned, isolated, `nohz_full` | `taskset -c 2` | the same core with the tick off |

The interesting output is not the best row. It is the difference between the
first and the last, because that difference is how much of an unpinned p99 was
operating system scheduling noise rather than the program. Being able to say
that with your own numbers is worth more than the best row on its own.

## Then go back and do NanoExchange

The last step is to retrofit the same harness to the sibling project and
republish its numbers pinned, so the "not pinned to an isolated core" caveat
comes off honestly rather than by being quietly dropped.

## What still will not be proved

Even on this box, a loopback run has no NIC, no driver, and no switch in it.
Two machines and a switch is the next step after this one, and until that
happens the results say "loopback" in the table rather than in a footnote.
