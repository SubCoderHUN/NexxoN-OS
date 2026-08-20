/* ============================================================================
 * NexxoN OS - Linux AF_UNIX stream sockets (in-kernel)
 * ============================================================================ */
#ifndef NEXXON_LIN_UNIX_H
#define NEXXON_LIN_UNIX_H

#include "types.h"
#include "lin_sock.h"

#define LIN_UNIX_PATH_MAX 108
#define LIN_UNIX_RX_CAP   8192

void     lin_unix_global_init(void);
void     lin_unix_sock_reset(lin_sock_t *s);

long     lin_unix_bind(lin_sock_t *s, const void *addr, int addrlen);
long     lin_unix_listen(lin_sock_t *s, int backlog);
long     lin_unix_connect(lin_sock_t *s, const void *addr, int addrlen);
long     lin_unix_accept(const lin_sock_t *listener, lin_sock_t *accepted,
                         void *addr, int *addrlen);
long     lin_unix_read(lin_sock_t *s, void *buf, uint64_t len, bool nonblock);
long     lin_unix_write(lin_sock_t *s, const void *buf, uint64_t len);
void     lin_unix_mark_steam_ui(int link_ix);
void     lin_unix_close(lin_sock_t *s);
void     lin_unix_link_dup_refs(int link_ix);
bool     lin_unix_poll_in(const lin_sock_t *s);
bool     lin_unix_poll_out(const lin_sock_t *s);
long     lin_unix_getsockname(const lin_sock_t *s, void *addr, int *addrlen);
long     lin_unix_socketpair(lin_sock_t *a, lin_sock_t *b);

#endif /* NEXXON_LIN_UNIX_H */
