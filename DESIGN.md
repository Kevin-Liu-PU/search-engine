# Implementation notes

`src/search.cpp` contains HTML extraction, local traversal, and the inverted index. `src/search.hpp` exposes the result types. `src/main.cpp` handles CLI options, text/JSON output, and timing.

Traversal uses a queue and canonical path identities to visit pages breadth first and deduplicate cycles. Only regular HTML files within the root are accepted. On Windows, handle-based path resolution follows intermediate directory symlinks. Relative links resolve from the source page; a leading slash means the fixture root. Schemes, protocol-relative URLs, backslashes, and percent-encoded paths are rejected. Query strings and fragments are removed.

The defaults are 64 pages, depth 8, 262,144 bytes per page, 4,194,304 total accepted bytes, and 4,096 considered links. A rejected or duplicate link still consumes the link budget. Oversized pages are skipped whole. These limits bound inputs and bookkeeping, not operating-system I/O or peak memory.

Each term maps to a sorted posting list of `(document ID, frequency)` pairs. IDs increase as pages are indexed. A query deduplicates its words, starts with the shortest posting list, and intersects the others. Ranking sums term counts and breaks ties by relative path. For posting lengths `p1..pk` and `h` hits, intersections take `O(sum(pi))` work and ranking takes `O(h log h)`, in addition to query normalization. Storage grows with document metadata and distinct document-term pairs; raw page text is discarded after indexing.

Extraction recognizes ordinary tags, quoted/unquoted anchor links, titles, comments, and script/style blocks. It decodes `amp`, `lt`, `gt`, `quot`, `apos`, `nbsp`, and numeric entities when their semicolon is within 12 characters of `&`. Tags separate tokens. It does not execute JavaScript, render CSS, honor `<base>`, infer directory indexes, or repair malformed HTML like a browser.

The corpus must remain unchanged during a run because path checks and file reads can race concurrent renames. Path containment preserves case; a differently cased lexical root prefix may be conservatively rejected on a case-insensitive filesystem. Distinct-case NTFS directories and Windows extended-length paths are not verified. The tool expects UTF-8 and does not validate the encoding before JSON output.
