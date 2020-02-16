#if __APPLE__
/* Apple already has these defines */
#else
#include <endian.h>
#define ntohll(x) be64toh(x)
#define htonll(x) htobe64(x)
#endif

bool convert_str_to_sa(std::string addr, uint16_t port, struct sockaddr_storage *sa);
