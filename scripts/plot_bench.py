#!/usr/bin/env python3
"""Render Tickerplant benchmark charts from a Google Benchmark JSON file.

The input is whatever bench_decode, bench_order_map and bench_book wrote with
--benchmark_out_format=json. The top level key benchmarks holds a list of rows.
Aggregate rows carry aggregate_name in the set mean, median, stddev, cv, and the
charts here use the median, because the numbers are taken on a machine that
cannot pin a thread to a core and the median is the only honest summary of a run
whose tail is operating system scheduling noise.

THE MACHINE LABEL IS NOT OPTIONAL. Every chart this script writes carries the
machine it was measured on, taken from the JSON context block, in its title or
its subtitle. A latency chart separated from its machine is not a result, and
the surest way to keep them together is to make it impossible to draw one
without the other. If the context block is missing the fields, the label says so
rather than guessing.

Charts written into the output directory.
    decode_handlers.png        zero-copy against copying, NullHandler and BookBuilder
    decode_by_message_type.png the same pair broken down by ITCH message type
    endian_loads.png           byteswap and unaligned load cost on its own
    order_map_shootout.png     the four order reference tables, mixed and erase heavy
    order_map_probes.png       average probe length for the flat map variants
    book_side_depth.png        tree against sorted vector as book depth grows
    book_builder.png           whole book throughput with each side container

The results file is produced by a separate benchmarking process and may be
missing or partial. That is handled with a clear message and a zero exit, so
this can be wired into a build before any results exist.
"""

import argparse
import json
import os
import sys
from collections import defaultdict

try:
    import matplotlib
except ImportError:
    sys.stderr.write(
        "matplotlib is not installed, so no charts can be drawn.\n"
        "  check:   python3 -c \"import matplotlib\"\n"
        "  install: python3 -m pip install matplotlib\n"
        "The benchmark JSON is unaffected and the charts can be made later.\n"
    )
    sys.exit(1)

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

try:
    import numpy as np
except ImportError:
    sys.stderr.write(
        "numpy is not installed, so no charts can be drawn.\n"
        "  check:   python3 -c \"import numpy\"\n"
        "  install: python3 -m pip install numpy\n"
    )
    sys.exit(1)


# The same palette NanoExchange uses, so the two projects' charts sit together
# in a portfolio without clashing. These read clearly in print and stay distinct
# for viewers with common colour vision deficiencies.
PALETTE = ["#2f6db5", "#e07b39", "#3f9d5a", "#b0413e", "#7d5ba6", "#4c4c4c"]
GRID_KW = dict(axis="y", color="#d9d9d9", linewidth=0.8, zorder=0)

UNIT_TO_NS = {"ns": 1.0, "us": 1000.0, "ms": 1000000.0, "s": 1000000000.0}


def apply_base_style():
    """Set a clean shared look for every figure."""
    plt.rcParams.update({
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "axes.edgecolor": "#666666",
        "axes.linewidth": 0.8,
        "axes.grid": False,
        "axes.titlesize": 13,
        "axes.titleweight": "bold",
        "axes.labelsize": 11,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "legend.frameon": False,
        "font.family": "DejaVu Sans",
    })


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_json(path):
    if not os.path.exists(path):
        print("Benchmark file not found at {}.".format(path))
        print("Run the benchmarks with --benchmark_out_format=json, then re run this.")
        return None
    try:
        with open(path, "r") as fh:
            return json.load(fh)
    except (ValueError, OSError) as exc:
        print("Could not read benchmark JSON at {}. Reason {}.".format(path, exc))
        return None


def machine_label(context):
    """One line naming the machine, built from the JSON context block.

    bench_common.hpp writes cpu_model, cores, os, compiler and pinned into the
    context with the same field names scripts/box_label.sh uses. Anything absent
    says unknown rather than being filled in with something plausible.
    """
    if not context:
        return "machine unknown: the results file carries no context block"

    cpu = context.get("cpu_model") or context.get("host_name") or "unknown CPU"
    cores = context.get("cores")
    if not cores:
        n = context.get("num_cpus")
        cores = str(n) if n else None
    os_name = context.get("os", "unknown OS")
    compiler = context.get("compiler", "unknown compiler")

    parts = [cpu]
    if cores:
        parts.append("{} cores".format(cores))
    parts.append(os_name)
    parts.append(compiler)
    label = ", ".join(parts)

    # The pinning state travels with the label, because it is the difference
    # between a tail number that means something and one that does not.
    pinned = str(context.get("pinned", "")).lower()
    if pinned == "true":
        label += " | pinned"
    else:
        label += " | UNPINNED, median of repeats, tail is scheduler noise"
    return label


def parse_rows(data):
    """Normalise the benchmark rows into records.

    Names look like family<TemplateArg>/arg/repeats:7_median. The aggregate
    suffix and the repeats segment are stripped, leaving a family and a tuple of
    real arguments.
    """
    raw = data.get("benchmarks")
    if not raw:
        print("No benchmarks list in the results file.")
        return None

    records = []
    for entry in raw:
        name = entry.get("name")
        if not name:
            continue
        aggregate = entry.get("aggregate_name")
        if aggregate is None:
            continue  # per iteration rows are not charted

        base = name
        for suffix in ("_mean", "_median", "_stddev", "_cv"):
            if base.endswith(suffix):
                base = base[: -len(suffix)]
                break

        parts = [p for p in base.split("/") if not p.startswith("repeats:")]
        family = parts[0] if parts else base
        args = tuple(parts[1:])

        unit = entry.get("time_unit", "ns")
        t = entry.get("real_time")
        if t is None:
            t = entry.get("cpu_time")
        time_ns = None if t is None else float(t) * UNIT_TO_NS.get(unit, 1.0)

        records.append({
            "family": family,
            "args": args,
            "aggregate": aggregate,
            "time_ns": time_ns,
            "items_per_second": entry.get("items_per_second"),
            "label": entry.get("label", ""),
            "counters": {k: v for k, v in entry.items()
                         if isinstance(v, (int, float)) and k not in
                         ("real_time", "cpu_time", "iterations", "repetitions",
                          "repetition_index", "threads", "items_per_second",
                          "bytes_per_second")},
        })
    if not records:
        print("The results file held no aggregate rows. Run with repetitions so a median exists.")
        return None
    return records


def index_cases(records):
    """Group records by family and argument tuple, keyed by aggregate name."""
    cases = defaultdict(dict)
    for rec in records:
        cases[(rec["family"], rec["args"])][rec["aggregate"]] = rec
    return cases


def median(case):
    rec = case.get("median") or case.get("mean")
    return rec


def throughput(case):
    """Items per second from the median row, falling back to the inverse time."""
    rec = median(case)
    if rec is None:
        return None
    if rec["items_per_second"]:
        return float(rec["items_per_second"])
    if rec["time_ns"]:
        return 1e9 / rec["time_ns"]
    return None


def stddev_items(case):
    rec = case.get("stddev")
    if rec is None or not rec.get("items_per_second"):
        return 0.0
    return float(rec["items_per_second"])


def save(fig, outdir, filename):
    path = os.path.join(outdir, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote {}".format(path))
    return True


def titled(fig, title, machine, y=1.0):
    """Put the title on, with the machine label directly underneath it."""
    fig.suptitle(title, y=y + 0.06, fontsize=13, fontweight="bold")
    fig.text(0.5, y, machine, ha="center", va="bottom", fontsize=8.5, color="#555555")


def short_template(arg):
    """tick::ZeroCopyDecoder inside a family name becomes ZeroCopy."""
    if "<" in arg and arg.endswith(">"):
        inner = arg[arg.index("<") + 1: -1]
    else:
        inner = arg
    inner = inner.split("::")[-1]
    for drop in ("Decoder", "Hash", "Side"):
        if inner.endswith(drop) and inner != drop:
            inner = inner[: -len(drop)]
    return inner


def family_base(family):
    return family.split("<")[0]


# ---------------------------------------------------------------------------
# Charts
# ---------------------------------------------------------------------------

def chart_decode_handlers(cases, outdir, machine):
    """Zero-copy against copying, once with NullHandler and once with a book.

    Both handlers appear because reporting only the NullHandler case would
    flatter the zero-copy path: a handler that ignores every field makes every
    load the zero-copy decoder would have done into dead code.
    """
    groups = [("decode_null", "NullHandler"), ("decode_book", "BookBuilder")]
    decoders, data = [], {}
    for (family, args), case in cases.items():
        base = family_base(family)
        if base not in ("decode_null", "decode_book"):
            continue
        dec = short_template(family)
        if dec not in decoders:
            decoders.append(dec)
        tp = throughput(case)
        if tp is not None:
            data[(base, dec)] = (tp, stddev_items(case))

    if not data:
        print("skip decode_handlers.png. no decode_null or decode_book rows.")
        return False

    decoders.sort()
    panels = [g for g in groups if any((g[0], d) in data for d in decoders)]

    # One subplot per handler, each with its own y axis. The NullHandler case
    # runs two orders of magnitude faster than the BookBuilder case, because a
    # handler that ignores every field leaves the decoder almost nothing to do,
    # and crushing both onto one axis would hide the comparison that matters.
    fig, axes = plt.subplots(1, len(panels), figsize=(4.6 * len(panels), 4.6))
    if len(panels) == 1:
        axes = [axes]
    x = np.arange(len(decoders))
    colors = [PALETTE[i % len(PALETTE)] for i in range(len(decoders))]
    for ax, (base, title) in zip(axes, panels):
        ax.set_axisbelow(True)
        ax.grid(**GRID_KW)
        vals = [data.get((base, d), (0.0, 0.0))[0] / 1e6 for d in decoders]
        errs = [data.get((base, d), (0.0, 0.0))[1] / 1e6 for d in decoders]
        ax.bar(x, vals, width=0.55, yerr=errs, capsize=4, color=colors, zorder=3,
               error_kw=dict(ecolor="#444444", lw=1.0))
        for xi, v, e in zip(x, vals, errs):
            ax.annotate("{:.1f}".format(v), (xi, v + e), ha="center", va="bottom",
                        fontsize=9, color="#333333")
        ax.set_xticks(x)
        ax.set_xticklabels(decoders)
        ax.set_title(title, fontsize=11)
        ax.margins(y=0.15)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
    axes[0].set_ylabel("messages per second (millions)")
    titled(fig, "ITCH decode throughput. higher is better. note the independent axes",
           machine, y=1.02)
    return save(fig, outdir, "decode_handlers.png")


def chart_decode_by_type(cases, outdir, machine):
    """Per message type breakdown, because the mix decides the average."""
    data, types, decoders = {}, [], []
    for (family, args), case in cases.items():
        base = family_base(family)
        if base not in ("decode_type_zerocopy", "decode_type_copying") or not args:
            continue
        dec = "zero-copy" if base.endswith("zerocopy") else "copying"
        msg_type = args[0]
        if msg_type not in types:
            types.append(msg_type)
        if dec not in decoders:
            decoders.append(dec)
        tp = throughput(case)
        if tp is not None:
            data[(msg_type, dec)] = (tp, stddev_items(case))

    if not data:
        print("skip decode_by_message_type.png. no per type rows.")
        return False

    types.sort()
    decoders.sort()
    fig, ax = plt.subplots(figsize=(max(6.0, len(types) * 1.5), 4.4))
    ax.set_axisbelow(True)
    ax.grid(**GRID_KW)
    x = np.arange(len(types))
    width = 0.8 / len(decoders)
    for i, dec in enumerate(decoders):
        vals = [data.get((t, dec), (0.0, 0.0))[0] / 1e6 for t in types]
        errs = [data.get((t, dec), (0.0, 0.0))[1] / 1e6 for t in types]
        ax.bar(x + i * width - 0.4 + width / 2, vals, width=width * 0.9, yerr=errs,
               capsize=3, color=PALETTE[i % len(PALETTE)], label=dec, zorder=3,
               error_kw=dict(ecolor="#444444", lw=1.0))
    ax.set_xticks(x)
    ax.set_xticklabels(types)
    ax.set_xlabel("ITCH message type")
    ax.set_ylabel("messages per second (millions)")
    ax.legend(title="decoder")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    titled(fig, "Decode throughput by message type. real messages, NullHandler",
           machine, y=1.0)
    return save(fig, outdir, "decode_by_message_type.png")


def chart_endian(cases, outdir, machine):
    """Byteswap and unaligned load cost, separated from dispatch."""
    items = []
    for (family, args), case in cases.items():
        base = family_base(family)
        if base not in ("endian_load", "endian_load_u48", "endian_header"):
            continue
        rec = median(case)
        if rec is None or rec["time_ns"] is None:
            continue
        ips = rec["items_per_second"]
        if not ips:
            continue
        ns_per_item = 1e9 / float(ips)
        if base == "endian_load":
            # The template argument comes through as the compiler spells it,
            # which is uint16_t on one toolchain and unsigned short on another.
            name = short_template(family).replace(" ", "")
            name = {"uint16_t": "16 bit", "unsignedshort": "16 bit",
                    "uint32_t": "32 bit", "unsignedint": "32 bit",
                    "uint64_t": "64 bit", "unsignedlong": "64 bit",
                    "unsignedlonglong": "64 bit"}.get(name, name)
        elif base == "endian_load_u48":
            name = "48 bit timestamp"
        else:
            name = "full header"
        items.append((name, ns_per_item))

    if not items:
        print("skip endian_loads.png. no endian rows.")
        return False

    items.sort(key=lambda it: it[1])
    fig, ax = plt.subplots(figsize=(max(5.0, len(items) * 1.3), 4.2))
    ax.set_axisbelow(True)
    ax.grid(**GRID_KW)
    x = np.arange(len(items))
    ax.bar(x, [i[1] for i in items], width=0.6,
           color=[PALETTE[i % len(PALETTE)] for i in range(len(items))], zorder=3)
    ax.set_xticks(x)
    ax.set_xticklabels([i[0] for i in items], rotation=15, ha="right")
    ax.set_ylabel("nanoseconds per load")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    titled(fig, "Big-endian load cost at real unaligned offsets", machine, y=1.0)
    return save(fig, outdir, "endian_loads.png")


def _order_map_series(cases):
    """Collect the four tables under each of the two workloads."""
    workloads = {"mixed": {}, "erase": {}}
    for (family, args), case in cases.items():
        base = family_base(family)
        tp = throughput(case)
        if tp is None:
            continue
        if base == "flat_mixed":
            workloads["mixed"]["flat, " + short_template(family).lower()] = (tp, stddev_items(case))
        elif base == "flat_erase_heavy":
            workloads["erase"]["flat, " + short_template(family).lower()] = (tp, stddev_items(case))
        elif base == "unordered_mixed" and args:
            key = "unordered, reserved" if args[0] == "reserved" else "unordered, plain"
            workloads["mixed"][key] = (tp, stddev_items(case))
        elif base == "unordered_erase_heavy" and args:
            key = "unordered, reserved" if args[0] == "reserved" else "unordered, plain"
            workloads["erase"][key] = (tp, stddev_items(case))
    return workloads


def chart_order_map(cases, outdir, machine):
    """The order reference table shootout, on the real operation trace."""
    workloads = _order_map_series(cases)
    present = [w for w in ("mixed", "erase") if workloads[w]]
    if not present:
        print("skip order_map_shootout.png. no order map rows.")
        return False

    titles = {"mixed": "real mixed trace\ninsert, lookup, erase",
              "erase": "erase heavy slice\ninsert and erase only"}
    names = sorted({n for w in present for n in workloads[w]})
    color_of = {n: PALETTE[i % len(PALETTE)] for i, n in enumerate(names)}

    fig, axes = plt.subplots(1, len(present), figsize=(4.8 * len(present), 4.6), sharey=True)
    if len(present) == 1:
        axes = [axes]
    x = np.arange(len(names))
    for ax, w in zip(axes, present):
        ax.set_axisbelow(True)
        ax.grid(**GRID_KW)
        vals = [workloads[w].get(n, (0.0, 0.0))[0] / 1e6 for n in names]
        errs = [workloads[w].get(n, (0.0, 0.0))[1] / 1e6 for n in names]
        ax.bar(x, vals, width=0.62, yerr=errs, capsize=4,
               color=[color_of[n] for n in names], zorder=3,
               error_kw=dict(ecolor="#444444", lw=1.0))
        ax.set_xticks(x)
        ax.set_xticklabels(names, rotation=20, ha="right")
        ax.set_title(titles[w], fontsize=11)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
    axes[0].set_ylabel("operations per second (millions)")
    titled(fig, "Order reference table. real ITCH reference numbers", machine, y=1.02)
    return save(fig, outdir, "order_map_shootout.png")


def chart_order_map_probes(cases, outdir, machine):
    """Average probe length for the flat map, which is why it wins or loses.

    probes_per_lookup counts probes beyond the first slot, so zero means every
    lookup hit on its first try.
    """
    items = []
    for (family, args), case in cases.items():
        if family_base(family) != "flat_mixed":
            continue
        rec = median(case)
        if rec is None:
            continue
        probes = rec["counters"].get("probes_per_lookup")
        load = rec["counters"].get("final_load_factor")
        if probes is None:
            continue
        items.append(("flat, " + short_template(family).lower(), float(probes),
                      float(load) if load is not None else None))

    if not items:
        print("skip order_map_probes.png. no probes_per_lookup counter present.")
        return False

    items.sort(key=lambda it: it[1])
    fig, ax = plt.subplots(figsize=(5.4, 4.2))
    ax.set_axisbelow(True)
    ax.grid(**GRID_KW)
    x = np.arange(len(items))
    bars = ax.bar(x, [i[1] for i in items], width=0.5,
                  color=[PALETTE[i % len(PALETTE)] for i in range(len(items))], zorder=3)
    for bar, it in zip(bars, items):
        note = "{:.3f}".format(it[1])
        if it[2] is not None:
            note += "\nload {:.2f}".format(it[2])
        ax.annotate(note, (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=9, color="#333333")
    ax.set_xticks(x)
    ax.set_xticklabels([i[0] for i in items])
    ax.set_ylabel("extra probes per lookup")
    ax.margins(y=0.2)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    titled(fig, "Linear probe length on real order references", machine, y=1.0)
    return save(fig, outdir, "order_map_probes.png")


def chart_book_side_depth(cases, outdir, machine):
    """Tree against sorted vector as the resting book gets deeper."""
    series = defaultdict(list)
    for (family, args), case in cases.items():
        if family_base(family) != "side_replay" or not args:
            continue
        try:
            depth = int(args[0])
        except ValueError:
            continue
        tp = throughput(case)
        if tp is None:
            continue
        series[short_template(family)].append((depth, tp))

    if not series:
        print("skip book_side_depth.png. no side_replay rows.")
        return False

    fig, ax = plt.subplots(figsize=(6.6, 4.4))
    ax.set_axisbelow(True)
    ax.grid(color="#e2e2e2", linewidth=0.8)
    for i, (name, pts) in enumerate(sorted(series.items())):
        pts.sort()
        ax.plot([p[0] for p in pts], [p[1] / 1e6 for p in pts], marker="o", markersize=5,
                linewidth=1.8, color=PALETTE[i % len(PALETTE)], label=name)
    ax.set_xlabel("prewarmed price levels per side")
    ax.set_ylabel("book events per second (millions)")
    ax.legend(title="container")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    titled(fig, "Book side container against real add and delete stream", machine, y=1.0)
    return save(fig, outdir, "book_side_depth.png")


def chart_book_builder(cases, outdir, machine):
    """Whole book throughput with each side container over the same stream."""
    items = []
    for (family, args), case in cases.items():
        if family_base(family) != "book_throughput":
            continue
        tp = throughput(case)
        if tp is None:
            continue
        items.append((short_template(family), tp, stddev_items(case)))

    if not items:
        print("skip book_builder.png. no book_throughput rows.")
        return False

    items.sort(key=lambda it: -it[1])
    fig, ax = plt.subplots(figsize=(5.2, 4.2))
    ax.set_axisbelow(True)
    ax.grid(**GRID_KW)
    x = np.arange(len(items))
    ax.bar(x, [i[1] / 1e6 for i in items], width=0.5,
           yerr=[i[2] / 1e6 for i in items], capsize=4,
           color=[PALETTE[i % len(PALETTE)] for i in range(len(items))], zorder=3,
           error_kw=dict(ecolor="#444444", lw=1.0))
    ax.set_xticks(x)
    ax.set_xticklabels([i[0] for i in items])
    ax.set_ylabel("messages per second (millions)")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    titled(fig, "Full book build from the real feed, by side container", machine, y=1.0)
    return save(fig, outdir, "book_builder.png")


# ---------------------------------------------------------------------------

def merge(paths):
    """Load one or more results files and merge their benchmark rows.

    Each benchmark binary writes its own JSON, and the charts here span all
    three, so several files are accepted. They are only merged when they agree
    on the machine. Charting rows measured on two different boxes on one axis
    would produce a picture that is wrong in a way no caption could fix, so a
    disagreement is refused rather than warned about.
    """
    merged, context, source = None, None, None
    for path in paths:
        data = load_json(path)
        if data is None:
            continue
        ctx = data.get("context") or {}
        if merged is None:
            merged, context, source = data, ctx, path
            continue
        for field in ("cpu_model", "os", "kernel", "compiler", "pinned"):
            if ctx.get(field) != context.get(field):
                print("refusing to merge {} with {}.".format(path, source))
                print("  {} differs: {!r} against {!r}".format(
                    field, ctx.get(field), context.get(field)))
                print("  results from two machines cannot share a chart.")
                return None
        merged["benchmarks"] = merged.get("benchmarks", []) + data.get("benchmarks", [])
    return merged


def run(input_paths, outdir):
    apply_base_style()
    data = merge(input_paths)
    if data is None:
        print("No charts were produced. This is expected when results are absent.")
        return 0

    records = parse_rows(data)
    if records is None:
        print("No charts were produced.")
        return 0

    machine = machine_label(data.get("context"))
    print("machine: {}".format(machine))

    os.makedirs(outdir, exist_ok=True)
    cases = index_cases(records)
    print("families found {}".format(sorted({family_base(f) for f, _ in cases})))

    written = 0
    written += chart_decode_handlers(cases, outdir, machine)
    written += chart_decode_by_type(cases, outdir, machine)
    written += chart_endian(cases, outdir, machine)
    written += chart_order_map(cases, outdir, machine)
    written += chart_order_map_probes(cases, outdir, machine)
    written += chart_book_side_depth(cases, outdir, machine)
    written += chart_book_builder(cases, outdir, machine)

    if written == 0:
        print("No matching benchmark families were found, so no charts were drawn.")
    else:
        print("done. {} chart file(s) written to {}.".format(written, outdir))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Plot Tickerplant benchmark charts from Google Benchmark JSON."
    )
    parser.add_argument("--input", nargs="+", default=["results/benchmarks.json"],
                        help="one or more Google Benchmark JSON results files. "
                             "Several are merged only when they agree on the machine.")
    parser.add_argument("--outdir", default="results",
                        help="directory to write PNG charts into")
    args = parser.parse_args(argv)
    return run(args.input, args.outdir)


if __name__ == "__main__":
    sys.exit(main())
