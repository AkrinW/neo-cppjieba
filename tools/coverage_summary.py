#!/usr/bin/env python3
"""Summarize an llvm-cov lcov export as Markdown: totals, directories, weakest files and changed lines."""

import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WEAKEST_FILES = 12
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
CLOSING_BRACES = {"}", "};"}


@dataclass
class FileCoverage:
    """Union coverage of one file: a line or branch counts once any binary or instantiation reaches it."""

    lines: dict = field(default_factory=dict)
    branches: dict = field(default_factory=dict)

    def covered_lines(self):
        """Count executable lines with a nonzero hit count."""
        return sum(1 for hits in self.lines.values() if hits > 0)

    def taken_branches(self):
        """Count branch outcomes taken at least once."""
        return sum(1 for taken in self.branches.values() if taken)


def parse_lcov(lines, root):
    """Merge lcov records by repository-relative path; files outside root are skipped."""
    files = defaultdict(FileCoverage)
    current = None
    for line in lines:
        line = line.strip()
        if line.startswith("SF:"):
            path = Path(line[3:])
            current = files[path.relative_to(root).as_posix()] if path.is_relative_to(root) else None
        elif current is None:
            continue
        elif line.startswith("DA:"):
            number, hits = (int(value) for value in line[3:].split(",")[:2])
            current.lines[number] = max(current.lines.get(number, 0), hits)
        elif line.startswith("BRDA:"):
            number, block, branch, taken = line[5:].split(",")
            key = (int(number), block, branch)
            current.branches[key] = current.branches.get(key, False) or (taken != "-" and int(taken) > 0)
    return dict(files)


def credit_closing_braces(files, sources):
    """Count a zero-count line holding only a closing brace as covered when the executable line before it is.

    llvm-cov gives the unreachable end of a block that returns, such as a switch case, its own zero-count region.
    """
    for path, item in files.items():
        text = sources(path)
        previous = 0
        for number in sorted(item.lines):
            brace = number <= len(text) and text[number - 1].strip() in CLOSING_BRACES
            if item.lines[number] == 0 and previous > 0 and brace:
                item.lines[number] = previous
            previous = item.lines[number]


def read_source(path):
    """Source lines of a repository-relative path; a missing file has none."""
    file = ROOT / path
    return file.read_text(encoding="utf-8", errors="replace").splitlines() if file.is_file() else []


def parse_diff(text):
    """Map each file a unified diff with zero context adds lines to onto the new line numbers it adds."""
    changed = defaultdict(set)
    path = None
    for line in text.splitlines():
        if line.startswith("+++ "):
            target = line[4:]
            path = target[2:] if target.startswith("b/") else None
        elif path is not None and (hunk := HUNK.match(line)):
            start, count = int(hunk[1]), int(hunk[2] or 1)
            changed[path].update(range(start, start + count))
    return dict(changed)


def changed_lines(base, paths):
    """Lines the current checkout adds or edits in paths since its merge base with base."""
    diff = subprocess.check_output(["git", "-C", str(ROOT), "diff", "-U0", "--no-color", "--no-ext-diff",
                                    f"{base}...HEAD", "--", *paths], encoding="utf-8", errors="replace")
    return parse_diff(diff)


def line_ranges(numbers):
    """Compress sorted line numbers into ranges such as 3-5, 9."""
    ranges = []
    for number in sorted(numbers):
        if ranges and number == ranges[-1][1] + 1:
            ranges[-1][1] = number
        else:
            ranges.append([number, number])
    return ", ".join(str(first) if first == last else f"{first}-{last}" for first, last in ranges)


def ratio(covered, total):
    """Format a coverage ratio, or a dash when nothing is measurable."""
    return f"{100 * covered / total:.2f}% ({covered}/{total})" if total else "-"


def render_totals(files):
    """Totals over every reported file."""
    lines = sum(len(item.lines) for item in files.values())
    covered = sum(item.covered_lines() for item in files.values())
    branches = sum(len(item.branches) for item in files.values())
    taken = sum(item.taken_branches() for item in files.values())
    return ["| Measure | Coverage |", "| --- | ---: |", f"| Lines | {ratio(covered, lines)} |",
            f"| Branches | {ratio(taken, branches)} |"]


def render_changed(files, changed, base):
    """Executable lines the change adds or edits, and which of them no test reaches."""
    rows = []
    executable = covered = 0
    for path in sorted(set(changed) & set(files)):
        lines = files[path].lines
        touched = [number for number in changed[path] if number in lines]
        missed = [number for number in touched if lines[number] == 0]
        executable += len(touched)
        covered += len(touched) - len(missed)
        if missed:
            rows.append(f"| `{path}` | {line_ranges(missed)} |")
    out = ["### Changed lines", ""]
    if not executable:
        return out + [f"No executable lines in reported files change against `{base}`."]
    out.append(f"Against `{base}`: {ratio(covered, executable)} of changed executable lines are covered.")
    if rows:
        out += ["", "| File | Uncovered changed lines |", "| --- | --- |"] + rows
    return out


def render_directories(files):
    """Line and branch coverage per directory."""
    groups = defaultdict(list)
    for path, item in files.items():
        groups[Path(path).parent.as_posix()].append(item)
    out = ["### Directories", "", "| Directory | Lines | Branches |", "| --- | ---: | ---: |"]
    for directory, items in sorted(groups.items()):
        lines = ratio(sum(item.covered_lines() for item in items), sum(len(item.lines) for item in items))
        branches = ratio(sum(item.taken_branches() for item in items), sum(len(item.branches) for item in items))
        out.append(f"| `{directory}` | {lines} | {branches} |")
    return out


def render_weakest(files):
    """The files with the most uncovered lines, then the most untaken branches."""
    def missed(item):
        return len(item.lines) - item.covered_lines(), len(item.branches) - item.taken_branches()

    weakest = sorted((path for path in files if any(missed(files[path]))),
                     key=lambda path: (*(-count for count in missed(files[path])), path))[:WEAKEST_FILES]
    out = ["### Least covered files", ""]
    if not weakest:
        return out + ["Every reported line and branch is covered."]
    out += ["| File | Missed lines | Lines | Branches |", "| --- | ---: | ---: | ---: |"]
    for path in weakest:
        item = files[path]
        out.append(f"| `{path}` | {missed(item)[0]} | {ratio(item.covered_lines(), len(item.lines))} "
                   f"| {ratio(item.taken_branches(), len(item.branches))} |")
    return out


def render(files, changed, base):
    """The whole Markdown summary; changed lines appear only when a base is given."""
    out = ["## Coverage of include/neo and the C ABI adapter", "",
           "Report only; no threshold is enforced. A line or branch counts as covered when any test binary or "
           "template instantiation reaches it, and a closing-brace line when the line before it is covered. "
           "HTML and lcov reports are in the coverage artifact.", ""]
    out += render_totals(files) + [""]
    if base:
        out += render_changed(files, changed, base) + [""]
    out += render_directories(files) + [""] + render_weakest(files)
    return "\n".join(out) + "\n"


def run():
    """Write the summary of one lcov file to standard output."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lcov", type=Path, help="lcov file written by llvm-cov export")
    parser.add_argument("--base", default="", help="git revision to report changed-line coverage against")
    args = parser.parse_args()
    with args.lcov.open(encoding="utf-8") as lcov:
        files = parse_lcov(lcov, ROOT)
    credit_closing_braces(files, read_source)
    changed = changed_lines(args.base, sorted(files)) if args.base else {}
    sys.stdout.write(render(files, changed, args.base))


if __name__ == "__main__":
    run()
