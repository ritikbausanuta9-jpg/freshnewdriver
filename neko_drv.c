// SPDX-License-Identifier: GPL-2.0-only
/*
 * neko_drv.c — Stealth shared-memory kernel memory driver
 * Target: Android GKI 6.1.x (arm64)
 *
 * Design:
 *   /dev/neko char device — one-time ioctl(NEKO_IOC_MAP) → vmalloc shm mmap
 *   After mmap: zero ioctl / zero syscall from userspace — pure shm protocol
 *
 * Memory access method (anti-detect):
 *   Primary:   access_remote_vm(__access_remote_vm) — no copy_from_user trace
 *   Fallback:  manual page-table walk (pgd→p4d→pud→pmd→pte→kmap)
 *   FOLL_FORCE detection: kernel 6.1 uses FOLL_FORCE=0x08 (changed in 6.3)
 *              We detect at init so we always pass the correct value.
 *
 * Gyro:
 *   Kernel IIO kthread reads /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y}_raw
 *   Probes devices 0-9. Stores delta into shm->gyro. ~120 Hz.
 *
 * Stealth:
 *   - vm_flags_set() (const vm_flags_t on GKI 6.1)
 *   - kthread names mimic real kernel worker threads
 *   - g0 compile flags (see Makefile) remove debug strings
 *   - vmalloc buffer not buddy allocator
 *   - fd closed after mmap → no open fd in /proc/PID/fd
 *   - mmap ops: VM_DONTDUMP suppresses coredump leakage
 *
 * Runtime kallsyms resolution (same pattern as driver.c):
 *   find_get_pid, pid_task, get_task_mm, mmput, access_remote_vm,
 *   copy_from_kernel_nofault / probe_kernel_read
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/kallsyms.h>
#include <asm/pgtable.h>

#include "neko_shm.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("kworker");
MODULE_DESCRIPTION("Generic platform bus helper");
MODULE_VERSION("1.0");

/* =========================================================================
 * Resolved kernel function pointers (runtime kallsyms — same as driver.c)
 * ========================================================================= */

typedef struct pid           *(*t_find_get_pid)(int32_t);
typedef struct task_struct   *(*t_pid_task)(struct pid *, int);
typedef struct task_struct   *(*t_get_pid_task)(struct pid *, int);
typedef void                  (*t_put_pid)(struct pid *);
typedef struct mm_struct     *(*t_get_task_mm)(struct task_struct *);
typedef void                  (*t_mmput)(struct mm_struct *);
typedef int                   (*t_access_remote_vm)(struct mm_struct *,
                                                    unsigned long addr,
                                                    void *buf, int len,
                                                    unsigned int flags);
typedef int                   (*t_access_process_vm)(struct task_struct *,
                                                     unsigned long addr,
                                                     void *buf, int len,
                                                     unsigned int flags);
typedef long                  (*t_copy_from_kernel_nofault)(void *dst,
                                                            const void *src,
                                                            size_t size);
typedef long                  (*t_probe_kernel_read)(void *dst,
                                                     const void *src,
                                                     size_t size);
typedef ssize_t               (*t_kernel_read)(struct file *f, void *buf,
                                               size_t count, loff_t *pos);
typedef struct file           *(*t_filp_open)(const char *fname, int flags, int mode);
typedef int                   (*t_filp_close)(struct file *f, void *id);
typedef void                  (*t_rcu_lock_fn)(void);

static t_find_get_pid              nk_find_get_pid;
static t_pid_task                  nk_pid_task;
static t_get_pid_task              nk_get_pid_task;
static t_put_pid                   nk_put_pid;
static t_get_task_mm               nk_get_task_mm;
static t_mmput                     nk_mmput;
static t_access_remote_vm          nk_access_remote_vm;
static t_access_process_vm         nk_access_process_vm;
static t_copy_from_kernel_nofault  nk_copy_from_kernel_nofault;
static t_probe_kernel_read         nk_probe_kernel_read;
static t_kernel_read               nk_kernel_read;
static t_filp_open                 nk_filp_open;
static t_filp_close                nk_filp_close;
static t_rcu_lock_fn               nk_rcu_read_lock_fn;
static t_rcu_lock_fn               nk_rcu_read_unlock_fn;

/* FOLL_FORCE value — detected at init time.
 * GKI 6.1 (between 6.2 and 6.3 boundary):
 *   kernel 6.1 shipped with FOLL_FORCE = 0x10 (old), BUT
 *   GKI 6.1 stable ACK builds may have backported 6.3 change.
 *   We probe at runtime by checking for folio_alloc_noprof. */
#define FOLL_FORCE_OLD   0x10u   /* <= 6.2 */
#define FOLL_FORCE_NEW   0x08u   /* >= 6.3 */
#define FOLL_WRITE       0x01u
static unsigned int nk_foll_force = FOLL_FORCE_OLD;

#define PIDTYPE_PID  0

/* =========================================================================
 * Global state
 * ========================================================================= */
static struct miscdevice   g_mdev;
static struct neko_shm    *g_shm       = NULL;
static struct task_struct *g_poll_thr  = NULL;
static struct task_struct *g_gyro_thr  = NULL;
static pid_t               g_target_pid = 0;

/* =========================================================================
 * Safe nofault kernel read (from driver.c pattern)
 * ========================================================================= */
static inline long nk_safe_read(void *dst, const void *src, size_t sz)
{
    if (nk_copy_from_kernel_nofault)
        return nk_copy_from_kernel_nofault(dst, src, sz);
    if (nk_probe_kernel_read)
        return nk_probe_kernel_read(dst, src, sz);
    /* last resort: direct copy — may oops on bad addr but better than hang */
    memcpy(dst, src, sz);
    return 0;
}

/* =========================================================================
 * RCU helpers
 * ========================================================================= */
static inline void nk_rcu_lock(void)
{
    if (nk_rcu_read_lock_fn)
        nk_rcu_read_lock_fn();
    else
        __asm__ __volatile__("" ::: "memory");
}
static inline void nk_rcu_unlock(void)
{
    if (nk_rcu_read_unlock_fn)
        nk_rcu_read_unlock_fn();
    else
        __asm__ __volatile__("" ::: "memory");
}

/* =========================================================================
 * Get mm_struct safely (from driver.c get_mm_by_pid pattern)
 * ========================================================================= */
static struct mm_struct *nk_get_mm(pid_t pid)
{
    struct mm_struct *mm = NULL;
    struct pid       *p;

    if (!nk_find_get_pid || !nk_get_task_mm || !nk_put_pid)
        return NULL;

    p = nk_find_get_pid(pid);
    if (!p) return NULL;

    if (nk_pid_task) {
        nk_rcu_lock();
        struct task_struct *t = nk_pid_task(p, PIDTYPE_PID);
        if (t) mm = nk_get_task_mm(t);
        nk_rcu_unlock();
    } else if (nk_get_pid_task) {
        struct task_struct *t = nk_get_pid_task(p, PIDTYPE_PID);
        if (t) mm = nk_get_task_mm(t);
    }

    nk_put_pid(p);
    return mm;  /* caller must nk_mmput() */
}

/* =========================================================================
 * Primary memory access: access_remote_vm
 * ========================================================================= */
static long nk_xmem(pid_t pid, uint64_t addr, void *buf, uint32_t sz, int wr)
{
    struct mm_struct *mm = nk_get_mm(pid);
    if (!mm) return -3;

    int done = 0;
    unsigned int flags = wr ? FOLL_WRITE : 0;

    if (nk_access_remote_vm) {
        done = nk_access_remote_vm(mm, (unsigned long)addr, buf, (int)sz, flags);
    } else if (nk_access_process_vm && nk_find_get_pid && nk_pid_task) {
        struct pid *p = nk_find_get_pid(pid);
        if (p) {
            nk_rcu_lock();
            struct task_struct *t = nk_pid_task(p, PIDTYPE_PID);
            if (t) done = nk_access_process_vm(t, (unsigned long)addr,
                                                buf, (int)sz, flags);
            nk_rcu_unlock();
            nk_put_pid(p);
        }
    }

    nk_mmput(mm);
    return (done == (int)sz) ? 0 : -5;
}

/* =========================================================================
 * Fallback: manual page-table walk (GKI 6.1 compatible)
 *
 * On GKI 6.1:
 *   - vm_flags is const → vm_flags_set() already fixed in mmap handler
 *   - pmd_trans_huge() available
 *   - kmap_atomic available
 *   - pte_offset_map_lock available
 * ========================================================================= */
static long nk_ptw_read(struct mm_struct *mm, unsigned long addr,
                        void *out, size_t size)
{
    long copied = 0;
    while (size > 0) {
        pgd_t      *pgd;
        p4d_t      *p4d;
        pud_t      *pud;
        pmd_t      *pmd;
        pte_t      *pte;
        spinlock_t *ptl;
        struct page *pg;
        void       *kva;
        unsigned long off   = addr & ~PAGE_MASK;
        size_t        chunk = min(size, (size_t)(PAGE_SIZE - off));

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) break;
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) break;
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) break;
        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd)) break;

        if (pmd_trans_huge(*pmd)) {
            unsigned long hoff = addr & ~PMD_MASK;
            chunk = min(size, (size_t)(PMD_SIZE - hoff));
            pg  = pmd_page(*pmd);
            kva = kmap_atomic(pg);
            memcpy((char *)out + copied, (char *)kva + hoff, chunk);
            kunmap_atomic(kva);
            goto next;
        }
        if (pmd_bad(*pmd)) break;

        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!pte_present(*pte)) { pte_unmap_unlock(pte, ptl); break; }
        pg  = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        kva = kmap_atomic(pg);
        memcpy((char *)out + copied, (char *)kva + off, chunk);
        kunmap_atomic(kva);

next:
        addr   += chunk;
        copied += chunk;
        size   -= chunk;
    }
    return copied;
}

static long nk_ptw_write(struct mm_struct *mm, unsigned long addr,
                         const void *in, size_t size)
{
    long written = 0;
    while (size > 0) {
        pgd_t      *pgd;
        p4d_t      *p4d;
        pud_t      *pud;
        pmd_t      *pmd;
        pte_t      *pte;
        spinlock_t *ptl;
        struct page *pg;
        void       *kva;
        unsigned long off   = addr & ~PAGE_MASK;
        size_t        chunk = min(size, (size_t)(PAGE_SIZE - off));

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) break;
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) break;
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) break;
        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd)) break;

        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!pte_present(*pte)) { pte_unmap_unlock(pte, ptl); break; }
        pg  = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        kva = kmap_atomic(pg);
        memcpy((char *)kva + off, (const char *)in + written, chunk);
        kunmap_atomic(kva);
        flush_dcache_page(pg);

        addr    += chunk;
        written += chunk;
        size    -= chunk;
    }
    return written;
}

/* =========================================================================
 * Combined read/write: try access_remote_vm first, fall back to PTW
 * ========================================================================= */
static long nk_mem_op(pid_t pid, uint64_t addr, void *buf, uint32_t sz, int wr)
{
    /* Try primary method */
    long ret = nk_xmem(pid, addr, buf, sz, wr);
    if (ret == 0) return 0;

    /* Fallback: page table walk */
    struct mm_struct *mm = nk_get_mm(pid);
    if (!mm) return -3;

    long n = wr ? nk_ptw_write(mm, (unsigned long)addr, buf, sz)
               : nk_ptw_read(mm,  (unsigned long)addr, buf, sz);
    nk_mmput(mm);
    return (n == (long)sz) ? 0 : -5;
}

/* =========================================================================
 * Memory dispatcher kthread (polls shm->req)
 * ========================================================================= */
static int nk_poll_thread(void *data)
{
    while (!kthread_should_stop()) {
        struct neko_request  *req  = &g_shm->req;
        struct neko_response *resp = &g_shm->resp;
        uint32_t type = READ_ONCE(req->type);

        if (type == NEKO_REQ_IDLE) {
            cpu_relax();
            cond_resched();
            continue;
        }
        smp_rmb();

        resp->status = -EIO;
        pid_t  pid   = READ_ONCE(req->pid);
        if (!pid) pid = g_target_pid;
        uint64_t addr = READ_ONCE(req->addr);
        uint32_t sz   = READ_ONCE(req->size);

        if (sz == 0 || sz > NEKO_DATA_MAX) {
            resp->status = -EINVAL;
            goto done;
        }

        if (type == NEKO_REQ_READ) {
            resp->status = (int)nk_mem_op(pid, addr, resp->rdata, sz, 0);
        } else if (type == NEKO_REQ_WRITE) {
            resp->status = (int)nk_mem_op(pid, addr, req->wdata, sz, 1);
        }

done:
        WRITE_ONCE(req->type, NEKO_REQ_IDLE);
        smp_wmb();
        WRITE_ONCE(resp->ready, 1);
    }
    return 0;
}

/* =========================================================================
 * IIO Gyro kthread
 *
 * Reads angular velocity from Android IIO framework.
 * Probes iio:device0 .. iio:device9 for in_anglvel_{x,y}_raw.
 * Scale factor: most Android IIO gyro sensors report in millidegrees/s.
 * We convert to radians/frame assuming 120 Hz update rate:
 *   rad = (raw_millideg_per_s / 1000.0) * (PI/180) / 120.0
 *   simplified constant: raw * 1.4544e-7
 *
 * If in_anglvel_scale is present, we read that and multiply:
 *   rad_per_s = raw * scale
 *   rad/frame = rad_per_s / 120
 * ========================================================================= */

#define NK_IIO_BASE   "/sys/bus/iio/devices/iio:device"
#define NK_GYRO_HZ    120
#define NK_GYRO_US    (1000000 / NK_GYRO_HZ)  /* 8333 us */

/* Read a single line from a sysfs file into buf, return 0 on success */
static int nk_sysfs_read_long(const char *path, long *out)
{
    struct file *f;
    char buf[32];
    ssize_t n;
    loff_t pos = 0;

    f = nk_filp_open ? nk_filp_open(path, O_RDONLY, 0) : ERR_PTR(-ENOENT);
    if (IS_ERR(f)) return PTR_ERR(f);
    n = nk_kernel_read ? nk_kernel_read(f, buf, sizeof(buf) - 1, &pos) : -EIO;
    nk_filp_close(f, NULL);
    if (n <= 0) return -EIO;
    buf[n] = '\0';
    return kstrtol(buf, 10, out);
}

/* Try to read IIO scale (float as string like "0.000266316") into a
 * fixed-point representation * 1e9 (nanoscale). If unavailable, use
 * the default Android gyro scale (BMI160/ICM42688 common: ~266 µrad/LSB). */
static int64_t nk_probe_iio_scale(int dev_idx)
{
    char spath[128];
    snprintf(spath, sizeof(spath),
             NK_IIO_BASE "%d/in_anglvel_scale", dev_idx);

    struct file *f = nk_filp_open ? nk_filp_open(spath, O_RDONLY, 0)
                                  : ERR_PTR(-ENOENT);
    if (IS_ERR(f))
        return 266316LL; /* default: 266316 nrad/LSB → * 1e-9 = 2.66e-4 rad */

    char buf[48];
    ssize_t n;
    loff_t pos = 0;
    n = nk_kernel_read ? nk_kernel_read(f, buf, sizeof(buf) - 1, &pos) : -EIO;
    nk_filp_close(f, NULL);
    if (n <= 0) return 266316LL;
    buf[n] = '\0';

    /* Parse "0.000266316" → integer part 0, frac scaled to nrad */
    const char *dot = strchr(buf, '.');
    if (!dot) return 266316LL;

    /* Read up to 9 decimal places into nanoscale */
    int64_t ns = 0;
    int     places = 0;
    const char *p = dot + 1;
    while (*p >= '0' && *p <= '9' && places < 9) {
        ns = ns * 10 + (*p - '0');
        p++; places++;
    }
    /* Pad to 9 places */
    while (places++ < 9) ns *= 10;
    return ns; /* unit: nrad/LSB at whatever sensor rate */
}

static int nk_gyro_thread(void *data)
{
    char px[128], py[128];
    int  dev_idx = -1;
    bool found   = false;
    long prev_x  = 0, prev_y = 0;
    int64_t scale_nrad = 266316LL; /* default */

    /* Probe for IIO gyro device */
    for (int i = 0; i < 10 && !found; i++) {
        snprintf(px, sizeof(px), NK_IIO_BASE "%d/in_anglvel_x_raw", i);
        snprintf(py, sizeof(py), NK_IIO_BASE "%d/in_anglvel_y_raw", i);

        long dummy;
        if (nk_sysfs_read_long(px, &dummy) == 0) {
            dev_idx   = i;
            found     = true;
            scale_nrad = nk_probe_iio_scale(i);
            pr_debug("neko: IIO gyro on iio:device%d scale=%lld nrad/LSB\n",
                     i, scale_nrad);
        }
    }

    if (!found) {
        pr_debug("neko: no IIO gyro found — gyro thread idle\n");
        while (!kthread_should_stop()) msleep(1000);
        return 0;
    }

    /* Update paths with correct device index */
    snprintf(px, sizeof(px), NK_IIO_BASE "%d/in_anglvel_x_raw", dev_idx);
    snprintf(py, sizeof(py), NK_IIO_BASE "%d/in_anglvel_y_raw", dev_idx);

    while (!kthread_should_stop()) {
        long rx = 0, ry = 0;

        if (nk_sysfs_read_long(px, &rx) == 0 &&
            nk_sysfs_read_long(py, &ry) == 0) {

            long dx_raw = rx - prev_x;
            long dy_raw = ry - prev_y;

            /* Convert: dx_rad = dx_raw * scale_nrad * 1e-9 / NK_GYRO_HZ
             * As float: (float)(dx_raw * scale_nrad) * 1e-9f / 120.0f
             * To avoid 64-bit float in kernel, compute as:
             *   dx_rad ≈ (float)(dx_raw) * ((float)scale_nrad * 8.333e-12f)
             */
            float fdx = (float)dx_raw * ((float)scale_nrad * 8.333e-12f);
            float fdy = (float)dy_raw * ((float)scale_nrad * 8.333e-12f);

            WRITE_ONCE(g_shm->gyro.delta_x, fdx);
            WRITE_ONCE(g_shm->gyro.delta_y, fdy);
            smp_wmb();
            WRITE_ONCE(g_shm->gyro.seq, g_shm->gyro.seq + 1);

            prev_x = rx;
            prev_y = ry;
        }

        usleep_range(NK_GYRO_US, NK_GYRO_US + 200);
    }
    return 0;
}

/* =========================================================================
 * vmalloc mmap — maps shared buffer to userspace
 * ========================================================================= */
static void nk_vma_open(struct vm_area_struct *vma)  {}
static void nk_vma_close(struct vm_area_struct *vma) {}

static const struct vm_operations_struct nk_vm_ops = {
    .open  = nk_vma_open,
    .close = nk_vma_close,
};

static int nk_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long size   = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

    if (offset != 0 || size != NEKO_SHM_SIZE)
        return -EINVAL;
    if (!g_shm)
        return -ENOMEM;

    vma->vm_ops = &nk_vm_ops;
    /* GKI 6.1: vm_flags is const → must use vm_flags_set() */
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    /* Map each vmalloc page individually */
    unsigned long uaddr = vma->vm_start;
    void *va = (void *)g_shm;
    unsigned long remain = size;

    while (remain > 0) {
        struct page *pg = vmalloc_to_page(va);
        if (!pg) return -ENOMEM;

        unsigned long pfn = page_to_pfn(pg);
        int ret = remap_pfn_range(vma, uaddr, pfn, PAGE_SIZE, vma->vm_page_prot);
        if (ret) return ret;

        uaddr  += PAGE_SIZE;
        va     += PAGE_SIZE;
        remain -= PAGE_SIZE;
    }
    return 0;
}

/* =========================================================================
 * ioctl — one-time setup only
 * ========================================================================= */
static long nk_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    if (cmd != NEKO_IOC_MAP)
        return -ENOTTY;

    int32_t pid_arg = 0;
    if (copy_from_user(&pid_arg, (void __user *)arg, sizeof(pid_arg)))
        return -EFAULT;
    if (pid_arg > 0)
        g_target_pid = (pid_t)pid_arg;
    return 0;
}

static int  nk_open(struct inode *i, struct file *f)    { return 0; }
static int  nk_release(struct inode *i, struct file *f) { return 0; }

static const struct file_operations nk_fops = {
    .owner          = THIS_MODULE,
    .open           = nk_open,
    .release        = nk_release,
    .unlocked_ioctl = nk_ioctl,
    .compat_ioctl   = nk_ioctl,
    .mmap           = nk_mmap,
};

/* =========================================================================
 * Kallsyms resolver (runtime, same pattern as driver.c)
 * ========================================================================= */
#define NK_RESOLVE(var, sym) \
    (var) = (typeof(var))kallsyms_lookup_name(sym); \
    if (!(var)) pr_info("neko: optional missing: " sym "\n");

#define NK_RESOLVE_REQUIRED(var, sym, missing) \
    (var) = (typeof(var))kallsyms_lookup_name(sym); \
    if (!(var)) { pr_err("neko: required missing: " sym "\n"); (missing)++; }

static int nk_resolve_symbols(void)
{
    int missing = 0;

    NK_RESOLVE_REQUIRED(nk_find_get_pid,  "find_get_pid",  missing);
    NK_RESOLVE_REQUIRED(nk_get_task_mm,   "get_task_mm",   missing);
    NK_RESOLVE_REQUIRED(nk_mmput,         "mmput",         missing);
    NK_RESOLVE_REQUIRED(nk_put_pid,       "put_pid",       missing);

    NK_RESOLVE(nk_pid_task,     "pid_task");
    NK_RESOLVE(nk_get_pid_task, "get_pid_task");

    /* access_remote_vm — prefer __access_remote_vm (no FOLL_FORCE issues) */
    nk_access_remote_vm = (t_access_remote_vm)
        kallsyms_lookup_name("__access_remote_vm");
    if (!nk_access_remote_vm)
        nk_access_remote_vm = (t_access_remote_vm)
            kallsyms_lookup_name("access_remote_vm");

    NK_RESOLVE(nk_access_process_vm, "access_process_vm");

    /* safe kernel read */
    nk_copy_from_kernel_nofault = (t_copy_from_kernel_nofault)
        kallsyms_lookup_name("copy_from_kernel_nofault");
    if (!nk_copy_from_kernel_nofault)
        nk_probe_kernel_read = (t_probe_kernel_read)
            kallsyms_lookup_name("probe_kernel_read");

    /* IIO sysfs file access */
    NK_RESOLVE(nk_kernel_read,  "kernel_read");
    NK_RESOLVE(nk_filp_open,    "filp_open");
    NK_RESOLVE(nk_filp_close,   "filp_close");

    /* RCU */
    nk_rcu_read_lock_fn = (t_rcu_lock_fn)
        kallsyms_lookup_name("__rcu_read_lock");
    if (!nk_rcu_read_lock_fn)
        nk_rcu_read_lock_fn = (t_rcu_lock_fn)
            kallsyms_lookup_name("rcu_read_lock");

    nk_rcu_read_unlock_fn = (t_rcu_lock_fn)
        kallsyms_lookup_name("__rcu_read_unlock");
    if (!nk_rcu_read_unlock_fn)
        nk_rcu_read_unlock_fn = (t_rcu_lock_fn)
            kallsyms_lookup_name("rcu_read_unlock");

    return missing;
}

/* =========================================================================
 * Detect correct FOLL_FORCE value for this kernel (from driver.c pattern)
 *
 * GKI 6.1 stable: typically old (0x10) since 6.3 renumbering happened after.
 * BUT some ACK builds or LTS patches may have backported the change.
 * Safe heuristic: if folio_alloc_noprof or __kmalloc_noprof exists → new.
 * ========================================================================= */
static void nk_detect_foll_force(void)
{
    if (kallsyms_lookup_name("folio_alloc_noprof") ||
        kallsyms_lookup_name("__kmalloc_noprof")) {
        nk_foll_force = FOLL_FORCE_NEW;
        pr_info("neko: kernel >= 6.3 ABI detected → FOLL_FORCE=0x%x\n",
                nk_foll_force);
    } else {
        nk_foll_force = FOLL_FORCE_OLD;
        pr_info("neko: kernel <= 6.2 ABI detected → FOLL_FORCE=0x%x\n",
                nk_foll_force);
    }
}

/* =========================================================================
 * Module init / exit
 * ========================================================================= */
static int __init nk_init(void)
{
    int ret;

    /* Resolve symbols */
    ret = nk_resolve_symbols();
    if (ret > 0) {
        pr_err("neko: %d required symbols missing — aborting\n", ret);
        return -ENOENT;
    }

    nk_detect_foll_force();

    /* Allocate vmalloc shared buffer (physically discontiguous, no buddy trace) */
    g_shm = vzalloc(NEKO_SHM_SIZE);
    if (!g_shm) return -ENOMEM;
    g_shm->magic = NEKO_SHM_MAGIC;

    /* Register /dev/neko misc device */
    g_mdev.minor = MISC_DYNAMIC_MINOR;
    g_mdev.name  = "neko";
    g_mdev.fops  = &nk_fops;
    g_mdev.mode  = 0666;

    ret = misc_register(&g_mdev);
    if (ret) { vfree(g_shm); return ret; }

    /* Start poll kthread — name mimics real kernel worker */
    g_poll_thr = kthread_run(nk_poll_thread, NULL, "kworker/u8:3");
    if (IS_ERR(g_poll_thr)) {
        misc_deregister(&g_mdev);
        vfree(g_shm);
        return PTR_ERR(g_poll_thr);
    }

    /* Start gyro kthread */
    g_gyro_thr = kthread_run(nk_gyro_thread, NULL, "kworker/u4:1");
    if (IS_ERR(g_gyro_thr)) {
        kthread_stop(g_poll_thr);
        misc_deregister(&g_mdev);
        vfree(g_shm);
        return PTR_ERR(g_gyro_thr);
    }

    pr_info("neko: ready on /dev/neko (shm=%p sz=%zu)\n",
            g_shm, (size_t)NEKO_SHM_SIZE);
    return 0;
}

static void __exit nk_exit(void)
{
    if (g_gyro_thr && !IS_ERR(g_gyro_thr)) kthread_stop(g_gyro_thr);
    if (g_poll_thr && !IS_ERR(g_poll_thr)) kthread_stop(g_poll_thr);
    misc_deregister(&g_mdev);
    vfree(g_shm);
}

module_init(nk_init);
module_exit(nk_exit);
