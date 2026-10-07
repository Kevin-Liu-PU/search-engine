# Validation

Checks run on 2026-10-07 against the source in this repository, using the included synthetic fixtures and generated temporary corpora.

| Environment | Build | CLI suite |
| --- | --- | --- |
| Windows, UCRT64 GCC 15.2, Python 3.10.11 | C++17 with `-O2 -Wall -Wextra -Wpedantic`; no warnings | 18 passed, 1 skipped (`/dev/full` is unavailable); 0.751 s |
| Ubuntu under WSL, GCC 13.3, Python 3.12.3 | Same compiler flags; no warnings | 19 passed, no skips; 0.179 s |
| Windows, CMake 4.3.2, MSYS Makefiles, GCC 15.2, Python 3.12.10 | Release configure and build passed | CTest integration target passed; 0.63 s |

File and directory symlink containment checks passed on both operating systems. Linux also verified output failure using `/dev/full`. The initial Windows launch failed because a conflicting runtime DLL was found; putting the selected compiler's runtime directory first on `PATH` resolved it. The README includes that setup step.

The [demo output](evidence/demo.txt) contains five pages, 118 tokens, and 69 terms. The suite checks the exact query rankings and repeated-run JSON equality.

The timing script generated 200 synthetic pages plus a catalog and ran four queries 200 times. Both runs returned match counts of 200, 100, 14, and 0, with checksum `20937200`.

| Capture | Index build | 800 searches |
| --- | ---: | ---: |
| [Windows](evidence/benchmark-windows.json) | 821.279 ms | 1.525 ms |
| [Ubuntu](evidence/benchmark-ubuntu.json) | 1.616 ms | 1.182 ms |

These are single observations of the same implementation, not a platform comparison or a claim about web-scale performance. Filesystem overhead, cache state, antivirus, and load were not controlled. Each runtime used its own temporary filesystem.
