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
- **UEFI boot** — static `EXEC`, no `.dynamic`. Both arches boot only via UEFI: the kernel ELF is wrapped as an EFI application and loaded from a GPT/ESP disk image by the firmware (OVMF/AAVMF), the same path AWS, GCP and Azure take. No `-kernel`/PVH/multiboot.
- **No syscall ABI** — the app calls libc/kernel functions directly (zero syscall instructions).
- **Pure-LLVM toolchain** — clang + `ld.lld` + llvm-libc + libc++ + libunwind + compiler-rt, plus a small OSv-owned libc core. No GNU toolchain (no binutils, no libgcc); aarch64 cross-compiles by target triple with no GNU cross sysroot.
- Builds and boots on both **x86-64** and **aarch64**, under QEMU and on AWS, GCP and Azure.

## Build & run

The kernel links a single user application statically into the image, taken from `app/`. That
directory is gitignored — every app lives outside the tree and gets copied in. A reference
hello-world sits under [`native-example/`](native-example); the Nix flake also packages
[miniduckdb](https://github.com/Martin-Lndbl/miniduckdb).

Whatever ends up in `app/` must be **miniOSv-compliant**: a top-level `Makefile` that the kernel
build includes via `include app/Makefile`, declaring `app-objects = ...` (paths relative to the
repo root, e.g. `app/foo.o`), and a C/C++ entry point `extern "C" void osv_app_main()`. The
sources are compiled with the kernel's own flags (no separate userspace toolchain, no libc
dependencies beyond what the kernel exposes). See [`native-example/`](native-example) for the
minimum viable shape.

For non-Nix builds, stage the app once before invoking `make`:

```bash
cp -r native-example app            # or clone any miniOSv-compliant app repo into app/
```

Then:

```bash
make                                # build the x86-64 boot image -> build/release.x64/loader.img
./scripts/run.py                    # boot it under QEMU + UEFI (uses KVM when available)

make arch=aarch64                   # build the aarch64 image -> build/release.aarch64/loader.img
./scripts/run.py --arch aarch64     # boot it under QEMU + UEFI (TCG on an x86 host)
```

Nix users don't need the copy step. Every target names both an app and an arch:

```bash
nix build .#miniosv-native-example-x86_64        # build the image
nix run   .#run-native-example-x86_64            # build + boot under QEMU
nix run   .#aws-deploy-native-example-x86_64 -- eu-north-1 c5.large --attach
```

`packages.miniosv-<app>-<arch>` is the image (build only); `apps.run-<app>-<arch>` and
`apps.aws-deploy-<app>-<arch>` are the runnable wrappers. There is no bare `nix build` /
`nix run` default and no host-arch alias — targets that appear in `apps` are the ones you
can actually run.

`make` produces a bootable GPT/ESP disk image (`loader.img`); `run.py` boots it as an NVMe disk
and puts the guest serial console on your terminal (Ctrl-A C for the QEMU monitor, Ctrl-A X to
quit). Requirements: QEMU, the UEFI firmware (`ovmf` for x86-64, `qemu-efi-aarch64` for aarch64),
and `mtools` + `gdisk` to build the disk image. Override the firmware with the `OVMF_CODE`/
`OVMF_VARS` or `AAVMF_CODE`/`AAVMF_VARS` env vars. `make smoke-test` boots the image headless and
checks it reaches the kernel.

Build the larger test suite with `make app=tests` (no `app/` staging needed — that mode reads
sources from `test/`).

## License

3-clause BSD (see [`LICENSE`](LICENSE)), inherited from OSv. Includes musl-derived code under the
MIT license (see [`LICENSE-musl`](LICENSE-musl)).
