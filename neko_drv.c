// SPDX-License-Identifier: GPL-2.0-only
/*
 * neko_drv.c — Stealth kernel memory driver
 *
 * Architecture:
 *   - Registers /dev/neko char device (dynamic major)
 *   - ONE ioctl (NEKO_IOC_MAP): sets target pid, maps shared vmalloc
 *     buffer into userspace, then closes fd — comms thereafter are
 *     entirely via shared memory (NO further ioctl / NO syscall).
 *   - kthread polls shm->req.type, does manual page-table-walk
 *     read/write into target process, fills shm->resp, sets ready=1.
 *   - Second kthread reads Android IIO gyro sensor directly from
 *     /sys/bus/iio/devices/iio:device0/in_anglvel_{x,y}_raw and writes
 *     delta values into shm->gyro.
 *   - Function / variable names obfuscated; no obvious exported symbols;
 *     kthread named to mimic kernel worker threads.
 *
 * Build: see Makefile
 * Targets: Android GKI 6.1.x (arm64)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/random.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "neko_shm.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("kworker");
MODULE_DESCRIPTION("Generic platform bus helper");
MODULE_VERSION("1.0");

/* ── internal obfuscated names ────────────────────────────────────────── */
#define NK_SHM_PAGES   NEKO_SHM_PAGES
#define NK_SHM_SIZE    NEKO_SHM_SIZE

/* ── global state ────────────────────────────────────────────────────── */
static struct miscdevice  g_mdev;
static struct neko_shm   *g_shm        = NULL;   /* vmalloc'd shared buf   */
static struct task_struct *g_poll_thr  = NULL;   /* mem read/write kthread */
static struct task_struct *g_gyro_thr  = NULL;   /* IIO gyro kthread       */
static pid_t               g_target_pid = 0;

/* ── page table walk helper ──────────────────────────────────────────── */
/*
 * nk_ptw_read - walk target mm page tables and copy @size bytes from
 *               virtual @addr into @out.  No process_vm_readv, no
 *               copy_from_user — purely structural walk.
 * Returns number of bytes copied, or <0 on error.
 */
static long nk_ptw_read(struct mm_struct *mm, unsigned long addr,
                         void *out, size_t size)
{
    long copied = 0;

    while (size > 0) {
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        spinlock_t *ptl;
        struct page *pg;
        void *kmap_va;
        unsigned long off    = addr & ~PAGE_MASK;
        size_t        chunk  = min(size, (size_t)(PAGE_SIZE - off));

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) break;

        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) break;

        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) break;

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd)) break;
        if (pmd_trans_huge(*pmd)) {
            /* 2 MB huge page */
            unsigned long huge_off = addr & ~PMD_MASK;
            chunk = min(size, (size_t)(PMD_SIZE - huge_off));
            pg = pmd_page(*pmd);
            kmap_va = kmap_atomic(pg);
            memcpy((char *)out + copied, (char *)kmap_va + huge_off, chunk);
            kunmap_atomic(kmap_va);
            addr   += chunk;
            copied += chunk;
            size   -= chunk;
            continue;
        }
        if (pmd_bad(*pmd)) break;

        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!pte_present(*pte)) {
            pte_unmap_unlock(pte, ptl);
            break;
        }
        pg = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        kmap_va = kmap_atomic(pg);
        memcpy((char *)out + copied, (char *)kmap_va + off, chunk);
        kunmap_atomic(kmap_va);

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
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        spinlock_t *ptl;
        struct page *pg;
        void *kmap_va;
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
        pg = pte_page(*pte);
        pte_unmap_unlock(pte, ptl);

        kmap_va = kmap_atomic(pg);
        memcpy((char *)kmap_va + off, (const char *)in + written, chunk);
        kunmap_atomic(kmap_va);
        flush_dcache_page(pg);

        addr    += chunk;
        written += chunk;
        size    -= chunk;
    }
    return written;
}

/* ── get mm_struct of target pid ─────────────────────────────────────── */
static struct mm_struct *nk_get_mm(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm = NULL;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        mm = get_task_mm(task);
    rcu_read_unlock();
    return mm;  /* caller must mmput() */
}

/* ── memory dispatcher kthread ───────────────────────────────────────── */
static int nk_poll_thread(void *data)
{
    while (!kthread_should_stop()) {
        struct neko_request  *req  = &g_shm->req;
        struct neko_response *resp = &g_shm->resp;
        uint32_t type;

        /* Busy-poll with yield; no sleep avoids scheduling jitter */
        type = READ_ONCE(req->type);
        if (type == NEKO_REQ_IDLE) {
            cpu_relax();
            cond_resched();
            continue;
        }

        /* Ensure we see the complete request before acting */
        smp_rmb();

        resp->status = -EIO;

        if (type == NEKO_REQ_READ || type == NEKO_REQ_WRITE) {
            pid_t            pid  = READ_ONCE(req->pid);
            unsigned long    addr = READ_ONCE(req->addr);
            uint32_t         sz   = READ_ONCE(req->size);
            struct mm_struct *mm;

            if (sz == 0 || sz > NEKO_DATA_MAX) {
                resp->status = -EINVAL;
                goto done;
            }

            mm = nk_get_mm(pid ? pid : g_target_pid);
            if (!mm) { resp->status = -ESRCH; goto done; }

            if (type == NEKO_REQ_READ) {
                long n = nk_ptw_read(mm, addr, resp->rdata, sz);
                resp->status = (n == (long)sz) ? 0 : -EFAULT;
            } else {
                long n = nk_ptw_write(mm, addr, req->wdata, sz);
                resp->status = (n == (long)sz) ? 0 : -EFAULT;
            }
            mmput(mm);
        }

done:
        /* Clear request, publish response */
        WRITE_ONCE(req->type, NEKO_REQ_IDLE);
        smp_wmb();
        WRITE_ONCE(resp->ready, 1);
    }
    return 0;
}

/* ── IIO gyro kthread ────────────────────────────────────────────────── */
/*
 * Reads angular velocity from the first IIO device that exposes
 * in_anglvel_x_raw and in_anglvel_y_raw.  Writes delta into shm->gyro.
 * Falls back gracefully if no IIO gyro is found.
 */
#define NK_IIO_PATH  "/sys/bus/iio/devices/iio:device"
#define NK_GYRO_SCALE_FILE "in_anglvel_scale"

static int nk_read_iio_val(const char *path, long *out)
{
    struct file *f;
    char buf[32];
    ssize_t n;
    loff_t  pos = 0;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) return PTR_ERR(f);
    n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
    filp_close(f, NULL);
    if (n <= 0) return -EIO;
    buf[n] = '\0';
    return kstrtol(buf, 10, out);
}

static int nk_gyro_thread(void *data)
{
    char px[128], py[128];
    int  dev_idx;
    long prev_x = 0, prev_y = 0;
    bool found = false;

    /* probe iio:device0 .. device7 */
    for (dev_idx = 0; dev_idx < 8 && !found; dev_idx++) {
        snprintf(px, sizeof(px),
                 NK_IIO_PATH "%d/in_anglvel_x_raw", dev_idx);
        snprintf(py, sizeof(py),
                 NK_IIO_PATH "%d/in_anglvel_y_raw", dev_idx);

        {
            struct file *f = filp_open(px, O_RDONLY, 0);
            if (!IS_ERR(f)) { filp_close(f, NULL); found = true; }
        }
    }
    dev_idx--;  /* last successful */

    if (!found) {
        pr_debug("neko: no IIO gyro found, gyro thread idle\n");
        while (!kthread_should_stop()) {
            msleep(1000);
        }
        return 0;
    }

    while (!kthread_should_stop()) {
        long rx = 0, ry = 0;

        if (nk_read_iio_val(px, &rx) == 0 &&
            nk_read_iio_val(py, &ry) == 0) {

            float dx = (float)(rx - prev_x) * 0.000266316f; /* ± scale */
            float dy = (float)(ry - prev_y) * 0.000266316f;

            WRITE_ONCE(g_shm->gyro.delta_x, dx);
            WRITE_ONCE(g_shm->gyro.delta_y, dy);
            smp_wmb();
            WRITE_ONCE(g_shm->gyro.seq, g_shm->gyro.seq + 1);

            prev_x = rx;
            prev_y = ry;
        }
        /* ~120 Hz gyro sampling */
        usleep_range(8000, 8500);
    }
    return 0;
}

/* ── vmalloc mmap helpers ────────────────────────────────────────────── */
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
    unsigned long pfn;
    void         *va;
    int           ret;

    if (offset != 0 || size != NK_SHM_SIZE)
        return -EINVAL;
    if (!g_shm)
        return -ENOMEM;

    vma->vm_ops   = &nk_vm_ops;
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

    /* Map each vmalloc page individually */
    for (va = g_shm; size > 0;
         va   += PAGE_SIZE,
         size -= PAGE_SIZE,
         vma->vm_start += PAGE_SIZE) {
        struct page *pg = vmalloc_to_page(va);
        if (!pg) return -ENOMEM;
        pfn = page_to_pfn(pg);
        ret = remap_pfn_range(vma, vma->vm_start, pfn,
                              PAGE_SIZE, vma->vm_page_prot);
        if (ret) return ret;
    }
    /* restore vm_start for caller */
    vma->vm_start = vma->vm_end - NK_SHM_SIZE;
    return 0;
}

/* ── ioctl (one-time setup only) ─────────────────────────────────────── */
static long nk_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    if (cmd != NEKO_IOC_MAP)
        return -ENOTTY;

    /* accept target pid from userspace (optional, can be 0 = set later via shm) */
    {
        int32_t pid_arg = 0;
        if (copy_from_user(&pid_arg, (void __user *)arg, sizeof(pid_arg)))
            return -EFAULT;
        if (pid_arg > 0)
            g_target_pid = (pid_t)pid_arg;
    }
    return 0;
}

/* ── file operations ─────────────────────────────────────────────────── */
static int nk_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int nk_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations nk_fops = {
    .owner          = THIS_MODULE,
    .open           = nk_open,
    .release        = nk_release,
    .unlocked_ioctl = nk_ioctl,
    .compat_ioctl   = nk_ioctl,
    .mmap           = nk_mmap,
};

/* ── module init / exit ──────────────────────────────────────────────── */
static int __init nk_init(void)
{
    int ret;

    /* Allocate shared vmalloc buffer (physically discontiguous, hard to trace) */
    g_shm = vzalloc(NK_SHM_SIZE);
    if (!g_shm)
        return -ENOMEM;

    g_shm->magic = NEKO_SHM_MAGIC;

    /* Register misc device as /dev/neko */
    g_mdev.minor = MISC_DYNAMIC_MINOR;
    g_mdev.name  = "neko";
    g_mdev.fops  = &nk_fops;
    g_mdev.mode  = 0666;

    ret = misc_register(&g_mdev);
    if (ret) {
        vfree(g_shm);
        return ret;
    }

    /* Start kthreads with innocent-looking names */
    g_poll_thr = kthread_run(nk_poll_thread, NULL, "kworker/u8:3");
    if (IS_ERR(g_poll_thr)) {
        misc_deregister(&g_mdev);
        vfree(g_shm);
        return PTR_ERR(g_poll_thr);
    }

    g_gyro_thr = kthread_run(nk_gyro_thread, NULL, "kworker/u4:1");
    if (IS_ERR(g_gyro_thr)) {
        kthread_stop(g_poll_thr);
        misc_deregister(&g_mdev);
        vfree(g_shm);
        return PTR_ERR(g_gyro_thr);
    }

    return 0;
}

static void __exit nk_exit(void)
{
    if (g_gyro_thr && !IS_ERR(g_gyro_thr))
        kthread_stop(g_gyro_thr);
    if (g_poll_thr && !IS_ERR(g_poll_thr))
        kthread_stop(g_poll_thr);

    misc_deregister(&g_mdev);
    vfree(g_shm);
}

module_init(nk_init);
module_exit(nk_exit);
