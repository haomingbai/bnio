#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_NAME="${0##*/}"

# ---- defaults -----------------------------------------------------------

BUILD_DIR="${BUILD_DIR:-/tmp/bupp-bench}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
MODE="${MODE:-http}"          # http | raw
PERF="${PERF:-0}"             # 0 | 1
WRK_BIN="${WRK_BIN:-}"
THREADS="${THREADS:-2}"
CONNECTIONS="${CONNECTIONS:-64}"
DURATION="${DURATION:-10s}"
BUPP_PORT_HTTP="${BUPP_PORT_HTTP:-18080}"
ASIO_PORT_HTTP="${ASIO_PORT_HTTP:-18081}"
BUPP_PORT_RAW="${BUPP_PORT_RAW:-8090}"
ASIO_PORT_RAW="${ASIO_PORT_RAW:-8091}"
MSG_SIZE="${MSG_SIZE:-1024}"  # raw echo message size

FETCH_WRK=false
FETCH_ASIO=false
EXTRA_CMAKE_ARGS=()

ARTIFACTS="${ROOT_DIR}/.artifacts"

# ---- usage --------------------------------------------------------------

usage() {
  cat <<EOF
usage: ${SCRIPT_NAME} [FLAGS...]

Flags:
  --mode MODE         Benchmark mode: http (default) | raw
  --build-dir DIR     CMake build directory (default: /tmp/bupp-bench)
  --perf              Record perf.data to .artifacts/
  --fetch-wrk         Pass -DBUPP_FETCH_WRK=ON and build wrk
  --fetch-asio        Pass -DBUPP_BUILD_ASIO_EXAMPLES=ON (auto-fetch Asio)
  --wrk-bin PATH      Path to wrk binary (auto-detected if not set)
  --cmake-args "..."  Extra arguments forwarded to cmake

Environment variables (overridden by CLI flags):
  BUILD_DIR, MODE, PERF, WRK_BIN, THREADS, CONNECTIONS, DURATION

Examples:
  ${SCRIPT_NAME}                                  # HTTP echo, system wrk
  ${SCRIPT_NAME} --mode raw --perf                # raw echo + perf
  ${SCRIPT_NAME} --fetch-wrk --fetch-asio         # auto-build everything
  ${SCRIPT_NAME} --build-dir /tmp/mybuild --perf
EOF
  exit 0
}

# ---- parse CLI ----------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)       MODE="$2"; shift 2 ;;
    --mode=*)     MODE="${1#*=}"; shift ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
    --build-dir=*) BUILD_DIR="${1#*=}"; shift ;;
    --perf)       PERF=1; shift ;;
    --fetch-wrk)  FETCH_WRK=true; shift ;;
    --no-fetch-wrk) FETCH_WRK=false; shift ;;
    --fetch-asio) FETCH_ASIO=true; shift ;;
    --wrk-bin)    WRK_BIN="$2"; shift 2 ;;
    --wrk-bin=*)  WRK_BIN="${1#*=}"; shift ;;
    --cmake-args) EXTRA_CMAKE_ARGS+=($2); shift 2 ;;
    --cmake-args=*) EXTRA_CMAKE_ARGS+=("${1#*=}"); shift ;;
    --help|-h)    usage ;;
    *)
      echo "${SCRIPT_NAME}: unknown flag: $1" >&2; usage ;;
  esac
done

if [[ "${MODE}" != "http" && "${MODE}" != "raw" ]]; then
  echo "${SCRIPT_NAME}: invalid mode '${MODE}' (use http or raw)" >&2; exit 2
fi

# ---- find wrk -----------------------------------------------------------

find_wrk() {
  if [[ -n "${WRK_BIN}" && -x "${WRK_BIN}" ]]; then
    return 0
  fi
  if [[ -x "${BUILD_DIR}/wrk-install/bin/wrk" ]]; then
    WRK_BIN="${BUILD_DIR}/wrk-install/bin/wrk"; return 0
  fi
  if command -v wrk >/dev/null 2>&1; then
    WRK_BIN=wrk; return 0
  fi
  return 1
}

# ---- cmake configure + build --------------------------------------------

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  -DBUPP_BUILD_EXAMPLES=OFF
)

if [[ "${MODE}" == "http" ]]; then
  CMAKE_ARGS+=(-DBUPP_BUILD_BENCHMARKS=ON)
  SERVER_TARGETS=(
    bupp_benchmark_bupp_http_echo_server
    bupp_benchmark_asio_http_echo_server
  )
else
  CMAKE_ARGS+=(-DBUPP_BUILD_EXAMPLES=ON -DBUPP_BUILD_ASIO_EXAMPLES=ON)
  SERVER_TARGETS=(
    bupp_raw_echo
    asio_raw_echo
    raw_echo_client
  )
fi

if ${FETCH_ASIO}; then
  CMAKE_ARGS+=(-DBUPP_BUILD_ASIO_EXAMPLES=ON)
fi

if ${FETCH_WRK}; then
  CMAKE_ARGS+=(-DBUPP_FETCH_WRK=ON)
  SERVER_TARGETS+=(wrk)
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "=== cmake configure ==="
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "=== build ${SERVER_TARGETS[*]} ==="
cmake --build "${BUILD_DIR}" --target "${SERVER_TARGETS[@]}"

# ---- resolve wrk (after potential build) --------------------------------

if [[ "${MODE}" == "http" ]]; then
  if ! find_wrk; then
    echo "wrk not found. Options:" >&2
    echo "  Install: dnf install wrk / apt install wrk" >&2
    echo "  Specify: --wrk-bin /path/to/wrk" >&2
    echo "  Auto-build: ${SCRIPT_NAME} --fetch-wrk" >&2
    exit 1
  fi
  echo "wrk: ${WRK_BIN}"
fi

# ---- perf setup ---------------------------------------------------------

if [[ "${PERF}" == "1" ]]; then
  mkdir -p "${ARTIFACTS}"
fi

# ---- helpers ------------------------------------------------------------

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

# ---- HTTP benchmark -----------------------------------------------------

run_http() {
  local name="$1" server="$2" port="$3" perf_tag="$4"

  cleanup
  echo "=== ${name} on 127.0.0.1:${port} ==="
  "${server}" "${port}" >"${BUILD_DIR}/${name}.log" 2>&1 &
  SERVER_PID="$!"

  if ! wait_for_port "${port}"; then
    echo "FAILED to start ${name}" >&2; exit 1
  fi

  local perf_cmd=()
  if [[ "${PERF}" == "1" ]]; then
    perf_cmd=(perf record -g -o "${ARTIFACTS}/perf-${perf_tag}.data" -p "${SERVER_PID}" --)
  fi

  "${perf_cmd[@]}" "${WRK_BIN}" -t"${THREADS}" -c"${CONNECTIONS}" -d"${DURATION}" \
    "http://127.0.0.1:${port}/echo"

  kill -INT "${SERVER_PID}" 2>/dev/null || true
  sleep 1
  kill -9 "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
  sleep 0.3
}

# ---- raw echo benchmark -------------------------------------------------

run_raw() {
  local name="$1" server="$2" port="$3" perf_tag="$4"

  cleanup
  echo "=== ${name} on 127.0.0.1:${port} ==="
  "${server}" >"${BUILD_DIR}/${name}.log" 2>&1 &
  SERVER_PID="$!"

  if ! wait_for_port "${port}"; then
    echo "FAILED to start ${name}" >&2; exit 1
  fi

  local perf_cmd=()
  if [[ "${PERF}" == "1" ]]; then
    perf_cmd=(perf record -g -o "${ARTIFACTS}/perf-${perf_tag}.data" -p "${SERVER_PID}" --)
  fi

  "${perf_cmd[@]}" "${BUILD_DIR}/examples/raw_echo/raw_echo_client" "${port}"

  kill -9 "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
  sleep 0.3
}

# ---- main ---------------------------------------------------------------

if [[ "${MODE}" == "http" ]]; then
  run_http "bupp_http"  "${BUILD_DIR}/examples/benchmark/bupp_benchmark_bupp_http_echo_server" "${BUPP_PORT_HTTP}" "http-bupp"
  run_http "asio_http"  "${BUILD_DIR}/examples/benchmark/bupp_benchmark_asio_http_echo_server" "${ASIO_PORT_HTTP}" "http-asio"
else
  run_raw  "bupp_raw"   "${BUILD_DIR}/examples/raw_echo/bupp_raw_echo"  "${BUPP_PORT_RAW}" "raw-bupp"
  run_raw  "asio_raw"   "${BUILD_DIR}/examples/raw_echo/asio_raw_echo"  "${ASIO_PORT_RAW}" "raw-asio"
fi

if [[ "${PERF}" == "1" ]]; then
  echo "perf data: ${ARTIFACTS}/"
  ls -lh "${ARTIFACTS}"/perf-*.data 2>/dev/null || true
fi

echo "done."
