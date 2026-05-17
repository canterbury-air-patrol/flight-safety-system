# syntax=docker/dockerfile:1

# ── Build stage ───────────────────────────────────────────────────────────────
FROM ubuntu:resolute AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential automake libtool pkg-config \
    libecpg-dev libjsoncpp-dev libgnutls28-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /code/
WORKDIR /code
RUN ./autogen.sh \
    && ./configure --prefix=/usr --enable-server --without-systemdsystemunitdir \
    && make -j"$(nproc)" \
    && make install DESTDIR=/install

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:resolute

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgnutls30t64 libjsoncpp26 libecpg6 \
    gnutls-bin jq \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /install/usr/sbin/fss-server /usr/sbin/fss-server
COPY --from=builder /install/usr/lib/ /usr/lib/
RUN ldconfig

RUN mkdir -p /cert /etc/fss
COPY docker/start-server.sh /usr/local/bin/start-server.sh
RUN chmod +x /usr/local/bin/start-server.sh

ENTRYPOINT ["/usr/local/bin/start-server.sh"]
