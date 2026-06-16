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
- **Pure-LLVM libc** — llvm-libc + libc++ + libunwind + compiler-rt, plus a small OSv-owned libc core. No GNU toolchain, no libgcc.
- Builds and boots on both **x86-64** and **aarch64**.

## Build & run

```bash
./scripts/build                    # build x86-64 kernel (app/app.cc)
./scripts/run.py                   # boot under QEMU/KVM

./scripts/build arch=aarch64
QEMU_PATH=/usr/bin/qemu-system-aarch64 ./scripts/run.py --arch aarch64
```

The default app (`app/app.cc`) is a conformance gate that prints a `PASS`/`FAIL` line per check.
Build the larger test suite with `make app=tests`.

## License

3-clause BSD (see [`LICENSE`](LICENSE)), inherited from OSv. Includes musl-derived code under the
MIT license (see [`documentation/LICENSE-musl`](documentation/LICENSE-musl)).
