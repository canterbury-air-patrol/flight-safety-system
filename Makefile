CXX?=g++
GCC?=gcc
ECPG?=ecpg

all: fss-server fss-client

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
CFLAGS=${WARNFLAGS} -fPIC -std=c99 -I. $(DEBUG_FLAGS) $(TEST_FLAGS)
CXXFLAGS=${WARNCXXFLAGS} -fPIC -std=c++11 -I. $(DEBUG_FLAGS) $(TEST_FLAGS)

PC_LIST=jsoncpp libecpg

EXTRA_CXXFLAGS:=`pkg-config --cflags ${PC_LIST}`
EXTRA_CFLAGS:=-D_GNU_SOURCE `pkg-config --cflags ${PC_LIST}`
EXTRA_LIBS:=`pkg-config --libs ${PC_LIST}`

SERVER_SOURCE=server.cpp db.cpp
SERVER_ECPG=server-db.pgc
SERVER_OBJS=$(SERVER_SOURCE:.cpp=.o) $(SERVER_ECPG:.pgc=.o)

CLIENT_SOURCE=client.cpp
CLIENT_OBJS=$(CLIENT_SOURCE:.cpp=.o)

LIBFSS_SOURCE=transport.cpp transport-helpers.cpp transport-messages.cpp
LIBFSS_OBJS=$(LIBFSS_SOURCE:.cpp=.o)

TESTSUITES_SOURCE=messages.cpp
TESTSUITES=$(addprefix tests/,$(TESTSUITES_SOURCE:.cpp=.test))

%.o: %.cpp $(INCLUDES)
	$(CXX) -c -o $(@) $(<) $(CXXFLAGS) $(EXTRA_CXXFLAGS)

%.c: %.pgc
	$(ECPG) -o $(@) $(<) $(ECPGFLAGS)

%.o: %.c
	$(GCC) -c -o $(@) $(<) $(CFLAGS) $(EXTRA_CFLAGS)

fss-server: libfss.so $(SERVER_OBJS)
	$(CXX) -o $(@) $(SERVER_OBJS) -L. -lfss $(EXTRA_LIBS) $(LDFLAGS)

fss-client: libfss.so $(CLIENT_OBJS)
	$(CXX) -o $(@) $(CLIENT_OBJS) -L. -lfss $(EXTRA_LIBS) $(LDFLAGS)

libfss.so: $(LIBFSS_OBJS)
	$(CXX) -shared -o $(@) $(LIBFSS_OBJS) $(LDFLAGS)

tests/%.test: tests/%.cpp libfss.so
	$(CXX) -o $(@) $(<) $(CXXFLAGS) -L. -lfss $(EXTRA_CXXFLAGS)

test: $(TESTSUITES)
	for t in $(TESTSUITES); do ./$$t; done

check-cppcheck:
	cppcheck *.cpp --enable=all

clean:
	rm -f fss-server fss-client libfss.so $(SERVER_OBJS) $(CLIENT_OBJS) $(LIBFSS_OBJS) $(TESTSUITES)
