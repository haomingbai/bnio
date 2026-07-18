#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_NAME="${0##*/}"

BUILD_DIR="${BUILD_DIR:-/tmp/bnio-bench}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
PERF="${PERF:-0}"
CONNECTIONS="${CONNECTIONS:-128}"
DURATION="${DURATION:-10s}"
MSG_SIZE="${MSG_SIZE:-1024}"
BNIO_PORT_RAW="${BNIO_PORT_RAW:-8090}"
ASIO_PORT_RAW="${ASIO_PORT_RAW:-8091}"
CLIENT_WORKERS="${CLIENT_WORKERS:-match}"

FETCH_ASIO=false
EXTRA_CMAKE_ARGS=()
ARTIFACTS="${ROOT_DIR}/.artifacts"

usage() {
  cat <<EOF
usage: ${SCRIPT_NAME} [FLAGS...]

Flags:
  --build-dir DIR     CMake build directory (default: /tmp/bnio-bench)
  --perf              Record perf.data to .artifacts/
  --fetch-asio        Pass -DBNIO_BUILD_ASIO_EXAMPLES=ON (auto-fetch Asio)
  --cmake-args "..."  Extra arguments forwarded to cmake
  --help, -h          Show this message

Environment variables:
  BUILD_DIR, PERF, CONNECTIONS, DURATION, MSG_SIZE
  WORKER_COUNTS       Space- or comma-separated server worker counts
                      (default: 1 and the online CPU count)
  CLIENT_WORKERS      Client worker count, or "match" to use the server count
                      (default: match)
  SERVER_WORKERS      Backward-compatible alias for WORKER_COUNTS
  BNIO_PORT_RAW, ASIO_PORT_RAW
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

online_cpu_count() {
  local count
  count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  if [[ ! "${count}" =~ ^[0-9]+$ ]] || [[ "${count}" -eq 0 ]]; then
    count="$(nproc 2>/dev/null || true)"
  fi
  if [[ ! "${count}" =~ ^[0-9]+$ ]] || [[ "${count}" -eq 0 ]]; then
    count=1
  fi
  echo "${count}"
}

default_worker_counts() {
  local count
  count="$(online_cpu_count)"
  if [[ "${count}" -gt 1 ]]; then
    echo "1 ${count}"
  else
    echo "1"
  fi
}

validate_worker_count() {
  local name="$1" value="$2"
  if [[ ! "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -eq 0 ]]; then
    echo "${SCRIPT_NAME}: ${name} must be a positive integer, got '${value}'" >&2
    exit 2
  fi
}

if [[ -z "${WORKER_COUNTS:-}" ]]; then
  if [[ -n "${SERVER_WORKERS:-}" ]]; then
    WORKER_COUNTS="${SERVER_WORKERS}"
  else
    WORKER_COUNTS="$(default_worker_counts)"
  fi
fi

WORKER_COUNTS="${WORKER_COUNTS//,/ }"
read -r -a WORKER_COUNT_LIST <<<"${WORKER_COUNTS}"
if [[ "${#WORKER_COUNT_LIST[@]}" -eq 0 ]]; then
  echo "${SCRIPT_NAME}: WORKER_COUNTS is empty" >&2
  exit 2
fi

for worker_count in "${WORKER_COUNT_LIST[@]}"; do
  validate_worker_count WORKER_COUNTS "${worker_count}"
done

if [[ "${CLIENT_WORKERS}" != "match" ]]; then
  validate_worker_count CLIENT_WORKERS "${CLIENT_WORKERS}"
fi

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  -DBNIO_BUILD_EXAMPLES=ON
  -DBNIO_BUILD_ASIO_EXAMPLES=ON
)

if ${FETCH_ASIO}; then
  CMAKE_ARGS+=(-DBNIO_BUILD_ASIO_EXAMPLES=ON)
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "=== cmake configure ==="
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "=== build raw echo targets ==="
cmake --build "${BUILD_DIR}" --target bnio_raw_echo asio_raw_echo raw_echo_client

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
  local name="$1" server="$2" port="$3" perf_tag="$4" server_workers="$5"
  local client_workers
  if [[ "${CLIENT_WORKERS}" == "match" ]]; then
    client_workers="${server_workers}"
  else
    client_workers="${CLIENT_WORKERS}"
  fi

  cleanup
  echo "=== ${name} on 127.0.0.1:${port} (${server_workers} server workers, ${client_workers} client workers) ==="
  "${server}" "${port}" "${server_workers}" >"${BUILD_DIR}/${name}-${server_workers}w.log" 2>&1 &
  SERVER_PID="$!"

  if ! wait_for_port "${port}"; then
    echo "FAILED to start ${name}; see ${BUILD_DIR}/${name}-${server_workers}w.log" >&2
    exit 1
  fi

  local perf_cmd=()
  if [[ "${PERF}" == "1" ]]; then
    perf_cmd=(perf record -g -o "${ARTIFACTS}/perf-${perf_tag}-${server_workers}w.data" -p "${SERVER_PID}" --)
  fi

  local output
  output="$("${perf_cmd[@]}" "${BUILD_DIR}/examples/raw_echo/raw_echo_client" \
    "${port}" "${CONNECTIONS}" "${DURATION_SEC}" "${MSG_SIZE}" "${client_workers}")"
  echo "${output}"

  local total rate throughput
  total="$(awk -F': ' '/^total:/ {print $2}' <<<"${output}" | awk '{print $1}')"
  rate="$(awk -F': ' '/^rate:/ {print $2}' <<<"${output}" | awk '{print $1}')"
  throughput="$(awk -F': ' '/^throughput:/ {print $2}' <<<"${output}" | awk '{print $1}')"
  RESULT_ROWS+=("${server_workers}|${client_workers}|${name}|${total}|${rate}|${throughput}")

  cleanup
  sleep 0.3
}

RESULT_ROWS=()
for worker_count in "${WORKER_COUNT_LIST[@]}"; do
  run_raw "bnio_raw" "${BUILD_DIR}/examples/raw_echo/bnio_raw_echo" \
    "${BNIO_PORT_RAW}" "raw-bnio" "${worker_count}"
  run_raw "asio_raw" "${BUILD_DIR}/examples/raw_echo/asio_raw_echo" \
    "${ASIO_PORT_RAW}" "raw-asio" "${worker_count}"
done

if [[ "${PERF}" == "1" ]]; then
  echo "perf data: ${ARTIFACTS}/"
  ls -lh "${ARTIFACTS}"/perf-*.data 2>/dev/null || true
fi

echo "=== summary ==="
printf '%-8s %-8s %-10s %-14s %-12s %-12s\n' \
  "server-w" "client-w" "target" "total" "req/s" "MB/s"
for row in "${RESULT_ROWS[@]}"; do
  IFS='|' read -r server_workers client_workers target total rate throughput <<<"${row}"
  printf '%-8s %-8s %-10s %-14s %-12s %-12s\n' \
    "${server_workers}" "${client_workers}" "${target}" "${total}" "${rate}" "${throughput}"
done

echo "done."
