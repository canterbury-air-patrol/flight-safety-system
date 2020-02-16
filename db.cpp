#include "fss-server.hpp"
#include "fss-internal.hpp"

extern "C" {
#include "server-db.h"
};

#include <string>

db_connection::db_connection(std::string host, std::string user, std::string pass, std::string db)
{
    db_connect(host.c_str(), user.c_str(), pass.c_str(), db.c_str());
}

db_connection::~db_connection()
{
    db_disconnect();
}
