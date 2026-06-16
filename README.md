# miniosv

A radically slimmed-down hard fork of the [OSv](https://github.com/cloudius-systems/osv) unikernel.

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
- **Pure-LLVM libc** — llvm-libc + libc++ + libunwind + compiler-rt, plus a small OSv-owned libc core. No GNU toolchain, no libgcc.
- Builds and boots on both **x86-64** and **aarch64**.

## Build & run

The default app (`app/app.cc`) is the LeanStore YCSB benchmark. It stores its data
on an **NVMe device**, so before running you must attach one — either an emulated
drive backed by a raw image, or a real NVMe device passed through from the host.
One of the two is required; the kernel has no other storage.

```bash
./scripts/build                    # build x86-64 kernel (LeanStore app)

# Option A — emulated NVMe backed by a raw disk image:
truncate -s 4G /tmp/nvme.raw
./scripts/run.py --emulated-nvme /tmp/nvme.raw

# Option B — pass through a real NVMe device (first bind it to vfio-pci on the
# host); needs sudo. Pass one or more PCI addresses:
sudo ./scripts/run.py --pass-pci 0000:01:00.0
```

aarch64:

```bash
./scripts/build arch=aarch64
QEMU_PATH=/usr/bin/qemu-system-aarch64 ./scripts/run.py --arch aarch64 --emulated-nvme /tmp/nvme.raw
```

Build the test suite (no NVMe needed) with `make app=tests`.

## License

3-clause BSD (see [`LICENSE`](LICENSE)), inherited from OSv. Includes musl-derived code under the
MIT license (see [`documentation/LICENSE-musl`](documentation/LICENSE-musl)).
