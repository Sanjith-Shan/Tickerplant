#!/usr/bin/env bash
#
# Every wire experiment in the README, in the order they are reported there.
#
# Run it from the repository root after a Release build. It writes one JSON per
# run into results/wire/ and prints a summary table. Each run is a publisher and
# a receiver as separate processes over loopback multicast, so it works on a
# laptop, and what it proves on a laptop is correctness rather than latency.
# See the honesty notes at the bottom of this file and in the README.
#
#   ./scripts/run_wire_experiments.sh [messages] [rate]
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MESSAGES="${1:-1000000}"
RATE="${2:-150000}"
FILE="${TICK_ITCH_FILE:-data/12302019.NASDAQ_ITCH50.gz}"
BIN="${TICK_BIN_DIR:-build/bin}"
OUT="results/wire"
WINDOW="${TICK_REORDER_WINDOW:-65536}"

PUB="$BIN/tickerplant-pub"
RX="$BIN/tickerplant-rx"
REPLAY="$BIN/tickerplant-replay"

for b in "$PUB" "$RX" "$REPLAY"; do
    if [ ! -x "$b" ]; then
        echo "missing $b. Build first with:"
        echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
        exit 1
    fi
done

if [ ! -f "$FILE" ]; then
    echo "missing $FILE. Fetch it with ./scripts/fetch_itch.sh, or point"
    echo "TICK_ITCH_FILE at a synthetic file made by make-synthetic-itch."
    exit 1
fi

mkdir -p "$OUT"

echo "Tickerplant wire experiments"
echo "  file      $FILE"
echo "  messages  $MESSAGES at $RATE msg/s"
echo "  window    $WINDOW"
echo

# The reference. Everything else is compared against the book this produces,
# because a book built from a file has no network in it to be wrong.
echo "Building the reference book from the file"
REF_DIGEST=$("$REPLAY" --file "$FILE" --limit "$MESSAGES" --digest --quiet 2>/dev/null \
             | awk '/book digest/ {print $3}')
echo "  reference digest $REF_DIGEST"
echo

# label drop_a drop_b skew_us extra_rx_args...
run_case() {
    local label="$1" drop_a="$2" drop_b="$3" skew="$4"
    shift 4

    rm -f "$OUT/$label.rx.json"
    "$RX" --lines 2 --strategy "${TICK_STRATEGY:-busypoll}" --limit "$MESSAGES" \
          --idle-exit-ms 5000 --quiet --reorder-window "$WINDOW" \
          --manifest "$OUT/manifest.json" --results "$OUT/$label.rx.json" \
          "$@" > "$OUT/$label.rx.txt" 2>&1 &
    local rx_pid=$!

    # The receiver has to have joined the groups before the first packet goes
    # out. A multicast join that lands late loses everything sent before it.
    sleep 1.5

    "$PUB" --file "$FILE" --limit "$MESSAGES" --pace rate --rate "$RATE" --batch 30 \
           --drop-a "$drop_a" --drop-b "$drop_b" --skew-b-us "$skew" --quiet \
           --manifest "$OUT/manifest.json" > "$OUT/$label.pub.txt" 2>&1

    wait $rx_pid
    local rx_rc=$?

    local digest state lost recovered detected from_rec wins_a wins_b p50 p99
    digest=$(awk '/book digest/ {print $3}' "$OUT/$label.rx.txt")
    state=$(awk '/state  / {print $2}' "$OUT/$label.rx.txt" | head -1)
    detected=$(awk '/gaps detected/ {print $3}' "$OUT/$label.rx.txt")
    recovered=$(awk '/gaps recovered/ {print $3}' "$OUT/$label.rx.txt")
    lost=$(awk '/messages lost/ {print $3}' "$OUT/$label.rx.txt")
    from_rec=$(awk '/from recovery/ {print $3}' "$OUT/$label.rx.txt")
    wins_a=$(awk '/first on A/ {print $4}' "$OUT/$label.rx.txt")
    wins_b=$(awk '/first on B/ {print $4}' "$OUT/$label.rx.txt")

    local verdict="BOOK DIFFERS"
    if [ "$digest" = "$REF_DIGEST" ]; then verdict="bit identical"; fi

    printf '%-14s loss %5s/%5s skew %6sus  gaps %5s recovered %5s lost %7s  A %-9s B %-9s  %s\n' \
        "$label" "$drop_a" "$drop_b" "$skew" "$detected" "$recovered" "$lost" \
        "$wins_a" "$wins_b" "$verdict"

    if [ "$rx_rc" -eq 3 ]; then
        printf '%-14s   ended in the stale state, which is the receiver saying the book\n' ""
        printf '%-14s   cannot be trusted. That is the intended behaviour, not a crash.\n' ""
    fi
}

echo "1. Clean run, no injected failures"
run_case clean 0 0 0

echo
echo "2. A/B arbitration. Independent loss on each line, no line skew."
echo "   Arbitration alone should cover loss that does not hit both lines."
run_case loss_1pct  0.01 0.01 0
run_case loss_5pct  0.05 0.05 0
run_case loss_10pct 0.10 0.10 0
run_case loss_20pct 0.20 0.20 0
run_case loss_30pct 0.30 0.30 0

echo
echo "3. The same loss with recovery switched off, which is the control."
echo "   Whatever arbitration cannot cover is simply gone."
run_case loss_10pct_norecovery 0.10 0.10 0 --no-recovery

echo
echo "4. Line skew. B is delayed, so A should win almost every sequence."
run_case skew_0us   0 0 0
run_case skew_50us  0 0 50
run_case skew_500us 0 0 500

echo
echo "5. Loss on one line only, which is what a single failing path looks like."
run_case loss_a_only 0.10 0 0
run_case loss_b_only 0 0.10 0

echo
echo "Reference digest was $REF_DIGEST"
echo
cat <<'NOTE'
What these runs prove, and what they do not.

They prove the sequencing, the A/B arbitration, the gap state machine, and the
retransmission path are correct, because the rebuilt book is compared against a
book built from the same file with no network involved, and the comparison is a
fingerprint of every price level of every symbol rather than a spot check.

They do not prove anything about a network. This is loopback multicast between
two processes on one machine. There is no interface, no driver, and no switch in
the path. The latency figures in these files are the decode and book path plus
whatever the local kernel does with a multicast datagram, and on macOS that last
part is measured in milliseconds and swamps everything else.
NOTE
