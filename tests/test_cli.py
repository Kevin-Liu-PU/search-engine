#!/usr/bin/env python3
"""Black-box checks for the CLI, using only Python's standard library."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]
BINARY = None


class SearchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fixture-search-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "site"
        self.root.mkdir()

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def run_cli(self, *args, root=None, expected=0):
        completed = subprocess.run(
            [str(BINARY), "--root", str(root or self.root), "--json", *args],
            capture_output=True, text=True, encoding="utf-8", timeout=10,
        )
        self.assertEqual(completed.returncode, expected, completed.stderr)
        if expected:
            self.assertTrue(completed.stderr.startswith("fixture-search:"))
            return completed.stderr
        return json.loads(completed.stdout)

    def paths(self, result):
        return [doc["path"] for doc in result["documents"]]

    def test_original_demo_and_determinism(self):
        args = ("--query", "soil water", "--query", "compost soil",
                "--query", "scriptghost", "--query", "styleghost", "--query", "commentghost")
        first = self.run_cli(*args, root=PROJECT / "fixtures")
        self.assertEqual(first, self.run_cli(*args, root=PROJECT / "fixtures"))
        self.assertEqual(self.paths(first), ["index.html", "guide/soil.html", "guide/water.html",
                                             "guide/compost.html", "calendar.html"])
        self.assertEqual([(h["path"], h["score"]) for h in first["queries"][0]["hits"]],
                         [("guide/soil.html", 7), ("guide/water.html", 6), ("index.html", 4)])
        self.assertEqual([(h["path"], h["score"]) for h in first["queries"][1]["hits"]],
                         [("guide/compost.html", 7), ("guide/soil.html", 7), ("index.html", 3)])
        self.assertTrue(all(not q["hits"] for q in first["queries"][2:]))
        self.assertEqual(first["stats"]["duplicate_links"], 7)
        self.assertEqual(first["stats"]["links_seen"], 13)

    def test_and_matching_case_duplicates_and_tie_order(self):
        self.write("index.html", '<p>alpha beta</p><a href="z.html">next</a><a href="a.html">next</a>')
        self.write("z.html", "alpha alpha beta")
        self.write("a.html", "alpha alpha beta")
        result = self.run_cli("--query", "ALPHA, beta", "--query", "beta alpha alpha",
                              "--query", "alpha absent", "--query", "---", "--query", "")
        hits = result["queries"][0]["hits"]
        self.assertEqual([(h["path"], h["score"]) for h in hits],
                         [("a.html", 3), ("z.html", 3), ("index.html", 2)])
        self.assertEqual(hits, result["queries"][1]["hits"])
        self.assertTrue(all(not q["hits"] for q in result["queries"][2:]))

    def test_html_subset_and_entities(self):
        self.write("index.html", '''<TITLE>Caf&#233; &amp; &quot;Tea&quot;</TITLE>
            <!-- commenttoken <a href="missing.html">ignore</a> -->
            <ScRiPt>scriptword <a href="missing.html">ignore</a></sCrIpT>
            <STYLE>styleword</STYLE><p>alpha&#32;beta &nbsp; gamma&#x20;delta</p>
            <A data-label="a > b" HREF='next.html?x=1&amp;y=2#section'>link</A>''')
        self.write("next.html", "alpha beta")
        result = self.run_cli("--query", "alpha beta gamma delta", "--query", "scriptword",
                              "--query", "styleword", "--query", "commenttoken")
        self.assertEqual(result["documents"][0]["title"], 'Caf\u00e9 & "Tea"')
        self.assertEqual(self.paths(result), ["index.html", "next.html"])
        self.assertEqual(result["stats"]["links_seen"], 1)
        self.assertEqual(result["queries"][0]["hits"][0]["score"], 4)
        self.assertTrue(all(not q["hits"] for q in result["queries"][1:]))

    def test_attribute_names_are_not_suffix_matched(self):
        self.write("index.html", '<a data.href="wrong.html" href="right.html">next</a>')
        self.write("wrong.html", "wrong")
        self.write("right.html", "right")
        result = self.run_cli()
        self.assertEqual(self.paths(result), ["index.html", "right.html"])

    def test_case_paths_follow_filesystem_identity(self):
        self.write("A.html", "upperword")
        try:
            with (self.root / "a.html").open("x", encoding="utf-8") as page:
                page.write("lowerword")
        except FileExistsError:
            self.write("index.html", '<a href="A.html">upper</a><a href="a.html">alias</a>')
            result = self.run_cli()
            self.assertEqual(self.paths(result), ["index.html", "A.html"])
            self.assertEqual(result["stats"]["duplicate_links"], 1)
            return
        outside = self.root.parent / "SITE"
        try:
            outside.mkdir()
        except FileExistsError:
            self.skipTest("fixture filesystem does not distinguish site from SITE")
        secret = outside / "secret.html"
        secret.write_text("secretword", encoding="utf-8")
        try:
            os.symlink(secret, self.root / "alias.html")
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink creation unavailable: {error}")
        self.write("index.html", '<a href="A.html">upper</a><a href="a.html">lower</a>'
                                '<a href="alias.html">outside</a>')
        result = self.run_cli("--query", "secretword")
        self.assertEqual(self.paths(result), ["index.html", "A.html", "a.html"])
        self.assertEqual(result["skipped"], [{"target": "alias.html", "reason": "outside_root"}])
        self.assertEqual(result["queries"][0]["hits"], [])

    def test_cycles_fragments_queries_and_root_relative_links(self):
        self.write("index.html", '<a href="guide/page.html">next</a><a href="#self">self</a>')
        self.write("guide/page.html", '<a href="/index.html?edition=2">home</a><a href="../index.html">home</a>')
        result = self.run_cli()
        self.assertEqual(self.paths(result), ["index.html", "guide/page.html"])
        self.assertEqual(result["stats"]["duplicate_links"], 3)

    def test_depth_limits_and_breadth_first_order(self):
        self.write("index.html", '<a href="a.html">a</a><a href="b.html">b</a>')
        self.write("a.html", '<a href="c.html">c</a>')
        self.write("b.html", '<a href="c.html">c</a>')
        self.write("c.html", "deep")
        self.assertEqual(self.paths(self.run_cli()), ["index.html", "a.html", "b.html", "c.html"])
        self.assertEqual(self.paths(self.run_cli("--max-depth", "0")), ["index.html"])
        bounded = self.run_cli("--max-depth", "1")
        self.assertEqual(self.paths(bounded), ["index.html", "a.html", "b.html"])
        self.assertEqual(bounded["skipped"], [{"target": "c.html", "reason": "depth_limit"}])

    def test_page_limit(self):
        self.write("index.html", '<a href="b.html">b</a><a href="a.html">a</a>')
        self.write("b.html", "beta")
        self.write("a.html", "alpha")
        result = self.run_cli("--max-pages", "2")
        self.assertEqual(self.paths(result), ["index.html", "b.html"])
        self.assertEqual(result["skipped"], [{"target": "a.html", "reason": "page_limit"}])

    def test_page_and_total_byte_limits(self):
        seed = '<a href="large.html">large</a><a href="small.html">small</a>'
        self.write("index.html", seed)
        self.write("large.html", "x" * 200)
        self.write("small.html", "small")
        page_bound = self.run_cli("--max-page-bytes", str(len(seed)))
        self.assertEqual(self.paths(page_bound), ["index.html", "small.html"])
        self.assertEqual(page_bound["skipped"][0]["reason"], "page_byte_limit")
        total_bound = self.run_cli("--max-total-bytes", str(len(seed) + 5))
        self.assertEqual(self.paths(total_bound), ["index.html", "small.html"])
        self.assertEqual(total_bound["skipped"][0]["reason"], "total_byte_limit")
        self.assertEqual(total_bound["stats"]["bytes_indexed"], len(seed) + 5)
        self.assertEqual(self.paths(self.run_cli("--max-page-bytes", "1")), [])

    def test_link_limit_counts_rejected_links(self):
        links = ['missing.html', 'https://example.invalid/', 'child.html']
        self.write("index.html", "".join(f'<a href="{url}">link</a>' for url in links))
        self.write("child.html", "child")
        result = self.run_cli("--max-links", "2")
        self.assertEqual(self.paths(result), ["index.html"])
        self.assertEqual(result["stats"]["links_seen"], 3)
        self.assertEqual(result["stats"]["links_omitted"], 1)
        self.assertEqual(len(result["skipped"]), 2)
        zero = self.run_cli("--max-links", "0")
        self.assertEqual(zero["stats"]["links_omitted"], 3)
        self.assertEqual(zero["skipped"], [])

    def test_many_links_remain_bounded(self):
        self.write("index.html", '<a href="missing.html">x</a>' * 2000)
        result = self.run_cli("--max-links", "5")
        self.assertEqual(result["stats"]["links_seen"], 2000)
        self.assertEqual(result["stats"]["links_omitted"], 1995)
        self.assertEqual(len(result["skipped"]), 5)

    def test_unsupported_urls_and_non_html(self):
        links = ['https://example.invalid/', '//example.invalid/a.html', 'file:///tmp/a.html',
                 'javascript:alert(1)', 'guide\\page.html', 'next%2ehtml', 'notes.txt', 'folder']
        self.write("index.html", "".join(f'<a href="{url}">link</a>' for url in links))
        self.write("notes.txt", "notindexed")
        (self.root / "folder").mkdir()
        result = self.run_cli("--query", "notindexed")
        self.assertEqual(self.paths(result), ["index.html"])
        self.assertEqual([s["reason"] for s in result["skipped"]],
                         ["unsupported_url"] * 6 + ["not_html", "not_regular_file"])
        self.assertEqual(result["queries"][0]["hits"], [])

    def test_parent_traversal_and_invalid_seed(self):
        (self.root.parent / "secret.html").write_text("secretword", encoding="utf-8")
        self.write("index.html", '<a href="../secret.html">a</a><a href="/../secret.html">b</a>')
        result = self.run_cli("--query", "secretword")
        self.assertEqual(self.paths(result), ["index.html"])
        self.assertEqual([s["reason"] for s in result["skipped"]], ["outside_root", "outside_root"])
        self.assertEqual(result["queries"][0]["hits"], [])
        self.assertIn("outside_root", self.run_cli("--start", "../secret.html", expected=2))

    def test_symlinks_cannot_escape_root(self):
        outside = self.root.parent / "outside"
        outside.mkdir()
        secret = outside / "secret.html"
        secret.write_text("secretword", encoding="utf-8")
        try:
            os.symlink(secret, self.root / "alias.html")
            os.symlink(outside, self.root / "escape", target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink creation unavailable: {error}")
        self.write("index.html", '<a href="alias.html">alias</a><a href="escape/secret.html">escape</a>')
        result = self.run_cli("--query", "secretword")
        self.assertEqual(self.paths(result), ["index.html"])
        self.assertEqual([s["reason"] for s in result["skipped"]], ["outside_root", "outside_root"])
        self.assertEqual(result["queries"][0]["hits"], [])

    def test_empty_file_and_title_fallback(self):
        self.write("index.html", "")
        result = self.run_cli("--query", "anything")
        self.assertEqual(result["documents"], [{"path": "index.html", "title": "index.html", "tokens": 0}])
        self.assertEqual(result["stats"]["bytes_indexed"], 0)
        self.assertEqual(result["queries"][0]["hits"], [])

    def test_malformed_entities_and_unclosed_script(self):
        self.write("index.html", "&" * 131072 + "<p>findable</p><script>hiddenword")
        result = self.run_cli("--query", "findable", "--query", "hiddenword")
        self.assertEqual(result["queries"][0]["hits"][0]["score"], 1)
        self.assertEqual(result["queries"][1]["hits"], [])

    def test_benchmark_checksum_scales_with_repetitions(self):
        self.write("index.html", "alpha alpha beta")
        args = ("--query", "alpha beta", "--query", "absent")
        two = self.run_cli(*args, "--benchmark", "2")["benchmark"]
        four = self.run_cli(*args, "--benchmark", "4")["benchmark"]
        self.assertEqual(two["checksum"], 6)
        self.assertEqual(four["checksum"], 12)
        self.assertEqual(two["queries_per_repetition"], 2)
        self.assertGreaterEqual(two["build_ms"], 0)
        self.assertGreaterEqual(two["query_ms"], 0)

    def test_invalid_options_and_roots(self):
        self.write("index.html", "seed")
        for value in ["-1", "0", "abc", "3.5", "999999999999999999999999999"]:
            with self.subTest(value=value):
                self.run_cli("--max-pages", value, expected=2)
        self.run_cli("--query", "seed", "--benchmark", "10001", expected=2)
        self.run_cli("--benchmark", "1", expected=2)
        self.run_cli("--bogus", expected=2)
        self.run_cli("--start", expected=2)
        self.run_cli(root=self.root / "absent", expected=2)
        self.run_cli(root=self.root / "index.html", expected=2)
        self.run_cli("--start", "missing.html", expected=2)

    @unittest.skipUnless(os.name == "posix" and Path("/dev/full").exists(),
                         "requires /dev/full")
    def test_output_write_failure(self):
        self.write("index.html", "alpha beta")
        with open("/dev/full", "wb") as full:
            result = subprocess.run(
                [str(BINARY), "--root", str(self.root), "--query", "alpha"],
                stdout=full, stderr=subprocess.PIPE, text=True, timeout=10,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot write stdout", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    if not BINARY.is_file():
        parser.error(f"binary does not exist: {BINARY}")
    unittest.main(argv=[__file__, *remaining], verbosity=2)
