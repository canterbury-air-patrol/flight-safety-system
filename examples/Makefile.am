AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++17 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I$(top_srcdir)/src -include config.h

# Whole-tree ThreadSanitizer (empty unless --enable-tsan): the example binaries
# link the instrumented libraries, so they must be built/linked with TSan too.
AM_CXXFLAGS += $(TSAN_CXXFLAGS)
AM_LDFLAGS = $(TSAN_LDFLAGS)

bin_PROGRAMS =

# fss-server.service.in is distributed unconditionally: a source template must
# ship in every tarball regardless of whether the host that ran `make dist`
# detected systemd, so the tarball can be rebuilt in a systemd-enabled config.
EXTRA_DIST=client.json server.json fss-server.service.in

if FAKE_CLIENT
bin_PROGRAMS += fss-fake-client

fss_fake_client_SOURCES = fake_client.cpp
fss_fake_client_LDADD = ../src/libfss-transport.la ../src/libfss.la $(JSONCPP_LIBS)
fss_fake_client_LDADD += ../src/libfss-client-ssl.la ../src/libfss-transport-ssl.la
endif

if HAVE_SYSTEMD
if SERVER
# fss-server.service is generated from the .in template: install it but do not
# distribute it (nodist_). The template itself ships via EXTRA_DIST above and
# must never be installed into the unit directory.
nodist_systemdsystemunit_DATA = fss-server.service
CLEANFILES = fss-server.service

SERVICE_SUBS = \
    s,[@]PREFIX[@],${sbindir},g

fss-server.service: fss-server.service.in
	$(SED) -e '$(SERVICE_SUBS)' < $< > $@
endif
endif
