#!/bin/bash
#
# Build a bootable GPT disk image with a single FAT32 EFI System Partition
# containing the miniosv UEFI application at the firmware's removable-media
# default path (\EFI\BOOT\BOOT{X64,AA64}.EFI). The resulting raw image is what
# the public clouds ingest (AWS raw/AMI, Azure VHD, GCP disk.raw).
#
# Usage: mkuefi.sh <out.img> <loader.efi> <BOOTX64.EFI|BOOTAA64.EFI>
#
set -euo pipefail

out="$1"; efi="$2"; bootname="$3"

if ! command -v mcopy >/dev/null 2>&1 || ! command -v mmd >/dev/null 2>&1; then
    echo "mkuefi.sh: needs mtools (mmd/mcopy) to populate the FAT ESP." >&2
    echo "           install it (e.g. 'apt-get install mtools') or build the" >&2
    echo "           'loader.efi' target and boot it via OVMF's fat: driver." >&2
    exit 1
fi

esp_mb=64
disk_mb=$((esp_mb + 2))         # leave room for primary/backup GPT
esp_start=2048                  # 1 MiB aligned

esp="$(mktemp)"
trap 'rm -f "$esp"' EXIT

# Build the FAT32 EFI System Partition and drop the loader in place.
dd if=/dev/zero of="$esp" bs=1M count=$esp_mb status=none
mkfs.fat -F32 -n ESP "$esp" >/dev/null
mmd   -i "$esp" ::/EFI ::/EFI/BOOT
mcopy -i "$esp" "$efi" "::/EFI/BOOT/$bootname"

# Wrap it in a GPT disk with an EFI System Partition (type ef00).
dd if=/dev/zero of="$out" bs=1M count=$disk_mb status=none
sgdisk -a 2048 -n 1:${esp_start}:0 -t 1:ef00 -c 1:"EFI System Partition" "$out" >/dev/null
dd if="$esp" of="$out" bs=512 seek=$esp_start conv=notrunc status=none

echo "mkuefi.sh: wrote $out ($bootname)"
