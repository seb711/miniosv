# Vendored Boost headers

This directory contains a **header-only subset of Boost 1.83.0**, vendored so
that building the OSv kernel does not depend on a system-installed Boost.

It is trimmed to exactly the Boost headers the **kernel** build pulls in (a
clean build shows no `/usr/include/boost` paths in any kernel `*.d`), plus the
full `boost/{atomic,predef,config}` modules so the aarch64 build - which selects
different per-architecture headers and is not built here - keeps working. The
unused per-compiler / per-architecture branches that `bcp` copies by default
(e.g. MSVC, PowerPC, SPARC variants) are dropped, which is most of the size.

The OSv **test suite** may pull a few additional Boost headers (e.g.
`boost/algorithm/string`) that the kernel does not use; those still come from a
system Boost if one is installed.

The kernel only uses Boost headers (intrusive containers, lockfree queues,
Spirit Qi for command-line parsing, a few range/optional/variant utilities,
etc.). Boost.System is header-only in modern Boost, so no Boost library is
linked either - the Makefile leaves `boost-libs` empty.

The build points the compiler here via `-isystem external/boost` (see the
`boost-includes` variable in the top-level `Makefile`).

## Regenerating

The subset was produced with `bcp` (from the `libboost-tools-dev` /
`boost-build` package) by copying the closure of the Boost headers `#include`d
by the kernel. `bcp` scans includes statically, so the copy contains every
`#ifdef` branch (all architectures and compilers), which is what makes it
usable for both the x64 and aarch64 builds.

```sh
bcp --boost=/usr/include \
    boost/intrusive/list.hpp boost/intrusive/set.hpp boost/intrusive/slist.hpp \
    boost/intrusive/unordered_set.hpp boost/intrusive/parent_from_member.hpp \
    boost/intrusive_ptr.hpp \
    boost/range/algorithm/find.hpp boost/range/algorithm/transform.hpp \
    boost/range/algorithm/remove.hpp boost/range/adaptor/reversed.hpp \
    boost/lockfree/policies.hpp boost/lockfree/stack.hpp boost/lockfree/queue.hpp \
    boost/optional/optional.hpp boost/optional.hpp boost/utility.hpp \
    boost/version.hpp boost/dynamic_bitset.hpp boost/algorithm/cxx11/all_of.hpp \
    boost/variant.hpp boost/config/warning_disable.hpp \
    boost/spirit/include/qi.hpp boost/spirit/include/qi_parse.hpp \
    boost/spirit/include/qi_what.hpp boost/spirit/include/qi_action.hpp \
    boost/spirit/include/qi_char.hpp boost/spirit/include/qi_directive.hpp \
    boost/spirit/include/qi_rule.hpp boost/spirit/include/qi_grammar.hpp \
    boost/spirit/include/qi_eoi.hpp boost/spirit/include/qi_operator.hpp \
    boost/spirit/include/qi_string.hpp \
    boost/asio/ip/address.hpp \
    boost/date_time/posix_time/posix_time_types.hpp \
    boost/date_time/gregorian/gregorian_types.hpp \
    /tmp/bcp-out
# then copy the resulting /tmp/bcp-out/boost into external/boost/boost
```

`bcp` also copies the non-Boost system headers it encounters into the output
root; only the `boost/` subtree is kept here.

If a future change starts using a Boost header that is not yet present, the
build will fall back to the system Boost (if installed); re-run `bcp` with the
new header added to make the vendored copy self-contained again. A clean build
should show no `/usr/include/boost` paths in any `build/<mode>/**/*.d` file.
