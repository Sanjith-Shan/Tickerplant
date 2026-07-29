#!/usr/bin/env bash
# Download one real NASDAQ TotalView-ITCH 5.0 sample day from NASDAQ's public
# server. The files are large, between three and five gigabytes compressed, so
# the download resumes if it is interrupted.
#
#   ./scripts/fetch_itch.sh                  # default day, 2019-12-30
#   ./scripts/fetch_itch.sh 01302019         # another day, MMDDYYYY
#
# Availability was last checked 2026-09-19. If the directory has moved or gone
# behind a login, fall back to IEX DEEP/TOPS pcaps and say so in the README.
set -euo pipefail

DAY="${1:-12302019}"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="${DAY}.NASDAQ_ITCH50.gz"
DEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data"
DEST="${DEST_DIR}/${FILE}"

mkdir -p "${DEST_DIR}"

echo "Fetching ${FILE} into ${DEST_DIR}"
echo "This is a multi-gigabyte download. It resumes with -C - if it is cut off."
curl -fL --retry 10 --retry-delay 5 --retry-all-errors -C - -o "${DEST}" "${BASE}/${FILE}"

echo "Downloaded $(du -h "${DEST}" | cut -f1) to ${DEST}"
echo "Checking the gzip stream is intact"
gzip -t "${DEST}" && echo "gzip integrity OK"
