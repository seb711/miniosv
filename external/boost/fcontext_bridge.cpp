// Bridge between Boost.Context's namespaced C++ API and its extern-"C" assembly.
//
// The vendored assembly under external/boost/asm/*.S defines the context-switch
// routines as plain C symbols (jump_fcontext / make_fcontext / ontop_fcontext),
// exactly as upstream Boost ships them. Our code, however, includes Boost's
// headers and so references the C++-namespaced declarations
// boost::context::detail::{jump,make,ontop}_fcontext (see the vendored
// boost/context/detail/fcontext.hpp). Boost's own build links a tiny shim TU to
// connect the two; this is that shim.
#include <boost/context/detail/fcontext.hpp>
#include <cstddef>

// The assembly entry points (global scope, C linkage).
extern "C" boost::context::detail::transfer_t
jump_fcontext(boost::context::detail::fcontext_t to, void* vp);
extern "C" boost::context::detail::fcontext_t
make_fcontext(void* sp, std::size_t size, void (*fn)(boost::context::detail::transfer_t));
extern "C" boost::context::detail::transfer_t
ontop_fcontext(boost::context::detail::fcontext_t to, void* vp,
               boost::context::detail::transfer_t (*fn)(boost::context::detail::transfer_t));

namespace boost {
namespace context {
namespace detail {

transfer_t jump_fcontext(fcontext_t to, void* vp)
{
   return ::jump_fcontext(to, vp);
}

fcontext_t make_fcontext(void* sp, std::size_t size, void (*fn)(transfer_t))
{
   return ::make_fcontext(sp, size, fn);
}

transfer_t ontop_fcontext(fcontext_t to, void* vp, transfer_t (*fn)(transfer_t))
{
   return ::ontop_fcontext(to, vp, fn);
}

}  // namespace detail
}  // namespace context
}  // namespace boost

// make_fcontext's "finish" path - reached only if a context-function returns
// instead of switching away (it never does in normal use) - calls _exit, which
// OSv's baremetal libc does not provide. Define it self-contained so we neither
// require an _exit nor pull in llvm-libc's exit machinery (which expects an OS
// hook the kernel does not supply): just trap hard, since reaching here is a bug.
extern "C" __attribute__((noreturn)) void _exit(int /*status*/)
{
   __builtin_trap();
   for (;;) {
   }
}
