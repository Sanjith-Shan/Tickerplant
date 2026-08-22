#!/usr/bin/env bash
# Print this machine's identity as a single JSON object, for a benchmark runner
# to embed at the top of a results file.
#
# This is the shell twin of tick::detect_box in include/tick/box_info.hpp and it
# reports the same fields with the same names, so a results file written by the
# runner and one written by a C++ benchmark can be compared field by field. If
# one of them is changed, change the other.
#
# The rule here is the same rule as in the header. Anything that cannot be read
# prints "unknown". Never a plausible default. A governor field that says
# "performance" on a box where the file was missing is a fabricated experimental
# condition, and a latency table resting on a fabricated condition is worse than
# no table.
#
#   ./scripts/box_label.sh              # one line of JSON on stdout
#   ./scripts/box_label.sh --pretty     # one field per line, still valid JSON
#
set -euo pipefail

PRETTY=0
[[ "${1:-}" == "--pretty" ]] && PRETTY=1

UNKNOWN="unknown"

# Escape a value for embedding in a JSON string. Backslash first, then quote,
# then strip anything that would break the line. Values here come from sysctl
# and from /proc, which are trusted but are not guaranteed free of quotes.
json_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr -d '\n\r\t'
}

emit_str() { printf '"%s":"%s"' "$1" "$(json_escape "$2")"; }
emit_raw() { printf '"%s":%s' "$1" "$2"; }

# Read a file and echo its first line trimmed, or the unknown marker.
read_or_unknown() {
  if [[ -r "$1" ]]; then
    local v
    v="$(head -n 1 "$1" 2>/dev/null | tr -d '\r' || true)"
    v="${v#"${v%%[![:space:]]*}"}"
    v="${v%"${v##*[![:space:]]}"}"
    [[ -n "$v" ]] && { printf '%s' "$v"; return; }
  fi
  printf '%s' "$UNKNOWN"
}

CPU_MODEL="$UNKNOWN"
OS="$UNKNOWN"
KERNEL="$UNKNOWN"
COMPILER="$UNKNOWN"
BUILD_TYPE="$UNKNOWN"
CORES=0
PINNED="false"
ISOLATED="false"
NOHZ_FULL="false"
GOVERNOR="$UNKNOWN"
NIC="$UNKNOWN"
NOTES=""

UNAME_S="$(uname -s 2>/dev/null || printf '%s' "$UNKNOWN")"

case "$UNAME_S" in

Darwin)
  CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || printf '%s' "$UNKNOWN")"
  PRODVER="$(sysctl -n kern.osproductversion 2>/dev/null || true)"
  [[ -n "$PRODVER" ]] && OS="macOS $PRODVER"
  OSREL="$(sysctl -n kern.osrelease 2>/dev/null || true)"
  [[ -n "$OSREL" ]] && KERNEL="Darwin $OSREL"
  NCPU="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
  [[ "$NCPU" =~ ^[0-9]+$ ]] && CORES="$NCPU"

  # There is no scaling governor on macOS that a program can read, no way to
  # pin a thread to a core, and no isolcpus or nohz_full equivalent. All four
  # stay at their honest defaults rather than being filled with something that
  # looks like an answer.
  PERF="$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || true)"
  EFF="$(sysctl -n hw.perflevel1.logicalcpu 2>/dev/null || true)"
  if [[ -n "$PERF" && -n "$EFF" ]]; then
    NOTES="$PERF performance cores and $EFF efficiency cores, placement between them is not observable or controllable from user space"
  fi
  NOTES="${NOTES:+$NOTES. }platform offers no per core pinning, any tail number from this box is unpinned"

  # The interface carrying the default route. A hint for the results file, not
  # a claim about which interface carried the feed.
  DEFIF="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}' || true)"
  [[ -n "$DEFIF" ]] && NIC="$DEFIF"
  ;;

Linux)
  MODEL="$(awk -F: '/^model name/{sub(/^[ \t]+/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
  [[ -z "$MODEL" ]] && MODEL="$(awk -F: '/^Model name/{sub(/^[ \t]+/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
  [[ -n "$MODEL" ]] && CPU_MODEL="$MODEL"

  # /etc/os-release is a shell fragment and sourcing it would run whatever is
  # in it, so the one line that is wanted is parsed out instead.
  if [[ -r /etc/os-release ]]; then
    PRETTY="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | head -n 1 | sed -e 's/^"//' -e 's/"$//' || true)"
    [[ -n "$PRETTY" ]] && OS="$PRETTY"
  fi

  KREL="$(read_or_unknown /proc/sys/kernel/osrelease)"
  [[ "$KREL" != "$UNKNOWN" ]] && KERNEL="Linux $KREL"

  NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  [[ "$NCPU" =~ ^[0-9]+$ ]] && CORES="$NCPU"

  GOVERNOR="$(read_or_unknown /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"

  if [[ -r /proc/cmdline ]]; then
    # Split on whitespace and match whole tokens, so isolcpus is found and a
    # substring of some unrelated parameter is not. A bare token with no value
    # still counts as present, which is why the two checks are separate.
    CMDLINE_TOKENS="$(tr -s ' \n' '\n\n' < /proc/cmdline)"
    if printf '%s\n' "$CMDLINE_TOKENS" | grep -qE '^isolcpus(=|$)'; then
      ISOLATED="true"
      ISO_VAL="$(printf '%s\n' "$CMDLINE_TOKENS" | sed -n 's/^isolcpus=//p' | head -n 1)"
      [[ -n "$ISO_VAL" ]] && NOTES="isolcpus=$ISO_VAL"
    fi
    if printf '%s\n' "$CMDLINE_TOKENS" | grep -qE '^nohz_full(=|$)'; then
      NOHZ_FULL="true"
      NOHZ_VAL="$(printf '%s\n' "$CMDLINE_TOKENS" | sed -n 's/^nohz_full=//p' | head -n 1)"
      [[ -n "$NOHZ_VAL" ]] && NOTES="${NOTES:+$NOTES }nohz_full=$NOHZ_VAL"
    fi
  fi

  # First interface that is up and is not loopback, with its driver.
  for IFDIR in /sys/class/net/*; do
    [[ -e "$IFDIR" ]] || continue
    IFNAME="$(basename "$IFDIR")"
    [[ "$IFNAME" == "lo" ]] && continue
    STATE="$(cat "$IFDIR/operstate" 2>/dev/null || true)"
    [[ "$STATE" == "up" ]] || continue
    DRV=""
    if [[ -L "$IFDIR/device/driver" ]]; then
      DRV="$(basename "$(readlink -f "$IFDIR/device/driver")" 2>/dev/null || true)"
    fi
    NIC="${IFNAME}${DRV:+ ($DRV)}"
    break
  done
  ;;

*)
  NOTES="unrecognised platform $UNAME_S, every field below that is not unknown should be treated with suspicion"
  ;;
esac

# The compiler the project would build with right now, which is not necessarily
# the one that built the binary being measured. The runner should prefer the
# value the binary reports about itself and use this only as a fallback.
CXX_BIN="${CXX:-c++}"
if command -v "$CXX_BIN" >/dev/null 2>&1; then
  CXX_VER="$("$CXX_BIN" --version 2>/dev/null | head -n 1 || true)"
  [[ -n "$CXX_VER" ]] && COMPILER="$CXX_VER"
fi

# Only reported if the caller actually says so. There is no way for a shell
# script to know the optimisation level a binary was built with.
[[ -n "${TICK_BUILD_TYPE:-}" ]] && BUILD_TYPE="$TICK_BUILD_TYPE"

# Only true if the caller confirms the run was pinned. This script describes the
# machine and cannot observe a run.
[[ "${TICK_PINNED:-}" == "1" ]] && PINNED="true"

[[ -z "$NOTES" ]] && NOTES=""

if [[ "$PRETTY" == "1" ]]; then
  SEP=$',\n  '
  OPEN=$'{\n  '
  CLOSE=$'\n}'
else
  SEP=","
  OPEN="{"
  CLOSE="}"
fi

printf '%s' "$OPEN"
emit_str cpu_model  "$CPU_MODEL";  printf '%s' "$SEP"
emit_str os         "$OS";         printf '%s' "$SEP"
emit_str kernel     "$KERNEL";     printf '%s' "$SEP"
emit_str compiler   "$COMPILER";   printf '%s' "$SEP"
emit_str build_type "$BUILD_TYPE"; printf '%s' "$SEP"
emit_raw cores      "$CORES";      printf '%s' "$SEP"
emit_raw pinned     "$PINNED";     printf '%s' "$SEP"
emit_raw isolated   "$ISOLATED";   printf '%s' "$SEP"
emit_raw nohz_full  "$NOHZ_FULL";  printf '%s' "$SEP"
emit_str governor   "$GOVERNOR";   printf '%s' "$SEP"
emit_str nic        "$NIC";        printf '%s' "$SEP"
emit_str notes      "$NOTES"
printf '%s\n' "$CLOSE"
