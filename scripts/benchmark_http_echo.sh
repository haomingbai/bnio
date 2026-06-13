#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# benchmark_http_echo — build and compare bupp vs Asio HTTP echo servers
# ---------------------------------------------------------------------------

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- defaults (overridable via env vars or CLI flags) ---------------------

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-benchmark}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
THREADS="${THREADS:-2}"
CONNECTIONS="${CONNECTIONS:-64}"
DURATION="${DURATION:-10s}"
BUPP_PORT="${BUPP_PORT:-18080}"
ASIO_PORT="${ASIO_PORT:-18081}"

FETCH_WRK=false
FETCH_ASIO=false
EXTRA_CMAKE_ARGS=()

# ---- usage -----------------------------------------------------------------

usage() {
  cat <<EOF
usage: ${0##*/} [FLAGS...]

Flags:
  --build-dir DIR     CMake build directory (default: \${ROOT_DIR}/build-benchmark,
                      env: BUILD_DIR)
  --fetch-wrk         Pass -DBUPP_FETCH_WRK=ON to cmake and build the wrk target
  --no-fetch-wrk      Do not auto-build wrk — it must be in PATH
  --fetch-asio        Pass -DBUPP_BUILD_ASIO_EXAMPLES=ON (auto-fetch Asio)
  --cmake-args "..."  Extra arguments forwarded to cmake (quoted)
  --help, -h          Show this message

Environment variables (overridden by CLI flags):
  BUILD_DIR          build directory
  CMAKE_BUILD_TYPE   Debug | Release (default: Release)
  THREADS            wrk -t (default: 2)
  CONNECTIONS        wrk -c (default: 64)
  DURATION           wrk -d (default: 10s)
  BUPP_PORT          port for the bupp server  (default: 18080)
  ASIO_PORT          port for the Asio server  (default: 18081)

Examples:
  ${0##*/}
  ${0##*/} --build-dir /tmp/mybuild
  ${0##*/} --fetch-wrk --fetch-asio
  ${0##*/} --build-dir build-bench --fetch-wrk --cmake-args "-DCMAKE_BUILD_TYPE=Debug"
EOF
  exit 0
}

# ---- parse CLI -------------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"; shift 2 ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"; shift ;;
    --fetch-wrk)
      FETCH_WRK=true; shift ;;
    --no-fetch-wrk)
      FETCH_WRK=false; shift ;;
    --fetch-asio)
      FETCH_ASIO=true; shift ;;
    --cmake-args)
      EXTRA_CMAKE_ARGS+=($2); shift 2 ;;
    --cmake-args=*)
      EXTRA_CMAKE_ARGS+=("${1#*=}"); shift ;;
    --help|-h)
      usage ;;
    *)
      echo "${0##*/}: unknown flag: $1" >&2
      usage ;;
  esac
done

# ---- resolve wrk -----------------------------------------------------------

find_wrk() {
  # 1. CMake-built wrk (when --fetch-wrk was used during a prior build)
  if [[ -x "${BUILD_DIR}/wrk-install/bin/wrk" ]]; then
    WRK_BIN="${BUILD_DIR}/wrk-install/bin/wrk"
    return 0
  fi
  # 2. System wrk
  if command -v wrk >/dev/null 2>&1; then
    WRK_BIN=wrk
    return 0
  fi
  return 1
}

# ---- cmake configure + build ------------------------------------------------

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  -DBUPP_BUILD_BENCHMARKS=ON
)

if ${FETCH_ASIO}; then
  CMAKE_ARGS+=(-DBUPP_BUILD_ASIO_EXAMPLES=ON)
fi

if ${FETCH_WRK}; then
  CMAKE_ARGS+=(-DBUPP_FETCH_WRK=ON)
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "benchmark_http_echo: cmake -S ${ROOT_DIR} -B ${BUILD_DIR} ${CMAKE_ARGS[*]}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

SERVER_TARGETS=(
  bupp_benchmark_bupp_http_echo_server
  bupp_benchmark_asio_http_echo_server
)

if ${FETCH_WRK}; then
  SERVER_TARGETS+=(wrk)
fi

echo "benchmark_http_echo: building ${SERVER_TARGETS[*]}"
cmake --build "${BUILD_DIR}" --target "${SERVER_TARGETS[@]}"

# ---- resolve wrk (after potential build) -----------------------------------

if ! find_wrk; then
  echo "benchmark_http_echo: wrk not found." >&2
  echo "  Options:" >&2
  echo "    - Install it:  dnf install wrk  /  apt install wrk" >&2
  echo "    - Auto-build:  ${0##*/} --fetch-wrk" >&2
  exit 1
fi
echo "benchmark_http_echo: using wrk at ${WRK_BIN}"

# ---- helpers ----------------------------------------------------------------

SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  SERVER_PID=""
}

trap cleanup EXIT

wait_for_port() {
  local port="$1"
  for _ in {1..50}; do
    if timeout 1 bash -c "</dev/tcp/127.0.0.1/${port}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

run_case() {
  local name="$1"
  local server="$2"
  local port="$3"
  local log_file="${BUILD_DIR}/${name}.log"

  cleanup
  echo "benchmark_http_echo: starting ${name} on 127.0.0.1:${port}"
  "${server}" "${port}" >"${log_file}" 2>&1 &
  SERVER_PID="$!"

  if ! wait_for_port "${port}"; then
    echo "benchmark_http_echo: ${name} did not open port ${port}" >&2
    echo "benchmark_http_echo: see ${log_file}" >&2
    exit 1
  fi

  echo "benchmark_http_echo: wrk ${name}"
  "${WRK_BIN}" -t"${THREADS}" -c"${CONNECTIONS}" -d"${DURATION}" \
    "http://127.0.0.1:${port}/echo"
}

# ---- run -------------------------------------------------------------------

run_case \
  "bupp_http_echo" \
  "${BUILD_DIR}/examples/benchmark/bupp_benchmark_bupp_http_echo_server" \
  "${BUPP_PORT}"

run_case \
  "asio_http_echo" \
  "${BUILD_DIR}/examples/benchmark/bupp_benchmark_asio_http_echo_server" \
  "${ASIO_PORT}"

cleanup
