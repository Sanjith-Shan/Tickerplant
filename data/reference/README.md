# Reference volume data

External, independently published share volume used to check the Tickerplant
order book rebuild. Nothing in this directory is produced by Tickerplant. Every
number here comes from Nasdaq and can be re-downloaded by anyone with the URLs
below, with no login and no API key.

## The one paragraph an interviewer should read

Nasdaq TotalView-ITCH carries executions that happen on The Nasdaq Stock Market
and nothing else. A share volume figure from a general finance data source is
consolidated tape volume, which adds up every US exchange plus every
off-exchange venue reporting through a FINRA TRF, and for a typical Nasdaq-listed
name it is several times larger than the Nasdaq-only figure. Comparing an ITCH
replay against consolidated volume would therefore be a check on order of
magnitude and not a reconciliation, because the two numbers are not supposed to
be equal. The reference in this directory avoids that problem. It is Nasdaq's own
published `NASDAQ Matched Volume` column, which is defined as volume matched on
the Nasdaq exchange, the same quantity the replay accumulates from the ITCH E, C
(printable only), P and Q message types. The remaining gap is time and not
definition. Nasdaq publishes this figure per calendar month rather than per
session, so the check is a per-symbol ratio of one replayed session to the month
that contains it, and the evidence is that those ratios are tightly clustered
across thousands of symbols rather than that any single number ties out exactly.

## What the file is

`nasdaq_matched_volume_201912.csv`, written by `scripts/fetch_reference_volume.py`.

Format is `symbol,shares`.

| Field | Meaning |
| --- | --- |
| `symbol` | Ticker as Nasdaq prints it in the workbook |
| `shares` | `NASDAQ Matched Volume` for the whole of December 2019 |

## Exactly where it came from

Downloaded from the Nasdaq Trader page "Market Share Statistics by Symbol"
(`https://www.nasdaqtrader.com/trader.aspx?ID=marketsharedaily`), which links two
workbooks per month.

- `https://www.nasdaqtrader.com/content/marketstatistics/marketshare/2019/NASDAQ201912.xlsx`
  HTTP 200, 643374 bytes, 3621 issues, 12921429246 shares.
  This is the "Nasdaq-Listed Securities" workbook, so Tape C.
- `https://www.nasdaqtrader.com/content/marketstatistics/marketshare/2019/Listed201912.xlsx`
  HTTP 200, 1027004 bytes, 3145 issues, 7181330702 shares.
  This is the "Non-Nasdaq Listed Securities" workbook. Its `Listing Market`
  column reads `NYSE` on every row, so it is Tape A only.

Merged total is 6760 symbols and 20102759948 shares.

Each workbook ends with a footer row whose `Issue` cell reads `TOTAL`. The fetch
script excludes that row from the data and uses it as a checksum on its own
parsing. Both workbooks matched their published total exactly on the run that
produced the file in this directory. If a future run does not match, the script
aborts rather than writing a file.

Six symbols appear in both workbooks because the issue changed listing market
during December 2019, so each workbook holds part of the month. Those are summed.
They are CNDT, NBL, NBLX, OAS, OMP and VG.

## What it covers

- Date range is the whole calendar month of December 2019.
- Venue is The Nasdaq Stock Market only. The same workbooks carry separate `BX
  Matched Volume` and `PSX Matched Volume` columns, which confirms that the
  column used here excludes Nasdaq's other two exchanges.
- Measure is matched volume, meaning volume that executed against the Nasdaq
  book. The workbooks report `NASDAQ TRF Volume`, `Routed Volume` and
  `Consolidated Volume` in separate columns, none of which are used here.

## What it does not cover

- **It is not a single day.** There is no per-symbol, Nasdaq-exchange-only daily
  volume file for 2019-12-30 available free and without a login. Every source
  probed is listed below. The comparison is therefore one session against one
  month.
- **It is not the whole symbol universe.** Nasdaq publishes a Tape C workbook and
  a Tape A workbook and no Tape B workbook. Issues listed on NYSE Arca, NYSE
  American and Cboe are absent, which removes most ETFs, including SPY, IWM, GDX
  and EEM, even though they trade on Nasdaq and appear in the ITCH feed.
  `scripts/reconcile_volume.py` prints how many replayed shares fall outside the
  reference for this reason, and that volume is verified by nothing.
- **It is not consolidated volume.** Anyone comparing these numbers to a figure
  from a retail finance site will find this one much smaller, and that is
  correct, not an error.

## Sources probed, with results

Recorded so the honest claim in the top-level README can be checked. All probes
were made directly rather than assumed.

Note that `nasdaqtrader.com` answers unknown paths with HTTP 200 and an HTML
"Page Not Available" body of roughly 43000 bytes, so a status code alone does not
indicate success. Those are marked as soft 404.

### Per-symbol, Nasdaq-exchange-only, for the single date 2019-12-30

| Source | Result |
| --- | --- |
| `https://www.nasdaqtrader.com/content/marketstatistics/marketshare/2019/NASDAQ201912.xlsx` | 200, valid xlsx. Per-symbol Nasdaq matched volume, but monthly. **Adopted.** |
| `https://www.nasdaqtrader.com/content/marketstatistics/marketshare/2019/Listed201912.xlsx` | 200, valid xlsx. Same, NYSE-listed issues. **Adopted.** |
| `https://www.nasdaqtrader.com/trader.aspx?ID=marketsharedaily` | 200. The page is titled "Daily and Historical by Symbol" but the daily calendar is commented out of the live HTML. Only monthly workbook links are active. |
| Same page archived at `http://web.archive.org/web/20200201155704/http://www.nasdaqtrader.com/trader.aspx?id=marketsharedaily` | 200. Even in February 2020 the daily calendar cells carry no links, so the daily series was already discontinued before then. |
| Same page archived at February 2016 | 200. Daily calendar cells did link, to `ftp://ftp.nasdaqtrader.com/files/marketshare/MMDDYY.csv`. This recovers the historical naming pattern. |
| `ftp://ftp.nasdaqtrader.com/files/MarketShare/123019.csv` | FTP 550, file does not exist. |
| `ftp://ftp.nasdaqtrader.com/files/MarketShare/` full listing | 200, 4122 entries. Two-digit years run 07 through 16 only. The daily per-symbol series stops in 2016, four years before the date needed. A sample from the era, `010215.csv`, has columns `RowNum,Symbol,Market,TradedShares,...` and would have been the ideal file had it still been published. |
| `https://www.nasdaqtrader.com/content/marketstatistics/marketshare/2019/Arca201912.xlsx`, and the same path with `AMEX`, `ETF`, `NYSEArca`, `TapeB`, `Other`, `NonNasdaq` | All soft 404. There is no Tape B workbook. |
| `https://emi.nasdaq.com/Web%20Based%20Reports/DSV/` | 200. Daily Share Volume archive, but it holds only six scattered sample dates (2010-05-14, 2015-03-19, 2016-03-01, 2018-06-15, 2020-05-06, 2024-04-25) plus six sample months. |
| `https://emi.nasdaq.com/Web%20Based%20Reports/DSV/DSV_20191230.zip` | 404. |
| `https://emi.nasdaq.com/Web%20Based%20Reports/DSV/DSVmonthly_201912.zip` | 404. |
| `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` | 200. Holds the raw ITCH files and their md5sums and nothing else. No summary or volume file ships alongside the data. |
| `https://emi.nasdaq.com/` and its sibling directories | 200. Directory listing enumerated. Nothing carrying per-symbol daily volume for 2019. |
| `https://emi.nasdaq.com/Web%20Based%20Reports/Nasdaq%20Short%20Sale%20Volume%20and%20Transaction%20Reports/NSDQshvol20220715.txt` | 200. Exactly the right shape, `DATE|SYMBOL|SHORT VOLUME|TOTAL VOLUME|MARKET` with `MARKET` equal to `Q`. That `TOTAL VOLUME` is Nasdaq-exchange-only per-symbol daily volume. Only this one sample date is published here. |
| `ftp://ftp.nasdaqtrader.com/files/shortsaledata/daily/nasdaq/` | 200, 260 entries, all 2009 and 2010. `NSDQshvol20191230.txt` returns FTP 550. |
| `ftp://ftp.nasdaqtrader.com/files/shortsaledata/monthly/nasdaq/` | Empty listing. |
| `https://www.nasdaqtrader.com/dynamic/symdir/shortsale/nasdaqshvol20191230.txt`, and the same file under `/Files/shortsale/` and as `NSDQshvol...` | Soft 404 in every spelling. |
| `https://www.nasdaqtrader.com/Trader.aspx?id=ShortSaleVolume` and four related page ids | Soft 404. The current Nasdaq short sale volume history is sold through Nasdaq Data Link as database `NSS`, which needs an account, so it is out of scope. |

### Nasdaq-exchange-only total for 2019-12-30, which would reconcile the aggregate

| Source | Result |
| --- | --- |
| `https://markets.cboe.com/us/equities/market_share/market/csv/?bias=Shares&auctions=1&oddLots=1&subdollars=1&s=2019-12-30` | 200 but the body is a header line and no rows. The same call for 2021-01-04, 2022-01-03, 2023-01-03, 2024-01-03 and 2026-09-16 returns full per-venue matched volume including a `NASDAQ (Q)` row. Probing backwards, 2020-01-02 also returns no rows, so Cboe's retention begins after the date needed. |
| `http://web.archive.org/cdx/search/cdx?url=markets.cboe.com/us/equities/market_share/market/2019-12-30*` | No captures. The dated Cboe page was never archived. |
| `https://www.nasdaqtrader.com/Trader.aspx?id=DailyMarketSummary` | 200, but it shows only the last five sessions and, in its own words, "represents volume from all trading venues on which Nasdaq Issues are traded". That is consolidated, not Nasdaq-only, so it would not have reconciled anything even if 2019 were still on the page. |
| `https://www.nasdaqtrader.com/dynamic/dailyfiles/daily2019.csv` | Soft 404. The 2026 equivalent exists but carries index-level totals, not per-symbol, and its volume column is consolidated. |

### Consolidated per-symbol volume, the labelled-as-weaker fallback

Not needed in the end, and both candidates failed anyway.

| Source | Result |
| --- | --- |
| `https://stooq.com/q/d/l/?s=aapl.us&d1=20191227&d2=20200102&i=d` | 200 but the body is a JavaScript proof-of-work browser challenge, not CSV. Confirmed identical through `curl` and through `urllib` with a browser user agent. Stooq no longer serves this without running the challenge. |
| `https://api.nasdaq.com/api/quote/AAPL/historical?assetclass=stocks&fromdate=2019-12-27&todate=2020-01-02` | 200 with `"totalRecords":0`. No 2019 retention. |

## Result of the check against the 2019-12-30 replay

Measured, not estimated. Reproduce with the commands below.

- Replay produced 8906 symbols and 971016019 executed shares for the session.
- 6103 symbols matched the reference by ticker. 2803 replay symbols are absent
  from the reference, carrying 149428878 shares, which is 15.39% of the session.
  Those are overwhelmingly the Tape B issues Nasdaq does not publish.
- 657 reference symbols are absent from the replay. Those traded on some other
  December session but not on 2019-12-30, and include names that were acquired or
  merged during the month.
- Over the 6078 comparable matched symbols the replay summed to 821587141 shares
  against 19946662236 for the month, an aggregate ratio of 0.041189. Inverted,
  that implies 24.28 sessions of this size in December 2019 against the 21
  sessions the month actually had, so the replayed session was about 86% of an
  average December session. That is the expected direction for the Monday between
  Christmas and New Year.
- Restricted to the 500 most active symbols, ranked by reference shares so the
  ranking is independent of the replay, the per-symbol ratio runs p10 0.020607,
  p50 0.032015, p90 0.057848, with an aggregate ratio of 0.042329 implying 23.62
  sessions. A band that tight across 500 independent symbols is the evidence
  worth quoting. A book that dropped, double counted or misattributed executions
  would not hold a consistent ratio across hundreds of unrelated names.

## Reproducing

```
python3 scripts/fetch_reference_volume.py --date 2019-12-30
python3 scripts/reconcile_volume.py \
    --replay /path/to/volume.csv \
    --reference data/reference/nasdaq_matched_volume_201912.csv \
    --reference-kind nasdaq-matched-monthly \
    --reference-sessions 21
```

`--reference-sessions` is optional and is only used to rescale the reported
ratios so an average session lands near 1.0. The value is not needed for the
comparison itself and nothing is inferred when it is omitted. December 2019 had
21 trading sessions, which is the count of weekdays in the month less Christmas
Day, and the reconcile script also prints the session count implied by the data
itself so the two can be compared.
