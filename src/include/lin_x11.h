/* ============================================================================
 * NexxoN OS - Minimal X11 display :0 over AF_UNIX abstract socket
 * ============================================================================ */
#ifndef NEXXON_LIN_X11_H
#define NEXXON_LIN_X11_H

#include "types.h"

void lin_x11_display_init(void);
void lin_x11_on_connect(int link_ix);
void lin_x11_on_disconnect(int link_ix);
void lin_x11_on_client_write(int link_ix, const void *data, uint64_t len);
void lin_x11_service(int link_ix);

#endif /* NEXXON_LIN_X11_H */
