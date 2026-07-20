#!/usr/bin/env python3
# Read STT_FUNC symbols from a stage-1 kernel and emit assembly defining
# __ksym_count / __ksym_addresses / __ksym_name_offsets / __ksym_names_blob
# in .rodata.ksymtab for the final link.

import argparse
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("--elf", required=True)
ap.add_argument("--nm", default="llvm-nm")
ap.add_argument("--out", required=True)
args = ap.parse_args()

nm_out = subprocess.check_output(
    [args.nm, "--defined-only", "--numeric-sort", "-f", "bsd", args.elf]
).decode()

syms = []
for line in nm_out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "TtWw":
        try:
            a = int(p[0], 16)
        except ValueError:
            continue
        if a and (not syms or syms[-1][0] != a):
            syms.append((a, p[2]))

with open(args.out, "w") as f:
    f.write('.section .rodata.ksymtab,"a",%progbits\n.align 8\n')
    f.write('.globl __ksym_count\n__ksym_count: .long {}, 0\n'.format(len(syms)))
    f.write('.globl __ksym_addresses\n__ksym_addresses:\n')
    for a, _ in syms:
        f.write('.quad 0x{:x}\n'.format(a))
    f.write('.globl __ksym_name_offsets\n__ksym_name_offsets:\n')
    off = 0
    for _, n in syms:
        f.write('.long {}\n'.format(off))
        off += len(n.encode()) + 1
    f.write('.globl __ksym_names_blob\n__ksym_names_blob:\n')
    for _, n in syms:
        esc = n.replace('\\', '\\\\').replace('"', '\\"')
        f.write('.asciz "{}"\n'.format(esc))
