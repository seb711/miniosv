// Minimal libc shim for the in-kernel leanstore app.
#include <stdlib.h>
extern "C" {
// boost.context's stack "finish" routine calls _exit(), which neither OSv nor
// llvm-libc provides. Route it to OSv's exit() (runtime.cc -> osv::shutdown()).
__attribute__((weak, noreturn))
void _exit(int code) { exit(code); __builtin_unreachable(); }
}
