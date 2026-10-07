# search-engine

A small C++17 search tool for a folder of linked HTML files. It follows local links, builds an inverted index, and answers AND queries with deterministic rankings.

The bundled garden pages and all generated test data are synthetic.

## What it does

- Traverses reachable `.html` and `.htm` files breadth first, with page, depth, byte, and link limits.
- Extracts text and titles, skips comments and script/style contents, and handles a small set of HTML entities.
- Indexes lowercase ASCII words and numbers as sorted postings of document IDs and term counts.
- Intersects postings for every distinct query word. Results sort by summed term frequency, then path.

There is no network fetching. Each invocation builds an in-memory index; nothing is persisted.

## Demo

[Watch the terminal demo](media/demo.mp4).

## Build and try it

Requires a C++17 compiler with filesystem support. Tests and the timing script use Python 3 and its standard library.

On Linux, macOS, or WSL, from the repository root:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic src/main.cpp src/search.cpp -o fixture-search
./fixture-search --root fixtures --query "soil water" --query "compost soil"
python3 tests/test_cli.py --binary ./fixture-search
```

On Windows with GCC and Python available on `PATH`:

```powershell
$env:PATH = (Split-Path (Get-Command g++).Source) + ';' + $env:PATH
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic src/main.cpp src/search.cpp -o fixture-search.exe
.\fixture-search.exe --root fixtures --query "soil water"
python tests/test_cli.py --binary fixture-search.exe
```

The first Windows command puts the compiler's runtime DLL directory first on `PATH`. CMake is also supported with an installed build tool:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `soil water` query returns:

```text
7  guide/soil.html
6  guide/water.html
4  index.html
```

The complete run is in [evidence/demo.txt](evidence/demo.txt). The external and missing links in the fixtures intentionally exercise skipped-link reporting. Add `--json` for machine-readable output, or `--help` for limits and options.

## Tests and timing

The CLI suite covers query matching, ranking, repeatability, HTML extraction, traversal bounds, symlink containment, invalid arguments, and timing checksums. Platform-dependent tests report explicit skips. See [VALIDATION.md](VALIDATION.md) for the verified runs.

```sh
python3 tools/benchmark.py --binary ./fixture-search --pages 200 --repetitions 200
```

This generates 200 synthetic pages plus a catalog, checks expected match counts, and times one index build and 800 queries. It measures this single implementation; it does not compare alternative data structures. Elapsed times vary with the machine and filesystem.

## Limits

The HTML parser implements a small subset, not full browser parsing. Tokens are ASCII only; there is no stemming, phrase search, or language-aware ranking. Titles and anchor text contribute to term counts. Unknown entities remain literal, and malformed markup can truncate extraction.

Use stable local files while a search runs. Canonical-path checks reject links outside the root, but validation and reads are separate operations, so this is not a hardened filesystem sandbox. Input bounds are not a hard memory cap. UTF-8 is expected, and portable ASCII filenames are recommended on Windows.

[DESIGN.md](DESIGN.md) describes the implementation and its tradeoffs.
