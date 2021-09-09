AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++11 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I../src -include config.h

if ENABLE_TESTS
TESTS = all_test
check_PROGRAMS = all_test
BUILT_SOURCES=

all_test_SOURCES = main.cpp messages.cpp connection.cpp client.cpp
all_test_LDADD = ../src/libfss-transport.la ../src/libfss-client.la ../src/libfss.la $(JSONCPP_LIBS)

if SSL
all_test_SOURCES += connection-ssl.cpp certs
all_test_LDADD += ../src/libfss-transport-ssl.la $(GNUTLS_LIBS) -lgnutlsxx

BUILT_SOURCES+=certs
certs:
	mkdir certs
	(cd certs; ../../certs/generate-ca.sh; ../../certs/generate-server.sh server; ../../certs/generate-client.sh client)

endif

endif
