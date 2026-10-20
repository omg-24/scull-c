#!/usr/bin/env bash
# test.sh — Shell smoke-test for the SCULL kernel driver
#
# Usage:
#   sudo bash test.sh
#
# This script MUST be run as root (or via sudo) because:
#   - insmod / rmmod require root.
#   - Reading kernel logs via dmesg may require root.
#   - /dev/scull is mode 0600 (or 0666 depending on your udev rules).
#
# Recommended: take a VM snapshot before running ANY kernel-module tests.
#
# Exit codes:
#   0 — all smoke tests passed
#   1 — one or more tests failed

set -euo pipefail

# ── Colours ────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'   # No colour

PASS=0
FAIL=0
DEVICE=/dev/scull
MODULE=scull
KO=scull.ko

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS+1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1: $2"; FAIL=$((FAIL+1)); }
info() { echo -e "  ${YELLOW}[INFO]${NC} $1"; }

# ── Pre-flight ─────────────────────────────────────────────────────────────────
echo "=== SCULL Shell Smoke Test ==="
echo ""

# Must run as root.
if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (sudo bash test.sh)." >&2
    exit 1
fi

# Module binary must exist.
if [[ ! -f "$KO" ]]; then
    echo "scull.ko not found.  Run 'make' first." >&2
    exit 1
fi

# ── Load module ────────────────────────────────────────────────────────────────
echo "--- Loading module ---"

# Remove any pre-existing instance (ignore error if not loaded).
rmmod "$MODULE" 2>/dev/null || true

insmod "$KO" && pass "insmod scull.ko" || { fail "insmod" "$(dmesg | tail -3)"; exit 1; }

# Give udev a moment to create /dev/scull.
sleep 0.5

if [[ -c "$DEVICE" ]]; then
    pass "/dev/scull device node exists"
else
    fail "/dev/scull" "device node not created (udev issue?)"
    info "Try: sudo mknod /dev/scull c \$(cat /sys/class/scull/scull/dev | cut -d: -f1) 0"
    info "Then: sudo chmod 666 /dev/scull"
fi

# Check dmesg for our init message.
if dmesg | tail -20 | grep -q "SCULL device ready"; then
    pass "Kernel init message visible in dmesg"
else
    fail "dmesg check" "init message not found"
fi

# ── Functional tests (all commands run as root = file readable/writable) ────────
echo ""
echo "--- Functional tests ---"

# Ensure device is readable and writable.
if [[ -r "$DEVICE" && -w "$DEVICE" ]]; then
    pass "/dev/scull is readable and writable"
else
    fail "Permissions" "/dev/scull not rw"
fi

# Write a message.
MSG="Hello from shell test"
echo -n "$MSG" > "$DEVICE" && pass "echo write to /dev/scull" \
    || fail "write" "echo to device failed"

# Read it back.
READBACK=$(head -c ${#MSG} "$DEVICE" 2>/dev/null)
if [[ "$READBACK" == "$MSG" ]]; then
    pass "Read back matches written data"
else
    fail "Read back" "got '${READBACK}' expected '${MSG}'"
fi

# Append more data.
EXTRA=" (appended)"
printf '%s' "$EXTRA" >> "$DEVICE" && pass "Append write" \
    || fail "Append" "append failed"

# dd write of binary-ish data.
dd if=/dev/urandom of="$DEVICE" bs=512 count=1 status=none 2>/dev/null \
    && pass "dd 512-byte random write" \
    || fail "dd write" "dd failed"

# Reset via ioctl using Python (widely available, avoids a custom C helper).
info "Attempting ioctl reset via Python..."
if python3 - <<'PYEOF' 2>/dev/null; then
import fcntl, struct, os
SCULL_IOC_MAGIC = ord('k')
SCULL_IOCRESET  = (0 << 30) | (SCULL_IOC_MAGIC << 8) | 0  # _IO('k', 0)
fd = os.open('/dev/scull', os.O_RDWR)
fcntl.ioctl(fd, SCULL_IOCRESET)
os.close(fd)
PYEOF
    pass "ioctl SCULL_IOCRESET via Python helper"
else
    fail "ioctl reset" "Python ioctl failed (non-fatal, test with test_scull C program)"
fi

# After reset, read should return nothing.
AFTER_RESET=$(cat "$DEVICE" 2>/dev/null)
if [[ -z "$AFTER_RESET" ]]; then
    pass "Read after reset returns empty"
else
    fail "Read after reset" "device not empty after reset"
fi

# Write data larger than one quantum (4000 bytes) using dd.
dd if=/dev/zero of="$DEVICE" bs=4096 count=3 status=none 2>/dev/null \
    && pass "dd write 3×4096 bytes (multi-quantum)" \
    || fail "large dd write" "dd failed"

# Verify size is non-zero using /proc/... or by seeking + reading.
BIGREAD=$(dd if="$DEVICE" bs=4096 count=3 status=none 2>/dev/null | wc -c)
if [[ "$BIGREAD" -eq 12288 ]]; then
    pass "Read back 12288 bytes after multi-quantum write"
else
    fail "Large read" "got ${BIGREAD} bytes, expected 12288"
fi

# ── Unload module ─────────────────────────────────────────────────────────────
echo ""
echo "--- Unloading module ---"

rmmod "$MODULE" && pass "rmmod scull" || fail "rmmod" "rmmod failed"

if dmesg | tail -10 | grep -q "SCULL unloaded"; then
    pass "Kernel exit message visible in dmesg"
else
    fail "dmesg exit" "exit message not found"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "=== Shell smoke test results: $PASS passed, $FAIL failed ==="

if [[ "$FAIL" -eq 0 ]]; then
    echo -e "${GREEN}All shell tests passed.${NC}"
    exit 0
else
    echo -e "${RED}Some tests FAILED.  Check dmesg for details:${NC}"
    echo "  sudo dmesg | tail -30"
    exit 1
fi
