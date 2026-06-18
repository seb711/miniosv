# Vendored Boost headers

This directory contains a **header-only subset of Boost 1.83.0**, vendored so
that building the OSv kernel does not depend on a system-installed Boost.

It is trimmed to exactly the Boost headers the **kernel** build pulls in - the
union of what the **x64** and **aarch64** kernel builds actually `#include`
(both architectures are built and the closure is taken from their dependency
files, so a clean build shows no `/usr/include/boost` paths in any kernel
`*.d`). Per-compiler / per-architecture branches that nothing here selects
(e.g. MSVC, PowerPC, SPARC, and the whole `boost/atomic` module, which the
kernel does not use) are dropped, which is most of the size.

The OSv **test suite** may pull a few additional Boost headers (e.g.
`boost/algorithm/string`) that the kernel does not use; those still come from a
system Boost if one is installed.

The kernel only uses a small set of Boost headers: intrusive containers
(`boost/intrusive/{list,set,slist,unordered_set}.hpp`), lockfree queues
(`boost/lockfree/{stack,policies}.hpp`), `boost/dynamic_bitset.hpp` and
`boost/intrusive_ptr.hpp`. Boost.System is header-only in modern Boost, so no
Boost library is linked either - the Makefile leaves `boost-libs` empty.

The build points the compiler here via `-isystem external/boost` (see the
`boost-includes` variable in the top-level `Makefile`).

## Regenerating

The subset is the transitive `#include` closure that the kernel build actually
opens, harvested from the compiler's own dependency (`*.d`) output for **both**
architectures. To reproduce it, point `-isystem` at a full system Boost
(install `libboost-dev`), do a clean build of both arches, then keep only the
headers the builds referenced:

```sh
# 1. build both arches against a full Boost (vendored or system)
make && make arch=aarch64

# 2. collect every Boost header the kernel objects pulled in
grep -rhoE 'external/boost/boost/[^ :\\]+' build/release.x64 build/release.aarch64 \
     --include='*.d' | sort -u
```

Because `boost/intrusive_ptr.hpp` (included only by `libc/pipe_buffer.hh`) is
not compiled into the default kernel image, its closure is added by compiling a
one-line probe that `#include`s it with the kernel's flags and harvesting that
`*.d` too. The union of all of these is the set kept here.

If a future change starts using a Boost header that is not yet present, the
build falls back to the system Boost (if installed); re-run the steps above so
the vendored copy stays self-contained. A clean build should show no
`/usr/include/boost` paths in any `build/<mode>/**/*.d` file.
