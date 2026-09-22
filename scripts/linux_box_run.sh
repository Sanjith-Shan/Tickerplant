#!/usr/bin/env bash
#
# The second half of the benchmark box. Run it after the reboot that
# scripts/linux_box_setup.sh performs.
#
# It verifies the isolation rather than trusting it, applies the per run
# tunables, builds, and then runs the two experiments that macOS cannot run at
# all. Everything it writes lands under results/linux/ next to the machine label
# that produced it.
#
#   ./scripts/linux_box_run.sh                  # 1M messages at 400k/s, 3 reps
#   ./scripts/linux_box_run.sh 5000000 500000 3
#
# Verification first, and it stops rather than continuing, because a shootout
# table taken on a box whose isolation did not take is a table of the scheduler.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MSGS="${1:-1000000}"
RATE="${2:-400000}"
REPS="${3:-3}"
RX_CORE="${TICK_RX_CORE:-2}"
PUB_CORE="${TICK_PUB_CORE:-6}"
SHARED_CORE="${TICK_SHARED_CORE:-1}"
OUT=results/linux
mkdir -p "$OUT"

[[ "$(uname -s)" == "Linux" ]] || { echo "this is the Linux half, run it on the box" >&2; exit 1; }

say() { printf '\n== %s\n' "$*"; }

say "verifying the isolation actually took"
ISOLATED="$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '')"
NOHZ="$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '')"
printf '  cmdline    %s\n' "$(cat /proc/cmdline)"
printf '  isolated   %s\n' "${ISOLATED:-none}"
printf '  nohz_full  %s\n' "${NOHZ:-none}"
if [[ -z "$ISOLATED" || "$ISOLATED" == "none" ]]; then
    echo "  no isolated cores. The boot line did not take, so stopping here." >&2
    echo "  Check /proc/cmdline against what linux_box_setup.sh wrote." >&2
    exit 1
fi

say "applying the per run tunables"
# Each of these changes the number, so box_label.sh records which of them held.
SUDO=""
[[ $EUID -eq 0 ]] || SUDO=sudo
$SUDO cpupower frequency-set -g performance >/dev/null 2>&1 || echo "  governor unchanged, the driver would not allow it"

# Turbo lives in two different places. Intel exposes no_turbo, and everything on
# acpi-cpufreq, which is where an AMD box lands once amd_pstate is disabled,
# exposes boost with the opposite polarity. Try both and say which one took.
if [[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    echo 1 | $SUDO tee /sys/devices/system/cpu/intel_pstate/no_turbo >/dev/null && echo "  turbo off via intel_pstate"
elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo 0 | $SUDO tee /sys/devices/system/cpu/cpufreq/boost >/dev/null && echo "  boost off via cpufreq"
else
    echo "  turbo unchanged, neither no_turbo nor boost is present"
fi

echo off | $SUDO tee /sys/devices/system/cpu/smt/control >/dev/null 2>&1 \
    && echo "  SMT off" || echo "  SMT unchanged, no sysfs control on this box"
$SUDO sysctl -q -w net.core.rmem_max=134217728
$SUDO sysctl -q -w net.core.rmem_default=134217728
$SUDO sysctl -q -w net.core.netdev_max_backlog=250000

say "the machine, as it will be labelled"
./scripts/box_label.sh --pretty | tee "$OUT/box.json"

say "building"
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release -G Ninja >/dev/null
cmake --build build-linux -j"$(nproc)" >/dev/null
export TICK_BIN_DIR=build-linux/bin

# The oracles gate the timings rather than decorating them. A failure here stops
# the run, loudly, because a latency table taken from a build whose tests do not
# pass is a table of something that does not work. Under set -e a failing ctest
# would end the script anyway, so say why rather than exiting in silence.
say "the correctness oracles, before any timing"
if ctest --test-dir build-linux --output-on-failure -j"$(nproc)" > "$OUT/ctest.txt" 2>&1; then
    tail -3 "$OUT/ctest.txt"
else
    echo "  tests failed, so stopping before any number is produced"
    grep -E "FAILED|Failed" "$OUT/ctest.txt" | head -20
    echo "  full output in $OUT/ctest.txt"
    exit 1
fi
FEED=/tmp/linux_run.itch
[[ -f "$FEED" ]] || "$TICK_BIN_DIR/make-synthetic-itch" --out "$FEED" \
    --messages "$MSGS" --symbols 150 --seed 42 >/dev/null
"$TICK_BIN_DIR/tickerplant-replay" --file "$FEED" --decoder both | tail -3

say "experiment one, the receive path shootout, receiver on core $RX_CORE"
TICK_RX_CORE="$RX_CORE" TICK_PUB_CORE="$PUB_CORE" TICK_BIN_DIR="$TICK_BIN_DIR" \
    ./scripts/receive_shootout.sh "$MSGS" "$RATE" "$REPS" | tee "$OUT/shootout.txt"

# Experiment two. The same run four ways, and the row that matters is the
# difference between the first and the last rather than the best one.
say "experiment two, the same benchmark four ways"
run_one() {
    local label="$1" pin="$2" taskset_arg="$3"
    rm -f "$OUT/pin_$label.json"
    if [[ -n "$taskset_arg" ]]; then
        taskset -c "$taskset_arg" "$TICK_BIN_DIR/tickerplant-rx" --lines 1 --strategy recvmmsg \
            --pin "$pin" --limit "$MSGS" --idle-exit-ms 4000 --quiet --no-recovery \
            --results "$OUT/pin_$label.json" > "$OUT/pin_$label.txt" 2>&1 &
    else
        "$TICK_BIN_DIR/tickerplant-rx" --lines 1 --strategy recvmmsg \
            --limit "$MSGS" --idle-exit-ms 4000 --quiet --no-recovery \
            --results "$OUT/pin_$label.json" > "$OUT/pin_$label.txt" 2>&1 &
    fi
    local rx=$!
    sleep 1.5
    taskset -c "$PUB_CORE" "$TICK_BIN_DIR/tickerplant-pub" --file "$FEED" \
        --pace rate --rate "$RATE" --limit "$MSGS" --quiet >/dev/null 2>&1 || true
    wait $rx 2>/dev/null || true
    printf '  %-28s %s\n' "$label" "$(grep -E 'p50|p99' "$OUT/pin_$label.txt" | tr '\n' ' ' || echo 'see the json')"
}

run_one "unpinned-shared-core"   "-1"       ""
run_one "pinned-shared-core"     "$SHARED_CORE" "$SHARED_CORE"
run_one "pinned-isolated-core"   "$RX_CORE" "$RX_CORE"

cat <<EOF

== done

Everything is under $OUT next to box.json, which is the label that says which
of the tunables above actually held on this machine.

The fourth row of experiment two, pinned and isolated with nohz_full, is the
pinned-isolated-core run on this box, because the boot line put nohz_full on
the same cores as isolcpus. Say so in the results table rather than listing it
as a separate condition that was not separately varied.

Next, copy the numbers into results/RESULTS.md with the command that produced
them and the box label, which is rule one in CLAUDE.md.
EOF
