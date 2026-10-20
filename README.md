# SCULL — Simple Character Utility for Loading and Unloading (C Implementation)

> A Linux kernel character device driver written in C, implementing the SCULL-style memory-backed device described in *Linux Device Drivers* (3rd ed.) by Rubini, Corbet & Kroah-Hartman.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Objectives](#2-objectives)
3. [SCULL Architecture](#3-scull-architecture)
4. [Quantum and Qset Explanation](#4-quantum-and-qset-explanation)
5. [Linked-List Storage Design](#5-linked-list-storage-design)
6. [File Operation Descriptions](#6-file-operation-descriptions)
7. [Memory Management](#7-memory-management)
8. [Synchronization Strategy](#8-synchronization-strategy)
9. [Ioctl Interface](#9-ioctl-interface)
10. [Build Instructions](#10-build-instructions)
11. [Module Loading and Unloading](#11-module-loading-and-unloading)
12. [Testing Instructions](#12-testing-instructions)
13. [Expected Results](#13-expected-results)
14. [Troubleshooting](#14-troubleshooting)
15. [Limitations and Safety Notes](#15-limitations-and-safety-notes)
16. [Project Directory Structure](#16-project-directory-structure)

---

## 1. Project Overview

SCULL is a **character device driver** that exposes a virtual block of memory as the file `/dev/scull`. User-space programs can `open`, `read`, `write`, `seek`, and `ioctl` the device exactly like a real hardware device, making it an ideal teaching tool for:

- Linux kernel module development
- Character device registration (cdev, major/minor numbers)
- Kernel memory management (`kzalloc`, `kcalloc`, `kfree`)
- Kernel/user-space data transfer (`copy_to_user`, `copy_from_user`)
- Kernel synchronization (`mutex`)
- Ioctl command design

No hardware is required. The driver runs entirely in RAM.

---

## 2. Objectives

| Goal | Status |
|------|--------|
| Genuine `.ko` kernel module (not FUSE/userspace simulation) | ✅ |
| Linux kernel module and userspace test program written in C | ✅ |
| Dynamic major number allocation | ✅ |
| Automatic `/dev/scull` via udev/devtmpfs | ✅ |
| Sparse quantum/qset memory model | ✅ |
| Full file operations: open, release, read, write, llseek, ioctl | ✅ |
| Mutex synchronization | ✅ |
| Complete memory cleanup on rmmod and ioctl reset | ✅ |
| Kernel 5.x and 6.x compatible | ✅ |
| Userspace C test program | ✅ |
| Shell smoke-test script | ✅ |

---

## 3. SCULL Architecture

```
User Space                   Kernel Space
──────────                   ─────────────
open("/dev/scull")  ──────▶  scull_open()
write(fd, buf, n)   ──────▶  scull_write()   ──▶  quantum buffers
read(fd, buf, n)    ──────▶  scull_read()    ──▶  quantum buffers
lseek(fd, off, w)   ──────▶  scull_llseek()
ioctl(fd, RESET, 0) ──────▶  scull_ioctl()   ──▶  scull_trim()
close(fd)           ──────▶  scull_release()
```

The driver is registered via `alloc_chrdev_region()` which assigns a **dynamic major number**. A **udev device class** (`/sys/class/scull`) causes udev to automatically create `/dev/scull` when the module is inserted.

---

## 4. Quantum and Qset Explanation

| Term | Default | Meaning |
|------|---------|---------|
| **quantum** | 4 000 bytes | The smallest unit of storage. One `kzalloc`'d buffer. |
| **qset** | 1 000 | Number of quantum-pointer slots in one qset node. |

**Maximum size of one qset node** = `quantum × qset` = 4 000 × 1 000 = **4 MB**

The driver can grow to an arbitrary number of qset nodes (limited only by available RAM).

```
Byte offset 0        4000       8000   ...   4,000,000  4,000,001
             │ quantum 0 │ quantum 1 │  ...  │ quantum 999│  ← qset node 0
             │                                             │
             ╰─────────────────── 4 MB ───────────────────╯
             │ quantum 0 │ quantum 1 │  ...  │ quantum 999│  ← qset node 1
             ...
```

Both `quantum` and `qset` are **module parameters**:
```bash
sudo insmod scull.ko quantum=8000 qset=500
```

---

## 5. Linked-List Storage Design

```
scull_dev
  │
  └─▶ scull_qset [node 0]
        data[0] ──▶ [ 4000 bytes ]    ← allocated on first write
        data[1] ──▶ NULL              ← hole: reads return zero bytes
        data[2] ──▶ [ 4000 bytes ]
        …
        data[999] ──▶ …
        next ──▶ scull_qset [node 1]
                   data[0] ──▶ …
                   next ──▶ NULL
```

Key properties:
- **Sparse**: NULL slots consume no RAM; reads from them return `\0` bytes.
- **Lazy allocation**: nodes and quanta are only allocated when data is **written**.
- **Linked list**: allows arbitrary growth without pre-allocating the full maximum.

---

## 6. File Operation Descriptions

### `open()`
- Recovers the `scull_dev` via `container_of(inode->i_cdev, ...)`.
- Stores the pointer in `filp->private_data` for use by subsequent calls.
- Does **not** reset the device (data persists across open/close).

### `release()`
- No-op. VFS handles reference counting. No per-open state to free.

### `read()`
1. Acquires the mutex.
2. Returns EOF if `f_pos >= dev->size`.
3. Clamps `count` to `dev->size - f_pos`.
4. Decomposes `f_pos` into `(item, s_pos, q_pos)`.
5. Calls `scull_follow()` (read-only: no allocation).
6. For NULL quantum slots (holes): copies zeros to user space.
7. Transfers at most `quantum - q_pos` bytes per call.
8. Advances `f_pos`.

### `write()`
1. Acquires the mutex.
2. Decomposes `f_pos`.
3. Calls `scull_follow()`, allocating qset nodes as needed.
4. Allocates the `data[]` pointer array if missing.
5. Allocates the specific quantum buffer if missing.
6. Transfers at most `quantum - q_pos` bytes per call.
7. Updates `dev->size = max(dev->size, f_pos + bytes_written)`.

### `llseek()`
- Supports `SEEK_SET`, `SEEK_CUR`, `SEEK_END`.
- Rejects negative resulting positions with `-EINVAL`.
- Seeking past the end is allowed (like regular files).

### `ioctl()`
- Validates magic number and command ordinal.
- `SCULL_IOCRESET`: calls `scull_trim()` under the mutex.
- Unknown commands: returns `-ENOTTY`.

---

## 7. Memory Management

| Allocation | Function | Freed by |
|-----------|----------|----------|
| `scull_qset` node | `kzalloc` | `scull_trim()` |
| `data[]` pointer array | `kcalloc` | `scull_trim()` |
| Individual quantum buffer | `kzalloc` | `scull_trim()` |
| `scull_dev` | static (not heap) | module exit |

### `scull_trim()` algorithm
```c
for each qset node in linked list:
    for i in 0..qset:
        kfree(data[i])   // each quantum (no-op if NULL)
    kfree(data)          // the pointer array
    next = node->next
    kfree(node)          // the node itself
dev->data = NULL
dev->size = 0
```

Trim is called from:
- `ioctl(SCULL_IOCRESET)` — explicit user reset
- `scull_cleanup_module()` — module removal (`rmmod`)

---

## 8. Synchronization Strategy

A single `struct mutex lock` embedded in `scull_dev` protects all device data.

| Operation | Lock taken? | Reason |
|-----------|-------------|--------|
| `open()` | No | Only reads `inode->i_cdev`, no device data touched |
| `release()` | No | No device data accessed |
| `read()` | Yes (`mutex_lock_interruptible`) | Reads `size`, `data` tree |
| `write()` | Yes | Modifies `data` tree, `size` |
| `llseek()` | Yes | Reads `size` |
| `ioctl(RESET)` | Yes (inside trim) | Frees all data |
| `cleanup_module` | Yes (around trim) | Prevents race with open files |

`mutex_lock_interruptible()` is used so that a signal (e.g. Ctrl-C) can unblock a process waiting for the mutex, returning `-ERESTARTSYS` to the VFS.

`copy_to_user()` / `copy_from_user()` can sleep, which is legal while holding a `mutex` (but not a `spinlock`).

---

## 9. Ioctl Interface

### Magic number: `'k'` (0x6b)

| Command | Value | Effect |
|---------|-------|--------|
| `SCULL_IOCRESET` | `_IO('k', 0)` | Free all data; reset size to 0 |

### Extending the interface
To add a new command (e.g., query current size):
1. Add to `scull.h`:
   ```c
   #define SCULL_IOCQSIZE  _IOR('k', 1, unsigned long)
   #define SCULL_IOC_MAXNUM 1
   ```
2. Handle in `scull_ioctl()`:
   ```c
   case SCULL_IOCQSIZE:
       return put_user(dev->size, (unsigned long __user *)arg);
   ```

---

## 10. Build Instructions

### Prerequisites (Ubuntu)

```bash
# Check kernel version
uname -r

# Install build tools and matching kernel headers
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)

# Verify headers are present
ls /lib/modules/$(uname -r)/build
```

> **If headers are not available for your exact kernel:**  
> The installed kernel and headers must match exactly.  
> Options:
> - `sudo apt install linux-headers-generic` (installs headers for the generic kernel)
> - Boot the matching generic kernel: `sudo apt install linux-generic`
> - Use a different Ubuntu release where headers match

### Build the module

```bash
cd scull-c/

# Build scull.ko
make

# Verify the module was created
ls -lh scull.ko
file scull.ko
```

Expected output:
```
scull.ko: ELF 64-bit LSB relocatable, x86-64, ...
```

### Build the userspace test program

```bash
gcc -Wall -Wextra -o test_scull test_scull.c
```

---

## 11. Module Loading and Unloading

### Load

```bash
# Insert the module
sudo insmod scull.ko

# Check kernel log for success
sudo dmesg | tail -10
# Expected: "scull: registered with major=NNN minor=0"
#           "scull: SCULL device ready: /dev/scull (quantum=4000, qset=1000)"

# Verify device node
ls -la /dev/scull
# Expected: crw------- 1 root root NNN, 0 ...
```

> **If `/dev/scull` does not appear:**  
> udev may not have processed the event. Wait 1 second and retry. If still missing:
> ```bash
> # Get major:minor from sysfs
> cat /sys/class/scull/scull/dev
> # Output: "NNN:0"
>
> # Create node manually
> sudo mknod /dev/scull c NNN 0
> sudo chmod 666 /dev/scull
> ```

### Allow non-root access (optional)

```bash
# udev rule (create once, survives reboots)
echo 'SUBSYSTEM=="scull", MODE="0666"' | sudo tee /etc/udev/rules.d/99-scull.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### Unload

```bash
sudo rmmod scull

# Verify cleanup
sudo dmesg | tail -5
# Expected: "scull: SCULL unloaded"
ls /dev/scull 2>&1
# Expected: No such file or directory
```

### Load with custom parameters

```bash
sudo insmod scull.ko quantum=8000 qset=500
cat /sys/module/scull/parameters/quantum   # → 8000
cat /sys/module/scull/parameters/qset      # → 500
```

---

## 12. Testing Instructions

### Quick shell smoke test

```bash
sudo bash test.sh
```

This tests: load, dmesg, write, read, multi-quantum write, ioctl reset, unload.

### Full C test suite

```bash
# Module must be loaded first
sudo insmod scull.ko

# Grant access to current user (or run tests as root)
sudo chmod 666 /dev/scull

# Run tests
./test_scull

# Unload when done
sudo rmmod scull
```

### Manual quick-check commands

```bash
# Write a message
echo "Hello SCULL" > /dev/scull

# Read it back
cat /dev/scull

# Write binary data
dd if=/dev/urandom of=/dev/scull bs=512 count=4

# Write data larger than one quantum (triggers multi-quantum allocation)
dd if=/dev/zero of=/dev/scull bs=4000 count=3

# Read it back
dd if=/dev/scull of=/dev/null bs=4000 count=3 status=progress

# Reset the device
python3 -c "
import fcntl, os
fd = os.open('/dev/scull', os.O_RDWR)
fcntl.ioctl(fd, (0 << 30) | (ord('k') << 8) | 0)
os.close(fd)
print('Reset OK')
"

# After reset, should be empty
cat /dev/scull && echo "empty" || echo "data present"
```

---

## 13. Expected Results

### `make` output
```
make -C /lib/modules/6.x.x-generic/build M=/path/to/scull-c modules
  CC [M]  /path/to/scull-c/scull.o
  MODPOST /path/to/scull-c/Module.symvers
  CC [M]  /path/to/scull-c/scull.mod.o
  LD [M]  /path/to/scull-c/scull.ko
```

### `sudo insmod scull.ko` then `sudo dmesg | tail -5`
```
[12345.678901] scull: registered with major=244 minor=0
[12345.678950] scull: SCULL device ready: /dev/scull (quantum=4000, qset=1000)
```

### `./test_scull`
```
=== SCULL Driver Test Suite ===
Device: /dev/scull

  [PASS] Open /dev/scull
  [PASS] Write/read short message
  [PASS] Seek to 0 and re-read
  [PASS] Partial seek and read
  [PASS] Quantum boundary write/read
  [PASS] ioctl SCULL_IOCRESET returns 0
  [PASS] Read after reset returns EOF (0)
  [PASS] Multiple sequential writes
  [PASS] Large write (~12 000 bytes)
  [PASS] Invalid ioctl returns ENOTTY

=== Results: 10/10 tests passed ===
```

### `sudo rmmod scull` then `sudo dmesg | tail -3`
```
[12400.123456] scull: SCULL unloaded
```

---

## 14. Troubleshooting

### `make` fails: "No rule to make target"
→ You are not in the `scull-c/` directory. `cd scull-c && make`.

### `make` fails: `/lib/modules/.../build: No such file or directory`
```bash
sudo apt install linux-headers-$(uname -r)
# If not found:
sudo apt install linux-headers-generic
uname -r   # note version
sudo apt install linux-image-$(uname -r)  # install matching kernel
```

### `insmod: ERROR: could not insert module: Invalid module format`
→ The `.ko` was compiled for a different kernel version than the one running. Rebuild after booting the correct kernel.

### `insmod: ERROR: could not insert module: Operation not permitted` (Secure Boot)
→ Secure Boot requires kernel modules to be signed.  
Options:
1. Disable Secure Boot in BIOS/UEFI (safe in a VM).
2. Enroll your own signing key: see `mokutil --import`.
3. Use a VM without Secure Boot.

### `/dev/scull` not created after insmod
```bash
# Check that class/device exist in sysfs
ls /sys/class/scull/

# Create manually
MAJ=$(cat /sys/class/scull/scull/dev | cut -d: -f1)
MIN=$(cat /sys/class/scull/scull/dev | cut -d: -f2)
sudo mknod /dev/scull c $MAJ $MIN
sudo chmod 666 /dev/scull
```

### `permission denied` when writing to `/dev/scull`
```bash
sudo chmod 666 /dev/scull
# Or run test_scull as root: sudo ./test_scull
```

### `dmesg: read kernel buffer failed: Operation not permitted`
```bash
# On Ubuntu 20.04+ dmesg requires root
sudo dmesg | tail -20
# Or relax restriction (not recommended on production):
sudo sysctl kernel.dmesg_restrict=0
```

### Module loaded but `cat /dev/scull` returns nothing after write
→ Check that your `echo`/`write` landed at position 0. The file offset after `write` is at the end; `cat` opens a fresh fd at position 0 so this should work. If you wrote via `>>` (append), confirm `echo -n "msg" > /dev/scull` (overwrite, not append).

### `FATAL: Module scull is in use`
→ A process still has `/dev/scull` open. Find and close it:
```bash
sudo lsof /dev/scull
sudo fuser /dev/scull
```

---

## 15. Limitations and Safety Notes

> [!CAUTION]
> Always test kernel modules inside a **virtual machine**. A bug in kernel code can crash (panic) the entire system or corrupt the filesystem. **Take a VM snapshot before loading any custom module.**

> [!WARNING]
> This driver does not implement:
> - `poll`/`select` support (the device never becomes "ready" asynchronously)
> - `mmap` support
> - Multiple device instances (only one `/dev/scull`)
> - Concurrent-open safety after `rmmod` while files are still open (use `kref` in production)
> - Module signing (required if Secure Boot is enabled)

> [!NOTE]
> **Sparse hole semantics**: Reading from a position that was never written returns zero bytes. This matches POSIX sparse-file behaviour. No attempt is made to distinguish "truly written zeros" from "hole zeros".

> [!TIP]
> To tune performance, try larger `quantum` values (e.g., `quantum=65536`) to reduce the number of `kzalloc` calls for large sequential writes.

---

## 16. Project Directory Structure

```
scull-c/
├── scull.c          ← complete kernel driver (the .ko source)
├── scull.h          ← shared constants, structs, ioctl definitions
├── Makefile         ← Kbuild + convenience targets (load, test, install)
├── test_scull.c     ← userspace C test program (10 tests)
├── test.sh          ← shell smoke-test script
├── README.md        ← this file
└── .gitignore       ← ignores *.ko, *.o, *.mod, test_scull binary
```

---
