# Flight-Safety-System

Flight-Safety-System is a system for maintaining control of RPAS. 

FSS provides a means of sending basic commands (RTL, Hold, Resume, etc) to an aircraft while monitoring the position, battery status, and mission progress.

## Basic Setup
#### Dependencies
Dependencies are jsoncpp, ecpg (for the server), catch (for tests).

On debian/ubuntu you will need to `apt install libjsoncpp-dev ecpg-dev catch`
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
 
The [web frontend](https://github.com/canterbury-air-patrol/flight-safety-system-web/) is a separate project and will need to be setup and have been connected to the database before the server is run.

Create a [server.json](examples/server.json) file with the correct port and database settings.

Then start the server `fss-server server.json`

### Client
There is no full client implementation shipped with flight-safety-system, however there is a [library](src/fss-client.hpp) to use and an [example client](examples/fake_client.cpp) that can be used as a starting point.

## Redundancy
Redundancy is available by running multiple independent servers, the normal client configuration allows for specifying multiple servers to connect to. 

Also, each server can include configuration of all known servers and this information will be provided to clients periodically to allow them to learn about and connect to all of the servers.

## Other software
Primarily flight-safety-system is designed to run alongside [Search Management Map](https://github.com/canterbury-air-patrol/search-management-map/)

[Canterbury Air Patrol](http://www.canterburyairpatrol.org) has a [client](https://github.com/canterbury-air-patrol/fss-smm-mav) that integrates Flight-Safety-System and Search Management Map to control an aircraft running [ArduPilot](https://www.ardupilot.org).

## License
This project is licensed under GNU GPLv2 see the [LICENSE](LICENSE.md) file for details.