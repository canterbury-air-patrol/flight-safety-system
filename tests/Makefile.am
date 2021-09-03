if CODE_COVERAGE_ENABLED
include $(top_srcdir)/aminclude_static.am
endif

AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_LIBS =
AM_CPPFLAGS =
AM_CFLAGS =
AM_CXXFLAGS = -pthread -fPIC -std=c++11 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I../src -include config.h
AM_LIBS += $(CODE_COVERAGE_LIBS)
AM_CFLAGS += $(CODE_COVERAGE_CFLAGS)
AM_CXXFLAGS += $(CODE_COVERAGE_CXXFLAGS)
AM_CPPFLAGS += $(CODE_COVERAGE_CPPFLAGS)

if ENABLE_TESTS
TESTS = all_test
check_PROGRAMS = all_test

all_test_SOURCES = main.cpp messages.cpp connection.cpp client.cpp
all_test_LDADD = ../src/libfss-transport.la ../src/libfss-client.la ../src/libfss.la $(JSONCPP_LIBS)

endif

clean-local: code-coverage-clean
distclean-local: code-coverage-dist-clean
