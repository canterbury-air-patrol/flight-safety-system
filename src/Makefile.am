AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)

AM_CXXFLAGS = -pthread -fPIC -std=c++17 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -include config.h -ggdb
AM_CFLAGS = -pthread -fPIC -std=c99 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -D_GNU_SOURCE -ggdb
AM_CPPFLAGS =

AM_CFLAGS += $(CODE_COVERAGE_CFLAGS)
AM_CXXFLAGS += $(CODE_COVERAGE_CXXFLAGS)
AM_CPPFLAGS += $(CODE_COVERAGE_CPPFLAGS)
AM_LDFLAGS = $(CODE_COVERAGE_LDFLAGS)

# Whole-tree ThreadSanitizer (empty unless --enable-tsan). Coverage and TSan are
# mutually exclusive at configure time, so at most one of these is non-empty.
AM_CXXFLAGS += $(TSAN_CXXFLAGS)
AM_LDFLAGS += $(TSAN_LDFLAGS)

bin_PROGRAMS =
sbin_PROGRAMS =
lib_LTLIBRARIES = libfss.la libfss-transport.la
include_HEADERS = fss.hpp fss-log.hpp
pkgconfig_DATA = fss.pc

libfss_la_LDFLAGS = -version-info 2:0:0
libfss_transport_la_LDFLAGS = -version-info 2:0:0

libfss_la_SOURCES = fss-main.cpp fss.hpp

include_HEADERS+= fss-transport.hpp secure-string.hpp
pkgconfig_DATA+= fss-transport.pc
libfss_transport_la_SOURCES = transport.cpp transport-helpers.cpp transport-messages.cpp transport.hpp fss-endian.hpp
libfss_transport_la_LIBADD = $(pthread_LIBS)

lib_LTLIBRARIES += libfss-transport-ssl.la
libfss_transport_ssl_la_LDFLAGS = -version-info 2:0:0
libfss_transport_ssl_la_SOURCES = transport-ssl.cpp
libfss_transport_ssl_la_CXXFLAGS = $(AM_CXXFLAGS) $(GNUTLS_CFLAGS)
libfss_transport_ssl_la_LIBADD = $(GNUTLS_LIBS) -L. libfss-transport.la
include_HEADERS += fss-transport-ssl.hpp
pkgconfig_DATA += fss-transport-ssl.pc

lib_LTLIBRARIES += libfss-client-ssl.la
libfss_client_ssl_la_LDFLAGS = -version-info 2:0:0
libfss_client_ssl_la_SOURCES = client-ssl.cpp
libfss_client_ssl_la_CXXFLAGS = $(AM_CXXFLAGS) $(JSONCPP_CFLAGS)
libfss_client_ssl_la_LIBADD = $(GNUTLS_LIBS) $(JSONCPP_LIBS) -L. libfss.la libfss-transport.la libfss-transport-ssl.la
include_HEADERS += fss-client-ssl.hpp
pkgconfig_DATA += fss-client-ssl.pc

.pgc.c:
	ecpg -o $@ $< $(ECPGFLAGS)

# server-db.c is generated from server-db.pgc, by either the server build or
# the tests (when the server is disabled). Clean it unconditionally so it is
# never left behind after distclean, which `make distcheck` rejects.
CLEANFILES = server-db.c

if SERVER
sbin_PROGRAMS += fss-server

fss_server_SOURCES = server.cpp client_session.cpp fss-server.hpp rate-limiter.hpp server-clients.hpp server-failsafe.hpp db.cpp server-db.pgc server-db.h db-write-queue.cpp db-write-queue.hpp
fss_server_CXXFLAGS = $(AM_CXXFLAGS) $(JSONCPP_CFLAGS)
fss_server_CFLAGS = $(AM_CFLAGS) $(ECPG_CFLAGS)
fss_server_LDADD = libfss.la libfss-transport.la $(JSONCPP_LIBS) $(ECPG_LIBS)
fss_server_LDADD += libfss-transport-ssl.la
else
if ENABLE_TESTS
BUILT_SOURCES = server-db.c
endif
endif
