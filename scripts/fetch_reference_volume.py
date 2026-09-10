#!/usr/bin/env python3
"""Fetch an independent, externally published NASDAQ-exchange-only share volume reference.

Source is the Nasdaq Trader monthly "Market Share Statistics by Symbol" workbooks.
Those workbooks carry a per-symbol column named "NASDAQ Matched Volume", which is
the share volume matched on The Nasdaq Stock Market itself. That is the same
quantity a TotalView-ITCH 5.0 replay accumulates from the E, C, P and Q message
types, so it is a true external oracle for the replay rather than a proxy.

The one mismatch is time granularity. Nasdaq publishes these numbers per calendar
month, not per trading session. There is no free, no-login, per-symbol,
NASDAQ-exchange-only DAILY file for 2019-12-30 (see data/reference/README.md for
every source that was probed and what each one returned). So this script fetches
the month that contains the requested date and the companion reconcile script
compares a single replayed session against that month.

Two workbooks are needed to cover every symbol that trades on Nasdaq.
  NASDAQ<YYYYMM>.xlsx covers Nasdaq-listed issues (Tape C).
  Listed<YYYYMM>.xlsx covers issues listed on other markets (Tape A and Tape B)
  that still trade on Nasdaq. The ITCH feed carries both, so both are merged.

Standard library only. No API key, no login, no third-party package.
"""

from __future__ import annotations

import argparse
import datetime as dt
import io
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

BASE_URL = "https://www.nasdaqtrader.com/content/marketstatistics/marketshare"

# The two workbooks are structurally identical for the columns we read, but the
# header text differs in its run of embedded spaces and newlines between the two
# files, so headers are always normalised before matching.
WORKBOOK_PREFIXES = ("NASDAQ", "Listed")

SYMBOL_HEADER = "issue"
VOLUME_HEADER = "nasdaq matched volume"

# Each workbook ends with a footer row whose "Issue" cell reads TOTAL. It is not
# a ticker, so it is excluded from the data and used instead as a checksum on
# this parser. If our per-symbol sum does not equal the published total, the
# parse is wrong and the run aborts.
TOTAL_ROW_LABEL = "TOTAL"

SPREADSHEET_NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
RELS_NS = "{http://schemas.openxmlformats.org/package/2006/relationships}"
DOC_REL_NS = "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}"

USER_AGENT = "Tickerplant-reference-volume-fetch/1.0 (+ITCH replay verification)"


class FetchError(RuntimeError):
    """Raised for any condition that would otherwise yield a partial or wrong file."""


def normalise_header(text: str) -> str:
    """Collapse the embedded newlines and runs of spaces Nasdaq puts in headers."""
    # Non-breaking spaces appear in some of these workbooks, so map them first.
    text = text.replace(" ", " ")
    return re.sub(r"\s+", " ", text).strip().lower()


def column_index(cell_ref: str) -> int:
    """Turn a cell reference such as AB7 into a zero-based column index.

    Sheets omit empty cells entirely, so positions cannot be inferred from order.
    """
    letters = re.match(r"([A-Z]+)", cell_ref)
    if letters is None:
        raise FetchError("cell reference %r has no column letters" % cell_ref)
    index = 0
    for char in letters.group(1):
        index = index * 26 + (ord(char) - ord("A") + 1)
    return index - 1


def download(url: str, timeout: int, attempts: int = 3) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    last_error = None

    # nasdaqtrader.com intermittently drops connections. A few retries separate
    # a flaky link from a genuinely absent file, so the eventual error message
    # means what it says.
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                status = getattr(response, "status", response.getcode())
                payload = response.read()
            if status != 200:
                raise FetchError("HTTP %s fetching %s" % (status, url))
            break
        except urllib.error.HTTPError as exc:
            raise FetchError("HTTP %s fetching %s" % (exc.code, url)) from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc
            if attempt == attempts:
                raise FetchError(
                    "network failure fetching %s after %d attempts (%s)"
                    % (url, attempts, last_error)
                ) from exc
            print("  attempt %d failed (%s), retrying" % (attempt, exc))
            time.sleep(2 * attempt)

    # nasdaqtrader.com answers unknown paths with a 200 and an HTML "Page Not
    # Available" body, so a status check alone would let a soft 404 through.
    if not payload.startswith(b"PK"):
        raise FetchError(
            "%s did not return an xlsx workbook. First bytes were %r. "
            "This is usually the site's soft 404 HTML page, meaning the month "
            "requested is not published." % (url, payload[:40])
        )
    return payload


def first_sheet_path(archive: zipfile.ZipFile) -> str:
    """Resolve the first worksheet part instead of assuming xl/worksheets/sheet1.xml."""
    workbook = ET.fromstring(archive.read("xl/workbook.xml"))
    sheets = workbook.find(SPREADSHEET_NS + "sheets")
    if sheets is None or len(sheets) == 0:
        raise FetchError("workbook declares no sheets")
    rel_id = sheets[0].get(DOC_REL_NS + "id")
    if rel_id is None:
        raise FetchError("first sheet has no relationship id")

    rels = ET.fromstring(archive.read("xl/_rels/workbook.xml.rels"))
    for rel in rels.findall(RELS_NS + "Relationship"):
        if rel.get("Id") == rel_id:
            target = rel.get("Target", "")
            return "xl/" + target.lstrip("/")
    raise FetchError("could not resolve relationship %s to a worksheet part" % rel_id)


def shared_strings(archive: zipfile.ZipFile) -> list[str]:
    if "xl/sharedStrings.xml" not in archive.namelist():
        return []
    root = ET.fromstring(archive.read("xl/sharedStrings.xml"))
    values = []
    for si in root.findall(SPREADSHEET_NS + "si"):
        values.append("".join(t.text or "" for t in si.iter(SPREADSHEET_NS + "t")))
    return values


def cell_text(cell: ET.Element, strings: list[str]) -> str:
    kind = cell.get("t")
    if kind == "inlineStr":
        node = cell.find(SPREADSHEET_NS + "is")
        if node is None:
            return ""
        return "".join(t.text or "" for t in node.iter(SPREADSHEET_NS + "t"))
    value = cell.find(SPREADSHEET_NS + "v")
    if value is None or value.text is None:
        return ""
    if kind == "s":
        try:
            return strings[int(value.text)]
        except (ValueError, IndexError) as exc:
            raise FetchError("bad shared string index %r" % value.text) from exc
    return value.text


def parse_workbook(payload: bytes, url: str) -> tuple[dict[str, int], int | None]:
    """Return per-symbol NASDAQ matched volume plus the workbook's own TOTAL row."""
    try:
        archive = zipfile.ZipFile(io.BytesIO(payload))
    except zipfile.BadZipFile as exc:
        raise FetchError("%s is not a readable xlsx archive" % url) from exc

    strings = shared_strings(archive)
    sheet = ET.fromstring(archive.read(first_sheet_path(archive)))
    sheet_data = sheet.find(SPREADSHEET_NS + "sheetData")
    if sheet_data is None:
        raise FetchError("%s has no sheetData" % url)

    symbol_col = None
    volume_col = None
    volumes: dict[str, int] = {}
    declared_total: int | None = None

    for row in sheet_data.findall(SPREADSHEET_NS + "row"):
        cells: dict[int, str] = {}
        for cell in row.findall(SPREADSHEET_NS + "c"):
            ref = cell.get("r")
            if ref is None:
                continue
            cells[column_index(ref)] = cell_text(cell, strings)

        if symbol_col is None:
            # Locate the header row by name so a future column reorder cannot
            # silently shift which numbers we read.
            for index, text in cells.items():
                header = normalise_header(text)
                if header == SYMBOL_HEADER:
                    symbol_col = index
                elif header == VOLUME_HEADER:
                    volume_col = index
            if symbol_col is not None and volume_col is None:
                raise FetchError(
                    "%s has an 'Issue' column but no 'NASDAQ Matched Volume' column. "
                    "Headers seen were %r" % (url, sorted(cells.values()))
                )
            continue

        symbol = cells.get(symbol_col, "").strip().upper()
        raw = cells.get(volume_col, "").strip()
        if not symbol or not raw:
            continue

        try:
            # Excel may store these as floats such as 1.87005411E8.
            numeric = float(raw)
        except ValueError as exc:
            raise FetchError(
                "%s row for %s has a non-numeric volume %r" % (url, symbol, raw)
            ) from exc
        if numeric < 0 or numeric != int(numeric):
            raise FetchError(
                "%s row for %s has a volume that is not a whole number of shares "
                "(%r)" % (url, symbol, raw)
            )

        shares = int(numeric)

        if symbol == TOTAL_ROW_LABEL:
            if declared_total is not None:
                raise FetchError("%s has more than one TOTAL row" % url)
            declared_total = shares
            continue

        if symbol in volumes and volumes[symbol] != shares:
            raise FetchError(
                "%s lists %s twice with different volumes (%d and %d)"
                % (url, symbol, volumes[symbol], shares)
            )
        volumes[symbol] = shares

    if symbol_col is None:
        raise FetchError("%s has no header row containing an 'Issue' column" % url)
    if not volumes:
        raise FetchError("%s parsed cleanly but yielded zero symbols" % url)

    if declared_total is not None and declared_total != sum(volumes.values()):
        raise FetchError(
            "%s failed its own checksum. The workbook's TOTAL row says %d shares "
            "but the per-symbol rows sum to %d. Do not trust this parse."
            % (url, declared_total, sum(volumes.values()))
        )
    return volumes, declared_total


def write_csv_atomically(path: str, rows: list[tuple[str, int]]) -> None:
    """Write through a temp file so an interrupted run leaves no partial CSV."""
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    handle, temp_path = tempfile.mkstemp(dir=directory, suffix=".tmp")
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as out:
            out.write("symbol,shares\n")
            for symbol, shares in rows:
                out.write("%s,%d\n" % (symbol, shares))
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp_path, path)
    except BaseException:
        if os.path.exists(temp_path):
            os.unlink(temp_path)
        raise


def main(argv: list[str]) -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(
        description=(
            "Fetch per-symbol NASDAQ-exchange-only matched share volume from the "
            "Nasdaq Trader monthly market share workbooks."
        )
    )
    parser.add_argument(
        "--date",
        default="2019-12-30",
        help=(
            "Trading date the replay covers, as YYYY-MM-DD. The reference fetched "
            "is the calendar month containing this date. Default is 2019-12-30, "
            "the date of the sample ITCH file in data/."
        ),
    )
    parser.add_argument(
        "--out-dir",
        default=os.path.join(repo_root, "data", "reference"),
        help="Directory the reference CSV is written into.",
    )
    parser.add_argument("--timeout", type=int, default=120, help="Per-request timeout in seconds.")
    args = parser.parse_args(argv)

    try:
        date = dt.date.fromisoformat(args.date)
    except ValueError:
        print("ERROR bad --date %r, expected YYYY-MM-DD" % args.date, file=sys.stderr)
        return 2

    period = "%04d%02d" % (date.year, date.month)
    merged: dict[str, int] = {}
    used_urls = []
    transferred: list[str] = []

    print("Nasdaq Trader monthly market share by symbol")
    print("  replay date requested   %s" % date.isoformat())
    print("  reference period        %s (whole calendar month)" % period)
    print("  measure                 NASDAQ Matched Volume, Nasdaq exchange only")
    print("")

    for prefix in WORKBOOK_PREFIXES:
        url = "%s/%d/%s%s.xlsx" % (BASE_URL, date.year, prefix, period)
        print("fetching %s" % url)
        try:
            payload = download(url, args.timeout)
            volumes, declared_total = parse_workbook(payload, url)
        except FetchError as exc:
            # Loud failure. A missing workbook means the merged file would be
            # short by a whole tape, which is exactly the silent partial result
            # this script must never produce.
            print("ERROR %s" % exc, file=sys.stderr)
            return 1

        total = sum(volumes.values())
        checksum = "matches workbook TOTAL row" if declared_total == total else "no TOTAL row"
        print("  %d bytes, %d symbols, %d shares, %s" % (len(payload), len(volumes), total, checksum))
        used_urls.append(url)

        for symbol, shares in volumes.items():
            if symbol in merged:
                # A handful of issues change listing market mid-month, so each
                # workbook holds the part of the month during which that issue
                # belonged to its tape. The month's Nasdaq matched volume is the
                # sum of the two partial figures, not either one alone.
                merged[symbol] += shares
                transferred.append(symbol)
            else:
                merged[symbol] = shares

    rows = sorted(merged.items())
    out_path = os.path.join(args.out_dir, "nasdaq_matched_volume_%s.csv" % period)
    write_csv_atomically(out_path, rows)

    print("")
    print("wrote %s" % out_path)
    print("  symbols  %d" % len(rows))
    print("  shares   %d" % sum(shares for _, shares in rows))
    if transferred:
        print(
            "  note     %d symbol(s) appear in both workbooks and were summed "
            "because the issue changed listing market during the month, so its "
            "volume is split across the two files. %s"
            % (len(transferred), " ".join(sorted(set(transferred))))
        )
    print("")
    print("provenance, quote these URLs when asked where the number came from")
    for url in used_urls:
        print("  %s" % url)
    print("")
    print(
        "This is NASDAQ-exchange-only matched volume, the same thing an ITCH "
        "replay counts, but aggregated over the whole month rather than one "
        "session. It is not consolidated tape volume."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
