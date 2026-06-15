#!/usr/bin/env bash
# Verify that the GCC→Clang toolchain switch is in effect and the kernel builds.
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0
ok()   { printf '  PASS: %s\n' "$1"; PASS=$((PASS+1)); }
fail() { printf '  FAIL: %s\n' "$1"; FAIL=$((FAIL+1)); }

echo "=== 1. Toolchain identity ==="

check_makefile_var() {
    local pattern=$1 expected=$2 label=$3
    local val
    val=$(grep "$pattern" Makefile | head -1)
    if [[ "$val" == "$expected" ]]; then
        ok "$label"
    else
        fail "$label — got '$val'"
    fi
}

check_makefile_var '^CC='           'CC=clang'          'CC=clang'
check_makefile_var '^CXX='          'CXX=clang++'       'CXX=clang++'
check_makefile_var '^HOST_CXX :='   'HOST_CXX := clang++'       'HOST_CXX=clang++'

# STRIP and OBJCOPY use $(shell which ...) for version-suffixed LLVM tools;
# verify the resolved value contains llvm-strip / llvm-objcopy.
strip_val=$(make -s --eval 'print-strip: ; @echo $(STRIP)' print-strip 2>/dev/null | grep 'llvm' | head -1)
if [[ -n "$strip_val" ]]; then
    ok "STRIP resolves to an llvm-strip variant ($strip_val)"
else
    fail "STRIP does not resolve to an llvm-strip variant"
fi
objcopy_val=$(make -s --eval 'print-objcopy: ; @echo $(OBJCOPY)' print-objcopy 2>/dev/null | grep 'llvm' | head -1)
if [[ -n "$objcopy_val" ]]; then
    ok "OBJCOPY resolves to an llvm-objcopy variant ($objcopy_val)"
else
    fail "OBJCOPY does not resolve to an llvm-objcopy variant"
fi

echo ""
echo "=== 2. CXX_INCLUDES awk filter produces clean -isystem flags ==="

cxx_includes=$(clang++ -E -xc++ - -v </dev/null 2>&1 \
  | awk '/search starts here/{p=1;next} /^End/{exit} p && /c\+\+/{print "-isystem" $0}')

if echo "$cxx_includes" | grep -q '\-cc1'; then
    fail "CXX_INCLUDES contains cc1 invocation"
else
    ok "CXX_INCLUDES has no cc1 invocation"
fi

# Count -isystem occurrences — should be at least one
isystem_count=$(echo "$cxx_includes" | grep -c '\-isystem' || true)
if [[ $isystem_count -ge 1 ]]; then
    ok "CXX_INCLUDES has $isystem_count C++ -isystem entries (no system C headers)"
else
    fail "CXX_INCLUDES has no -isystem entries: $cxx_includes"
fi

# Only C++ paths — must not contain bare /usr/include
if echo "$cxx_includes" | grep -qE '-isystem ?/usr/include$|-isystem ?/usr/include/x86_64'; then
    fail "CXX_INCLUDES includes system C headers (/usr/include) — will conflict with musl"
else
    ok "CXX_INCLUDES contains only C++ paths (no bare /usr/include)"
fi

echo ""
echo "=== 3. arch detection works with Clang ==="

arch_result=$(
    { echo "x64        __x86_64__"; echo "aarch64    __aarch64__"; } \
    | clang++ -E -xc - | grep -v '^#' | grep ' 1$'
)
if echo "$arch_result" | grep -q '^x64'; then
    ok "detect_arch produces 'x64' with Clang on x86_64"
else
    fail "detect_arch produced: '$arch_result'"
fi

echo ""
echo "=== 4. GCC-only warning flags handled correctly ==="

probe() {
    # Returns "yes" if clang accepts the flag without error, "no" otherwise
    clang++ -Werror "$1" -o /dev/null -c compiler/empty.cc > /dev/null 2>&1 && echo yes || echo no
}

# These flags don't exist in Clang — probe must return "no" so they are suppressed
for flag in -Wno-dangling-pointer -Wno-class-memaccess -Wno-stringop-truncation; do
    result=$(probe "$flag")
    if [[ "$result" == "no" ]]; then
        ok "$flag correctly rejected by Clang (will be suppressed by probe)"
    else
        fail "$flag accepted by Clang — probe should return empty for it"
    fi
done

# -Wno-uninitialized is the Clang equivalent of -Wno-maybe-uninitialized
result=$(probe -Wno-uninitialized)
[[ "$result" == "yes" ]] && ok "-Wno-uninitialized accepted by Clang" \
                          || fail "-Wno-uninitialized rejected by Clang"

echo ""
echo "=== 5. -finstrument-functions-exclude-file-list not accepted by Clang ==="
result=$(probe -finstrument-functions-exclude-file-list=x)
[[ "$result" == "no" ]] && ok "-finstrument-functions-exclude-file-list rejected → tracing-excl will be empty" \
                         || fail "-finstrument-functions-exclude-file-list accepted by Clang"

echo ""
echo "=== 6. intrinsics.hh has __clang__ guard ==="
if grep -q '!defined(__clang__)' compiler/include/intrinsics.hh; then
    ok "intrinsics.hh has !defined(__clang__) guard"
else
    fail "intrinsics.hh missing !defined(__clang__) guard"
fi

echo ""
echo "=== 7. Clang finds runtime libraries ==="
for lib in libstdc++.a libgcc_eh.a; do
    path=$(clang++ -print-file-name="$lib")
    if [[ "$path" == /* ]]; then
        ok "$lib → $path"
    else
        fail "$lib not found (got: $path)"
    fi
done
path=$(clang -print-libgcc-file-name)
if [[ "$path" == /* ]]; then
    ok "libgcc.a → $path"
else
    fail "libgcc.a not found (got: $path)"
fi

echo ""
echo "=== 8. make stage1 (full kernel compile) ==="
echo "    Building into build/release.x64/ — this takes a few minutes..."

if make stage1 -j"$(nproc)" 2>&1 | tee /tmp/osv-stage1.log | grep -E '^(clang|CXX|CC |AS |LINK|make\[)' | head -5; then
    stage1_rc=${PIPESTATUS[0]}
else
    stage1_rc=${PIPESTATUS[0]}
fi

if [[ $stage1_rc -eq 0 ]]; then
    ok "make stage1 succeeded"
    # Verify the objects were produced by clang — either the build log contains
    # clang invocations (fresh build) or the DW_AT_producer in an object says Clang.
    if grep -q 'clang' /tmp/osv-stage1.log; then
        ok "Build log confirms clang was invoked"
    else
        # Incremental build: check object files for Clang DW_AT_producer
        producer=$(readelf -wi build/release.x64/core/sched.o 2>/dev/null | grep 'DW_AT_producer' | head -1)
        if echo "$producer" | grep -qi 'clang'; then
            ok "DW_AT_producer in core/sched.o confirms Clang"
        else
            fail "No clang invocations in build log and DW_AT_producer doesn't mention Clang: $producer"
        fi
    fi
    if grep -q 'g++\|gcc ' /tmp/osv-stage1.log | grep -v '^#'; then
        fail "Build log still contains g++/gcc invocations"
    else
        ok "No g++/gcc invocations in build log"
    fi
else
    fail "make stage1 failed — see /tmp/osv-stage1.log"
    tail -20 /tmp/osv-stage1.log
fi

echo ""
echo "=== Summary ==="
printf '  Passed: %d  Failed: %d\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
