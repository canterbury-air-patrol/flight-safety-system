#!/usr/bin/env bash

set -euo pipefail

clang-format --dry-run -Werror src/*.cpp src/*.hpp src/server-db.h tests/*.cpp tests/*.hpp examples/*.cpp

cppcheck --enable=warning,performance,portability,style --error-exitcode=1 src/*.cpp

run-clang-tidy -p . 'src/(?!server-db).*\.cpp$' 2>&1 | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log
