# miniosv

A radically slimmed-down hard fork of the [OSv](https://github.com/cloudius-systems/osv) unikernel. Mostly done by [Claude Opus 4.8](https://www.anthropic.com/claude/opus).

## Motivation

OSv is a full-featured, Linux-compatible unikernel: ELF loader, syscall ABI, ZFS/ROFS/RAMFS,
a networking stack, loadable modules, and a GNU/musl toolchain. miniosv strips all of that away
to expose the smallest core that still boots and runs a real program — a single statically-linked
binary where the kernel and the application are compiled together, booted diskless, with no
syscall layer, no filesystem, and no networking. The goal is understanding and experimentation,
not Linux compatibility.

## What's left

- **Single binary** — the app is compiled into the kernel and entered via `osv_app_main()`; no ELF loader.
- **Diskless boot** — static `EXEC`, no `.dynamic`. x86-64 boots directly via QEMU `-kernel` (PVH); aarch64 via a small preboot stub.
- **No syscall ABI** — the app calls libc/kernel functions directly (zero syscall instructions).
- **Pure-LLVM toolchain** — clang + `ld.lld` + llvm-libc + libc++ + libunwind + compiler-rt, plus a small OSv-owned libc core. No GNU toolchain (no binutils, no libgcc); aarch64 cross-compiles by target triple with no GNU cross sysroot.
- Builds and boots on both **x86-64** and **aarch64**.

## Build & run

```bash
make                               # build x86-64 kernel (app/app.cc)
```

The default app (`app/app.cc`) is the in-kernel **LeanStore** YCSB benchmark. It needs an
**NVMe device** as its backing store and **4 vCPUs** (the LeanStore runtime currently assumes
4 cores — do not lower `-c`/`--vcpus` below 4).

### NVMe backing store

LeanStore drives the NVMe device directly through OSv's user-queue NVMe driver; it is not exposed
as a filesystem. Provide one of the following.

**Emulated drive (QEMU).** Create a raw image and attach it as an NVMe device:

```bash
truncate -s 4G nvme.img                                # >= target_gib (currently 2 GiB)
./scripts/run.py --emulated-nvme nvme.img -m 4G -c 4
```

`--emulated-nvme` expands to this QEMU device pair:

```
-drive file=nvme.img,if=none,id=nvm1 -device nvme,serial=deadbeef,drive=nvm1
```

**Real device (VFIO passthrough).** Bind the host NVMe controller to `vfio-pci` and pass it through.
Find its PCI address with `lspci -nn | grep -i nvme` (here `0000:01:00.0`):

```bash
sudo driverctl set-override 0000:01:00.0 vfio-pci      # bind to vfio-pci (persists)
sudo ./scripts/run.py --pass-pci 0000:01:00.0 -m 4G -c 4
sudo driverctl unset-override 0000:01:00.0             # restore the host driver afterwards
```

VFIO passthrough requires the IOMMU enabled on the host (`intel_iommu=on` / `amd_iommu=on`) and
root privileges for both `vfio-pci` and KVM.

Build the test suite instead with `make app=tests`.

## License

3-clause BSD (see [`LICENSE`](LICENSE)), inherited from OSv. Includes musl-derived code under the
MIT license (see [`LICENSE-musl`](LICENSE-musl)).
