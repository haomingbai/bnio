#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

mapfile -t files < <(
  find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/tests" "${ROOT_DIR}/examples" \
    -type f \
    \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.cxx" \) \
    | sort
)

if ((${#files[@]} == 0)); then
  exit 0
fi

clang-format --dry-run --Werror "${files[@]}"
