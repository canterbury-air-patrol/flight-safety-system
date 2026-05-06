AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++17 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I../src -include config.h

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
	ssl_failure_test.cpp \
	server_session_test.cpp \
	async_db_write_test.cpp \
	timeout_test.cpp \
	../src/client_session.cpp \
	../src/db-write-queue.cpp
all_test_LDADD = ../src/libfss-transport.la ../src/libfss-client-ssl.la ../src/libfss.la $(JSONCPP_LIBS)

all_test_SOURCES += connection-ssl.cpp certs certs-alt certs/expired
all_test_LDADD += ../src/libfss-transport-ssl.la $(GNUTLS_LIBS) $(CATCH2_LIBS)

EXTRA_DIST = test_helpers.hpp mock_database.hpp

BUILT_SOURCES += certs certs-alt certs/expired
certs:
	mkdir certs
	(cd certs; ../../certs/generate-ca.sh; ../../certs/generate-server.sh localhost; ../../certs/generate-client.sh client)

certs-alt:
	mkdir certs-alt
	(cd certs-alt; ../../certs/generate-ca.sh; ../../certs/generate-server.sh localhost; ../../certs/generate-client.sh client)

certs/expired: certs
	mkdir -p certs/expired
	cp certs/ca.public.pem certs/ca.private.pem certs/expired/
	(cd certs/expired; \
	 certtool --generate-privkey > client.private.pem; \
	 printf '\norganization = Flight Safety System\ncn = expired-client\ntls_www_client\nencryption_key\nsigning_key\ndns_name = expired-client\nexpiration_days = -1\n' > client.tmpl; \
	 certtool --generate-certificate --load-privkey client.private.pem \
	   --load-ca-certificate ca.public.pem --load-ca-privkey ca.private.pem \
	   --template client.tmpl --outfile client.public.pem)
endif
