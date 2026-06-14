#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_NAME="${0##*/}"

BUILD_DIR="${BUILD_DIR:-/tmp/bupp-bench}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
PERF="${PERF:-0}"
CONNECTIONS="${CONNECTIONS:-128}"
DURATION="${DURATION:-10s}"
MSG_SIZE="${MSG_SIZE:-1024}"
BUPP_PORT_RAW="${BUPP_PORT_RAW:-8090}"
ASIO_PORT_RAW="${ASIO_PORT_RAW:-8091}"
BUPP_DIRECT_PORT_RAW="${BUPP_DIRECT_PORT_RAW:-8092}"

FETCH_ASIO=false
EXTRA_CMAKE_ARGS=()
ARTIFACTS="${ROOT_DIR}/.artifacts"

usage() {
  cat <<EOF
usage: ${SCRIPT_NAME} [FLAGS...]

Flags:
  --build-dir DIR     CMake build directory (default: /tmp/bupp-bench)
  --perf              Record perf.data to .artifacts/
  --fetch-asio        Pass -DBUPP_BUILD_ASIO_EXAMPLES=ON (auto-fetch Asio)
  --cmake-args "..."  Extra arguments forwarded to cmake
  --help, -h          Show this message

Environment variables:
  BUILD_DIR, PERF, CONNECTIONS, DURATION, MSG_SIZE, BUPP_PORT_RAW, ASIO_PORT_RAW
EOF
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --build-dir=*) BUILD_DIR="${1#*=}"; shift ;;
    --perf) PERF=1; shift ;;
    --fetch-asio) FETCH_ASIO=true; shift ;;
    --cmake-args) EXTRA_CMAKE_ARGS+=($2); shift 2 ;;
    --cmake-args=*) EXTRA_CMAKE_ARGS+=("${1#*=}"); shift ;;
    --help|-h) usage ;;
    *) echo "${SCRIPT_NAME}: unknown flag: $1" >&2; usage ;;
  esac
done

duration_seconds() {
  local value="$1"
  if [[ "${value}" =~ ^([0-9]+)s?$ ]]; then
    echo "${BASH_REMATCH[1]}"
    return 0
  fi
  echo "${SCRIPT_NAME}: DURATION must be an integer second count, got '${value}'" >&2
  exit 2
}

DURATION_SEC="$(duration_seconds "${DURATION}")"

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  -DBUPP_BUILD_EXAMPLES=ON
  -DBUPP_BUILD_ASIO_EXAMPLES=ON
)

if ${FETCH_ASIO}; then
  CMAKE_ARGS+=(-DBUPP_BUILD_ASIO_EXAMPLES=ON)
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "=== cmake configure ==="
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "=== build raw echo targets ==="
cmake --build "${BUILD_DIR}" --target bupp_raw_echo bupp_raw_echo_direct asio_raw_echo raw_echo_client

if [[ "${PERF}" == "1" ]]; then
  mkdir -p "${ARTIFACTS}"
fi

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

run_raw() {
  local name="$1" server="$2" port="$3" perf_tag="$4"

  cleanup
  echo "=== ${name} on 127.0.0.1:${port} ==="
  "${server}" "${port}" >"${BUILD_DIR}/${name}.log" 2>&1 &
  SERVER_PID="$!"

  if ! wait_for_port "${port}"; then
    echo "FAILED to start ${name}; see ${BUILD_DIR}/${name}.log" >&2
    exit 1
  fi

  local perf_cmd=()
  if [[ "${PERF}" == "1" ]]; then
    perf_cmd=(perf record -g -o "${ARTIFACTS}/perf-${perf_tag}.data" -p "${SERVER_PID}" --)
  fi

  "${perf_cmd[@]}" "${BUILD_DIR}/examples/raw_echo/raw_echo_client" \
    "${port}" "${CONNECTIONS}" "${DURATION_SEC}" "${MSG_SIZE}"

  kill -9 "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
  SERVER_PID=""
  sleep 0.3
}

run_raw "bupp_raw" "${BUILD_DIR}/examples/raw_echo/bupp_raw_echo" \
  "${BUPP_PORT_RAW}" "raw-bupp"
run_raw "asio_raw" "${BUILD_DIR}/examples/raw_echo/asio_raw_echo" \
  "${ASIO_PORT_RAW}" "raw-asio"
run_raw "bupp_raw_direct" "${BUILD_DIR}/examples/raw_echo/bupp_raw_echo_direct" \
  "${BUPP_DIRECT_PORT_RAW}" "raw-bupp-direct"

if [[ "${PERF}" == "1" ]]; then
  echo "perf data: ${ARTIFACTS}/"
  ls -lh "${ARTIFACTS}"/perf-*.data 2>/dev/null || true
fi

echo "done."
