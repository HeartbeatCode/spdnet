#include <spdnet/base/socket_api.h>
#include <cstring>
#include <arpa/inet.h>

namespace spdnet::net::socket_ops {
    int socketNoDelay(sock_t fd) {
        int on = 1;
        return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &on, sizeof(on));
    }

    bool socketBlock(sock_t fd) {
        unsigned long mode = 0;
        return ::ioctl(fd, FIONBIO, &mode) != -1;
    }

    bool socketNonBlock(sock_t fd) {
        unsigned long mode = 1;
        return ::ioctl(fd, FIONBIO, &mode) != -1;
    }

    int socketSendBufSize(sock_t fd, int size) {
        return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *) &size, sizeof(size));
    }

    int socketRecvBufSize(sock_t fd, int size) {
        return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *) &size, sizeof(size));
    }

    void closeSocket(sock_t fd) {
        ::close(fd);
    }

    std::string getIPFromSockaddr(const struct sockaddr *from) {
        char tmp[64]{0};
        auto *sin = (sockaddr_in *) &from;
        ::inet_ntop(AF_INET, (const void *) &sin->sin_addr, tmp, sizeof(tmp));
        return tmp;
    }

    std::string getIPFromSockFd(sock_t fd) {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getpeername(fd, (struct sockaddr *) &addr, &len) == 0) {
            return getIPFromSockaddr((const struct sockaddr *) &addr);
        }
        return "";
    }

    bool checkSelfConnect(sock_t fd) {
        struct sockaddr_in local_addr{0}, remote_addr{0};
        auto addr_len = static_cast<socklen_t>(sizeof(local_addr));
        ::getsockname(fd, (struct sockaddr *) (&local_addr), &addr_len);
        ::getpeername(fd, (struct sockaddr *) (&remote_addr), &addr_len);

        return local_addr.sin_port == remote_addr.sin_port &&
               local_addr.sin_addr.s_addr == remote_addr.sin_addr.s_addr;
    }
}