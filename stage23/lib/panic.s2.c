#include <lib/print.h>
#include <lib/real.h>
#include <lib/trace.h>
#if uefi == 1
#   include <efi.h>
#endif
#include <lib/blib.h>
#include <lib/readline.h>
#include <lib/gterm.h>
#include <lib/term.h>
#include <mm/pmm.h>

__attribute__((noreturn)) void panic(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);

    quiet = false;

    if (term_backend == NOT_READY) {
#if bios == 1
        term_textmode();
#elif uefi == 1
        term_vbe(0, 0);
#endif
    }

    print("\033[31mPANIC\033[37;1m\033[0m: ");
    vprint(fmt, args);

    va_end(args);

    print("\n");
    print_stacktrace(NULL);

#if bios == 1
    print("System halted.");
    rm_hcf();
#elif uefi == 1
    if (efi_boot_services_exited == false) {
        print("Press [ENTER] to return to firmware.");
        while (getchar() != '\n');
        fb_clear(&fbinfo);

        // release all uefi memory and return to firmware
        pmm_release_uefi_mem();
        gBS->Exit(efi_image_handle, EFI_ABORTED, 0, NULL);
        __builtin_unreachable();
    } else {
        print("System halted.");
        for (;;) {
            asm ("hlt");
        }
    }
#endif
}
