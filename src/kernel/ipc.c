/* ============================================================================
 * NexxoN OS - Inter-Process Communication (IPC) framework
 * ----------------------------------------------------------------------------
 * BSS-resident shared memory pool + ring-buffered message queues.  A
 * small, well-bounded surface that the rest of the OS can rely on:
 *
 *   - Drag-and-drop in the compositor uses queue 0 (well-known).
 *   - Cross-app clipboard updates use queue 1.
 *   - Future system audio events would use queue 2, etc.
 *
 * We deliberately do NOT add fine-grained access control - the kernel
 * runs in a single address space and every Ring 3 app trusts the kernel.
 * Per-segment user-id / mode bits become meaningful once a real per-
 * process MMU is wired (TASK 24's swapper does some of this groundwork).
 * ============================================================================ */
#include "ipc.h"
#include "string.h"
#include "debug.h"

typedef struct {
    bool     in_use;
    uint32_t key;
    uint32_t size;
    int      ref_count;
    uint8_t  data[IPC_SEG_SIZE];
} shm_segment_t;

typedef struct {
    uint32_t len;
    uint8_t  data[IPC_MSG_MAX];
} msg_record_t;

typedef struct {
    bool         in_use;
    int          head;
    int          tail;
    int          count;
    msg_record_t records[IPC_QUEUE_DEPTH];
} msg_queue_t;

static shm_segment_t g_shm[IPC_MAX_SEGMENTS];
static msg_queue_t   g_msq[IPC_MAX_QUEUES];

/* Reserved queue ids. */
#define IPC_QUEUE_DND        0
#define IPC_QUEUE_CLIPBOARD  1

void ipc_init(void) {
    memset(g_shm, 0, sizeof(g_shm));
    memset(g_msq, 0, sizeof(g_msq));
    /* Eagerly enable the well-known queues. */
    g_msq[IPC_QUEUE_DND].in_use       = true;
    g_msq[IPC_QUEUE_CLIPBOARD].in_use = true;
}

/* ---- Shared memory ----------------------------------------------------- */
int ipc_shm_get(uint32_t key, uint32_t size, uint32_t flags) {
    if (size > IPC_SEG_SIZE) return -1;
    /* Search for an existing segment with this key. */
    if (key != 0) {
        for (int i = 0; i < IPC_MAX_SEGMENTS; i++) {
            if (g_shm[i].in_use && g_shm[i].key == key) {
                if (flags & IPC_FLAG_EXCL) return -1;
                return i;
            }
        }
    }
    if (!(flags & IPC_FLAG_CREATE) && key != 0) return -1;
    /* Allocate. */
    for (int i = 0; i < IPC_MAX_SEGMENTS; i++) {
        if (!g_shm[i].in_use) {
            g_shm[i].in_use    = true;
            g_shm[i].key       = key;
            g_shm[i].size      = size ? size : IPC_SEG_SIZE;
            g_shm[i].ref_count = 0;
            memset(g_shm[i].data, 0, sizeof(g_shm[i].data));
            debug_printf("[ipc] shm_get: alloc id=%d key=0x%x size=%u\n",
                         i, key, size);
            return i;
        }
    }
    return -1;
}

void *ipc_shm_attach(int id) {
    if (id < 0 || id >= IPC_MAX_SEGMENTS) return NULL;
    if (!g_shm[id].in_use) return NULL;
    g_shm[id].ref_count++;
    return g_shm[id].data;
}

int ipc_shm_detach(int id) {
    if (id < 0 || id >= IPC_MAX_SEGMENTS || !g_shm[id].in_use) return -1;
    if (g_shm[id].ref_count > 0) g_shm[id].ref_count--;
    return 0;
}

int ipc_shm_destroy(int id) {
    if (id < 0 || id >= IPC_MAX_SEGMENTS || !g_shm[id].in_use) return -1;
    g_shm[id].in_use = false;
    g_shm[id].key    = 0;
    return 0;
}

/* ---- Message queues ---------------------------------------------------- */
static msg_queue_t *get_or_create_queue(int qid) {
    if (qid < 0 || qid >= IPC_MAX_QUEUES) return NULL;
    if (!g_msq[qid].in_use) {
        g_msq[qid].in_use = true;
        g_msq[qid].head = g_msq[qid].tail = g_msq[qid].count = 0;
    }
    return &g_msq[qid];
}

int ipc_msg_send(int qid, const void *buf, uint32_t len) {
    msg_queue_t *q = get_or_create_queue(qid);
    if (!q) return -1;
    if (len > IPC_MSG_MAX) return -1;
    if (q->count >= IPC_QUEUE_DEPTH) return -2;     /* full */
    msg_record_t *r = &q->records[q->head];
    r->len = len;
    memcpy(r->data, buf, len);
    q->head = (q->head + 1) % IPC_QUEUE_DEPTH;
    q->count++;
    return (int)len;
}

int ipc_msg_recv(int qid, void *buf, uint32_t cap) {
    if (qid < 0 || qid >= IPC_MAX_QUEUES) return -1;
    msg_queue_t *q = &g_msq[qid];
    if (!q->in_use) return -1;
    if (q->count == 0) return 0;
    msg_record_t *r = &q->records[q->tail];
    uint32_t want = r->len < cap ? r->len : cap;
    memcpy(buf, r->data, want);
    q->tail = (q->tail + 1) % IPC_QUEUE_DEPTH;
    q->count--;
    return (int)want;
}

int ipc_msg_peek(int qid) {
    if (qid < 0 || qid >= IPC_MAX_QUEUES) return 0;
    return g_msq[qid].count;
}

/* ---- Compositor drag-and-drop ----------------------------------------- */
int ipc_dnd_post(const char *abs_path) {
    uint32_t n = 0;
    while (abs_path[n] && n < IPC_MSG_MAX - 1) n++;
    return ipc_msg_send(IPC_QUEUE_DND, abs_path, n + 1);
}

int ipc_dnd_consume(char *out, uint32_t cap) {
    int n = ipc_msg_recv(IPC_QUEUE_DND, out, cap);
    if (n > 0 && (uint32_t)n < cap) out[n] = 0;
    return n;
}
