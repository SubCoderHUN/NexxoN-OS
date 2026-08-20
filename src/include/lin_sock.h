/* ============================================================================
 * NexxoN OS - Linux guest socket bridge to kernel net.c
 * ============================================================================ */
#ifndef NEXXON_LIN_SOCK_H
#define NEXXON_LIN_SOCK_H

#include "types.h"
#include "net.h"

#define LIN_AF_UNIX   1
#define LIN_AF_INET   2
#define LIN_SOCK_STREAM 1
#define LIN_SOCK_DGRAM  2

typedef enum {
    LSK_NONE = 0,
    LSK_STREAM,
    LSK_DGRAM,
} lin_sock_kind_t;

typedef enum {
    LSUN_IDLE = 0,
    LSUN_BOUND,
    LSUN_LISTENING,
    LSUN_CONNECTED,
} lin_unix_phase_t;

typedef struct {
    lin_sock_kind_t kind;
    int             domain;
    tcp_handle_t    tcp;
    int             udp_slot;
    uint32_t        peer_ip;
    uint16_t        peer_port;
    uint16_t        local_port;
    bool            connected;
    char            unix_path[108];
    bool            unix_abstract;
    lin_unix_phase_t unix_phase;
    int             unix_link;
    int             unix_listener;
} lin_sock_t;

void     lin_sock_reset(lin_sock_t *s);
long     lin_sock_create(lin_sock_t *s, int domain, int type, int protocol);
long     lin_sock_connect(lin_sock_t *s, const void *addr, int addrlen);
long     lin_sock_bind(lin_sock_t *s, const void *addr, int addrlen);
long     lin_sock_listen(lin_sock_t *s, int backlog);
long     lin_sock_accept(const lin_sock_t *listener, lin_sock_t *accepted,
                         void *addr, int *addrlen);
long     lin_sock_getsockname(const lin_sock_t *s, void *addr, int *addrlen);
long     lin_sock_sendto(lin_sock_t *s, const void *buf, uint64_t len,
                         const void *addr, int addrlen);
long     lin_sock_recvfrom(lin_sock_t *s, void *buf, uint64_t len,
                           void *addr, int *addrlen, bool nonblock);
long     lin_sock_read(lin_sock_t *s, void *buf, uint64_t len, bool nonblock);
long     lin_sock_write(lin_sock_t *s, const void *buf, uint64_t len);
void     lin_sock_close(lin_sock_t *s);
bool     lin_sock_poll_in(const lin_sock_t *s);
bool     lin_sock_poll_out(const lin_sock_t *s);

#endif /* NEXXON_LIN_SOCK_H */
