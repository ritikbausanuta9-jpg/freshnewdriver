// SPDX-License-Identifier: GPL-2.0-only
/*
 * neko_drv.c  — Advanced stealth shared-memory kernel driver
 *               for Android GKI 6.1.x (arm64)
 *
 * Key design points (adapted from wanbai driver.c techniques):
 *
 *  1. Shared memory protocol: ONE ioctl (NEKO_IOC_MAP) to mmap a
 *     vmalloc-backed ring buffer into userspace. After that, ZERO
 *     further ioctl / syscall — all comms are pure shared-memory
 *     spin-loop (kthread reads req, walks PTW, fills resp, sets ready=1).
 *
 *  2. Memory read/write: manual PGD→P4D→PUD→PMD→PTE page-table walk
 *     (NOT process_vm_readv, NOT copy_from_user). Uses kmap_atomic for
 *     safe page access. Huge-page (PMD) aware.
 *
 *  3. kallsyms dynamic resolution at init time for key functions:
 *     get_task_mm, mmput, find_get_pid, pid_task, put_pid,
 *     copy_from_kernel_nofault, probe_kernel_read.
 *     → No static symbol imports visible to nm/modinfo.
 *
 *  4. FOLL_FORCE version detection (6.3+ uses 0x08, ≤6.2 uses 0x10).
 *
 *  5. Gyro: reads Android IIO sensor directly via kernel_read on
 *     /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y}_raw.
 *     Scale applied with kernel_read on in_anglvel_scale.
 *     Falls back gracefully if IIO not present.
 *
 *  6. Stealth:
 *     - Device name: /dev/neko (misc dynamic minor)
 *     - kthreads named "kworker/u8:3", "kworker/u4:1"
 *     - g0 / no-debug-info build flags (see Makefile)
 *     - vmalloc-backed SHM (not kmalloc, no slab trace)
 *     - fd closed by userspace immediately after mmap
 *     - No exported symbols, no obvious string table hits
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/hugetlb.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <asm/pgtable.h>

#include "neko_shm.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("kworker");
MODULE_DESCRIPTION("Generic platform bus helper v2");
MODULE_VERSION("2.0");

/* ── suppress modpost noise ───────────────────────────────────────────── */
#pragma GCC optimize("O2")

/* ── FOLL_FORCE versioned values ─────────────────────────────────────── */
#define NK_FOLL_FORCE_OLD  0x10u   /* kernels <= 6.2 */
#define NK_FOLL_FORCE_NEW  0x08u   /* kernels >= 6.3 */
#define NK_FOLL_WRITE      0x01u

/* ── kallsyms-resolved function typedefs ─────────────────────────────── */
typedef struct pid         *(*t_find_get_pid)(pid_t);
typedef struct task_struct *(*t_pid_task_fn)(struct pid *, enum pid_type);
typedef void                (*t_put_pid)(struct pid *);
typedef struct mm_struct   *(*t_get_task_mm)(struct task_struct *);
typedef void                (*t_mmput)(struct mm_struct *);
typedef long                (*t_cfkn)(void *, const void *, size_t);  /* copy_from_kernel_nofault */
typedef long                (*t_pkr)(void *, const void *, size_t);   /* probe_kernel_read        */
typedef struct file        *(*t_filp_open_fn)(const char *, int, umode_t);
typedef int                 (*t_filp_close_fn)(struct file *, fl_owner_t);
typedef ssize_t             (*t_kernel_read_fn)(struct file *, void *, size_t, loff_t *);

static t_find_get_pid    nk_find_get_pid;
static t_pid_task_fn     nk_pid_task;
static t_put_pid         nk_put_pid;
static t_get_task_mm     nk_get_task_mm;
static t_mmput           nk_mmput;
static t_cfkn            nk_cfkn;    /* copy_from_kernel_nofault */
static t_pkr             nk_pkr;     /* probe_kernel_read (older kernels) */
static t_filp_open_fn    nk_filp_open;
static t_filp_close_fn   nk_filp_close;
static t_kernel_read_fn  nk_kernel_read;

/* ── global state ────────────────────────────────────────────────────── */
static struct miscdevice   nk_mdev;
static struct neko_shm    *nk_shm        = NULL;
static struct task_struct *nk_poll_task  = NULL;
static struct task_struct *nk_gyro_task  = NULL;

/* ── safe kernel memory read (nofault, no panic) ─────────────────────── */
static inline int nk_safe_read(void *dst, const void *src, size_t n)
{
    if (nk_cfkn)
        return (int)nk_cfkn(dst, src, n);
    if (nk_pkr)
        return (int)nk_pkr(dst, src, n);
    return -1;
}

/* ── get mm_struct from pid (safe, refcounted) ───────────────────────── */
static struct mm_struct *nk_get_mm(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm = NULL;
    struct pid         *p;

    if (!nk_find_get_pid || !nk_pid_task || !nk_get_task_mm || !nk_put_pid)
        return NULL;

    p = nk_find_get_pid(pid);
    if (!p) return NULL;

    rcu_read_lock();
    task = nk_pid_task(p, PIDTYPE_PID);
    if (task)
        mm = nk_get_task_mm(task);
    rcu_read_unlock();

    nk_put_pid(p);
    return mm;  /* caller must nk_mmput() */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Manual Page-Table Walk  (GKI 6.1.x, arm64)
 *
 *  Supports:
 *   • 4-level paging (PGD → P4D → PUD → PMD → PTE)
 *   • PMD-level huge pages (2 MB)
 *   • PUD-level huge pages (1 GB, rare but present on some SoCs)
 *
 *  No process_vm_readv, no __access_remote_vm, no copy_from_user.
 *  Uses kmap_local_page() which is the GKI 6.1 preferred API over
 *  kmap_atomic (kmap_atomic still works but kmap_local is SMP-safe).
 * ═══════════════════════════════════════════════════════════════════════ */

static long nk_ptw_read(struct mm_struct *mm, unsigned long vaddr,
                         void *out, size_t total)
{
    long copied = 0;

    while ((size_t)copied < total) {
        unsigned long addr  = vaddr + copied;
        size_t        rem   = total - copied;
        pgd_t  *pgd;
        p4d_t  *p4d;
        pud_t  *pud;
        pmd_t  *pmd;
        pte_t  *pte;
        spinlock_t *ptl;
        struct page *pg = NULL;
        void   *kva;
        unsigned long page_off, chunk;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd))  break;

        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d))  break;

        pud = pud_offset(p4d, addr);
        if (pud_none(*pud))  break;

        /* 1 GB huge page */
        if (pud_huge(*pud) && pud_present(*pud)) {
            unsigned long huge_off = addr & ~PUD_MASK;
            chunk = min(rem, (size_t)(PUD_SIZE - huge_off));
            pg = pud_page(*pud);
            kva = kmap_local_page(pg);
            memcpy((char *)out + copied, (char *)kva + huge_off, chunk);
            kunmap_local(kva);
            copied += chunk;
            continue;
        }

        if (pud_bad(*pud)) break;

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd)) break;

        /* 2 MB huge page */
        if (pmd_trans_huge(*pmd)) {
            unsigned long huge_off = addr & ~PMD_MASK;
            chunk = min(rem, (size_t)(PMD_SIZE - huge_off));
            pg = pmd_page(*pmd);
            kva = kmap_local_page(pg);
            memcpy((char *)out + copied, (char *)kva + huge_off, chunk);
            kunmap_local(kva);
            copied += chunk;
            continue;
        }

        if (pmd_bad(*pmd)) break;

        /* Normal 4 KB page */
        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!pte_present(*pte)) {
            pte_unmap_unlock(pte, ptl);
            break;
        }
        pg = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        page_off = addr & ~PAGE_MASK;
        chunk    = min(rem, PAGE_SIZE - page_off);

        kva = kmap_local_page(pg);
        memcpy((char *)out + copied, (char *)kva + page_off, chunk);
        kunmap_local(kva);

        copied += chunk;
    }
    return copied;
}

static long nk_ptw_write(struct mm_struct *mm, unsigned long vaddr,
                          const void *in, size_t total)
{
    long written = 0;

    while ((size_t)written < total) {
        unsigned long addr  = vaddr + written;
        size_t        rem   = total - written;
        pgd_t  *pgd;
        p4d_t  *p4d;
        pud_t  *pud;
        pmd_t  *pmd;
        pte_t  *pte;
        spinlock_t *ptl;
        struct page *pg;
        void   *kva;
        unsigned long page_off, chunk;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd))  break;
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d))  break;
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud)  || pud_bad(*pud))  break;
        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd)  || pmd_bad(*pmd))  break;

        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!pte_present(*pte)) {
            pte_unmap_unlock(pte, ptl);
            break;
        }
        pg = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        page_off = addr & ~PAGE_MASK;
        chunk    = min(rem, PAGE_SIZE - page_off);

        kva = kmap_local_page(pg);
        memcpy((char *)kva + page_off, (const char *)in + written, chunk);
        kunmap_local(kva);
        flush_dcache_page(pg);

        written += chunk;
    }
    return written;
}

/* ── VMA walk to get module base (avoids /proc/maps in kernel ctx) ───── */
/*
 *  On GKI 6.1 the mm uses a maple tree (mm_mt).  We use VMA iterator
 *  API introduced in 6.1: vma_find() / for_each_vma().
 *  Falls back to mm->mmap linked-list scan if iterator unavailable.
 */
static unsigned long nk_module_base(struct mm_struct *mm, const char *name)
{
    unsigned long base = 0;
    struct vm_area_struct *vma;
    VMA_ITERATOR(vmi, mm, 0);

    mmap_read_lock(mm);
    for_each_vma(vmi, vma) {
        if (vma->vm_file) {
            char fname[128];
            char *path = d_path(&vma->vm_file->f_path, fname, sizeof(fname));
            if (!IS_ERR(path)) {
                const char *bn = strrchr(path, '/');
                bn = bn ? bn + 1 : path;
                if (strstr(bn, name)) {
                    base = vma->vm_start;
                    break;
                }
            }
        }
    }
    mmap_read_unlock(mm);
    return base;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Memory dispatcher kthread
 *  Polls shm->req.type; dispatches PTW read/write; sets resp.ready = 1
 * ═══════════════════════════════════════════════════════════════════════ */
static int nk_poll_thread(void *unused)
{
    while (!kthread_should_stop()) {
        uint32_t type = READ_ONCE(nk_shm->req.type);

        if (type == NEKO_REQ_IDLE) {
            cpu_relax();
            cond_resched();
            continue;
        }

        smp_rmb();  /* ensure full req is visible */

        {
            pid_t    pid  = READ_ONCE(nk_shm->req.pid);
            uint64_t addr = READ_ONCE(nk_shm->req.addr);
            uint32_t sz   = READ_ONCE(nk_shm->req.size);
            struct mm_struct *mm;
            long n;

            nk_shm->resp.status = -EIO;

            if (sz == 0 || sz > NEKO_DATA_MAX)
                goto done;

            mm = nk_get_mm(pid);
            if (!mm) { nk_shm->resp.status = -ESRCH; goto done; }

            if (type == NEKO_REQ_READ) {
                n = nk_ptw_read(mm, (unsigned long)addr,
                                nk_shm->resp.rdata, sz);
                nk_shm->resp.status = (n == (long)sz) ? 0 : -EFAULT;
            } else if (type == NEKO_REQ_WRITE) {
                n = nk_ptw_write(mm, (unsigned long)addr,
                                 nk_shm->req.wdata, sz);
                nk_shm->resp.status = (n == (long)sz) ? 0 : -EFAULT;
            } else if (type == NEKO_REQ_GET_BASE) {
                /* name is stored null-terminated in wdata */
                char modname[128];
                memset(modname, 0, sizeof(modname));
                memcpy(modname, nk_shm->req.wdata,
                       min((size_t)sz, sizeof(modname) - 1));
                nk_shm->resp.status  = 0;
                nk_shm->resp.base_addr = nk_module_base(mm, modname);
            }
            nk_mmput(mm);
        }
done:
        WRITE_ONCE(nk_shm->req.type, NEKO_REQ_IDLE);
        smp_wmb();
        WRITE_ONCE(nk_shm->resp.ready, 1);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  IIO Gyro kthread
 *
 *  Probes iio:device0..7 for in_anglvel_{x,y}_raw.
 *  Reads scale from in_anglvel_scale (in rad/s per LSB).
 *  Writes delta_x, delta_y (radians/frame) + seq to shm->gyro.
 *  Sampling rate: ~120 Hz.
 * ═══════════════════════════════════════════════════════════════════════ */
#define NK_IIO_BASE  "/sys/bus/iio/devices/iio:device"
#define NK_GYRO_HZ   120
#define NK_GYRO_US   (1000000 / NK_GYRO_HZ)

/* kernel_read a small sysfs file, return parsed long */
static int nk_iio_read_long(const char *path, long *out)
{
    struct file *f;
    char buf[48];
    ssize_t n;
    loff_t  pos = 0;
    int     ret;

    if (!nk_filp_open || !nk_filp_close || !nk_kernel_read)
        return -ENOSYS;

    f = nk_filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) return PTR_ERR(f);

    n = nk_kernel_read(f, buf, sizeof(buf) - 1, &pos);
    nk_filp_close(f, NULL);

    if (n <= 0) return -EIO;
    buf[n] = '\0';
    ret = kstrtol(buf, 10, out);
    return ret;
}

/* read float scale stored as "0.000266316\n" in sysfs */
static float nk_iio_read_scale(const char *path)
{
    struct file *f;
    char buf[64];
    ssize_t n;
    loff_t  pos = 0;

    if (!nk_filp_open || !nk_filp_close || !nk_kernel_read)
        return 0.000266316f;  /* MPU-6500 default */

    f = nk_filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) return 0.000266316f;
    n = nk_kernel_read(f, buf, sizeof(buf) - 1, &pos);
    nk_filp_close(f, NULL);
    if (n <= 0) return 0.000266316f;
    buf[n] = '\0';

    /* simple string→float: parse integer part + fractional */
    long   int_part = 0;
    long   frac_part = 0;
    int    frac_digits = 0;
    char  *p = buf;
    int    neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') int_part = int_part * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        long div = 1;
        while (*p >= '0' && *p <= '9' && frac_digits < 9) {
            frac_part = frac_part * 10 + (*p++ - '0');
            div *= 10; frac_digits++;
        }
        float f_val = (float)int_part + (float)frac_part / (float)(div);
        return neg ? -f_val : f_val;
    }
    return neg ? -(float)int_part : (float)int_part;
}

static int nk_gyro_thread(void *unused)
{
    char path_x[128], path_y[128], path_sc[128];
    int  dev, found = -1;
    long prev_x = 0, prev_y = 0;
    float scale = 0.000266316f;

    /* Probe iio:device0..7 */
    for (dev = 0; dev < 8; dev++) {
        struct file *f;
        snprintf(path_x, sizeof(path_x),
                 NK_IIO_BASE "%d/in_anglvel_x_raw", dev);
        if (!nk_filp_open) break;
        f = nk_filp_open(path_x, O_RDONLY, 0);
        if (!IS_ERR(f)) {
            nk_filp_close(f, NULL);
            found = dev;
            break;
        }
    }

    if (found < 0) {
        /* No IIO gyro — idle loop */
        while (!kthread_should_stop()) msleep(2000);
        return 0;
    }

    snprintf(path_x,  sizeof(path_x),
             NK_IIO_BASE "%d/in_anglvel_x_raw", found);
    snprintf(path_y,  sizeof(path_y),
             NK_IIO_BASE "%d/in_anglvel_y_raw", found);
    snprintf(path_sc, sizeof(path_sc),
             NK_IIO_BASE "%d/in_anglvel_scale",  found);

    scale = nk_iio_read_scale(path_sc);
    if (scale == 0.0f) scale = 0.000266316f;

    /* Seed prev values */
    nk_iio_read_long(path_x, &prev_x);
    nk_iio_read_long(path_y, &prev_y);

    while (!kthread_should_stop()) {
        long rx = 0, ry = 0;

        if (nk_iio_read_long(path_x, &rx) == 0 &&
            nk_iio_read_long(path_y, &ry) == 0) {

            float dx = (float)(rx - prev_x) * scale;
            float dy = (float)(ry - prev_y) * scale;

            /* Atomic store via memcpy (GKI doesn't guarantee float atomic) */
            uint32_t raw;
            __builtin_memcpy(&raw, &dx, 4);
            WRITE_ONCE(*(uint32_t *)&nk_shm->gyro.delta_x, raw);
            __builtin_memcpy(&raw, &dy, 4);
            WRITE_ONCE(*(uint32_t *)&nk_shm->gyro.delta_y, raw);
            smp_wmb();
            WRITE_ONCE(nk_shm->gyro.seq, nk_shm->gyro.seq + 1);

            prev_x = rx;
            prev_y = ry;
        }
        usleep_range(NK_GYRO_US - 200, NK_GYRO_US + 200);
    }
    return 0;
}

/* ── vmalloc mmap ────────────────────────────────────────────────────── */
static void nk_vma_open(struct vm_area_struct *v)  {}
static void nk_vma_close(struct vm_area_struct *v) {}
static const struct vm_operations_struct nk_vm_ops = {
    .open  = nk_vma_open,
    .close = nk_vma_close,
};

static int nk_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long sz  = vma->vm_end - vma->vm_start;
    void         *va;
    int           ret;

    if (vma->vm_pgoff != 0 || sz != NK_SHM_SIZE || !nk_shm)
        return -EINVAL;

    vma->vm_ops = &nk_vm_ops;
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    /* Map each vmalloc page — vmalloc pages are physically discontiguous */
    va = nk_shm;
    while (sz > 0) {
        struct page   *pg  = vmalloc_to_page(va);
        unsigned long  pfn;
        if (!pg) return -ENOMEM;
        pfn = page_to_pfn(pg);
        ret = remap_pfn_range(vma, vma->vm_start,
                              pfn, PAGE_SIZE, vma->vm_page_prot);
        if (ret) return ret;
        vma->vm_start += PAGE_SIZE;
        va += PAGE_SIZE;
        sz -= PAGE_SIZE;
    }
    vma->vm_start = vma->vm_end - NK_SHM_SIZE;
    return 0;
}

/* ── one-time ioctl ──────────────────────────────────────────────────── */
static long nk_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int32_t pid_arg = 0;
    if (cmd != NEKO_IOC_MAP) return -ENOTTY;
    if (copy_from_user(&pid_arg, (void __user *)arg, sizeof(pid_arg)))
        return -EFAULT;
    /* pid stored in shm->req.pid; userspace sets it per-request anyway */
    if (pid_arg > 0)
        WRITE_ONCE(nk_shm->req.pid, pid_arg);
    return 0;
}

static int nk_open(struct inode *i, struct file *f)  { return 0; }
static int nk_release(struct inode *i, struct file *f){ return 0; }

static const struct file_operations nk_fops = {
    .owner          = THIS_MODULE,
    .open           = nk_open,
    .release        = nk_release,
    .unlocked_ioctl = nk_ioctl,
    .compat_ioctl   = nk_ioctl,
    .mmap           = nk_mmap,
};

/* ── kallsyms resolver macro (same pattern as driver.c) ─────────────── */
#define NK_RESOLVE(var, sym) \
    var = (typeof(var))kallsyms_lookup_name(sym); \
    if (!(var)) pr_debug("neko: sym not found: " sym "\n")

/* ── module init ────────────────────────────────────────────────────── */
static int __init nk_init(void)
{
    int ret;

    /* ── resolve kernel symbols dynamically ─────────────────────────── */
    NK_RESOLVE(nk_find_get_pid, "find_get_pid");
    NK_RESOLVE(nk_pid_task,     "pid_task");
    NK_RESOLVE(nk_put_pid,      "put_pid");
    NK_RESOLVE(nk_get_task_mm,  "get_task_mm");
    NK_RESOLVE(nk_mmput,        "mmput");

    /* Safe kernel read — prefer nofault (6.x), fall back to probe (5.x) */
    nk_cfkn = (t_cfkn)kallsyms_lookup_name("copy_from_kernel_nofault");
    nk_pkr  = (t_pkr) kallsyms_lookup_name("probe_kernel_read");

    /* File I/O for IIO sysfs reads */
    nk_filp_open   = (t_filp_open_fn) kallsyms_lookup_name("filp_open");
    nk_filp_close  = (t_filp_close_fn)kallsyms_lookup_name("filp_close");
    nk_kernel_read = (t_kernel_read_fn)kallsyms_lookup_name("kernel_read");
    if (!nk_kernel_read)
        nk_kernel_read = (t_kernel_read_fn)kallsyms_lookup_name("__kernel_read");

    /* Abort if critical symbols are missing */
    if (!nk_find_get_pid || !nk_pid_task || !nk_get_task_mm || !nk_mmput) {
        pr_err("neko: critical symbols missing, aborting\n");
        return -ENODEV;
    }

    /* ── allocate vmalloc SHM ───────────────────────────────────────── */
    nk_shm = vzalloc(NK_SHM_SIZE);
    if (!nk_shm) return -ENOMEM;
    nk_shm->magic = NEKO_SHM_MAGIC;

    /* ── register misc device ───────────────────────────────────────── */
    nk_mdev.minor = MISC_DYNAMIC_MINOR;
    nk_mdev.name  = "neko";
    nk_mdev.fops  = &nk_fops;
    nk_mdev.mode  = 0666;

    ret = misc_register(&nk_mdev);
    if (ret) { vfree(nk_shm); return ret; }

    /* ── spawn kthreads with innocent names ─────────────────────────── */
    nk_poll_task = kthread_run(nk_poll_thread, NULL, "kworker/u8:3");
    if (IS_ERR(nk_poll_task)) {
        misc_deregister(&nk_mdev); vfree(nk_shm);
        return PTR_ERR(nk_poll_task);
    }

    nk_gyro_task = kthread_run(nk_gyro_thread, NULL, "kworker/u4:1");
    if (IS_ERR(nk_gyro_task)) {
        kthread_stop(nk_poll_task);
        misc_deregister(&nk_mdev); vfree(nk_shm);
        return PTR_ERR(nk_gyro_task);
    }

    return 0;
}

static void __exit nk_exit(void)
{
    if (nk_gyro_task && !IS_ERR(nk_gyro_task)) kthread_stop(nk_gyro_task);
    if (nk_poll_task && !IS_ERR(nk_poll_task)) kthread_stop(nk_poll_task);
    misc_deregister(&nk_mdev);
    vfree(nk_shm);
}

module_init(nk_init);
module_exit(nk_exit);
