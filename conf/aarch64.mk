# You will die horribly without -mstrict-align, due to
# unaligned access to a stack attr variable with stp.
# Relaxing alignment checks via sctlr_el1 A bit setting should solve
# but it doesn't - setting ignored?
#
# Also, mixed TLS models resulted in different var addresses seen by
# different objects depending on the TLS model used.
# Force all __thread variables encountered to local exec. local-exec emits
# direct TPREL (R_AARCH64_TLSLE_*) accesses off TPIDR_EL0 and never generates
# TLSDESC relocations, so no -mtls-dialect is needed (and clang, our only
# compiler, doesn't accept the GCC-only -mtls-dialect= flag on aarch64 anyway).
conf_compiler_cflags = -mstrict-align -ftls-model=local-exec -DAARCH64_PORT_STUB
