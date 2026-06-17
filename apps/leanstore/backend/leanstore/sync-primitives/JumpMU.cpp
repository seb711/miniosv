#include "JumpMU.hpp"
//#include "TaskManager.hpp"

#include <signal.h>
// -------------------------------------------------------------------------------------
namespace jumpmu
{
constinit thread_local JumpMUContext* thread_local_jumpmu_ctx = nullptr;
#ifdef NEW_JUMPMU
constinit thread_local JumpMUContext thread_local_jumpmu __attribute__ ((tls_model("local-exec")));
#endif
}  // namespace jumpmu