#!/usr/bin/env python3
"""Reconcile the Tickerplant replay's per-symbol executed volume against an external reference.

The replay CSV is `symbol,executed_shares`, produced by
`tickerplant-replay --volume-csv`. It is the sum of executed shares from the
ITCH 5.0 E, C (printable only), P and Q message types, which is NASDAQ-exchange
matched volume for the single session in the ITCH file.

The reference CSV is `symbol,shares`, produced by scripts/fetch_reference_volume.py.
What that reference actually measures depends on which source was available, so
--reference-kind is required and the banner printed on every run restates it.
Nothing here is allowed to imply a tighter proof than the reference supports.

Standard library only.
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

# Each kind carries the honest claim and the honest disclaimer that must appear
# in the banner every single run, so the limits of the check can never be lost
# by someone reading only the terminal output.
REFERENCE_KINDS = {
    "nasdaq-matched-monthly": {
        "label": "NASDAQ exchange matched volume, whole calendar month",
        "same_measure": True,
        "same_period": False,
        "proves": [
            "The reference counts exactly what the replay counts, which is share",
            "volume matched on The Nasdaq Stock Market itself. It is published by",
            "Nasdaq and was not derived from this codebase, so agreement is real",
            "external evidence and not self-consistency.",
            "If the book were wrong the per-symbol ratios would scatter. A tight",
            "distribution across thousands of symbols is the signal to read here.",
        ],
        "does_not_prove": [
            "This is NOT a day-for-day tie-out. The reference covers the whole",
            "calendar month and the replay covers one session, so the ratio is",
            "expected to be near one over the number of trading sessions in that",
            "month, not near one. No free per-symbol NASDAQ-only DAILY file for",
            "this date was found, so an exact daily reconciliation is not",
            "available from this source.",
            "Per-symbol ratios legitimately vary because a symbol's share of the",
            "month's volume is not uniform across sessions.",
            "The reference may not cover every symbol the feed carries. The",
            "coverage block below reports how much replayed volume sits outside",
            "the reference, and that volume is checked by nothing here.",
        ],
    },
    "nasdaq-matched-daily": {
        "label": "NASDAQ exchange matched volume, single trading session",
        "same_measure": True,
        "same_period": True,
        "proves": [
            "Same measure and same session, so this is a true reconciliation.",
            "Ratios should be 1.0 and any symbol that is not is a real defect in",
            "either the book or the reference.",
        ],
        "does_not_prove": [
            "Nasdaq restates some published figures, so a small number of",
            "mismatches may be the reference rather than the replay. Check any",
            "mismatch by hand before claiming the book is wrong.",
        ],
    },
    "consolidated-daily": {
        "label": "CONSOLIDATED volume across all US venues, single trading session",
        "same_measure": False,
        "same_period": True,
        "proves": [
            "Only that the replay's magnitude is plausible. The ratio printed",
            "below is an implied NASDAQ market share per symbol, nothing more.",
        ],
        "does_not_prove": [
            "THIS IS NOT A RECONCILIATION. The reference is consolidated tape",
            "volume summed over every US exchange and every off-exchange venue,",
            "while the replay counts only what matched on the Nasdaq exchange.",
            "The reference is therefore several times larger by construction and",
            "the two numbers are not supposed to be equal. Do not present any",
            "agreement here as proof that the order book is correct.",
        ],
    },
}

BANNER_WIDTH = 78


def percentile(sorted_values: list[float], fraction: float) -> float:
    """Linear interpolation percentile so no dependency on numpy is needed."""
    if not sorted_values:
        raise ValueError("no values")
    if len(sorted_values) == 1:
        return sorted_values[0]
    position = fraction * (len(sorted_values) - 1)
    low = int(position)
    high = min(low + 1, len(sorted_values) - 1)
    weight = position - low
    return sorted_values[low] * (1.0 - weight) + sorted_values[high] * weight


def load_csv(path: str, expected_header: tuple[str, str], what: str) -> dict[str, int]:
    if not os.path.exists(path):
        print("ERROR %s file not found at %s" % (what, path), file=sys.stderr)
        raise SystemExit(2)

    values: dict[str, int] = {}
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration:
            print("ERROR %s file %s is empty" % (what, path), file=sys.stderr)
            raise SystemExit(2)

        normalised = tuple(column.strip().lower() for column in header[:2])
        if normalised != expected_header:
            print(
                "ERROR %s file %s has header %r, expected %r"
                % (what, path, header[:2], list(expected_header)),
                file=sys.stderr,
            )
            raise SystemExit(2)

        for line_number, row in enumerate(reader, start=2):
            if not row or not row[0].strip():
                continue
            if len(row) < 2:
                print(
                    "ERROR %s file %s line %d has fewer than two fields"
                    % (what, path, line_number),
                    file=sys.stderr,
                )
                raise SystemExit(2)
            symbol = row[0].strip().upper()
            try:
                shares = int(row[1].strip())
            except ValueError:
                print(
                    "ERROR %s file %s line %d has a non-integer share count %r"
                    % (what, path, line_number, row[1]),
                    file=sys.stderr,
                )
                raise SystemExit(2)
            values[symbol] = values.get(symbol, 0) + shares

    if not values:
        print("ERROR %s file %s has a header but no rows" % (what, path), file=sys.stderr)
        raise SystemExit(2)
    return values


def rule(char: str = "=") -> str:
    return char * BANNER_WIDTH


def print_banner(kind: str, replay_path: str, reference_path: str, reference_url: str) -> None:
    info = REFERENCE_KINDS[kind]
    print(rule())
    print("Tickerplant volume reconciliation")
    print(rule())
    print("")
    print("WHAT IS BEING COMPARED")
    print("  replay      %s" % replay_path)
    print("              per-symbol executed shares from ITCH 5.0 E, C (printable),")
    print("              P and Q messages, which is NASDAQ exchange matched volume")
    print("              for the one session contained in the ITCH file")
    print("  reference   %s" % reference_path)
    print("              kind %s" % kind)
    print("              %s" % info["label"])
    if reference_url:
        print("              source %s" % reference_url)
    print("")

    if not info["same_measure"]:
        # The loudest possible statement, printed before any numbers, because a
        # consolidated reference is the one case where a reader could mistake
        # this output for a reconciliation.
        print(rule("!"))
        print("WARNING  THE REFERENCE IS CONSOLIDATED VOLUME, NOT NASDAQ-ONLY VOLUME.")
        print("WARNING  THIS RUN IS A SANITY CHECK ON ORDER OF MAGNITUDE.")
        print("WARNING  IT IS NOT A RECONCILIATION AND MUST NOT BE CALLED ONE.")
        print(rule("!"))
        print("")

    print("WHAT THIS PROVES")
    for line in info["proves"]:
        print("  %s" % line)
    print("")
    print("WHAT THIS DOES NOT PROVE")
    for line in info["does_not_prove"]:
        print("  %s" % line)
    print("")
    print(rule("-"))
    print("")


def main(argv: list[str]) -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(
        description="Compare replayed per-symbol executed volume against an external reference."
    )
    parser.add_argument("--replay", required=True, help="CSV of symbol,executed_shares.")
    parser.add_argument("--reference", required=True, help="CSV of symbol,shares.")
    parser.add_argument(
        "--reference-kind",
        required=True,
        choices=sorted(REFERENCE_KINDS),
        help="What the reference actually measures. This drives the banner and the maths.",
    )
    parser.add_argument(
        "--reference-url",
        default="",
        help="Source URL for the reference, echoed in the banner for provenance.",
    )
    parser.add_argument(
        "--reference-sessions",
        type=int,
        default=0,
        help=(
            "Number of trading sessions the reference spans. Only meaningful for a "
            "monthly reference. When given, ratios are also reported normalised by "
            "this count so a correct book centres them on 1.0."
        ),
    )
    parser.add_argument(
        "--show",
        type=int,
        default=10,
        help="How many extreme-ratio symbols to list at each tail.",
    )
    parser.add_argument(
        "--liquid",
        type=int,
        default=500,
        help=(
            "Also report the ratio distribution over this many most active symbols, "
            "ranked by reference shares. Set to 0 to skip."
        ),
    )
    args = parser.parse_args(argv)

    if args.reference_sessions < 0:
        print("ERROR --reference-sessions cannot be negative", file=sys.stderr)
        return 2

    replay = load_csv(args.replay, ("symbol", "executed_shares"), "replay")
    reference = load_csv(args.reference, ("symbol", "shares"), "reference")

    print_banner(args.reference_kind, os.path.abspath(args.replay),
                 os.path.abspath(args.reference), args.reference_url)

    replay_symbols = set(replay)
    reference_symbols = set(reference)
    both = replay_symbols & reference_symbols
    only_replay = replay_symbols - reference_symbols
    only_reference = reference_symbols - replay_symbols

    replay_grand_total = sum(replay.values())
    unmatched_replay_shares = sum(replay[symbol] for symbol in only_replay)
    unmatched_reference_shares = sum(reference[symbol] for symbol in only_reference)

    print("SYMBOL COVERAGE")
    print("  symbols in replay            %8d" % len(replay_symbols))
    print("  symbols in reference         %8d" % len(reference_symbols))
    print("  matched on both sides        %8d" % len(both))
    print("  only in replay               %8d" % len(only_replay))
    print("  only in reference            %8d" % len(only_reference))
    print("")
    # Coverage counted in symbols hides how much volume sits outside the
    # comparison, so the same split is reported in shares.
    print("  replay shares, all symbols   %20d" % replay_grand_total)
    print("  replay shares not in ref     %20d" % unmatched_replay_shares)
    if replay_grand_total:
        print("  that is                      %19.2f%% of the replayed session"
              % (100.0 * unmatched_replay_shares / replay_grand_total))
    print("  reference shares not in rep  %20d" % unmatched_reference_shares)
    print("")

    if only_replay:
        print("  sample only in replay        %s" % " ".join(sorted(only_replay)[: args.show]))
    if only_reference:
        print("  sample only in reference     %s" % " ".join(sorted(only_reference)[: args.show]))
    if only_replay or only_reference:
        print("")

    if not both:
        print("ERROR no symbol appears on both sides, so nothing can be compared.",
              file=sys.stderr)
        return 1

    exact = sum(1 for symbol in both if replay[symbol] == reference[symbol])
    zero_reference = sorted(symbol for symbol in both if reference[symbol] == 0)
    comparable = sorted(symbol for symbol in both if reference[symbol] > 0)

    replay_total = sum(replay[symbol] for symbol in comparable)
    reference_total = sum(reference[symbol] for symbol in comparable)

    print("TOTALS OVER THE %d COMPARABLE MATCHED SYMBOLS" % len(comparable))
    print("  replay shares                %20d" % replay_total)
    print("  reference shares             %20d" % reference_total)
    print("  aggregate ratio              %20.6f" % (replay_total / reference_total))
    print("  exact per-symbol equality    %8d of %d" % (exact, len(both)))
    if zero_reference:
        print("  reference is zero for        %8d symbol(s), excluded from ratios"
              % len(zero_reference))
    print("")

    ratios = sorted(replay[symbol] / reference[symbol] for symbol in comparable)
    points = [("p1", 0.01), ("p10", 0.10), ("p50", 0.50), ("p90", 0.90), ("p99", 0.99)]

    print("DISTRIBUTION OF replay_shares / reference_shares, PER SYMBOL")
    for name, fraction in points:
        print("  %-4s                         %20.6f" % (name, percentile(ratios, fraction)))
    print("  min                          %20.6f" % ratios[0])
    print("  max                          %20.6f" % ratios[-1])
    print("")

    # Thousands of barely traded names dominate the distribution above, and for
    # those a single session is a coarse, lumpy fraction of the month purely
    # because the share counts are tiny. Repeating the distribution over the
    # most active symbols shows whether the book agrees where there is enough
    # volume for the comparison to mean anything.
    if args.liquid > 0 and len(comparable) > args.liquid:
        liquid = sorted(comparable, key=lambda s: reference[s], reverse=True)[: args.liquid]
        liquid_ratios = sorted(replay[symbol] / reference[symbol] for symbol in liquid)
        liquid_replay = sum(replay[symbol] for symbol in liquid)
        liquid_reference = sum(reference[symbol] for symbol in liquid)
        print("SAME DISTRIBUTION, RESTRICTED TO THE %d MOST ACTIVE SYMBOLS" % args.liquid)
        print("  ranked by reference shares, which is independent of the replay")
        for name, fraction in points:
            print("  %-4s                         %20.6f"
                  % (name, percentile(liquid_ratios, fraction)))
        print("  aggregate ratio              %20.6f" % (liquid_replay / liquid_reference))
        if liquid_replay:
            print("  implied session count        %20.2f" % (liquid_reference / liquid_replay))
        print("")

    info = REFERENCE_KINDS[args.reference_kind]
    if not info["same_period"] and info["same_measure"]:
        median = percentile(ratios, 0.50)
        print("IMPLIED SESSION COUNT")
        print("  The reference spans a whole month. One over the ratio estimates how")
        print("  many sessions of this size the month contained. A replayed session")
        print("  that is typical for the month lands near the real session count.")
        print("  from aggregate ratio         %20.2f" % (reference_total / replay_total))
        if median > 0:
            print("  from median symbol ratio     %20.2f" % (1.0 / median))
        print("")

        if args.reference_sessions > 0:
            print("NORMALISED BY --reference-sessions %d" % args.reference_sessions)
            print("  A correct book on an average session centres these on 1.0.")
            normalised = [value * args.reference_sessions for value in ratios]
            for name, fraction in points:
                print("  %-4s                         %20.6f" % (name, percentile(normalised, fraction)))
            print("")

    def show_tail(title: str, symbols: list[str]) -> None:
        print(title)
        for symbol in symbols:
            print("  %-10s replay %14d   reference %14d   ratio %12.6f"
                  % (symbol, replay[symbol], reference[symbol],
                     replay[symbol] / reference[symbol]))
        print("")

    by_ratio = sorted(comparable, key=lambda s: replay[s] / reference[s])
    if args.show > 0:
        show_tail("LOWEST RATIOS, replay small relative to reference", by_ratio[: args.show])
        show_tail("HIGHEST RATIOS, replay large relative to reference", by_ratio[-args.show :][::-1])

    print(rule("-"))
    if not info["same_measure"]:
        print("Reminder. The reference was consolidated volume. Nothing above is a")
        print("reconciliation of the order book.")
    else:
        print("Reference measure matches the replay measure, which is NASDAQ exchange")
        print("matched volume. Read the period caveat above before quoting any number.")
    print(rule("-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
