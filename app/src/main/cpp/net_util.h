/*
 * net_util.h
 * Scopo: creazione dei socket con le stesse regole per tutto il motore di rete.
 *  - Sempre AF_INET6 con IPV6_V6ONLY=0 (dual-stack): iOS si collega sia in IPv4 sia
 *    in IPv6 link-local, e un solo tipo di indirizzo semplifica il confronto col peer.
 *  - FD_CLOEXEC su tutto: nessun descrittore passa a processi figli.
 *  - UDP con SO_RCVBUF a 4 MB, riletto con getsockopt: Android lo limita in silenzio
 *    a net.core.rmem_max, e con buffer piccoli i kernel Amlogic/Realtek perdono
 *    pacchetti audio quando la CPU è occupata dal decoder video.
 */
#pragma once

#include <cstdint>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

namespace irplay::net {

#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;   // nessun SIGPIPE se il client chiude la connessione
#else
    constexpr int kSendFlags = 0;              // macOS (solo test host): si usa SO_NOSIGPIPE sul socket
#endif

    void setCloexec(int fd);
    void setNoSigpipe(int fd);

    /** Socket TCP in ascolto. port: preferita in ingresso (0 = scelta dal sistema), reale in uscita. -1 se fallisce. */
    int tcpListen(uint16_t& port, int backlog);
    /** Socket UDP con buffer di ricezione grande. port come sopra. */
    int udpBind(uint16_t& port, const char* what);

    /** accept() con FD_CLOEXEC. */
    int acceptConn(int listenFd, sockaddr_storage& peer);

    /** Converte un indirizzo in IPv6 (gli IPv4 diventano ::ffff:a.b.c.d). false se non è IP. */
    bool toIn6(const sockaddr_storage& ss, sockaddr_in6& out);
    /** Stesso host? Confronta solo l'indirizzo, non porta né scope. */
    bool sameHost(const sockaddr_in6& a, const sockaddr_storage& b);
    std::string toString(const sockaddr_in6& a);

} // namespace irplay::net
