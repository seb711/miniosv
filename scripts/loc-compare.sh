#!/bin/bash
# Compare kernel C/C++ line counts between two git refs of this repo,
# e.g. upstream OSv (master) vs the slim rework branch:
#
#   scripts/loc-compare.sh [ref-before] [ref-after]
#
# Counts *.c *.h *.cc *.hh *.cpp *.hpp tracked in each ref. Excluded in
# both refs: the libc (libc/ and the musl submodule / vendored musl trees,
# never counted), the libc API headers (include/api/), vendored third-party
# code (external/, fastlz/, kbuild/), apps/modules/tests, build tooling and
# non-source data (scripts/, tools/, docker/, images/, static/, licenses/,
# documentation/, exported_symbols/) - so the comparison is OSv kernel code
# only. Lines are raw (wc -l equivalent), counted via git so no checkout is
# needed.

set -euo pipefail

BEFORE=${1:-master}
AFTER=${2:-slim-osv}

EXTS=('*.c' '*.h' '*.cc' '*.hh' '*.cpp' '*.hpp')
EXCLUDES=(
    ':(exclude)libc/'
    ':(exclude)musl/'
    ':(exclude)musl_0.9.12/'
    ':(exclude)musl_1.1.24/'
    ':(exclude)include/api/'   # libc API headers
    ':(exclude)external/'
    ':(exclude)fastlz/'
    ':(exclude)kbuild/'
    ':(exclude)apps/'
    ':(exclude)app/'
    ':(exclude)test/'
    ':(exclude)modules/'
    ':(exclude)tests/'
    ':(exclude)tools/'
    ':(exclude)scripts/'
    ':(exclude)docker/'
    ':(exclude)images/'
    ':(exclude)static/'
    ':(exclude)licenses/'
    ':(exclude)documentation/'
    ':(exclude)exported_symbols/'
)

# Print "<lines> <top-level-dir>" for every counted file in a ref.
count() {
    local ref=$1
    # git grep -c '' = lines per file; output "ref:path:count"
    git grep -c '' "$ref" -- "${EXTS[@]}" "${EXCLUDES[@]}" 2>/dev/null |
    awk -F: '{
        n = $NF
        path = $2
        split(path, parts, "/")
        dir = (path ~ /\//) ? parts[1] : "(root)"
        print n, dir
    }'
}

summarize() {
    local agg
    agg=$(awk '{ per[$2] += $1 } END { for (d in per) printf "  %-12s %9d\n", d, per[d] }')
    echo "$agg" | sort -k2 -nr
    echo "$agg" | awk '{ t += $2 } END { printf "  %-12s %9d\n", "TOTAL", t }'
}

echo "== $BEFORE =="
count "$BEFORE" | summarize
echo
echo "== $AFTER =="
count "$AFTER" | summarize
echo

b=$(count "$BEFORE" | awk '{s+=$1} END {print s}')
a=$(count "$AFTER"  | awk '{s+=$1} END {print s}')
echo "$BEFORE: $b lines   $AFTER: $a lines"
awk -v b="$b" -v a="$a" 'BEGIN {
    printf "delta: %+d lines (%.1f%% of the original remains)\n", a-b, 100.0*a/b
}'
