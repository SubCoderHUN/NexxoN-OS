/* Minimal i386 steamui.so stub — satisfies dlopen from Steam client */
#include <stddef.h>

void SteamUI_Init(void) {}
int SteamUI_Shutdown(void) { return 0; }
void SteamUI_Run(void) {}
int SteamUI_BNeedsUpdate(void) { return 0; }

/* Valve interface factory: the bootstrap resolves this before handing
 * control to the client UI.  Unknown interfaces return NULL, which the
 * caller reports as a clean startup failure instead of a crash. */
void *CreateInterface(const char *name, int *ret_code) {
    (void)name;
    if (ret_code)
        *ret_code = 1; /* IFACE_FAILED */
    return NULL;
}
