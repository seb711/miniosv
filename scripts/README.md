Scripts to build and run the slim OSv kernel.

The application is statically linked into the kernel and the kernel boots
diskless via QEMU `-kernel` (PVH on x64, the `loader.img` preboot on aarch64).
There is no filesystem, no module system and no data image, so this directory
is small.

# Building
* **build** — drives the kernel build (delegates to the top-level Makefile).
  `./scripts/build --help` for usage; `./scripts/build clean` to clean.
* **build-llvm-libc.sh** — builds the no-syscall baremetal llvm-libc
  (`libc.a`/`libm.a`) from the pinned `external/llvm-project`, using the curated
  config in `external/llvm-libc-config/`. Auto-invoked by the Makefile.
* **build-libcxx.sh** — builds libc++/libc++abi/libunwind against OSv's headers.
  Auto-invoked by the Makefile.
* **build-compiler-rt.sh** — cross-builds the compiler-rt builtins archive
  (aarch64). Auto-invoked by the Makefile when cross-compiling.
* **gen-version-header** / **osv-version.sh** — generate `osv/version.h`.

# Running
* **run.py** — boots the built kernel under QEMU. `./scripts/run.py --help`.

# aarch64 cross build helpers
* **download_aarch64_toolchain.sh**, **download_aarch64_packages.py**,
  **download_fedora_aarch64_rpm_package.sh**,
  **download_ubuntu_aarch64_deb_package.sh** — fetch the aarch64 toolchain/sysroot.
* **imgedit.py** + **nbd_client.py** — used ONLY to stamp the aarch64 `loader.img`
  (kernel size + command line). The x64 build no longer uses them.

# Slimming guardrails (project tooling, not build-essential)
* **audit-libc-surface.py**, **loc-compare.sh** — track the libc surface / LOC.
