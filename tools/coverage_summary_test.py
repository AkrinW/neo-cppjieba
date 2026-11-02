"""Behavior checks for the coverage summary: lcov merging, diff parsing and the Markdown it renders."""

from pathlib import Path
import unittest

import coverage_summary as summary

ROOT = Path("/repo")

LCOV = """\
SF:/repo/include/neo/A.hpp
DA:1,4
DA:2,0
DA:3,0
BRDA:2,0,0,3
BRDA:2,0,1,-
end_of_record
SF:/repo/include/neo/A.hpp
DA:2,5
BRDA:2,0,1,0
end_of_record
SF:/repo/api/src/b.cpp
DA:10,1
end_of_record
SF:/usr/include/c++/vector
DA:1,1
end_of_record
"""

DIFF = """\
diff --git a/include/neo/A.hpp b/include/neo/A.hpp
--- a/include/neo/A.hpp
+++ b/include/neo/A.hpp
@@ -1,0 +2,2 @@
+changed
+changed
@@ -9 +11 @@
-old
+new
@@ -20,3 +22,0 @@
-removed
diff --git a/old.txt b/old.txt
deleted file mode 100644
--- a/old.txt
+++ /dev/null
@@ -1 +0,0 @@
-gone
"""


class CoverageSummaryTest(unittest.TestCase):
    """Small hand-written lcov and diff inputs cover each rule the CI summary relies on."""

    def setUp(self):
        self.files = summary.parse_lcov(LCOV.splitlines(), ROOT)

    def test_records_for_one_file_merge_by_maximum_and_skip_outside_paths(self):
        self.assertEqual(sorted(self.files), ["api/src/b.cpp", "include/neo/A.hpp"])
        self.assertEqual(self.files["include/neo/A.hpp"].lines, {1: 4, 2: 5, 3: 0})

    def test_a_branch_counts_once_any_record_takes_it(self):
        branches = self.files["include/neo/A.hpp"].branches
        self.assertEqual(branches, {(2, "0", "0"): True, (2, "0", "1"): False})

    def test_closing_brace_after_a_covered_line_counts_as_covered(self):
        lcov = "SF:/repo/a.hpp\nDA:1,2\nDA:2,0\nDA:3,0\nDA:4,0\nDA:5,0\nend_of_record\n"
        files = summary.parse_lcov(lcov.splitlines(), ROOT)
        source = ["    return x;", "    }", "    call();", "    }", "};"]
        summary.credit_closing_braces(files, lambda path: source)
        self.assertEqual(files["a.hpp"].lines, {1: 2, 2: 2, 3: 0, 4: 0, 5: 0})

    def test_diff_maps_added_lines_and_ignores_pure_removals(self):
        self.assertEqual(summary.parse_diff(DIFF), {"include/neo/A.hpp": {2, 3, 11}})

    def test_line_ranges_compress_consecutive_numbers(self):
        self.assertEqual(summary.line_ranges([9, 3, 4, 5]), "3-5, 9")

    def test_changed_lines_count_only_executable_lines_and_list_misses(self):
        changed = summary.parse_diff(DIFF)
        text = "\n".join(summary.render_changed(self.files, changed, "origin/master"))
        self.assertIn("50.00% (1/2) of changed executable lines are covered", text)
        self.assertIn("| `include/neo/A.hpp` | 3 |", text)

    def test_changed_lines_without_executable_lines_say_so(self):
        text = "\n".join(summary.render_changed(self.files, {"docs/x.md": {1}}, "origin/master"))
        self.assertIn("No executable lines in reported files change against `origin/master`.", text)

    def test_totals_and_weakest_files_reflect_union_coverage(self):
        text = summary.render(self.files, {}, "")
        self.assertIn("| Lines | 75.00% (3/4) |", text)
        self.assertIn("| Branches | 50.00% (1/2) |", text)
        self.assertIn("| `include/neo/A.hpp` | 1 |", text)
        self.assertNotIn("### Changed lines", text)


if __name__ == "__main__":
    unittest.main()
