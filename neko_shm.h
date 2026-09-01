// SPDX-License-Identifier: GPL-2.0-only
#ifndef NEKO_SHM_H
#define NEKO_SHM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ── one-time ioctl ───────────────────────────────────────────────────── */
#define NEKO_IOC_MAGIC   'N'
#define NEKO_IOC_MAP     _IOW(NEKO_IOC_MAGIC, 1, int32_t)

/* ── request types ────────────────────────────────────────────────────── */
#define NEKO_REQ_IDLE    0
#define NEKO_REQ_READ    1
#define NEKO_REQ_WRITE   2

/* ── limits ───────────────────────────────────────────────────────────── */
#define NEKO_DATA_MAX    512
#define NEKO_SHM_PAGES   4
#define NEKO_SHM_SIZE    (NEKO_SHM_PAGES * 4096)
#define NEKO_SHM_MAGIC   0x4E454B4FU   /* "NEKO" */

struct neko_gyro {
    volatile float    delta_x;
    volatile float    delta_y;
    volatile uint32_t seq;
};

struct neko_request {
    volatile uint32_t type;
    int32_t           pid;
    uint64_t          addr;
    uint32_t          size;
    uint8_t           wdata[NEKO_DATA_MAX];
};

struct neko_response {
    volatile uint32_t ready;
    int32_t           status;
    uint8_t           rdata[NEKO_DATA_MAX];
};

struct neko_shm {
    uint32_t             magic;
    uint32_t             _pad0;
    struct neko_request  req;
    struct neko_response resp;
    struct neko_gyro     gyro;
};

#endif /* NEKO_SHM_H */
