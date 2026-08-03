#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_perf.sh - run the padded and unpadded builds under `perf stat` and print
# the hardware counters side by side.
#
# The two binaries are built from byte-identical source; the only difference is
# -DSPSC_DISABLE_PADDING, which moves two integers onto the same cache line. So
# any difference in the counters below is caused by that move and nothing else.
# The counter to watch is cache-misses (and its ratio to cache-references): in
# the unpadded build, every store the producer makes to tail_ invalidates the
# line the consumer is reading head_ from, so both sides start missing on data
# that never actually changed for them. That is false sharing, visible.
#
# Linux only - `perf` is a Linux tool built on perf_event_open(2).
# ---------------------------------------------------------------------------
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
OUT_DIR="${OUT_DIR:-perf-out}"
BENCH="${BENCH:-bench_throughput}"

PRODUCER_CORE="${1:-1}"
CONSUMER_CORE="${2:-2}"
COUNT="${3:-10000000}"
WARMUP="${4:-100000}"

# context-switches is in the list for a reason: it is the sanity check. A run
# with a non-trivial number of involuntary switches was sharing its cores with
# something else, and its tail latency belongs to that something else.
EVENTS="cache-misses,cache-references,L1-dcache-load-misses,L1-dcache-loads,context-switches"

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

command -v perf >/dev/null 2>&1 || die "perf not found (Debian/Ubuntu: apt install linux-tools-\$(uname -r))"
command -v awk  >/dev/null 2>&1 || die "awk not found"

# perf_event_paranoid > 2 blocks unprivileged counter access entirely; > 1 blocks
# most hardware events. Say so up front rather than printing a table of zeros.
if [ -r /proc/sys/kernel/perf_event_paranoid ]; then
  PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid)
  if [ "$PARANOID" -gt 1 ]; then
    printf 'note: /proc/sys/kernel/perf_event_paranoid = %s\n' "$PARANOID" >&2
    printf '      hardware events may be unavailable. To allow them for this boot:\n' >&2
    printf '        sudo sysctl -w kernel.perf_event_paranoid=1\n\n' >&2
  fi
fi

for variant in padded unpadded; do
  [ -x "${BUILD_DIR}/${BENCH}_${variant}" ] \
    || die "${BUILD_DIR}/${BENCH}_${variant} not found or not executable - build first"
done

mkdir -p "$OUT_DIR"

printf 'perf events : %s\n' "$EVENTS"
printf 'benchmark   : %s (%s messages, %s warmup)\n' "$BENCH" "$COUNT" "$WARMUP"
printf 'cores       : producer=%s consumer=%s\n\n' "$PRODUCER_CORE" "$CONSUMER_CORE"

for variant in padded unpadded; do
  BIN="${BUILD_DIR}/${BENCH}_${variant}"
  printf '=== %s =====================================================\n' "$variant"
  # -x, gives machine-readable CSV; -o sends it to a file so the benchmark's own
  # stdout stays clean and readable.
  perf stat -x, -e "$EVENTS" -o "${OUT_DIR}/${variant}.csv" -- \
    "$BIN" "$PRODUCER_CORE" "$CONSUMER_CORE" "$COUNT" "$WARMUP" \
    | tee "${OUT_DIR}/${variant}.stdout"
  printf '\n'
done

printf '=== perf counters, padded vs unpadded ==================================\n\n'

# perf's CSV columns are: value,unit,event,run-time,percentage,[metric,metric-unit]
# Comment lines start with '#'; unsupported events come through as text such as
# "<not supported>", which is passed through untouched rather than turned into a
# zero that would look like a real measurement.
awk -F, '
  FNR == 1 { file_index++ }
  /^#/     { next }
  NF < 3   { next }
  {
    value = $1; event = $3
    gsub(/^[ \t]+|[ \t]+$/, "", value)
    gsub(/^[ \t]+|[ \t]+$/, "", event)
    if (file_index == 1) {
      padded[event] = value
      if (!(event in seen)) { seen[event] = 1; order[++n] = event }
    } else {
      unpadded[event] = value
    }
  }
  function ratio(a, b) {
    if (b + 0 > 0) { return sprintf("%.2f%%", 100.0 * (a + 0) / (b + 0)) }
    return "n/a"
  }
  END {
    printf "%-26s %20s %20s\n", "EVENT", "PADDED", "UNPADDED"
    printf "%-26s %20s %20s\n", "--------------------------", "--------------------", "--------------------"
    for (i = 1; i <= n; i++) {
      e = order[i]
      printf "%-26s %20s %20s\n", e, padded[e], (e in unpadded ? unpadded[e] : "-")
    }
    printf "\n"
    printf "%-26s %20s %20s\n", "cache miss rate",
           ratio(padded["cache-misses"], padded["cache-references"]),
           ratio(unpadded["cache-misses"], unpadded["cache-references"])
    printf "%-26s %20s %20s\n", "L1-dcache miss rate",
           ratio(padded["L1-dcache-load-misses"], padded["L1-dcache-loads"]),
           ratio(unpadded["L1-dcache-load-misses"], unpadded["L1-dcache-loads"])
  }
' "${OUT_DIR}/padded.csv" "${OUT_DIR}/unpadded.csv"

printf '\nraw perf output : %s/{padded,unpadded}.csv\n' "$OUT_DIR"
printf 'benchmark output: %s/{padded,unpadded}.stdout\n' "$OUT_DIR"
printf '\nFor a per-source-line view of where the misses land:\n'
printf '  perf record -e cache-misses -c 1000 -g -- %s/%s_unpadded %s %s %s\n' \
       "$BUILD_DIR" "$BENCH" "$PRODUCER_CORE" "$CONSUMER_CORE" "$COUNT"
printf '  perf report --stdio\n'
