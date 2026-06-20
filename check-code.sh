#!/usr/bin/env bash

set -euo pipefail

# clang-format output is not stable across major versions. The tree is
# formatted with clang-format 22.1.x; CI pins that exact version and points
# CLANG_FORMAT at it. Locally, set CLANG_FORMAT if your system clang-format
# is a different major version.
clang_format="${CLANG_FORMAT:-clang-format}"
"$clang_format" --dry-run -Werror src/*.cpp src/*.hpp src/server-db.h tests/*.cpp tests/*.hpp examples/*.cpp

# knownConditionTrueFalse in transport-ssl.cpp is a false positive emitted by
# some cppcheck versions (notably the one on the CI runners); suppress it so
# this script behaves identically locally and in CI.
cppcheck --enable=warning,performance,portability,style --error-exitcode=1 \
	--suppress=knownConditionTrueFalse:src/transport-ssl.cpp src/*.cpp

run-clang-tidy -p . 'src/(?!server-db).*\.cpp$' 2>&1 | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log

# Lint the e2e Python harness. ruff's rule set is version-dependent, so CI pins
# the exact version in a venv and points RUFF at it (see e2e/requirements-dev.txt).
# Locally, install that pin into a venv and set RUFF, or rely on a ruff on PATH.
ruff="${RUFF:-ruff}"
"$ruff" check e2e/
