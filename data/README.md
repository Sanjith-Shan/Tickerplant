# Data

Nothing in this directory is committed. The sample files are gigabytes each.

## The real thing

NASDAQ publishes full day TotalView-ITCH 5.0 sample files at
`https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`. No login, no key.

```
./scripts/fetch_itch.sh              # 2019-12-30, 3.28 GiB compressed
./scripts/fetch_itch.sh 01302019     # another day, MMDDYYYY
```

The default is **2019-12-30**, chosen because it is the smallest full day on
the server. It is the Monday between Christmas and New Year, so it is a light
session, which the volume reconciliation notices and reports. Every number in
`results/RESULTS.md` that says "the real file" means this one.

Availability was checked 2026-09-19. If the directory moves or goes behind a
login, IEX publishes DEEP and TOPS as free pcaps, and the substitution belongs
in the README rather than being made quietly.

## The synthetic day

CI cannot download four gigabytes, and a test that reproduces bit for bit from a
seed is worth more than a sample of real data that might drift under it.

```
./build/bin/make-synthetic-itch --out /tmp/day.itch --messages 1000000 \
    --symbols 200 --seed 42 --gzip
```

The generator is its own oracle. It keeps a live order set so it never emits an
execution or a cancel against a reference it has not added, and it reports the
expected per-symbol executed volume, the expected count by message type, and the
expected number of orders still resting at the end. The tests check the book
against those rather than against themselves.

It uses `std::mt19937_64` explicitly rather than `std::default_random_engine`,
which is implementation defined, so the same seed produces the same bytes on
every platform.

## Reference volume

`scripts/fetch_reference_volume.py` downloads Nasdaq's published monthly market
share workbooks into `data/reference/`. That is the external oracle for the
volume reconciliation. `data/reference/README.md` says exactly what it covers
and, more importantly, what it does not.
