AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)

AM_CXXFLAGS = -pthread -fPIC -std=c++11 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++
AM_CFLAGS = -pthread -fPIC -std=c99 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -D_GNU_SOURCE

bin_PROGRAMS =
sbin_PROGRAMS =
lib_LTLIBRARIES = libfss.la libfss-client.la libfss-transport.la
include_HEADERS = fss.hpp
pkgconfig_DATA = fss.pc

libfss_la_LDFLAGS = -version-info 0:0:0
libfss_client_la_LDFLAGS = -version-info 0:0:0
libfss_transport_la_LDFLAGS = -version-info 0:0:0

libfss_la_SOURCES = fss.hpp

include_HEADERS+= fss-client.hpp
pkgconfig_DATA+= fss-client.pc
libfss_client_la_SOURCES = client.cpp fss-client.hpp
libfss_client_la_CXXFLAGS = $(AM_CXXFLAGS) $(JSONCPP_CFLAGS)
libfss_client_la_LIBADD = $(JSONCPP_LIBS) libfss-transport.la

libfss_transport_la_SOURCES = transport.cpp transport-helpers.cpp transport-messages.cpp transport.hpp


.pgc.c:
	ecpg -o $@ $< $(ECPGFLAGS)

if SERVER
sbin_PROGRAMS += fss-server

fss_server_SOURCES = server.cpp fss-server.hpp db.cpp server-db.c server-db.h
fss_server_CXXFLAGS = $(AM_CXXFLAGS) $(JSONCPP_CFLAGS)
fss_server_CFLAGS = $(AM_CFLAGS) $(ECPG_CFLAGS)
fss_server_LDADD = libfss.la libfss-transport.la $(JSONCPP_LIBS) $(ECPG_LIBS)

endif
