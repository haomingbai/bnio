#!/usr/bin/env bash
# Per-thread CPU sampler for a given PID.
# Usage: watch_cpu.sh <pid> <interval_sec> <samples>
# Reads /proc/<pid>/task/*/stat (utime+stime, jiffies) and prints per-thread
# delta CPU usage as a percentage of one CPU per interval.
set -u

PID="${1:?pid required}"
INTERVAL="${2:-2}"
SAMPLES="${3:-15}"

CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"

# snapshot: <tid> <comm> <utime+stime_jiffies>
snapshot() {
  local tid comm jiffies line
  for stat_file in /proc/"$PID"/task/*/stat; do
    [ -r "$stat_file" ] || continue
    line="$(cat "$stat_file" 2>/dev/null)" || continue
    tid="${line%% *}"
    comm="$(printf '%s' "$line" | sed -n 's/^[0-9]* (\([^)]*\)).*/\1/p')"
    jiffies="$(printf '%s' "$line" | awk '{print $14+$15}')"
    printf '%s %s %s\n' "$tid" "$comm" "$jiffies"
  done
}

prev="$(snapshot)"
for ((i = 1; i <= SAMPLES; i++)); do
  sleep "$INTERVAL"
  cur="$(snapshot)"
  ts="$(date +%H:%M:%S)"
  echo "--- sample $i @ $ts ---"
  printf '%-8s %-24s %10s %8s\n' TID COMM "cpu_jiffies" "cpu_pct"
  while read -r tid comm jiffies; do
    prev_jiffies="$(printf '%s\n' "$prev" | awk -v t="$tid" '$1==t {print $3}')"
    if [ -n "$prev_jiffies" ]; then
      pct="$(awk -v d="$((jiffies - prev_jiffies))" -v s="$INTERVAL" -v c="$CLK_TCK" 'BEGIN{printf "%.1f", 100.0*d/(s*c)}')"
    else
      pct="new"
    fi
    printf '%-8s %-24s %10s %8s\n' "$tid" "$comm" "$jiffies" "$pct"
  done <<< "$cur"
  prev="$(snapshot)"
done
