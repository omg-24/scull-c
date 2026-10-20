// SPDX-License-Identifier: GPL-2.0
/*
 * scull.c — SCULL Linux Character Device Driver (C implementation)
 *
 * SCULL: Simple Character Utility for Loading and Unloading
 *
 * This is a faithful C port of the SCULL driver described in
 * "Linux Device Drivers" (3rd ed.) by Rubini, Corbet, & Kroah-Hartman,
 * and informed by the Rust reference implementation at:
 *   https://github.com/Anas-github-acc/rust_scull
 *
 * The driver exposes a single character device (/dev/scull) backed by a
 * sparse, dynamically-allocated memory model:
 *
 *   linked list of scull_qset nodes
 *       │
 *       ├─ node 0 ── data[0..qset-1] ── each slot → kzalloc'd quantum buffer
 *       ├─ node 1 ── data[0..qset-1]
 *       └─ …
 *
 * Reads from unwritten slots return zero bytes (sparse / "hole" semantics).
 * All device data is freed on reset (ioctl SCULL_IOCRESET) and on rmmod.
 *
 * Tested against Linux 5.15 LTS and 6.x kernels on Ubuntu 22.04 / 24.04.
 * The class_create() call is compatible with both the old two-arg form
 * (≤ 6.2) and the new one-arg form (≥ 6.3) via a compile-time check.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <linux/version.h>

#include "scull.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/* MODULE METADATA                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

MODULE_AUTHOR("C port based on Anas-github-acc/rust_scull & LDD3");
MODULE_DESCRIPTION("SCULL simple character device driver (C implementation)");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");

/* ─────────────────────────────────────────────────────────────────────────── */
/* MODULE PARAMETERS                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static int quantum = SCULL_QUANTUM_DEFAULT;
static int qset    = SCULL_QSET_DEFAULT;

module_param(quantum, int, 0444);
MODULE_PARM_DESC(quantum, "Quantum size in bytes (default 4000)");

module_param(qset, int, 0444);
MODULE_PARM_DESC(qset, "Number of quantum pointers per qset node (default 1000)");

/* ─────────────────────────────────────────────────────────────────────────── */
/* GLOBAL STATE                                                                */
/* ─────────────────────────────────────────────────────────────────────────── */

static dev_t       scull_devno;       /* major:minor numbers allocated by kernel */
static struct scull_dev scull_device; /* the single device instance             */
static struct class   *scull_class;  /* /sys/class/scull for udev               */
static struct device  *scull_dev_node; /* the /dev/scull device node            */

/* ─────────────────────────────────────────────────────────────────────────── */
/* FORWARD DECLARATIONS                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

static int     scull_open   (struct inode *, struct file *);
static int     scull_release(struct inode *, struct file *);
static ssize_t scull_read   (struct file *, char __user *, size_t, loff_t *);
static ssize_t scull_write  (struct file *, const char __user *, size_t, loff_t *);
static loff_t  scull_llseek (struct file *, loff_t, int);
static long    scull_ioctl  (struct file *, unsigned int, unsigned long);

/* ─────────────────────────────────────────────────────────────────────────── */
/* FILE OPERATIONS TABLE                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static const struct file_operations scull_fops = {
    .owner          = THIS_MODULE,
    .open           = scull_open,
    .release        = scull_release,
    .read           = scull_read,
    .write          = scull_write,
    .llseek         = scull_llseek,
    .unlocked_ioctl = scull_ioctl,
};

/* ─────────────────────────────────────────────────────────────────────────── */
/* INTERNAL MEMORY HELPERS                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * scull_trim() — Free ALL data allocated for the device.
 *
 * Iterates the linked list of scull_qset nodes.  For each node:
 *   1. Walk the data[] array and kfree() every non-NULL quantum buffer.
 *   2. kfree() the data[] array itself.
 *   3. Move to the next node, then kfree() the current node.
 *
 * After returning, dev->data is NULL and dev->size is 0.
 *
 * LOCKING: Caller MUST hold dev->lock.
 */
static int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int i;
    int qset_size = dev->qset; /* snapshot before resetting */

    for (dptr = dev->data; dptr != NULL; dptr = next) {
        if (dptr->data) {
            for (i = 0; i < qset_size; i++)
                kfree(dptr->data[i]);   /* free each quantum (NULL-safe) */
            kfree(dptr->data);          /* free the pointer array */
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);                    /* free the qset node itself */
    }

    dev->data    = NULL;
    dev->size    = 0;
    dev->quantum = quantum; /* restore module-parameter defaults */
    dev->qset    = qset;
    return 0;
}

/*
 * scull_follow() — Walk (or extend) the qset linked list to the Nth node.
 *
 * @dev  : the device
 * @item : zero-based index of the target qset node
 *
 * If nodes are missing they are allocated with kzalloc(GFP_KERNEL).
 * Returns a pointer to the Nth scull_qset node, or NULL on ENOMEM.
 *
 * LOCKING: Caller MUST hold dev->lock.
 *
 * Design note: allocating nodes on demand (only when writing) keeps the
 * list short for small devices and avoids wasting memory.
 */
static struct scull_qset *scull_follow(struct scull_dev *dev, int item)
{
    struct scull_qset *qs = dev->data;

    /* Allocate the head node if the list is completely empty. */
    if (!qs) {
        qs = dev->data = kzalloc(sizeof(*qs), GFP_KERNEL);
        if (!qs)
            return NULL;
    }

    /* Walk forward, allocating nodes as needed. */
    while (item--) {
        if (!qs->next) {
            qs->next = kzalloc(sizeof(*qs), GFP_KERNEL);
            if (!qs->next)
                return NULL;
        }
        qs = qs->next;
    }
    return qs;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* FILE OPERATION IMPLEMENTATIONS                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * scull_open() — Called when a user process calls open(2) on /dev/scull.
 *
 * Stores a pointer to the global scull_dev in filp->private_data so
 * that read/write/ioctl can retrieve it without a global variable lookup.
 *
 * We do NOT trim the device on open: data persists between open/close pairs.
 * The device is only reset explicitly via ioctl(SCULL_IOCRESET) or rmmod.
 */
static int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;

    /* Recover the scull_dev embedded inside the cdev via container_of(). */
    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;

    pr_debug("open: pid=%d flags=0x%x\n", current->pid, filp->f_flags);
    return 0;
}

/*
 * scull_release() — Called when the last file descriptor referencing the
 * device is closed.
 *
 * Nothing to do: we do not allocate per-open state.  The VFS handles
 * reference counting automatically.
 */
static int scull_release(struct inode *inode, struct file *filp)
{
    pr_debug("release: pid=%d\n", current->pid);
    return 0;
}

/*
 * scull_read() — Read up to @count bytes starting at *@f_pos.
 *
 * The SCULL memory model stores data in a sparse grid:
 *
 *   byte offset  →  qset node index  +  quantum-array index  +  offset-within-quantum
 *
 * Given file position f:
 *   itemsize  = qset * quantum          (bytes addressable by one qset node)
 *   item      = f / itemsize            (which qset node)
 *   rest      = f % itemsize            (offset within that node)
 *   s_pos     = rest / quantum          (which quantum slot in data[])
 *   q_pos     = rest % quantum          (byte offset within that quantum)
 *
 * Each read transfers at most one quantum worth of data.  The caller
 * (user-space read loop or the C library) repeats the call until done.
 *
 * Holes (NULL data[s_pos] slots) are returned as zero bytes — memset-zero
 * because kzalloc is used for quantums, but here we explicitly handle NULL
 * slots by copying from a zero page rather than allocating.
 *
 * LOCKING: mutex held for the entire read to prevent concurrent writes from
 * changing the data mid-copy.  copy_to_user() can sleep, which is legal
 * while holding a mutex (but NOT a spinlock).
 */
static ssize_t scull_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *f_pos)
{
    struct scull_dev  *dev  = filp->private_data;
    struct scull_qset *dptr;
    int    quantum_local, qset_local;
    int    itemsize, item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    quantum_local = dev->quantum;
    qset_local    = dev->qset;
    itemsize      = quantum_local * qset_local; /* bytes per qset node */

    /* EOF: file position is at or past the end of written data. */
    if (*f_pos >= dev->size)
        goto out;

    /* Clamp count so we never read past the end of written data. */
    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    /* Decompose the file offset into (item, s_pos, q_pos). */
    item  = (long)*f_pos / itemsize;          /* which qset node    */
    rest  = (long)*f_pos % itemsize;          /* offset within node  */
    s_pos = rest / quantum_local;             /* quantum index       */
    q_pos = rest % quantum_local;             /* byte in quantum     */

    /* Walk to the correct qset node (read-only: scull_follow allocates on
     * write paths, but here we only follow existing nodes).
     * Note: scull_follow() never allocates new nodes unless item > list length,
     * which is safe since we already checked f_pos < dev->size.
     * A NULL return means a hole (the node exists only partially, or not at all).
     */
    dptr = scull_follow(dev, item);
    if (!dptr || !dptr->data || !dptr->data[s_pos]) {
        /*
         * Sparse hole: this quantum was never written.
         * Return zero bytes to the user.  We cap at the quantum boundary
         * to preserve the "one quantum per call" contract.
         * clear_user() is the correct kernel API: it writes zeros to a
         * user-space buffer without needing a kernel-side zero buffer,
         * and it works for any count up to quantum_local.
         */
        count = min_t(size_t, count,
                      (size_t)(quantum_local - q_pos));
        if (clear_user(buf, count)) {
            retval = -EFAULT;
            goto out;
        }
        *f_pos += count;
        retval  = count;
        goto out;
    }

    /* Don't read past the current quantum boundary in one call. */
    if (count > (size_t)(quantum_local - q_pos))
        count = quantum_local - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += count;
    retval  = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/*
 * scull_write() — Write up to @count bytes starting at *@f_pos.
 *
 * Uses the same decomposition as scull_read().  Allocates:
 *   • the data[] pointer array for a qset node on first write to that node.
 *   • individual quantum buffers on first write to each quantum slot.
 *
 * Like read, a single write() call transfers at most one quantum's worth
 * of data.  The VFS re-calls write() until the user buffer is exhausted.
 *
 * If any allocation fails, -ENOMEM is returned.  Partially completed
 * allocations are retained (not rolled back) so the caller can retry
 * at the updated position.
 *
 * LOCKING: mutex held for the entire write (see scull_read notes).
 */
static ssize_t scull_write(struct file *filp, const char __user *buf,
                           size_t count, loff_t *f_pos)
{
    struct scull_dev  *dev  = filp->private_data;
    struct scull_qset *dptr;
    int    quantum_local, qset_local;
    int    itemsize, item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    quantum_local = dev->quantum;
    qset_local    = dev->qset;
    itemsize      = quantum_local * qset_local;

    /* Decompose the file offset into (item, s_pos, q_pos). */
    item  = (long)*f_pos / itemsize;
    rest  = (long)*f_pos % itemsize;
    s_pos = rest / quantum_local;
    q_pos = rest % quantum_local;

    /* Walk to (or allocate) the correct qset node. */
    dptr = scull_follow(dev, item);
    if (!dptr)
        goto out;

    /* Allocate the quantum pointer array for this node if not present. */
    if (!dptr->data) {
        dptr->data = kcalloc(qset_local, sizeof(void *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
    }

    /* Allocate the specific quantum buffer if not present. */
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kzalloc(quantum_local, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }

    /* Don't write past the current quantum boundary in one call. */
    if (count > (size_t)(quantum_local - q_pos))
        count = quantum_local - q_pos;

    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += count;
    retval  = count;

    /*
     * Update device size: the logical size is the maximum file position
     * ever written to.  This mirrors how a regular file grows.
     */
    if ((unsigned long)*f_pos > dev->size)
        dev->size = *f_pos;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/*
 * scull_llseek() — Update the file position.
 *
 * Supports SEEK_SET, SEEK_CUR, and SEEK_END.
 * SEEK_END is measured from the device's logical size (dev->size).
 *
 * A negative resulting position is rejected with -EINVAL.
 * Seeking past the end of written data is allowed (like on regular files).
 *
 * LOCKING: mutex taken to read dev->size consistently.
 */
static loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
    struct scull_dev *dev = filp->private_data;
    loff_t newpos;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    switch (whence) {
    case SEEK_SET:
        newpos = off;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + off;
        break;
    case SEEK_END:
        newpos = (loff_t)dev->size + off;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (newpos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);
    return newpos;
}

/*
 * scull_ioctl() — Handle ioctl commands.
 *
 * Currently defined commands:
 *   SCULL_IOCRESET  (_IO('k', 0))
 *       Free all quantum memory, reset the device to an empty state.
 *       After reset, reads return EOF immediately.
 *
 * Unknown commands return -ENOTTY ("inappropriate ioctl for device"),
 * which is the POSIX-compliant error for an unrecognized ioctl.
 *
 * LOCKING: mutex taken inside scull_trim(), which is called here.
 */
static long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct scull_dev *dev = filp->private_data;
    int err = 0;

    /*
     * Validate the magic number.  Commands with a wrong magic number
     * are rejected before we touch any device state.
     */
    if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC)
        return -ENOTTY;
    if (_IOC_NR(cmd) > SCULL_IOC_MAXNUM)
        return -ENOTTY;

    switch (cmd) {
    case SCULL_IOCRESET:
        pr_debug("ioctl: RESET requested by pid=%d\n", current->pid);
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
        err = scull_trim(dev);
        mutex_unlock(&dev->lock);
        break;

    default:
        return -ENOTTY;
    }

    return err;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MODULE INIT / EXIT                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * scull_setup_cdev() — Initialise and register the cdev object.
 *
 * Called from scull_init_module().  The cdev is embedded inside scull_dev
 * so that container_of() can recover the device pointer in scull_open().
 */
static int scull_setup_cdev(struct scull_dev *dev, dev_t devno)
{
    int err;

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;

    err = cdev_add(&dev->cdev, devno, 1 /* one minor number */);
    if (err)
        pr_err("cdev_add failed: %d\n", err);

    return err;
}

/*
 * scull_init_module() — Module entry point.
 *
 * Performs, in order:
 *   1. alloc_chrdev_region() — ask the kernel for a dynamic major number.
 *   2. Initialise the scull_dev structure (mutex, default parameters).
 *   3. scull_setup_cdev() — attach the file_operations.
 *   4. class_create() — create /sys/class/scull for udev.
 *   5. device_create() — create the /dev/scull node via udev/devtmpfs.
 *
 * On any failure, all previously completed steps are reversed (goto-based
 * error ladder) so no resources are leaked.
 *
 * class_create() API compatibility:
 *   Kernels ≥ 6.3 changed class_create() to take only one argument (the
 *   name string).  Kernels ≤ 6.2 take (THIS_MODULE, name).
 *   We use LINUX_VERSION_CODE to select the correct call at compile time.
 */
static int __init scull_init_module(void)
{
    int result;

    /* 1. Allocate a dynamic major number (one minor: 0). */
    result = alloc_chrdev_region(&scull_devno, 0, 1, "scull");
    if (result < 0) {
        pr_err("alloc_chrdev_region failed: %d\n", result);
        return result;
    }
    pr_info("registered with major=%d minor=%d\n",
            MAJOR(scull_devno), MINOR(scull_devno));

    /* 2. Initialise the device structure. */
    memset(&scull_device, 0, sizeof(scull_device));
    mutex_init(&scull_device.lock);
    scull_device.quantum = quantum;
    scull_device.qset    = qset;

    /* 3. Register the cdev. */
    result = scull_setup_cdev(&scull_device, scull_devno);
    if (result)
        goto fail_cdev;

    /* 4. Create /sys/class/scull so udev can create the device node.
     *
     *    class_create() signature changed in kernel 6.3:
     *      ≤ 6.2 : class_create(owner, name)
     *      ≥ 6.3 : class_create(name)           ← no owner argument
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    scull_class = class_create("scull");
#else
    scull_class = class_create(THIS_MODULE, "scull");
#endif
    if (IS_ERR(scull_class)) {
        result = PTR_ERR(scull_class);
        pr_err("class_create failed: %d\n", result);
        goto fail_class;
    }

    /* 5. Create the /dev/scull device node. */
    scull_dev_node = device_create(scull_class, NULL, scull_devno,
                                   NULL, "scull");
    if (IS_ERR(scull_dev_node)) {
        result = PTR_ERR(scull_dev_node);
        pr_err("device_create failed: %d\n", result);
        goto fail_device;
    }

    pr_info("SCULL device ready: /dev/scull (quantum=%d, qset=%d)\n",
            quantum, qset);
    return 0;

fail_device:
    class_destroy(scull_class);
fail_class:
    cdev_del(&scull_device.cdev);
fail_cdev:
    unregister_chrdev_region(scull_devno, 1);
    return result;
}

/*
 * scull_cleanup_module() — Module exit point.
 *
 * Reverses every step performed by scull_init_module() in opposite order.
 * Also calls scull_trim() to free any quantum data that was never freed
 * during normal device use.
 */
static void __exit scull_cleanup_module(void)
{
    device_destroy(scull_class, scull_devno);
    class_destroy(scull_class);
    cdev_del(&scull_device.cdev);

    /*
     * Free all quantum data.  The device is unregistered above, so no
     * new file operations can start.  Existing opens: we do not wait for
     * them here (a real production driver would use kref/srcu).
     * For this educational driver, assuming users close before rmmod.
     */
    mutex_lock(&scull_device.lock);
    scull_trim(&scull_device);
    mutex_unlock(&scull_device.lock);

    unregister_chrdev_region(scull_devno, 1);
    pr_info("SCULL unloaded\n");
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
