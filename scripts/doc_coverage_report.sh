#!/usr/bin/env bash
# Generate the documentation coverage report for bnio headers.
#
# Unlike scripts/check-doc.sh this report is advisory: it never fails,
# no matter what doxygen reports (or whether doxygen is installed at all).
# The report is written as Markdown to <output-dir>/doc-coverage.md and
# echoed on stdout, so CI can append it to the job summary.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUTPUT_DIR="doc-coverage"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    *)
      echo "usage: $0 [--output-dir DIR]" >&2
      exit 0
      ;;
  esac
done

mkdir -p "${OUTPUT_DIR}"

REPORT="${OUTPUT_DIR}/doc-coverage.md"

if ! command -v doxygen >/dev/null 2>&1; then
  cat >"${REPORT}" <<'EOF'
## Documentation coverage report

> doxygen was not found; the documentation coverage report was skipped.

EOF
  cat "${REPORT}"
  exit 0
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

DOXYFILE="${WORK_DIR}/Doxyfile"
LOG="${WORK_DIR}/doxygen.log"

cat >"${DOXYFILE}" <<EOF
PROJECT_NAME = bnio
OUTPUT_DIRECTORY = "${WORK_DIR}/out"

INPUT = "${ROOT_DIR}/include" "${ROOT_DIR}/examples"
RECURSIVE = YES
FILE_PATTERNS = *.h *.hpp

EXTRACT_ALL = NO
EXTRACT_PRIVATE = NO
EXTRACT_STATIC = NO
HIDE_UNDOC_MEMBERS = NO
HIDE_UNDOC_CLASSES = NO

WARN_IF_UNDOCUMENTED = YES
WARN_IF_DOC_ERROR = YES
WARN_AS_ERROR = NO
QUIET = YES

GENERATE_HTML = NO
GENERATE_LATEX = NO
GENERATE_XML = YES
XML_OUTPUT = xml

ENABLE_PREPROCESSING = YES
MACRO_EXPANSION = YES
PREDEFINED = BNIO_EXPORT=
EOF

doxygen "${DOXYFILE}" > /dev/null 2> "${LOG}"

total=0
declare -a entries=()
if [[ -s "${LOG}" ]]; then
  while IFS= read -r line; do
    entries+=("${line}")
    total=$((total + 1))
  done < <(sort -u "${LOG}")
fi

{
  echo "## Documentation coverage report"
  echo
  if [[ ${total} -eq 0 ]]; then
    echo "All public headers and examples are fully documented."
  else
    echo "doxygen reported **${total}** documentation issue(s):"
    echo
    echo '```'
    for line in "${entries[@]}"; do
      echo "${line#${ROOT_DIR}/}"
    done
    echo '```'
  fi
  echo
  echo "> This report is advisory: it never fails the build."
  echo
} > "${REPORT}"

cat "${REPORT}"
exit 0
