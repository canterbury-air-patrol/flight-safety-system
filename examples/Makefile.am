AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++17 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I$(top_srcdir)/src -include config.h

bin_PROGRAMS =

EXTRA_DIST=client.json server.json

if FAKE_CLIENT
bin_PROGRAMS += fss-fake-client

fss_fake_client_SOURCES = fake_client.cpp
fss_fake_client_LDADD = ../src/libfss-transport.la ../src/libfss.la $(JSONCPP_LIBS)
fss_fake_client_LDADD += ../src/libfss-client-ssl.la ../src/libfss-transport-ssl.la
endif

if HAVE_SYSTEMD
if SERVER
# fss-server.service is generated from the .in template: install it but do not
# distribute it (nodist_). The template itself ships in the tarball via
# EXTRA_DIST and must never be installed into the unit directory.
nodist_systemdsystemunit_DATA = fss-server.service
EXTRA_DIST += fss-server.service.in
CLEANFILES = fss-server.service

SERVICE_SUBS = \
    s,[@]PREFIX[@],${sbindir},g

fss-server.service: fss-server.service.in
	$(SED) -e '$(SERVICE_SUBS)' < $< > $@
endif
endif
