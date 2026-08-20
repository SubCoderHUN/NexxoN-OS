/* ============================================================================
 * NexxoN OS - System clipboard (TASK 25)
 * ----------------------------------------------------------------------------
 * Single kernel-resident buffer with MIME-type tagging.  Capacity is
 * fixed at compile time (no kmalloc yet); large payloads are silently
 * truncated and the truncation flag is returned to the caller.
 *
 * API mirrors the eventual `int 0x80` syscalls (sys_clipboard_set /
 * sys_clipboard_get) so the future Ring-3 `clipd` daemon can route
 * through the same kernel state without an ABI change.
 * ============================================================================ */
#ifndef NEXXON_CLIPBOARD_H
#define NEXXON_CLIPBOARD_H

#include "types.h"

#define CLIPBOARD_MAX_BYTES  4096
#define CLIPBOARD_MIME_MAX   32

typedef enum {
    CLIP_MIME_TEXT_PLAIN = 0,
    CLIP_MIME_IMAGE_BMP  = 1,
    CLIP_MIME_BINARY     = 2,
} clip_mime_t;

/* Store a payload.  Returns the number of bytes actually retained
 * (caps at CLIPBOARD_MAX_BYTES).  Passing len=0 clears the clipboard. */
uint32_t    clipboard_set(const void *data, uint32_t len, clip_mime_t mime);

/* Retrieve a copy into the caller's buffer.  Returns the number of
 * bytes copied (capped at cap).  If out_mime != NULL it receives the
 * stored MIME tag. */
uint32_t    clipboard_get(void *buf, uint32_t cap, clip_mime_t *out_mime);

/* Current byte count (0 = empty). */
uint32_t    clipboard_len(void);

/* Quick text helpers — most clients only care about NUL-terminated
 * strings.  text_set copies up to len bytes (or strlen(s) if len==0)
 * and appends a NUL inside the kernel store. */
uint32_t    clipboard_set_text(const char *s, uint32_t len);
const char *clipboard_peek_text(void);

/* TASK 25: multi-format MIME-typed clipboard.  Each MIME-tag payload
 * sits in its own slot.  set replaces any prior content of the same
 * type; get fails with -1 if no payload of that type is present.
 * list_mimes fills `out` with the i'th stored MIME type. */
#define CLIP_FORMAT_MAX     6
#define CLIP_TYPED_MAX      4096

int  clipboard_set_mime  (const char *mime, const void *data, uint32_t len);
int  clipboard_get_mime  (const char *mime, void *buf, uint32_t cap);
int  clipboard_list_mimes(int idx, char *out, uint32_t cap);

#endif /* NEXXON_CLIPBOARD_H */
