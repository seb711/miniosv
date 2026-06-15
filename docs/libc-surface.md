# The libc surface OSv actually uses

Phase 0 guardrail. Before we delete subsystems we need to know which libc
symbols the linked image actually references — you delete musl objects with
confidence only against a list like this. This is also the input list for the
eventual musl → llvm-libc swap (Phase 7): llvm-libc only has to provide the
symbols that survive Phases 3–6.

## How it is computed

`scripts/audit-libc-surface.py [builddir=build/last]` computes the transitive
closure of musl objects reachable from the linked image:

- **Roots** are the *undefined* symbols of the OSv kernel/app objects
  (`core/`, `libc/`, `arch/`, `drivers/`, `bsd/`, `app/`, `apps/`, `fastlz/`,
  `test/`, plus `loader.o` and `runtime.o`) and of the statically linked
  toolchain archives (`libstdc++.a`, `libgcc.a`, `libgcc_eh.a`, boost, gflags).
  The toolchain set is taken conservatively from the whole archive's undefined
  set, not just the members the linker would pull in.
- **`libc/aliases.ld`** defines link-time `LHS = RHS` aliases. A reference to
  `LHS` is really a requirement on `RHS`, so alias targets are folded into the
  root set; alias lines whose `LHS` nobody references can be dropped.
- The script then walks musl's own inter-object references to the fixed point.

It prints four lists: the externally required musl symbols (the surface),
the needed musl objects, the prunable musl objects, and the `aliases.ld` lines
whose target is no longer defined anywhere.

## Regenerating

```
make -j$(nproc)                       # produces build/last/...
scripts/audit-libc-surface.py > docs/libc-surface.txt
```

## Baseline (to populate)

The concrete listing is generated against a build and will differ at each
phase boundary — the surface *shrinks* as Phases 3–6 delete the code that pulls
musl in. Record the symbol count here at each phase so the shrink is tracked:

| Phase | externally required symbols | needed musl objects |
|-------|-----------------------------|---------------------|
| 0 (master baseline) | 161 | 205 of 582 |

v2_osv's slim end state reached **87** referenced symbols once the app was
in-kernel — that number is only that small *because* the earlier phases deleted
everything that pulled in the rest. The Phase 0 baseline on master will be much
larger; that is the honest starting point.
