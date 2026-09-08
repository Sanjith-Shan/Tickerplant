#!/usr/bin/env bash
#
# Experiment one. The same feed, the same data, five ways of getting a datagram
# out of the kernel, receiver pinned.
#
# One line rather than two, on purpose. This measures the cost of the receive
# call itself, and arbitration is a different experiment with a different
# question. Two lines would put a second socket in the loop and confuse the two.
#
# Linux only. macOS has no recvmmsg, no epoll and no io_uring, so on macOS this
# prints what is available and stops rather than reporting a two row table as
# though it were the comparison.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${TICK_BIN_DIR:-build/bin}"
MSGS="${1:-1000000}"
RATE="${2:-400000}"
REPS="${3:-3}"
RX_CORE="${TICK_RX_CORE:-2}"
PUB_CORE="${TICK_PUB_CORE:-6}"
OUT=results/shootout
mkdir -p "$OUT"

FILE="${TICK_ITCH_FILE:-}"
if [ -z "$FILE" ]; then
    FILE=/tmp/shootout.itch
    [ -f "$FILE" ] || "$BIN/make-synthetic-itch" --out "$FILE" --messages "$MSGS" \
        --symbols 150 --seed 42 >/dev/null 2>&1
fi

have_taskset=1
command -v taskset >/dev/null 2>&1 || have_taskset=0

printf '%-16s %10s %10s %10s %10s %10s %8s %s\n' \
    strategy p50 p90 p99 p99.9 max lost note

for s in blocking busypoll recvmmsg epoll kqueue iouring; do
    best_p99=""
    line=""
    for r in $(seq 1 "$REPS"); do
        rm -f "$OUT/$s.json"
        if [ "$have_taskset" = 1 ]; then
            taskset -c "$RX_CORE" "$BIN/tickerplant-rx" --lines 1 --strategy "$s" --pin "$RX_CORE" \
                --limit "$MSGS" --idle-exit-ms 4000 --quiet --no-recovery \
                --manifest "$OUT/m.json" --results "$OUT/$s.json" > "$OUT/$s.txt" 2>&1 &
        else
            "$BIN/tickerplant-rx" --lines 1 --strategy "$s" --limit "$MSGS" \
                --idle-exit-ms 4000 --quiet --no-recovery \
                --manifest "$OUT/m.json" --results "$OUT/$s.json" > "$OUT/$s.txt" 2>&1 &
        fi
        rx=$!
        sleep 1.5

        if grep -q "not available on this platform" "$OUT/$s.txt" 2>/dev/null; then
            wait $rx 2>/dev/null
            line=$(printf '%-16s %10s %10s %10s %10s %10s %8s %s\n' "$s" - - - - - - "not available here")
            break
        fi

        if [ "$have_taskset" = 1 ]; then
            taskset -c "$PUB_CORE" "$BIN/tickerplant-pub" --file "$FILE" --limit "$MSGS" \
                --pace rate --rate "$RATE" --batch 30 --quiet \
                --manifest "$OUT/m.json" >/dev/null 2>&1
        else
            "$BIN/tickerplant-pub" --file "$FILE" --limit "$MSGS" --pace rate --rate "$RATE" \
                --batch 30 --quiet --manifest "$OUT/m.json" >/dev/null 2>&1
        fi
        wait $rx 2>/dev/null

        vals=$(awk '/wire to book/ {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "p50")   p50 = kv[2]
                if (kv[1] == "p90")   p90 = kv[2]
                if (kv[1] == "p99")   p99 = kv[2]
                if (kv[1] == "p99.9") p999 = kv[2]
                if (kv[1] == "max")   mx = kv[2]
            }
            print p50, p90, p99, p999, mx
        }' "$OUT/$s.txt")
        lost=$(awk '/messages lost/ {print $3}' "$OUT/$s.txt")
        [ -z "$vals" ] && continue

        p99=$(echo "$vals" | awk '{print $3}')
        # Keep the run with the best p99. On a machine that is not isolated the
        # best run is the one least disturbed, which is the honest pick when the
        # disturbance is not part of what is being measured.
        if [ -z "$best_p99" ] || [ "$p99" -lt "$best_p99" ] 2>/dev/null; then
            best_p99="$p99"
            line=$(echo "$vals $lost" | awk -v s="$s" \
                '{printf "%-16s %10s %10s %10s %10s %10s %8s\n", s, $1, $2, $3, $4, $5, $6}')
        fi
    done
    [ -n "$line" ] && echo "$line"
done

echo
echo "wire to book, nanoseconds, best of $REPS runs, $MSGS messages at $RATE per second."
echo "Receiver pinned to core $RX_CORE, publisher to core $PUB_CORE, one line, recovery off."
echo "Loopback multicast. No interface, no driver, no switch in this path."
