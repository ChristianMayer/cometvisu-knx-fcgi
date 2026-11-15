#!/usr/bin/env python3
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Coverage gate: every executable line and every branch arm needs a test.

Reads the gcov counters produced by a ``-DENABLE_COVERAGE=ON`` build and fails
when any line in ``src/`` was never executed or any branch arm was never taken
(including "the other side of an if").

Lines that cannot be exercised in-process are marked in the source:

    initialize_logging();   // GCOV_EXCL_LINE      line is never required
    if (fd < 0) { ... }     // GCOV_EXCL_BR_LINE   branches of this line optional
    // GCOV_EXCL_START
    ...fork()-only code...
    // GCOV_EXCL_STOP

Markers are only acceptable for code that a test process genuinely cannot
reach: fork() children that exec/exit, signal handlers, and defensive paths
for OS failures that cannot be injected.  Everything else needs a test.

Usage:
    tools/check_coverage.py                    # check, fail on any gap
    tools/check_coverage.py --report           # also print every file
    tools/check_coverage.py --list-uncovered   # show line numbers / branches
    tools/check_coverage.py --min-branch 90    # looser gate while closing gaps
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class FileCoverage:
    path: Path
    lines_total: int = 0
    lines_hit: int = 0
    branches_total: int = 0
    branches_hit: int = 0
    uncovered_lines: list[int] = field(default_factory=list)
    uncovered_branches: list[int] = field(default_factory=list)

    @property
    def line_pct(self) -> float:
        return 100.0 * self.lines_hit / self.lines_total if self.lines_total else 100.0

    @property
    def branch_pct(self) -> float:
        return 100.0 * self.branches_hit / self.branches_total if self.branches_total else 100.0


EXCL_LINE = re.compile(r"GCOV_EXCL_LINE\b(?!_)")
EXCL_BR_LINE = re.compile(r"GCOV_EXCL_BR_LINE")
EXCL_START = re.compile(r"GCOV_EXCL_START")
EXCL_STOP = re.compile(r"GCOV_EXCL_STOP")


def marker_maps(source: Path) -> tuple[set[int], set[int]]:
    """Return (excluded_lines, branch_optional_lines) from the source markers."""
    excluded: set[int] = set()
    branch_optional: set[int] = set()
    inside = False
    try:
        text = source.read_text(errors="replace").splitlines()
    except OSError:
        return excluded, branch_optional
    for number, line in enumerate(text, start=1):
        if EXCL_START.search(line):
            inside = True
            excluded.add(number)
            continue
        if EXCL_STOP.search(line):
            excluded.add(number)
            inside = False
            continue
        if inside:
            excluded.add(number)
            continue
        if EXCL_LINE.search(line):
            excluded.add(number)
        if EXCL_BR_LINE.search(line):
            branch_optional.add(number)
    return excluded, branch_optional


def gcov_json(gcno: Path) -> dict:
    result = subprocess.run(
        [  # noqa: S603 - fixed tool, no shell
            "gcov",
            "--json-format",
            "--branch-probabilities",
            "--stdout",
            "--object-directory",
            str(gcno.parent),
            str(gcno),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError(f"gcov failed for {gcno}: {result.stderr.strip()}")
    return json.loads(result.stdout)


def collect(build_dir: Path, source_root: Path) -> dict[Path, FileCoverage]:
    gcno_files = sorted(build_dir.rglob("*.gcno"))
    if not gcno_files:
        raise SystemExit(
            f"no .gcno files under {build_dir} — configure with -DENABLE_COVERAGE=ON, "
            "build and run the test suite first"
        )

    coverages: dict[Path, FileCoverage] = {}
    for gcno in gcno_files:
        data = gcov_json(gcno)
        for entry in data.get("files", []):
            path = Path(entry["file"])
            try:
                path.relative_to(source_root)
            except ValueError:
                continue  # system headers, gtest, ...
            if path.suffix not in (".cpp", ".h"):
                continue
            if "src" != path.relative_to(source_root).parts[0]:
                continue  # only production code is gated

            excluded, branch_optional = marker_maps(path)
            coverage = coverages.setdefault(path, FileCoverage(path=path))
            for line in entry.get("lines", []):
                number = line["line_number"]
                if line.get("count", 0) == 0:
                    if number in excluded:
                        continue
                    coverage.lines_total += 1
                    coverage.uncovered_lines.append(number)
                elif number not in excluded:
                    coverage.lines_total += 1
                    coverage.lines_hit += 1

                if number in excluded or number in branch_optional:
                    continue
                for branch in line.get("branches", []):
                    # Exception-unwind arms only trigger on allocation/throw
                    # failures and are not reachable from a test process.
                    if branch.get("throw"):
                        continue
                    coverage.branches_total += 1
                    if branch.get("count", 0) > 0:
                        coverage.branches_hit += 1
                    else:
                        coverage.uncovered_branches.append(number)
    return coverages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-cov")
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--min-line", type=float, default=100.0)
    parser.add_argument("--min-branch", type=float, default=100.0)
    parser.add_argument("--only", default="", help="restrict to paths containing this text")
    parser.add_argument(
        "--baseline",
        default="",
        help="JSON with per-file line/branch percentages; files not listed must be at 100%%, "
        "listed files must not regress below their recorded value",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="ignore the baseline: every file must reach --min-line/--min-branch",
    )
    parser.add_argument(
        "--write-baseline",
        default="",
        help="record the current numbers into this JSON file and exit 0",
    )
    parser.add_argument("--report", action="store_true", help="print every file, not only failures")
    parser.add_argument(
        "--list-uncovered", action="store_true", help="print uncovered line numbers and branches"
    )
    args = parser.parse_args()

    if shutil.which("gcov") is None:
        print("gcov not found — install gcc", file=sys.stderr)
        return 2

    source_root = Path(args.source_root).resolve()
    coverages = collect(Path(args.build_dir).resolve(), source_root)
    if not coverages:
        print("no coverage data for src/ — did the tests run?", file=sys.stderr)
        return 2

    baseline: dict[str, dict[str, float]] = {}
    if args.baseline and not args.strict:
        baseline = json.loads(Path(args.baseline).read_text())

    if args.write_baseline:
        recorded = {
            str(path.relative_to(source_root)): {
                "line": round(coverage.line_pct, 4),
                "branch": round(coverage.branch_pct, 4),
            }
            for path, coverage in sorted(coverages.items())
        }
        Path(args.write_baseline).write_text(json.dumps(recorded, indent=2, sort_keys=True) + "\n")
        print(f"recorded {len(recorded)} file(s) into {args.write_baseline}")
        return 0

    failures: list[FileCoverage] = []
    rows = []
    for path, coverage in sorted(coverages.items()):
        relative = path.relative_to(source_root)
        if args.only and args.only not in str(relative):
            continue
        # A file recorded in the baseline may not regress; every other file
        # (i.e. anything new) has to be covered completely.
        entry = baseline.get(str(relative))
        min_line = entry["line"] if entry else args.min_line
        min_branch = entry["branch"] if entry else args.min_branch
        # Small epsilon: the baseline stores rounded percentages.
        ok = coverage.line_pct + 1e-3 >= min_line and coverage.branch_pct + 1e-3 >= min_branch
        if entry is None:
            ok = ok and not coverage.uncovered_lines and not coverage.uncovered_branches
        rows.append((relative, coverage, ok))
        if not ok:
            failures.append(coverage)

    width = max(len(str(row[0])) for row in rows) if rows else 10
    print(f"{'file':<{width}}  {'lines':>13}  {'branches':>13}")
    print(f"{'-' * width}  {'-' * 13}  {'-' * 13}")
    for relative, coverage, ok in rows:
        if not ok and not args.report:
            continue
        print(
            f"{str(relative):<{width}}  "
            f"{coverage.lines_hit:>5}/{coverage.lines_total:<5} "
            f"{coverage.line_pct:5.1f}%  "
            f"{coverage.branches_hit:>5}/{coverage.branches_total:<5} "
            f"{coverage.branch_pct:5.1f}%"
        )

    if failures and args.list_uncovered:
        print("\nuncovered code:")
        for coverage in failures:
            relative = coverage.path.relative_to(source_root)
            if coverage.uncovered_lines:
                numbers = " ".join(str(n) for n in sorted(set(coverage.uncovered_lines)))
                print(f"  {relative}: lines {numbers}")
            if coverage.uncovered_branches:
                numbers = " ".join(str(n) for n in sorted(set(coverage.uncovered_branches)))
                print(f"  {relative}: branch arms at lines {numbers}")

    if failures:
        print(
            f"\nFAIL: {len(failures)} file(s) with uncovered code "
            f"(gate: {args.min_line:.0f}% lines, {args.min_branch:.0f}% branches)"
        )
        return 1

    print(f"\nOK: {len(rows)} file(s) at or above the required coverage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
