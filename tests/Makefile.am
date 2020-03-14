AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = $(ACLOCAL_FLAGS)
AM_CXXFLAGS = -pthread -fPIC -std=c++11 -I. -Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++ -I../src

if ENABLE_TESTS
TESTS = messages_test connection_test
check_PROGRAMS = messages_test connection_test

messages_test_SOURCES = messages.cpp
messages_test_LDADD = ../src/libfss-transport.la

connection_test_SOURCES = connection.cpp
connection_test_LDADD = ../src/libfss-transport.la

endif
