# Flight-Safety-System

[![C/C++ CI](https://github.com/canterbury-air-patrol/flight-safety-system/actions/workflows/c-cpp.yml/badge.svg?branch=develop)](https://github.com/canterbury-air-patrol/flight-safety-system/actions/workflows/c-cpp.yml)
[![codecov](https://codecov.io/gh/canterbury-air-patrol/flight-safety-system/branch/develop/graph/badge.svg)](https://codecov.io/gh/canterbury-air-patrol/flight-safety-system)

Flight-Safety-System is a system for maintaining control of RPAS.

FSS provides a means of sending basic commands (RTL, Hold, Resume, etc) to an aircraft while monitoring the position, battery status, and mission progress.

See [CHANGELOG.md](CHANGELOG.md) for a full history of changes.

## Basic Setup

### Dependencies

Dependencies are jsoncpp (load/read the config), gnutls/gnutlsxx (for secure connections), ecpg (for the server), and catch2 (for tests).

Building from source also requires autoconf, automake, and libtool to generate the build system.

On Debian/Ubuntu:
```
apt install autoconf automake libtool libjsoncpp-dev libgnutls28-dev libecpg-dev catch2
```

### Build/Install

You can build this package from source:
```
git clone https://github.com/canterbury-air-patrol/flight-safety-system.git
cd flight-safety-system
./autogen.sh
./configure --enable-server
make
make install
```

### Running the Server

The flight-safety-system server uses a [postgresql](https://www.postgresql.org/)+[postgis](https://postgis.net/) database for storing configuration, commands, and recording historic data.

The [web frontend](https://github.com/canterbury-air-patrol/flight-safety-system-web/) is a separate project and will need to be set up and connected to the database before the server is run.

Create a [server.json](examples/server.json) file with the correct port and database settings.

Then start the server:
```
fss-server server.json
```

#### Signal handling

| Signal  | Effect |
|---------|--------|
| SIGTERM | Graceful shutdown — stops accepting connections, drains the write queue, and exits cleanly. Compatible with `systemctl stop` and `docker stop`. |
| SIGINT  | Same as SIGTERM. |
| SIGHUP  | Reloads the CRL file (if configured) and disconnects any currently connected clients whose certificates appear on the updated CRL. No restart required. |

#### Supported scale

The server is sized for RPAS fleets of tens of aircraft against a single local PostgreSQL+PostGIS database. Command dispatch runs from in-memory caches on the main loop; the poller performs one batched database read per tick for the newest pending command across all connected aircraft, so the steady-state command-read rate is a constant (~10 queries/second) rather than growing with the number of aircraft. Each connection uses a small, fixed number of threads. Deployments approaching hundreds of concurrent aircraft should re-evaluate the single read-connection and per-connection thread model first; see [docs/decisions/23](docs/decisions/23-supported-scale-assumption.md) for what sets the ceiling and the order in which to raise it.

### Client

There is no full client implementation shipped with flight-safety-system, however there is a [library](src/fss-client-ssl.hpp) to use and an [example client](examples/fake_client.cpp) that can be used as a starting point.

## Redundancy

Redundancy is available by running multiple independent servers; the normal client configuration allows for specifying multiple servers to connect to.

Each server can also be configured with a list of all known servers, and this information is provided to clients periodically so they can discover and connect to all servers automatically.

## SSL Support

TLS is required to protect the connection between clients and servers. A shared CA is needed so that clients and servers can verify each other by certificate. The server uses the client certificate's Common Name as the asset identity.

### Generating certificates

Scripts are provided in the `certs` directory to help generate and sign certificates.

Generate a CA certificate:
```
cd certs
./generate-ca.sh
```
Keep `ca.private.pem` in a safe place — it is needed to sign all server and client certificates. If a certificate signed by this CA is compromised, you will need to either revoke it (see below) or recreate the CA and regenerate all certificates.

Generate a certificate for each server:
```
cd certs
./generate-server.sh server1.my.domain
```
Use the IP address or DNS name that clients will connect to. Clients verify both that the server is trusted and that they have connected to the correct host.

Generate a certificate for each client:
```
cd certs
./generate-client.sh client-name
```
Use the name of the client as enrolled in the database. The server matches the certificate CN against the asset registry to authenticate the client.

### Certificate revocation (CRL)

To revoke a certificate without replacing the CA, add a `crl_file` entry to `server.json`:
```json
"ssl": {
    "ca_public_key": "certs/ca.public.pem",
    "server_private_key": "certs/server.private.pem",
    "server_public_key": "certs/server.public.pem",
    "crl_file": "certs/revoked.crl"
}
```
Send `SIGHUP` to a running server to reload the CRL and immediately disconnect any clients whose certificates appear on it. No restart is required.

## Other software

Primarily flight-safety-system is designed to run alongside [Search Management Map](https://github.com/canterbury-air-patrol/search-management-map/).

[Canterbury Air Patrol](http://www.canterburyairpatrol.org) has a [client](https://github.com/canterbury-air-patrol/fss-smm-mav) that integrates Flight-Safety-System and Search Management Map to control an aircraft running [ArduPilot](https://www.ardupilot.org).

There is an [ADS-B Integration](https://github.com/canterbury-air-patrol/fss-adsb/) that allows position reports from ADS-B Out aircraft to be relayed to FSS clients.

## Design record

Settled design decisions — including the alternatives that were considered and
rejected, and the evidence behind each rejection — are recorded in
[docs/](docs/README.md). Start there before changing protocol semantics, the
database seam, or the fail-safe behaviour.

## Release strategy

Stable releases are cut from long-lived `release/X.Y` branches. The `develop` branch carries ongoing work and merges into a `release/X.Y` branch when a release is prepared. Bug fixes are applied to `develop` first and then cherry-picked to the relevant `release/X.Y` branch, resulting in patch releases (`X.Y.1`, `X.Y.2`, etc.). The `master` branch always points to the latest stable release.

In short:

- `master` — latest stable release
- `release/X.Y` — maintenance branch for the X.Y line; source of all X.Y.Z tags
- `develop` — integration branch for the next release

## License

This project is licensed under GNU GPLv2 — see the [LICENSE](LICENSE.md) file for details.
