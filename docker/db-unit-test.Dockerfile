# syntax=docker/dockerfile:1
#
# Builds the unit-test binary with coverage instrumentation and runs all
# tests against a PostgreSQL+PostGIS database reachable via the test-net
# Docker network.  Use docker-compose.db-unit-tests.yaml to orchestrate.

FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential automake libtool pkg-config \
    libecpg-dev libjsoncpp-dev libgnutls28-dev gnutls-bin \
    catch2 postgresql-client lcov \
    && rm -rf /var/lib/apt/lists/*

COPY . /code/
WORKDIR /code

RUN ./autogen.sh \
    && ./configure --enable-tests --enable-server --enable-coverage \
    && make -j"$(nproc)" \
    && make -j"$(nproc)" -C tests all_test

WORKDIR /code/tests

ENTRYPOINT ["/code/docker/db-unit-test-entrypoint.sh"]
