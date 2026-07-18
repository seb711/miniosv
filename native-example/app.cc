// Native hello-world reference app.
//
// Staged into app/ by the Nix build so the kernel's Makefile picks it up via
// `include app/Makefile`. osv_app_main() is entered once on a dedicated
// thread after early kernel init.

#include <cstdint>
#include <cstdio>

extern "C" void osv_app_main()
{
    printf("Hello, world from miniOSv!\n");
    // Spin forever so cloud serial consoles keep the boot output visible
    // instead of the instance stopping the moment the app returns. Empty asm
    // keeps the compiler from optimizing the loop away.
    while (true) {
        asm volatile("" ::: "memory");
    }
}
