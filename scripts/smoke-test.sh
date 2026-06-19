#!/bin/bash
#
# Boot the UEFI kernel image (loader.efi) under QEMU + UEFI firmware and check
# that it reaches its early serial banner. This is a smoke test of the whole
# UEFI/ACPI boot path - firmware -> stub -> kernel premain - not a full test.
#
# Usage: smoke-test.sh <x64|aarch64> <loader.efi> [timeout_seconds]
#
# Firmware is auto-detected but can be overridden:
#   x64:     OVMF_CODE,  OVMF_VARS
#   aarch64: AAVMF_CODE, AAVMF_VARS
#
set -euo pipefail

arch="${1:?usage: smoke-test.sh <x64|aarch64> <loader.efi> [timeout]}"
efi="${2:?missing path to loader.efi}"
timeout_s="${3:-30}"

# First firmware candidate that exists, else empty. Always succeeds (returns 0)
# so it is safe under `set -e` inside a command substitution.
pick() { for f in "$@"; do [ -f "$f" ] && { echo "$f"; break; }; done; return 0; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
esp="$work/esp/EFI/BOOT"
mkdir -p "$esp"
log="$work/serial.log"

case "$arch" in
x64)
    boot_name="BOOTX64.EFI"
    code="${OVMF_CODE:-$(pick /usr/share/OVMF/OVMF_CODE_4M.fd \
                              /usr/share/OVMF/OVMF_CODE.fd \
                              /usr/share/ovmf/OVMF_CODE.fd)}"
    vars="${OVMF_VARS:-$(pick /usr/share/OVMF/OVMF_VARS_4M.fd \
                              /usr/share/OVMF/OVMF_VARS.fd \
                              /usr/share/ovmf/OVMF_VARS.fd)}"
    qemu=qemu-system-x86_64
    machine=(-machine q35 -cpu max)
    ;;
aarch64)
    boot_name="BOOTAA64.EFI"
    code="${AAVMF_CODE:-$(pick /usr/share/AAVMF/AAVMF_CODE.fd \
                               /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
                               /usr/share/edk2/aarch64/QEMU_EFI.fd)}"
    vars="${AAVMF_VARS:-$(pick /usr/share/AAVMF/AAVMF_VARS.fd \
                               /usr/share/qemu-efi-aarch64/QEMU_VARS.fd)}"
    qemu=qemu-system-aarch64
    # GICv3 + ACPI (QEMU virt provides ACPI tables; the firmware passes the
    # RSDP via the UEFI configuration table, which is what the kernel consumes).
    machine=(-machine virt,gic-version=3 -cpu max)
    ;;
*)
    echo "smoke-test: unknown arch '$arch' (expected x64 or aarch64)" >&2
    exit 2
    ;;
esac

if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "smoke-test: $qemu not installed - skipping $arch smoke test." >&2
    exit 3
fi

# Acceleration: x86 OSv's only clock source is kvmclock, which needs KVM, so
# prefer KVM for an x64 guest on a KVM-capable host; otherwise fall back to TCG
# (fine for aarch64, whose clock is the always-available ARM generic timer).
# Override with ACCEL=tcg|kvm.
accel="${ACCEL:-tcg}"
if [ -z "${ACCEL:-}" ] && [ "$arch" = x64 ] && [ -w /dev/kvm ]; then
    accel=kvm
fi
if [ -z "${code:-}" ] || [ -z "${vars:-}" ]; then
    echo "smoke-test: no UEFI firmware found for $arch." >&2
    echo "  install it (x64: 'ovmf', aarch64: 'qemu-efi-aarch64') or set" >&2
    echo "  ${arch}=... CODE/VARS env vars; then re-run." >&2
    exit 3
fi

cp "$efi" "$esp/$boot_name"
# pflash needs a writable copy of the variable store.
varscopy="$work/vars.fd"
cp "$vars" "$varscopy"

# QEMU's read-write vvfat ("fat:rw:") keeps its backing file under /var/tmp;
# create it if the environment is missing it (some minimal images are).
mkdir -p /var/tmp 2>/dev/null || true

echo "smoke-test: $arch via $qemu (accel=$accel)"
echo "  firmware: $code"
echo "  booting:  $boot_name (timeout ${timeout_s}s)"

# -accel tcg so the test does not depend on KVM being available. OVMF/AAVMF
# mirror the UEFI console to the serial port, so $log captures both the
# firmware/stub messages and the kernel's COM/PL011 early-console output.
# QEMU's own diagnostics go to qemu.out so failures (e.g. firmware/vvfat) show.
set +e
timeout --foreground "$timeout_s" "$qemu" \
    "${machine[@]}" -m 512 -accel "$accel" -no-reboot -display none \
    -drive "if=pflash,format=raw,readonly=on,file=$code" \
    -drive "if=pflash,format=raw,file=$varscopy" \
    -drive "format=raw,file=fat:rw:$work/esp" \
    -serial "file:$log" >"$work/qemu.out" 2>&1
set -e

echo "----- serial output -----"
cat "$log" 2>/dev/null || true
echo "-------------------------"
if [ -s "$work/qemu.out" ]; then
    echo "----- qemu diagnostics -----"
    cat "$work/qemu.out"
    echo "----------------------------"
fi

# The kernel prints "OSv <version>" from premain() over the early console as
# soon as it is running (loader.cc). Seeing it means firmware, the UEFI stub,
# the arch entry and premain all worked.
if grep -q "OSv " "$log" 2>/dev/null; then
    echo "smoke-test: PASS ($arch reached the kernel banner)"
    exit 0
fi
if grep -q "miniosv UEFI stub starting" "$log" 2>/dev/null; then
    echo "smoke-test: PARTIAL - stub ran but kernel banner not seen ($arch)" >&2
else
    echo "smoke-test: FAIL - kernel did not reach its banner ($arch)" >&2
fi
exit 1
