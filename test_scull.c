/*
 * test_scull.c — Userspace test program for the SCULL character device driver
 *
 * Build:  gcc -Wall -Wextra -o test_scull test_scull.c
 * Run  :  sudo ./test_scull          (module must be loaded first)
 *
 * Tests performed:
 *   1.  Device can be opened.
 *   2.  A short string can be written and read back correctly.
 *   3.  Seeking to offset 0 and re-reading returns the same data.
 *   4.  Seeking to a non-zero offset and reading returns the expected sub-string.
 *   5.  Write data spanning a quantum boundary; read it back in pieces.
 *   6.  SCULL_IOCRESET frees all data.
 *   7.  Reading after reset returns EOF (0 bytes).
 *   8.  Multiple sequential writes accumulate data correctly.
 *   9.  A large write (~3 × quantum = ~12 000 bytes) spanning multiple quanta.
 *  10.  Invalid ioctl command returns ENOTTY.
 *
 * Each test prints PASS or FAIL and, on failure, the errno message.
 * The program exits with 0 if all tests pass, 1 if any test fails.
 */

#define _GNU_SOURCE     /* for pread/pwrite if needed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

/* Include the shared header for SCULL_IOCRESET etc.
 * __KERNEL__ is not defined in userspace, so only the ioctl definitions
 * and the constants will be visible (the structs are guarded). */
#include "scull.h"

/* ─── Helpers ──────────────────────────────────────────────────────────────── */

#define DEVICE_PATH "/dev/scull"

static int  total_tests  = 0;
static int  passed_tests = 0;

static void report(const char *name, int passed, const char *detail)
{
    total_tests++;
    if (passed) {
        passed_tests++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s — %s\n", name,
               detail ? detail : strerror(errno));
    }
}

/* Open the device, print an error and exit if it fails. */
static int open_device(void)
{
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", DEVICE_PATH, strerror(errno));
        fprintf(stderr, "Make sure scull.ko is loaded: sudo insmod scull.ko\n");
        exit(EXIT_FAILURE);
    }
    return fd;
}

/* Reset the device via ioctl. */
static int do_reset(int fd)
{
    return ioctl(fd, SCULL_IOCRESET, 0);
}

/* ─── Test functions ────────────────────────────────────────────────────────── */

/*
 * Test 1: Open the device.
 * (If open_device() fails it exits, so reaching here means PASS.)
 */
static void test_open(void)
{
    int fd = open_device();
    report("Open /dev/scull", fd >= 0, NULL);
    close(fd);
}

/*
 * Test 2: Write a short message and read it back verbatim.
 */
static void test_write_read(void)
{
    int   fd  = open_device();
    const char *msg = "Hello, SCULL!";
    char   buf[64];
    ssize_t n;

    /* Reset so we start clean. */
    do_reset(fd);

    n = write(fd, msg, strlen(msg));
    if (n != (ssize_t)strlen(msg)) {
        report("Write short message", 0, "write returned wrong count");
        close(fd); return;
    }

    /* Seek back to beginning. */
    if (lseek(fd, 0, SEEK_SET) != 0) {
        report("Write short message", 0, "lseek failed");
        close(fd); return;
    }

    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    if (n != (ssize_t)strlen(msg)) {
        report("Write/read short message", 0, "read returned wrong count");
        close(fd); return;
    }

    report("Write/read short message",
           memcmp(buf, msg, strlen(msg)) == 0,
           "data mismatch");
    close(fd);
}

/*
 * Test 3: Seek to offset 0 after writing and re-read.
 */
static void test_seek_and_read(void)
{
    int   fd = open_device();
    const char *msg = "SeekTest1234";
    char  buf[64];
    ssize_t n;

    do_reset(fd);
    write(fd, msg, strlen(msg));

    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, strlen(msg));

    report("Seek to 0 and re-read",
           n == (ssize_t)strlen(msg) && memcmp(buf, msg, strlen(msg)) == 0,
           NULL);
    close(fd);
}

/*
 * Test 4: Seek to a non-zero offset and read the expected substring.
 */
static void test_partial_seek_read(void)
{
    int   fd = open_device();
    const char *msg = "ABCDEFGHIJ";
    char  buf[8];
    ssize_t n;
    int offset = 3;  /* expect "DEFGHIJ" starting at byte 3 */

    do_reset(fd);
    write(fd, msg, strlen(msg));

    lseek(fd, offset, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 4);  /* read "DEFG" */

    report("Partial seek and read",
           n == 4 && memcmp(buf, msg + offset, 4) == 0,
           NULL);
    close(fd);
}

/*
 * Test 5: Write data that spans a quantum boundary.
 *
 * SCULL_QUANTUM_DEFAULT = 4000.  We write 4 bytes at position 3998, so bytes
 * 3998-3999 land in quantum 0 and bytes 4000-4001 land in quantum 1.
 * Then we read them back across the boundary.
 *
 * Note: the VFS write() call will be split by the driver into two sub-calls
 * (one per quantum).  The C library's write() does not guarantee a single
 * kernel call, so we loop until all bytes are written.
 */
static void test_quantum_boundary(void)
{
    int   fd = open_device();
    const char payload[4] = { 'Q', 'B', 'D', 'Y' };
    char  rbuf[4];
    ssize_t n;
    size_t written = 0, to_write = 4;
    off_t  offset  = SCULL_QUANTUM_DEFAULT - 2; /* 3998 */

    do_reset(fd);

    /* Seek to position 3998. */
    if (lseek(fd, offset, SEEK_SET) != offset) {
        report("Quantum boundary write/read", 0, "lseek failed");
        close(fd); return;
    }

    /* Write loop — driver may split across quantum boundary. */
    while (written < to_write) {
        n = write(fd, payload + written, to_write - written);
        if (n <= 0) {
            report("Quantum boundary write/read", 0, "write failed");
            close(fd); return;
        }
        written += n;
    }

    /* Seek back and read. */
    lseek(fd, offset, SEEK_SET);
    size_t rd = 0;
    while (rd < to_write) {
        n = read(fd, rbuf + rd, to_write - rd);
        if (n <= 0) {
            report("Quantum boundary write/read", 0, "read failed");
            close(fd); return;
        }
        rd += n;
    }

    report("Quantum boundary write/read",
           memcmp(rbuf, payload, to_write) == 0,
           "data mismatch across quantum");
    close(fd);
}

/*
 * Test 6: ioctl SCULL_IOCRESET clears all data.
 */
static void test_ioctl_reset(void)
{
    int   fd = open_device();
    const char *msg = "DataToReset";
    int   ret;

    do_reset(fd);
    write(fd, msg, strlen(msg));

    /* Reset. */
    ret = ioctl(fd, SCULL_IOCRESET, 0);
    report("ioctl SCULL_IOCRESET returns 0", ret == 0, NULL);
    close(fd);
}

/*
 * Test 7: After reset, reading returns EOF (0 bytes).
 */
static void test_read_after_reset(void)
{
    int   fd = open_device();
    const char *msg = "SomeData";
    char  buf[64];
    ssize_t n;

    do_reset(fd);
    write(fd, msg, strlen(msg));

    ioctl(fd, SCULL_IOCRESET, 0);
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf));

    report("Read after reset returns EOF (0)", n == 0, NULL);
    close(fd);
}

/*
 * Test 8: Multiple sequential writes accumulate data.
 */
static void test_multiple_writes(void)
{
    int   fd = open_device();
    char  wbuf[32];
    char  rbuf[32];
    int   i;
    ssize_t n;

    do_reset(fd);

    /* Write 8 × 4-byte chunks = 32 bytes total. */
    for (i = 0; i < 8; i++) {
        memset(wbuf + i * 4, 'A' + i, 4);
        n = write(fd, wbuf + i * 4, 4);
        if (n != 4) {
            report("Multiple sequential writes", 0, "write failed in loop");
            close(fd); return;
        }
    }

    lseek(fd, 0, SEEK_SET);
    memset(rbuf, 0, sizeof(rbuf));
    size_t rd = 0;
    while (rd < 32) {
        n = read(fd, rbuf + rd, 32 - rd);
        if (n <= 0) break;
        rd += n;
    }

    report("Multiple sequential writes",
           rd == 32 && memcmp(rbuf, wbuf, 32) == 0,
           "accumulation mismatch");
    close(fd);
}

/*
 * Test 9: Large write spanning multiple quanta (≈3 × SCULL_QUANTUM_DEFAULT).
 *
 * Allocates a 12 000-byte buffer, fills it with a simple pattern, writes it
 * to the device in a loop, then reads it back and verifies.
 */
static void test_large_write(void)
{
    int     fd = open_device();
    size_t  sz = 3 * SCULL_QUANTUM_DEFAULT;  /* 12 000 bytes */
    char   *wbuf = malloc(sz);
    char   *rbuf = malloc(sz);
    size_t  written = 0, rd = 0;
    ssize_t n;
    int     ok = 1;

    if (!wbuf || !rbuf) {
        report("Large write (~12 000 bytes)", 0, "malloc failed");
        free(wbuf); free(rbuf); close(fd); return;
    }

    /* Fill with rotating pattern. */
    for (size_t i = 0; i < sz; i++)
        wbuf[i] = (char)(i & 0xFF);

    do_reset(fd);

    /* Write loop. */
    while (written < sz) {
        n = write(fd, wbuf + written, sz - written);
        if (n <= 0) { ok = 0; break; }
        written += n;
    }

    if (!ok || written != sz) {
        report("Large write (~12 000 bytes)", 0, "write incomplete");
        free(wbuf); free(rbuf); close(fd); return;
    }

    /* Read loop. */
    lseek(fd, 0, SEEK_SET);
    while (rd < sz) {
        n = read(fd, rbuf + rd, sz - rd);
        if (n <= 0) { ok = 0; break; }
        rd += n;
    }

    report("Large write (~12 000 bytes)",
           ok && rd == sz && memcmp(wbuf, rbuf, sz) == 0,
           "data mismatch on large buffer");
    free(wbuf);
    free(rbuf);
    close(fd);
}

/*
 * Test 10: Invalid ioctl command returns ENOTTY.
 */
static void test_invalid_ioctl(void)
{
    int  fd  = open_device();
    int  ret = ioctl(fd, _IO('Z', 99), 0); /* wrong magic + wrong nr */

    report("Invalid ioctl returns ENOTTY",
           ret == -1 && errno == ENOTTY,
           "expected ENOTTY");
    close(fd);
}

/* ─── Entry point ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== SCULL Driver Test Suite ===\n");
    printf("Device: %s\n\n", DEVICE_PATH);

    test_open();
    test_write_read();
    test_seek_and_read();
    test_partial_seek_read();
    test_quantum_boundary();
    test_ioctl_reset();
    test_read_after_reset();
    test_multiple_writes();
    test_large_write();
    test_invalid_ioctl();

    printf("\n=== Results: %d/%d tests passed ===\n", passed_tests, total_tests);
    return (passed_tests == total_tests) ? EXIT_SUCCESS : EXIT_FAILURE;
}
