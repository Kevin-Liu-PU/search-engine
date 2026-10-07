#!/usr/bin/env python3
"""Generate a bounded corpus and measure one index build plus repeated queries."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def bounded_number(low, high):
    def parse(value):
        number = int(value)
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return number
    return parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--pages", type=bounded_number(1, 2000), default=200)
    parser.add_argument("--repetitions", type=bounded_number(1, 10000), default=200)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary does not exist: {binary}")
    queries = ["garden", "garden amber", "birch cedar", "amber absent"]
    with tempfile.TemporaryDirectory(prefix="fixture-search-bench-") as directory:
        root = Path(directory)
        links = "".join(f'<a href="page-{i:04d}.html">entry</a>' for i in range(args.pages))
        (root / "index.html").write_text(f"<title>Catalog</title>{links}", encoding="utf-8")
        for i in range(args.pages):
            terms = ["garden"] * (1 + i % 5)
            for divisor, term in [(2, "amber"), (3, "birch"), (5, "cedar")]:
                if i % divisor == 0:
                    terms.append(term)
            body = " ".join(terms)
            (root / f"page-{i:04d}.html").write_text(
                f"<title>Entry {i:04d}</title><p>{body}</p><a href='index.html'>catalog</a>",
                encoding="utf-8",
            )
        command = [str(binary), "--root", str(root), "--json", "--max-pages", str(args.pages + 1),
                   "--max-links", str(args.pages * 2), "--benchmark", str(args.repetitions)]
        for query in queries:
            command.extend(["--query", query])
        completed = subprocess.run(command, check=True, capture_output=True,
                                   text=True, encoding="utf-8", timeout=60)
        result = json.loads(completed.stdout)
        expected_matches = [args.pages, (args.pages + 1) // 2, (args.pages + 14) // 15, 0]
        matches = [len(q["hits"]) for q in result["queries"]]
        if result["stats"]["pages"] != args.pages + 1 or matches != expected_matches or result["skipped"]:
            raise RuntimeError("generated corpus validation failed")
        print(json.dumps({
            "corpus_pages": args.pages + 1,
            "stats": result["stats"],
            "queries": [{"query": q, "matches": n} for q, n in zip(queries, matches)],
            "benchmark": result["benchmark"],
        }, indent=2))


if __name__ == "__main__":
    main()
