/* stub for MinGW syntax check only */
typedef unsigned short sa_family_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_in { sa_family_t sin_family; unsigned short sin_port; struct in_addr { unsigned long s_addr; } sin_addr; };
#define AF_INET 2
#define SOCK_STREAM 1
int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, unsigned int len);
int inet_pton(int af, const char *src, void *dst);
unsigned short htons(unsigned short v);
