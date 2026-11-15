/*
 * net_util.cpp
 */
#define LOG_TAG "iRPlay_Net"
#include "net_util.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "log.h"

namespace irplay::net {
    namespace {
        constexpr int kUdpRcvBuf = 4 * 1024 * 1024;

        int makeSocket(int type) {
            const int fd = ::socket(AF_INET6, type, 0);
            if (fd < 0) return -1;
            setCloexec(fd);
            const int off = 0;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
            return fd;
        }

        bool bindAny(int fd, uint16_t& port) {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_any;
            addr.sin6_port = htons(port);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
            sockaddr_in6 bound{};
            socklen_t len = sizeof bound;
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return false;
            port = ntohs(bound.sin6_port);
            return true;
        }
    } // namespace

    void setCloexec(int fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    void setNoSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
        (void) fd;
#endif
    }

    int tcpListen(uint16_t& port, int backlog) {
        const int fd = makeSocket(SOCK_STREAM);
        if (fd < 0) return -1;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (!bindAny(fd, port) || ::listen(fd, backlog) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    int udpBind(uint16_t& port, const char* what) {
        const int fd = makeSocket(SOCK_DGRAM);
        if (fd < 0) return -1;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kUdpRcvBuf, sizeof kUdpRcvBuf);
        int actual = 0;
        socklen_t len = sizeof actual;
        ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len);
        // Linux restituisce il doppio del valore effettivo (spazio per i metadati del kernel).
        LOGI("UDP %s: SO_RCVBUF richiesto %d, ottenuto %d", what, kUdpRcvBuf, actual);
        if (!bindAny(fd, port)) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    int acceptConn(int listenFd, sockaddr_storage& peer) {
        socklen_t len = sizeof peer;
        const int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd >= 0) {
            setCloexec(fd);
            setNoSigpipe(fd);
        }
        return fd;
    }

    bool toIn6(const sockaddr_storage& ss, sockaddr_in6& out) {
        if (ss.ss_family == AF_INET6) {
            std::memcpy(&out, &ss, sizeof out);
            return true;
        }
        if (ss.ss_family == AF_INET) {
            const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
            out = sockaddr_in6{};
            out.sin6_family = AF_INET6;
            out.sin6_port = a->sin_port;
            out.sin6_addr.s6_addr[10] = 0xFF;
            out.sin6_addr.s6_addr[11] = 0xFF;
            std::memcpy(&out.sin6_addr.s6_addr[12], &a->sin_addr, 4);
            return true;
        }
        return false;
    }

    bool sameHost(const sockaddr_in6& a, const sockaddr_storage& b) {
        sockaddr_in6 bb{};
        if (!toIn6(b, bb)) return false;
        return std::memcmp(&a.sin6_addr, &bb.sin6_addr, sizeof a.sin6_addr) == 0;
    }

    std::string toString(const sockaddr_in6& a) {
        char ip[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &a.sin6_addr, ip, sizeof ip);
        return ip;
    }

} // namespace irplay::net
