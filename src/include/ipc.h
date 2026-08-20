/* ============================================================================
 * NexxoN OS - Inter-Process Communication (IPC) framework  (v1.0)
 * ----------------------------------------------------------------------------
 * Two complementary primitives:
 *
 *   * Shared Memory Segments - identified by a 32-bit key.  Created with
 *     sys_shm_get(key, size, flags); attached via sys_shm_attach(id).
 *     Backing storage lives in a small static pool (16 segments x 64 KiB
 *     max each).  Multiple processes can attach to the same segment by
 *     re-issuing sys_shm_get with the same key.
 *
 *   * Message Queues - fixed-size byte-record queues identified by an
 *     integer id.  sys_msg_send copies a payload into the queue head;
 *     sys_msg_recv pops the queue tail.  Used by the compositor to ship
 *     drag-and-drop payloads from the source app to the drop target.
 *
 * Everything is in BSS - no heap.  The kernel runs a single address
 * space so attach simply returns the segment's static base address;
 * full Ring 3 paging support (per-process page tables that map the
 * segment) is a future patch.
 * ============================================================================ */
#ifndef NEXXON_IPC_H
#define NEXXON_IPC_H

#include "types.h"

#define IPC_MAX_SEGMENTS    16
#define IPC_SEG_SIZE        (64 * 1024)
#define IPC_MAX_QUEUES      8
#define IPC_QUEUE_DEPTH     16
#define IPC_MSG_MAX         256

#define IPC_FLAG_CREATE     1
#define IPC_FLAG_EXCL       2

void   ipc_init       (void);

/* Shared memory. */
int    ipc_shm_get    (uint32_t key, uint32_t size, uint32_t flags);
void  *ipc_shm_attach (int id);
int    ipc_shm_detach (int id);
int    ipc_shm_destroy(int id);

/* Message queues. */
int    ipc_msg_send   (int qid, const void *buf, uint32_t len);
int    ipc_msg_recv   (int qid, void *buf, uint32_t cap);
int    ipc_msg_peek   (int qid);

/* Drag-and-drop helpers used by the compositor. */
int    ipc_dnd_post   (const char *abs_path);
int    ipc_dnd_consume(char *out, uint32_t cap);

#endif /* NEXXON_IPC_H */
