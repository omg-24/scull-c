/* SPDX-License-Identifier: GPL-2.0
 *
 * scull.h — Public header for the SCULL character device driver (C port)
 *
 * SCULL: Simple Character Utility for Loading and Unloading
 * Based on the design from "Linux Device Drivers" (Rubini, Corbet, Kroah-Hartman)
 *
 * This header is shared by the kernel driver (scull.c) and the
 * userspace test program (test_scull.c).  The ioctl definitions are
 * placed here so both sides use identical numbers.
 */

#ifndef _SCULL_H_
#define _SCULL_H_

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * DEFAULT TUNEABLE PARAMETERS
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * quantum   : The size (in bytes) of a single data block ("quantum").
 *             Reads and writes never cross a quantum boundary in one step,
 *             mirroring the original LDD3 design.
 *
 * qset      : The number of quantum pointers in a single qset node.
 *             Each node in the linked list holds an array of (qset) pointers
 *             to individually allocated quanta.
 *
 * Maximum theoretical device size = quantum * qset * (number of qset nodes)
 * Storage is allocated lazily on write, so an empty device uses no quantum RAM.
 */
#define SCULL_QUANTUM_DEFAULT   4000    /* bytes per quantum */
#define SCULL_QSET_DEFAULT      1000    /* quantum pointers per qset node */

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * IOCTL DEFINITIONS
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Magic number: 'k' (0x6b).  The kernel ioctl(2) conventions require a unique
 * magic byte per driver so that incorrect ioctl numbers are caught early.
 * 0x6b is not claimed by any driver in <linux/ioctl.h> Documentation/ioctl/.
 *
 * _IO(type, nr) defines a command that carries no data.
 * Ordinal 0 is SCULL_IOC_RESET — free all quantum memory and reset size to 0.
 */
#define SCULL_IOC_MAGIC         'k'
#define SCULL_IOCRESET          _IO(SCULL_IOC_MAGIC, 0)

/* Sentinels used to range-check ioctl commands inside the driver. */
#define SCULL_IOC_MINNUM        0
#define SCULL_IOC_MAXNUM        0

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * The structures below are kernel-only.  They are hidden behind an #ifdef so
 * that test_scull.c (compiled in user space) can safely #include this header
 * for the ioctl definitions without pulling in <linux/…> headers.
 */
#ifdef __KERNEL__

#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/types.h>

/*
 * scull_qset — one node in the linked list of qset nodes
 *
 * Each node owns an array of (qset) char-pointer slots (data[]).
 * Each non-NULL slot points to a separately kzalloc'd quantum buffer.
 * NULL slots represent unwritten ("hole") regions; reads from holes
 * return zero bytes (like a sparse file).
 *
 *  ┌─────────────┐     ┌─────────────┐
 *  │ scull_qset  │────▶│ scull_qset  │────▶ NULL
 *  │  data[0] ───┼──▶ quantum_buf_0  │
 *  │  data[1]  = NULL (hole)         │
 *  │  data[2] ───┼──▶ quantum_buf_2  │
 *  │  …          │                   │
 *  └─────────────┘     └─────────────┘
 */
struct scull_qset {
    void            **data;         /* array of (dev->qset) quantum pointers */
    struct scull_qset *next;        /* next node in the linked list, or NULL */
};

/*
 * scull_dev — the top-level device structure
 *
 * One instance is allocated by scull_init_module() and freed by
 * scull_cleanup_module().  All fields except cdev and class_dev (which are
 * set up once at module init time and torn down at exit time) are protected
 * by the mutex.
 *
 * Locking strategy:
 *   - The mutex is taken at the start of open(), read(), write(), llseek(),
 *     and ioctl(), and released before returning.
 *   - release() does NOT take the mutex because it performs no device-data
 *     access; it merely decrements a reference counter managed by the VFS.
 *   - Trim (reset) always holds the mutex to prevent concurrent reads or
 *     writes from accessing freed memory.
 */
struct scull_dev {
    struct scull_qset   *data;      /* pointer to the first qset node */
    int                  quantum;   /* current quantum size (bytes) */
    int                  qset;      /* current qset size (# of quantum ptrs) */
    unsigned long        size;      /* total bytes written to device so far */
    struct mutex         lock;      /* serialises all device-data access */
    struct cdev          cdev;      /* Linux character-device object */
};

#endif /* __KERNEL__ */
#endif /* _SCULL_H_ */
