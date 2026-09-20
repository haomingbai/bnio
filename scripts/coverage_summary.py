#!/usr/bin/env python3
"""Generate a non-gating line-coverage summary from gcovr Cobertura XML."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cobertura",
        type=Path,
        required=True,
        help="Cobertura XML file produced by gcovr",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def collect_coverage(cobertura: Path) -> list[dict]:
    tree = ET.parse(cobertura)
    root = tree.getroot()

    rows = []
    for cls in root.iter("class"):
        source = cls.get("filename")
        lines_node = cls.find("lines")
        if source is None or lines_node is None:
            continue
        # gcovr does not put per-class line counts in class attributes; they
        # live in the per-line hits.
        total = 0
        covered = 0
        for line in lines_node.iter("line"):
            total += 1
            if int(line.get("hits", "0")) > 0:
                covered += 1
        rows.append(
            {
                "file": Path(source).as_posix(),
                "covered_lines": covered,
                "total_lines": total,
                "line_coverage": (covered / total) if total > 0 else 0.0,
            }
        )

    rows.sort(key=lambda row: row["file"])
    return rows


def write_reports(rows: list[dict], output_dir: Path) -> None:
    if not rows:
        raise RuntimeError("Cobertura XML contained no per-file classes")

    total_lines = sum(row["total_lines"] for row in rows)
    covered_lines = sum(row["covered_lines"] for row in rows)
    line_coverage = (covered_lines / total_lines) if total_lines > 0 else 0.0

    report = {
        "covered_lines": covered_lines,
        "total_lines": total_lines,
        "line_coverage": line_coverage,
        "files": rows,
    }
    (output_dir / "coverage-summary.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    markdown = [
        "## Coverage report",
        "",
        (
            f"Line coverage: **{covered_lines}/{total_lines} "
            f"({line_coverage:.2%})**"
        ),
        "",
        "> Coverage is informational and has no pass/fail threshold.",
        "",
        "| File | Covered | Total | Coverage |",
        "| --- | ---: | ---: | ---: |",
    ]
    for row in rows:
        markdown.append(
            f"| `{row['file']}` | {row['covered_lines']} | "
            f"{row['total_lines']} | {row['line_coverage']:.2%} |"
        )
    markdown.append("")
    (output_dir / "coverage-summary.md").write_text(
        "\n".join(markdown), encoding="utf-8"
    )


def main() -> int:
    args = parse_args()
    cobertura = args.cobertura.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        if not cobertura.is_file():
            raise RuntimeError(f"no Cobertura XML at {cobertura}")
        rows = collect_coverage(cobertura)
        write_reports(rows, output_dir)
    except (OSError, ET.ParseError, RuntimeError) as error:
        print(f"coverage summary generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
