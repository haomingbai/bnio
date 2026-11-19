#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
PASS=0
FAIL=0

run_test() {
  local name="$1"; shift
  echo "=== $name ==="
  if "$@"; then
    echo "  PASS"
    PASS=$((PASS + 1))
  else
    local status=$?
    echo "  FAIL (exit=${status})"
    FAIL=$((FAIL + 1))
  fi
  echo
}

# 1. timer_chain
run_test "timer_chain" timeout 5 "${BUILD}/examples/timer_chain/bnio_timer_chain" 2>&1

# 2. dns_lookup
run_test "dns_lookup" timeout 5 "${BUILD}/examples/dns_lookup/bnio_dns_lookup" localhost 80 2>&1

# 3. echo_server + nc pair test
echo "=== echo_server + nc pair ==="
PORT=19999
"${BUILD}/examples/echo_server/bnio_echo_server" "${PORT}" &
PID=$!
sleep 1

if echo "round1" | timeout 2 nc -w1 127.0.0.1 "${PORT}" | grep -q round1; then
  echo "  round1 PASS"
  PASS=$((PASS + 1))
else
  echo "  round1 FAIL"
  FAIL=$((FAIL + 1))
fi

if echo "round2" | timeout 2 nc -w1 127.0.0.1 "${PORT}" | grep -q round2; then
  echo "  round2 PASS"
  PASS=$((PASS + 1))
else
  echo "  round2 FAIL"
  FAIL=$((FAIL + 1))
fi

# graceful shutdown
kill -INT "${PID}" 2>/dev/null || true
status=0
wait "${PID}" 2>/dev/null || status=$?
echo "  server stopped, exit=${status}"
echo

# 4. mini_curl HTTP test
run_test "mini_curl (HTTP)" timeout 10 "${BUILD}/examples/mini_curl/bnio_mini_curl" \
  http://httpbin.org/get 2>&1

echo "=== results: ${PASS} passed, ${FAIL} failed ==="
exit $FAIL
