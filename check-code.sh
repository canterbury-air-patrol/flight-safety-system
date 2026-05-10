#!/usr/bin/env bash

set -euo pipefail

cppcheck --enable=warning,performance,portability,style --error-exitcode=1 src/*.cpp

run-clang-tidy -p . 'src/(?!server-db).*\.cpp$' 2>&1 | tee clang-tidy.log
! grep -q "warning:" clang-tidy.log
