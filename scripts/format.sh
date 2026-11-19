#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

# Keep in sync with check-format.sh. The names are fixed, whitespace-free literals.
SOURCE_DIRS="include src tests examples benchmarks"
set --
for dir in $SOURCE_DIRS; do
  set -- "$@" "$ROOT_DIR/$dir"
done

find "$@" \
  -type f \
  \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.cxx" \) \
  -exec clang-format -i {} +
