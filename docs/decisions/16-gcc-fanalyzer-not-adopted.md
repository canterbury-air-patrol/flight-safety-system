# 16 — GCC `-fanalyzer` evaluated and not adopted

**Measured 2026-06-13; not adopted.** This record exists so the question is not
re-litigated from first principles.

## What was measured

The full project was built with `-fanalyzer` on the CI-relevant compilers
(Ubuntu 24.04 containers, gcc-13.3 and gcc-14.2) and locally on gcc-16.1.

**gcc-13 / gcc-14: ~52 analyzer warnings, all false positives.**

- 41 are locationless `cc1plus: warning: use of uninitialized value
  '<unknown>'` — analyzer-internal noise with no source location, and therefore
  not suppressible by a source pragma.
- ~10 are `-Wanalyzer-use-of-uninitialized-value` where the analyzer loses track
  of `std::shared_ptr` / `std::function` contents while iterating the
  `snapshotClients()` vectors in `server-clients.hpp`, and in the gnutls session
  handling in `transport-ssl.cpp`.
- 1 is `-Wanalyzer-malloc-leak` flagging `credentials(new ...)` in the
  `transport_ssl` constructor — a `std::unique_ptr` member freed in the
  destructor; the analyzer does not model smart-pointer ownership through the
  gnutls C++ wrapper.

**gcc-16: 6 warnings** — the same `shared_ptr` false positives. Newer gcc
suppresses more of the noise but adds nothing of value.

**Zero** warnings in `server-db.c`, the generated ECPG C file that is excluded
from clang-tidy and cppcheck, and the one place the analyzer would actually be
trustworthy. That was the only potential payoff, and it found nothing.

## Why not adopted

- The project builds with `-Werror`, so a gating job is red on day one.
- Suppressing the noise means disabling
  `-Wanalyzer-use-of-uninitialized-value` — which is also the category most
  likely to find a *real* uninitialized-value bug. Gating after that suppression
  provides almost nothing.
- An informational, non-gating job would emit ~52 noise lines per run that
  nobody reads.
- The warning set varies by gcc version, so any suppression list is fragile.
- `-fanalyzer` is mature on C but still false-positive-prone on C++/STL, and
  this codebase is almost entirely C++.

## What already covers this ground

clang-tidy and cppcheck (`check-code.sh`), ASan+UBSan, ThreadSanitizer,
valgrind, and CodeQL. These found and gated the real defects during the 1.0.3
work; `-fanalyzer` adds nothing they do not.

## If revisited

Only worth reconsidering if either:

- a future gcc materially cuts the C++ false-positive rate; or
- substantial new hand-written **C** lands — the analyzer's strong suit — in
  which case gate `-fanalyzer` on just those C translation units, not the C++
  tree.
