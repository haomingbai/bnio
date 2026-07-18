#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v doxygen >/dev/null 2>&1; then
  echo "doxygen not found" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

DOXYFILE="${WORK_DIR}/Doxyfile"
OUTPUT_DIR="${WORK_DIR}/out"

cat >"${DOXYFILE}" <<EOF
PROJECT_NAME = bnio
OUTPUT_DIRECTORY = "${OUTPUT_DIR}"

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
WARN_AS_ERROR = YES
QUIET = YES

GENERATE_HTML = NO
GENERATE_LATEX = NO
GENERATE_XML = YES
XML_OUTPUT = xml

ENABLE_PREPROCESSING = YES
MACRO_EXPANSION = YES
PREDEFINED = BNIO_EXPORT=
EOF

doxygen "${DOXYFILE}"
