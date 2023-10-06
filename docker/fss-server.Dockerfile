FROM debian:bookworm

RUN apt update && apt upgrade -y
RUN apt install -y build-essential automake libtool pkg-config
RUN apt install -y libjsoncpp-dev libgnutlsxx30 libgnutls28-dev libecpg-dev gnutls-bin

COPY . /code/

WORKDIR /code

RUN ./autogen.sh && ./configure --prefix=/usr --enable-server --without-systemdsystemunitdir && make && make install

RUN mkdir -p /cert /etc/fss

ENTRYPOINT /code/docker/start-server.sh
