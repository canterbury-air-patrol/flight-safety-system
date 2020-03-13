CXX?=g++
GCC?=gcc
ECPG?=ecpg

all: fss-server example-client

WARNFLAGS=-Werror -Wall -Wshadow -Wunused -Wnull-dereference -Wformat=2 -pedantic
WARNCXXFLAGS=${WARNFLAGS} -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Weffc++
ifeq (($REAL_GCC),1)
WARNFLAGS+=-Wduplicated-cond -Wduplicated-branches -Wmisleading-indentation -Wlogical-op 
WARNCXXFLAGS+=-Wuseless-cast
endif
DEBUG_FLAGS?=-g2
ifeq ($(DEBUG),1)
DEBUG_FLAGS+=-DDEBUG
endif
TEST_FLAGS?=
ifeq ($(TEST),1)
TEST_FLAGS+=-ftest-coverage -fprofile-arcs -g -O0
LDFLAGS+=-lgcov
endif
CFLAGS=${WARNFLAGS} -pthread -fPIC -std=c99 -I. $(DEBUG_FLAGS) $(TEST_FLAGS)
CXXFLAGS=${WARNCXXFLAGS} -pthread -fPIC -std=c++11 -I. $(DEBUG_FLAGS) $(TEST_FLAGS)

PC_LIST=jsoncpp libecpg

EXTRA_CXXFLAGS:=`pkg-config --cflags ${PC_LIST}`
EXTRA_CFLAGS:=-D_GNU_SOURCE `pkg-config --cflags ${PC_LIST}`
EXTRA_LIBS:=`pkg-config --libs ${PC_LIST}`

SERVER_SOURCE=server.cpp db.cpp
SERVER_ECPG=server-db.pgc
SERVER_OBJS=$(SERVER_SOURCE:.cpp=.o) $(SERVER_ECPG:.pgc=.o)

EXAMPLE_CLIENT_SOURCE=examples/fake_client.cpp
EXAMPLE_CLIENT_OBJS=$(EXAMPLE_CLIENT_SOURCE:.cpp=.o)

LIBFSS_CLIENT_SOURCE=client.cpp
LIBFSS_CLIENT_OBJS=$(LIBFSS_CLIENT_SOURCE:.cpp=.o)

LIBFSS_SOURCE=transport.cpp transport-helpers.cpp transport-messages.cpp
LIBFSS_OBJS=$(LIBFSS_SOURCE:.cpp=.o)

TESTSUITES_SOURCE=messages.cpp connection.cpp
TESTSUITES=$(addprefix tests/,$(TESTSUITES_SOURCE:.cpp=.test))

$(LIBFSS_OBJS) $(SERVER_OBJS) $(LIBFSS_CLIENT_OBJS): fss.hpp fss-internal.hpp Makefile
$(LIBFSS_OBJS): transport.hpp
$(SERVER_OBJS): fss-server.hpp

%.o: %.cpp $(INCLUDES)
	$(CXX) -c -o $(@) $(<) $(CXXFLAGS) $(EXTRA_CXXFLAGS)

%.c: %.pgc
	$(ECPG) -o $(@) $(<) $(ECPGFLAGS)

%.o: %.c
	$(GCC) -c -o $(@) $(<) $(CFLAGS) $(EXTRA_CFLAGS)

fss-server: libfss.so $(SERVER_OBJS)
	$(CXX) -o $(@) $(SERVER_OBJS) -L. -lfss $(EXTRA_LIBS) $(LDFLAGS)

example-client: libfss.so libfss-client.so $(EXAMPLE_CLIENT_OBJS)
	$(CXX) -o $(@) $(EXAMPLE_CLIENT_OBJS) -L. -lfss -lfss-client $(EXTRA_LIBS) $(LDFLAGS)

libfss.so: $(LIBFSS_OBJS)
	$(CXX) -shared -o $(@) $(LIBFSS_OBJS) $(LDFLAGS)

libfss-client.so: $(LIBFSS_CLIENT_OBJS)
	$(CXX) -shared -o $(@) $(LIBFSS_CLIENT_OBJS) $(LDFLAGS) -L. -lfss $(EXTRA_LIBS)

tests/%.test: tests/%.cpp libfss.so
	$(CXX) -o $(@) $(<) $(CXXFLAGS) -L. -lfss $(EXTRA_CXXFLAGS)

test: $(TESTSUITES)
	for t in $(TESTSUITES); do ./$$t; done

cov.info: test
	lcov --capture --directory . --output-file cov.info

cov.html: cov.info
	genhtml cov.info --output-directory cov.html

coverage: cov.html

check-cppcheck:
	cppcheck *.cpp tests/*.cpp examples/*.cpp --enable=all

clean:
	rm -f fss-server fss-client libfss.so libfss-client.so $(SERVER_OBJS) $(CLIENT_OBJS) $(LIBFSS_OBJS) $(TESTSUITES)
	rm -fr *.gcno *.gcda cov.info cov.html
