CXX?=g++
GCC?=gcc
ECPG?=ecpg

all: fss-server fss-client

CFLAGS=-Wall -Werror -fPIC -std=c99 -g2
CXXFLAGS=-Wall -Werror -fPIC -std=c++11 -g2

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

%.o: %.cpp $(INCLUDES)
	$(CXX) -c -o $(@) $(<) $(CXXFLAGS) $(EXTRA_CXXFLAGS)

%.c: %.pgc
	$(ECPG) -o $(@) $(<) $(ECPGFLAGS)

%.o: %.c
	$(GCC) -c -o $(@) $(<) $(CFLAGS) $(EXTRA_CFLAGS)

fss-server: libfss.so $(SERVER_OBJS)
	$(CXX) -o $(@) $(SERVER_OBJS) -L. -lfss $(EXTRA_LIBS)

fss-client: libfss.so $(CLIENT_OBJS)
	$(CXX) -o $(@) $(CLIENT_OBJS) -L. -lfss $(EXTRA_LIBS)

libfss.so: $(LIBFSS_OBJS)
	$(CXX) -shared -o $(@) $(LIBFSS_OBJS)


clean:
	rm -f fss-server fss-client libfss.so $(SERVER_OBJS) $(CLIENT_OBJS) $(LIBFSS_OBJS)
