#!/usr/bin/env bash
# Entrypoint for the db-unit-test container.
#
# Pre-populates the test database with fixtures that the DB unit tests
# expect to find, then runs the full test binary.  The Docker healthcheck
# on the db service guarantees Postgres is accepting connections before
# this script runs, so no additional wait loop is needed.
#
# The fixtures live in tests/db-fixtures.sql rather than inline here because
# CI's coverage job seeds the same rows to run this suite against a service
# container; two copies would drift.
set -euo pipefail

PGPASSWORD="${TEST_DB_PASS:-password}" psql \
    -h "${TEST_DB_HOST:-db}" \
    -p "${TEST_DB_PORT:-5432}" \
    -U "${TEST_DB_USER:-postgres}" \
    -d "${TEST_DB_NAME:-postgres}" \
    -f /code/tests/db-fixtures.sql

exec /code/tests/all_test "$@"
