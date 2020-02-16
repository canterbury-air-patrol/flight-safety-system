#include <string>

namespace  flight_safety_system {
class db_connection {
private:
public:
    db_connection(std::string host, std::string user, std::string pass, std::string db);
    ~db_connection();
};
}
