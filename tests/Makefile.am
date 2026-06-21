AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++17 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I$(top_srcdir)/src -include config.h $(CODE_COVERAGE_CXXFLAGS)

# Whole-tree ThreadSanitizer (empty unless --enable-tsan): instrument the test
# objects and link all_test against the (also-instrumented) libraries with TSan.
AM_CXXFLAGS += $(TSAN_CXXFLAGS)
AM_LDFLAGS = $(TSAN_LDFLAGS)

if ENABLE_TSAN
# Under --enable-tsan the whole suite runs instrumented, so `make check` needs
# the suppression file in the environment or the documented kernel-synchronised
# false positives (see tsan.supp) would fail the run.
AM_TESTS_ENVIRONMENT = TSAN_OPTIONS='suppressions=$(abs_srcdir)/tsan.supp halt_on_error=1'; export TSAN_OPTIONS;
endif

if ENABLE_TESTS
TESTS = all_test
check_PROGRAMS = all_test
BUILT_SOURCES=

all_test_SOURCES = main.cpp messages.cpp connection.cpp client.cpp \
	log_test.cpp \
	malformed_message_test.cpp \
	alignment_regression_test.cpp \
	endianness_test.cpp \
	coordinate_precision_test.cpp \
	queue_test.cpp \
	reconnect_test.cpp \
	connection_negative_test.cpp \
	oversized_message_test.cpp \
	ssl_failure_test.cpp \
	server_session_test.cpp \
	async_db_write_test.cpp \
	timeout_test.cpp \
	db_connection_test.cpp \
	crl_revocation_test.cpp \
	secure_string_test.cpp \
	rate_limiter_test.cpp \
	server_clients_test.cpp \
	exception_isolation_test.cpp \
	../src/client_session.cpp \
	../src/db-write-queue.cpp \
	../src/db.cpp
# server-db.c is generated from server-db.pgc by ecpg (see src/Makefile.am).
# It must be nodist_ so `make dist` distributes the .pgc source, not the
# generated .c, and doesn't demand a build rule that only exists in src/.
nodist_all_test_SOURCES = ../src/server-db.c
# Header search path belongs at the preprocessor layer so it applies to both
# the C (server-db.c) and C++ sources, independent of CFLAGS/CXXFLAGS.
all_test_CPPFLAGS = -I$(top_srcdir)/src
all_test_CFLAGS = $(ECPG_CFLAGS)
all_test_LDADD = ../src/libfss-transport.la ../src/libfss-client-ssl.la ../src/libfss.la $(JSONCPP_LIBS) $(ECPG_LIBS)

all_test_SOURCES += connection-ssl.cpp
all_test_LDADD += ../src/libfss-transport-ssl.la $(GNUTLS_LIBS) $(CATCH2_LIBS)

EXTRA_DIST = test_helpers.hpp mock_database.hpp

BUILT_SOURCES += certs certs-alt certs/expired certs/ghost certs/client.crl.pem certs/empty.crl.pem

# Generated TLS fixtures live only in the build tree; remove them on clean
# so `make distcheck` sees a pristine tree after distclean.
clean-local:
	rm -rf certs certs-alt

certs:
	mkdir certs
	(cd certs; $(abs_top_srcdir)/certs/generate-ca.sh; $(abs_top_srcdir)/certs/generate-server.sh localhost; $(abs_top_srcdir)/certs/generate-client.sh client)

certs-alt:
	mkdir certs-alt
	(cd certs-alt; $(abs_top_srcdir)/certs/generate-ca.sh; $(abs_top_srcdir)/certs/generate-server.sh localhost; $(abs_top_srcdir)/certs/generate-client.sh client)

certs/expired: certs
	mkdir -p certs/expired
	cp certs/ca.public.pem certs/ca.private.pem certs/expired/
	(cd certs/expired; \
	 certtool --generate-privkey > client.private.pem; \
	 printf '\norganization = Flight Safety System\ncn = expired-client\ntls_www_client\nencryption_key\nsigning_key\ndns_name = expired-client\nexpiration_days = -1\n' > client.tmpl; \
	 certtool --generate-certificate --load-privkey client.private.pem \
	   --load-ca-certificate ca.public.pem --load-ca-privkey ca.private.pem \
	   --template client.tmpl --outfile client.public.pem)

certs/ghost: certs
	mkdir -p certs/ghost
	cp certs/ca.public.pem certs/ca.private.pem certs/ghost/
	(cd certs/ghost; \
	 certtool --generate-privkey > client.private.pem; \
	 printf '\norganization = Flight Safety System\ncn = ghost-asset\ntls_www_client\nencryption_key\nsigning_key\ndns_name = ghost-asset\nexpiration_days = 3650\n' > client.tmpl; \
	 certtool --generate-certificate --load-privkey client.private.pem \
	   --load-ca-certificate ca.public.pem --load-ca-privkey ca.private.pem \
	   --template client.tmpl --outfile client.public.pem)

certs/client.crl.pem: certs
	printf 'crl_next_update = 3650\ncrl_number = 1\n' > certs/crl.tmpl
	certtool --generate-crl \
	  --load-ca-privkey certs/ca.private.pem \
	  --load-ca-certificate certs/ca.public.pem \
	  --load-certificate certs/client.public.pem \
	  --template certs/crl.tmpl \
	  --outfile certs/client.crl.pem

certs/empty.crl.pem: certs
	printf 'crl_next_update = 3650\ncrl_number = 1\n' > certs/crl.tmpl
	certtool --generate-crl \
	  --load-ca-privkey certs/ca.private.pem \
	  --load-ca-certificate certs/ca.public.pem \
	  --template certs/crl.tmpl \
	  --outfile certs/empty.crl.pem

if ENABLE_TSAN
# Dedicated concurrency stress binary. It is NOT in TESTS (so `make check`
# doesn't run it), and compiles the transport-layer sources in directly to keep
# its own focused harness; under --enable-tsan the whole tree is instrumented,
# so this binary is TSan-built via the AM_CXXFLAGS/AM_LDFLAGS above. The
# per-target CXXFLAGS only forces object renaming (main.cpp is shared with
# all_test).
check_PROGRAMS += concurrency_tsan_test
concurrency_tsan_test_SOURCES = main.cpp \
	concurrency_connection_test.cpp \
	concurrency_client_test.cpp \
	../src/transport.cpp \
	../src/transport-helpers.cpp \
	../src/transport-messages.cpp \
	../src/client_session.cpp \
	../src/db-write-queue.cpp \
	../src/fss-main.cpp
concurrency_tsan_test_CXXFLAGS = $(AM_CXXFLAGS)
concurrency_tsan_test_LDADD = $(CATCH2_LIBS) $(pthread_LIBS)

EXTRA_DIST += tsan.supp

# Run the full unit suite *and* the concurrency stress binary under TSan in one
# command. `make check` already runs all_test under TSan (TESTS +
# AM_TESTS_ENVIRONMENT); this target adds the concurrency binary, which is not
# in TESTS, and re-runs all_test so a single command covers everything.
.PHONY: check-tsan
check-tsan:
	$(MAKE) $(AM_MAKEFLAGS) all_test concurrency_tsan_test
	TSAN_OPTIONS="suppressions=$(srcdir)/tsan.supp halt_on_error=1" ./all_test
	TSAN_OPTIONS="suppressions=$(srcdir)/tsan.supp halt_on_error=1" ./concurrency_tsan_test
endif
endif
