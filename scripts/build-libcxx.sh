#!/bin/bash
# Build LLVM's C++ runtime (libc++ + libc++abi + libunwind) as static archives
# for the OSv kernel, replacing the host GNU libstdc++.a.
#
# Phase 8 of LLVM_LIBC_PLAN.md, the libstdc++ -> libc++ swap. Built against the
# same pinned llvm-project as llvm-libc (external/llvm-project), with
# localization OFF - that is the whole point: libstdc++'s locale facets are the
# only consumer of the wide-ctype / locale-_l / iconv musl families we still
# carry, so a localization-free libc++ lets those go and kills the last glibc
# dependency (the host libstdc++.a pulled __isoc23_strtoul et al.).
#
# Produces, for the requested arch:
#   build/libcxx/<arch>/lib/{libc++.a,libc++abi.a,libunwind.a}
# Invoked automatically by the Makefile (a prerequisite of the kernel link).
# Only x86_64 ("x64") and aarch64 ("arm") are supported.

set -euo pipefail

osv_arch="${1:-x64}"
case "$osv_arch" in
    x64|x86_64|x86)    osv_arch=x64;     llvm_arch=x86_64 ;;
    aarch64|arm|arm64) osv_arch=aarch64; llvm_arch=aarch64 ;;
    *)
        echo "build-libcxx.sh: unsupported arch '$osv_arch' (only x86/arm)" >&2
        exit 2 ;;
esac

LLVM_TAG=llvmorg-22.1.7
OSV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_DIR="$OSV_ROOT/external/llvm-project"
BUILD_DIR="$OSV_ROOT/build/libcxx/$osv_arch"

# 1. fetch (shares the llvm-libc checkout; just widen the sparse set)
if [ ! -d "$LLVM_DIR/libcxx" ]; then
    if [ ! -d "$LLVM_DIR/.git" ]; then
        git clone --depth 1 --branch "$LLVM_TAG" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$LLVM_DIR"
    fi
    git -C "$LLVM_DIR" sparse-checkout add libcxx libcxxabi libunwind runtimes cmake \
        llvm/cmake llvm/utils
fi

# 2. configure + build (static, no localization, exceptions+RTTI on, llvm unwinder)
# Codegen must match the kernel (see COMMON in the top-level Makefile).
#
# Header environment must also match the kernel: put OSv's own libc headers
# (include/api, musl-derived) ahead of the host's so the standard C headers
# resolve to ours, exactly as the kernel compiles its C++. Without this the
# build picks up the host glibc <stdlib.h>, whose C23 mode (_GNU_SOURCE ->
# _ISOC23_SOURCE) redirects strtol/strtoul/strtoll/strtoull to __isoc23_* and
# leaves those undefined references in libc++.a (the last glibc dependency).
# include/api also supplies unistd/pthread/dlfcn/link/elf/sys headers that
# libc++abi and libunwind need; the host headers stay only as a fallback.
# include/api/stdint.h et al. pull the generated bits/alltypes.h; generate it
# into a private dir (same recipe as the kernel Makefile) so this script stays
# self-contained and independent of the kernel out-dir.
GEN_INC="$BUILD_DIR/gen/include"
mkdir -p "$GEN_INC/bits"
sh "$OSV_ROOT/include/api/$osv_arch/bits/alltypes.h.sh" > "$GEN_INC/bits/alltypes.h"
OSV_HEADERS="-isystem $OSV_ROOT/include/api -isystem $OSV_ROOT/include/api/$osv_arch -isystem $GEN_INC"
KERNEL_FLAGS="-fno-pie -fno-stack-protector -ftls-model=local-exec -fno-omit-frame-pointer $OSV_HEADERS"

mkdir -p "$BUILD_DIR"
cmake -S "$LLVM_DIR/runtimes" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBUNWIND_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBUNWIND_ENABLE_STATIC=ON \
    -DLIBCXX_ENABLE_LOCALIZATION=OFF \
    -DLIBCXX_ENABLE_UNICODE=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM=OFF \
    -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF \
    -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
    -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_INCLUDE_DOCS=OFF \
    -DCMAKE_C_FLAGS="$KERNEL_FLAGS" \
    -DCMAKE_CXX_FLAGS="$KERNEL_FLAGS" \
    > "$BUILD_DIR-configure.log" 2>&1 || {
        echo "configure failed, see $BUILD_DIR-configure.log"; exit 1; }

make -C "$BUILD_DIR" -j"$(nproc)" cxx cxxabi unwind > "$BUILD_DIR-build.log" 2>&1 || {
        echo "build failed, see $BUILD_DIR-build.log"; exit 1; }

for name in libc++.a libc++abi.a libunwind.a; do
    ARCHIVE=$(find "$BUILD_DIR" -name "$name" | head -1)
    if [ -z "$ARCHIVE" ]; then
        echo "error: $name not produced" >&2; exit 1
    fi
    echo "OK: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
done
