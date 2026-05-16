#!/usr/bin/env bash

set -euo pipefail

clang-format --dry-run -Werror src/*.cpp src/*.hpp src/server-db.h tests/*.cpp tests/*.hpp examples/*.cpp

# knownConditionTrueFalse in transport-ssl.cpp is a false positive emitted by
# some cppcheck versions (notably the one on the CI runners); suppress it so
# this script behaves identically locally and in CI.
cppcheck --enable=warning,performance,portability,style --error-exitcode=1 \
	--suppress=knownConditionTrueFalse:src/transport-ssl.cpp src/*.cpp

run-clang-tidy -p . 'src/(?!server-db).*\.cpp$' 2>&1 | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log
