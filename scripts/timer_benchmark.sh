#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_NAME="${0##*/}"

BUILD_DIR="${BUILD_DIR:-/tmp/bnio-timer-bench}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
TIMERS="${TIMERS:-1024}"
UPDATES="${UPDATES:-100}"
REPLACE_COUNT="${REPLACE_COUNT:-}"

EXTRA_CMAKE_ARGS=()

usage() {
  cat <<EOF
usage: ${SCRIPT_NAME} [FLAGS...]

Builds and runs the bnio and standalone-Asio versions of the same active
timer-churn scenario. Both executables receive identical workload arguments.

Flags:
  --build-dir DIR     CMake build directory (default: /tmp/bnio-timer-bench)
  --cmake-args "..."  Extra arguments forwarded to cmake
  --help, -h          Show this message

Environment variables:
  BUILD_DIR, CMAKE_BUILD_TYPE, TIMERS, UPDATES, REPLACE_COUNT
EOF
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --build-dir=*) BUILD_DIR="${1#*=}"; shift ;;
    --cmake-args) EXTRA_CMAKE_ARGS+=($2); shift 2 ;;
    --cmake-args=*) EXTRA_CMAKE_ARGS+=("${1#*=}"); shift ;;
    --help|-h) usage ;;
    *) echo "${SCRIPT_NAME}: unknown flag: $1" >&2; usage ;;
  esac
done

BENCHMARK_ARGS=(--timers "${TIMERS}" --updates "${UPDATES}")
if [[ -n "${REPLACE_COUNT}" ]]; then
  BENCHMARK_ARGS+=(--replace-count "${REPLACE_COUNT}")
fi

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  -DBNIO_BUILD_TESTS=OFF
  -DBNIO_BUILD_EXAMPLES=OFF
  -DBNIO_BUILD_BENCHMARKS=ON
)
CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "=== cmake configure ==="
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "=== build timer benchmarks ==="
cmake --build "${BUILD_DIR}" --target bnio_timer_churn_benchmark \
  asio_timer_churn_benchmark

echo "=== bnio timer churn ==="
"${BUILD_DIR}/benchmarks/timer_churn/bnio_timer_churn_benchmark" \
  "${BENCHMARK_ARGS[@]}"

echo "=== asio timer churn ==="
"${BUILD_DIR}/benchmarks/timer_churn/asio_timer_churn_benchmark" \
  "${BENCHMARK_ARGS[@]}"
