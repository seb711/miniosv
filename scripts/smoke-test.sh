#!/bin/bash
#
# Boot the real bootable disk image (loader.img - a GPT/ESP raw image, exactly
# what the public clouds ingest) under QEMU + UEFI firmware and check that it
# reaches its early serial banner. This is a smoke test of the whole UEFI/ACPI
# boot path - firmware -> stub -> kernel premain - not a full test.
#
# The image is attached as an NVMe disk (as on AWS Nitro) and the guest is given
# >= 2 GiB of RAM on purpose: booting the vvfat ESP at 512 MiB - the old setup -
# hid a whole class of bugs that only appear with a real GPT disk and multi-GiB
# memory maps (hand-off structures placed high, the >1 GiB / >4 GiB mapping
# paths, ExitBootServices on real firmware). Override the RAM with SMOKE_MEM.
#
# Usage: smoke-test.sh <x64|aarch64> <loader.img> [timeout_seconds] [marker]
#
# Passes when [marker] appears on the serial console (default: the kernel's
# early "OSv " banner - i.e. it reached the kernel). CI passes the test-suite
# success string instead. Firmware is auto-detected but can be overridden:
#   x64:     OVMF_CODE,  OVMF_VARS
#   aarch64: AAVMF_CODE, AAVMF_VARS
#
set -euo pipefail

arch="${1:?usage: smoke-test.sh <x64|aarch64> <loader.img> [timeout] [marker]}"
img="${2:?missing path to loader.img}"
timeout_s="${3:-30}"
marker="${4:-OSv }"
mem="${SMOKE_MEM:-2048}"

# First firmware candidate that exists, else empty. Always succeeds (returns 0)
# so it is safe under `set -e` inside a command substitution.
pick() { for f in "$@"; do [ -f "$f" ] && { echo "$f"; break; }; done; return 0; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
log="$work/serial.log"

case "$arch" in
x64)
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
    code="${AAVMF_CODE:-$(pick /usr/share/AAVMF/AAVMF_CODE.fd \
                               /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
                               /usr/share/edk2/aarch64/QEMU_EFI.fd \
                               "$HOME/.cache/aavmf/AAVMF_CODE.fd")}"
    vars="${AAVMF_VARS:-$(pick /usr/share/AAVMF/AAVMF_VARS.fd \
                               /usr/share/qemu-efi-aarch64/QEMU_VARS.fd \
                               "$HOME/.cache/aavmf/AAVMF_VARS.fd")}"
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
if [ ! -f "$img" ]; then
    echo "smoke-test: disk image '$img' not found (build it with" >&2
    echo "  'make arch=$arch $img'; it needs mtools + gdisk for mkuefi.sh)." >&2
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

# pflash needs a writable copy of the variable store. QEMU's aarch64 virt
# pflash also requires the firmware images to be exactly 64 MiB; the typical
# distro/AAVMF files already are, but pad copies defensively so an unpadded
# QEMU_EFI works too.
codecopy="$work/code.fd"; cp "$code" "$codecopy"
varscopy="$work/vars.fd"; cp "$vars" "$varscopy"
if [ "$arch" = aarch64 ]; then
    truncate -s 64M "$codecopy"
    truncate -s 64M "$varscopy"
fi

echo "smoke-test: $arch via $qemu (accel=$accel, ${mem} MiB, NVMe disk)"
echo "  firmware: $code"
echo "  booting:  $img (timeout ${timeout_s}s)"

# Boot the GPT/ESP image as an NVMe disk and let the firmware find the default
# \EFI\BOOT\BOOT{X64,AA64}.EFI - the exact path AWS Nitro takes. OVMF/AAVMF
# mirror the UEFI console to the serial port, so $log captures the firmware,
# stub and kernel early-console output. QEMU diagnostics go to qemu.out.
set +e
timeout --foreground "$timeout_s" "$qemu" \
    "${machine[@]}" -m "$mem" -accel "$accel" -no-reboot \
    -display none -vga none -nic none \
    -drive "if=pflash,format=raw,readonly=on,file=$codecopy" \
    -drive "if=pflash,format=raw,file=$varscopy" \
    -drive "id=smokedisk,format=raw,if=none,file=$img" \
    -device nvme,serial=smoke,drive=smokedisk \
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

# Pass when the expected marker appears. The default ("OSv ") is the kernel's
# early premain banner - seeing it means firmware, the UEFI stub, the arch
# entry and premain all worked. CI overrides it with the test-suite result.
if grep -qF "$marker" "$log" 2>/dev/null; then
    echo "smoke-test: PASS ($arch saw \"$marker\")"
    exit 0
fi
if grep -q "miniosv UEFI stub starting" "$log" 2>/dev/null; then
    echo "smoke-test: FAIL - stub ran but \"$marker\" not seen ($arch)" >&2
else
    echo "smoke-test: FAIL - kernel did not start ($arch)" >&2
fi
exit 1
