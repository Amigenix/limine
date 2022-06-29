#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <config.h>
#include <protos/multiboot1.h>
#include <lib/libc.h>
#include <lib/elf.h>
#include <lib/blib.h>
#include <lib/config.h>
#include <lib/print.h>
#include <lib/uri.h>
#include <lib/fb.h>
#include <lib/term.h>
#include <sys/pic.h>
#include <sys/cpu.h>
#include <sys/idt.h>
#include <fs/file.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <drivers/vga_textmode.h>

extern symbol multiboot_reloc_stub, multiboot_reloc_stub_end;

noreturn void multiboot1_spinup_32(uint32_t entry_point, uint32_t multiboot1_info);

static uint32_t kernel_top;

#define LIMINE_BRAND "Limine " LIMINE_VERSION

// Returns the size required to store the multiboot info.
static size_t get_multiboot1_info_size(
    char *cmdline,
    size_t modules_count, size_t modules_cmdlines_size,
    uint32_t section_entry_size, uint32_t section_num
) {
    return ALIGN_UP(sizeof(struct multiboot1_info), 16) +                   // base structure
           ALIGN_UP(strlen(cmdline) + 1, 16) +                              // cmdline
           ALIGN_UP(sizeof(LIMINE_BRAND), 16) +                             // bootloader brand
           ALIGN_UP(sizeof(section_entry_size * section_num), 16) +         // ELF info
           ALIGN_UP(sizeof(struct multiboot1_module) * modules_count, 16) + // modules count
           ALIGN_UP(modules_cmdlines_size, 16) +                            // modules command lines
           ALIGN_UP(sizeof(struct multiboot1_mmap_entry) * 256, 16);        // memory map
}

static void *mb1_info_alloc(void **mb1_info_raw, size_t size) {
    void *ret = *mb1_info_raw;
    *mb1_info_raw += ALIGN_UP(size, 16);
    return ret;
}

static bool mb1_overlap_check(uint64_t base1, uint64_t top1,
                              uint64_t base2, uint64_t top2) {
    return ((base1 >= base2 && base1 <  top2)
         || (top1  >  base2 && top1  <= top2));
}

static void *mb1_alloc(size_t size) {
    void *ret = (void *)(uintptr_t)ALIGN_UP(kernel_top, 4096);

    while (!memmap_alloc_range((uintptr_t)ret, size, MEMMAP_KERNEL_AND_MODULES,
                               true, false, false, false)) {
        ret += 0x200000;
    }

    kernel_top = (uintptr_t)ret + size;
    return ret;
}

bool multiboot1_load(char *config, char *cmdline) {
    struct file_handle *kernel_file;

    char *kernel_path = config_get_value(config, 0, "KERNEL_PATH");
    if (kernel_path == NULL)
        panic(true, "multiboot1: KERNEL_PATH not specified");

    if ((kernel_file = uri_open(kernel_path)) == NULL)
        panic(true, "multiboot1: Failed to open kernel with path `%s`. Is the path correct?", kernel_path);

    uint8_t *kernel = freadall(kernel_file, MEMMAP_KERNEL_AND_MODULES);

    size_t kernel_file_size = kernel_file->size;

    fclose(kernel_file);

    struct multiboot1_header header = {0};
    size_t header_offset = 0;

    for (header_offset = 0; header_offset < 8192; header_offset += 4) {
        uint32_t v = *(uint32_t *)(kernel+header_offset);

        if (v == MULTIBOOT1_HEADER_MAGIC) {
            memcpy(&header, kernel + header_offset, sizeof(header));
            break;
        }
    }

    if (header.magic != MULTIBOOT1_HEADER_MAGIC) {
        pmm_free(kernel_file, kernel_file_size);
        return false;
    }

    print("multiboot1: Loading kernel `%s`...\n", kernel_path);

    if (header.magic + header.flags + header.checksum)
        panic(true, "multiboot1: Header checksum is invalid");

    uint32_t entry_point;

    struct elf_section_hdr_info *section_hdr_info = NULL;

    struct elf_range *elf_ranges;
    uint64_t elf_ranges_count, slide = 0;

    if (header.flags & (1 << 16)) {
        if (header.load_addr > header.header_addr)
            panic(true, "multiboot1: Illegal load address");

        size_t load_size;
        if (header.load_end_addr)
            load_size = header.load_end_addr - header.load_addr;
        else
            load_size = kernel_file_size;

        uint32_t bss_size = 0;
        if (header.bss_end_addr) {
            uintptr_t bss_addr = header.load_addr + load_size;
            if (header.bss_end_addr < bss_addr)
                panic(true, "multiboot1: Illegal bss end address");

            bss_size = header.bss_end_addr - bss_addr;
        }

        size_t full_size = load_size + bss_size;

        bool simulation = true;
        size_t try_count = 0;
        size_t max_simulated_tries = 0x100000;

retry_raw_load:;
        uint64_t load_addr = header.load_addr + slide;

        if (!memmap_alloc_range(load_addr, full_size, MEMMAP_BOOTLOADER_RECLAIMABLE, true, false, simulation, false)) {
            if (simulation == false || ++try_count == max_simulated_tries) {
                panic(true, "multiboot1: Failed to allocate necessary memory range (%X-%X)", load_addr, load_addr + full_size);
            }
            slide += 0x1000;
            goto retry_raw_load;
        }

        if (simulation) {
            simulation = false;
            goto retry_raw_load;
        }

        memset((void *)(uintptr_t)load_addr, 0, full_size);

        memcpy((void *)(uintptr_t)load_addr, kernel + (header_offset
                - (header.header_addr - header.load_addr)), load_size);

        kernel_top = load_addr + full_size;

        entry_point = header.entry_addr + slide;

        elf_ranges_count = 1;
        elf_ranges = ext_mem_alloc(sizeof(struct elf_range));

        elf_ranges->base = load_addr;
        elf_ranges->length = full_size;
    } else {
        int bits = elf_bits(kernel);

        switch (bits) {
            case 32:
                section_hdr_info = elf32_section_hdr_info(kernel);
                break;
            case 64: {
                section_hdr_info = elf64_section_hdr_info(kernel);
                break;
            }
        }

        switch (bits) {
            case 32:
                if (elf32_load(kernel, &entry_point, &kernel_top, MEMMAP_BOOTLOADER_RECLAIMABLE, &slide, &elf_ranges, &elf_ranges_count))
                    panic(true, "multiboot1: ELF32 load failure");

                break;
            case 64: {
                uint64_t e, t;
                if (elf64_load(kernel, &e, &t, &slide, MEMMAP_BOOTLOADER_RECLAIMABLE, false, true, &elf_ranges, &elf_ranges_count, false, NULL, NULL, NULL, NULL))
                    panic(true, "multiboot1: ELF64 load failure");
                entry_point = e;
                kernel_top = t;

                break;
            }
            default:
                panic(true, "multiboot1: Invalid ELF file bitness");
        }
    }

    entry_point -= slide;

    kernel_top -= slide;
    if (kernel_top < 0x100000) {
        kernel_top = 0x100000;
    }

    size_t n_modules;
    size_t modules_cmdlines_size = 0;

    for (n_modules = 0;; n_modules++) {
        struct conf_tuple conf_tuple = config_get_tuple(config, n_modules, "MODULE_PATH", "MODULE_STRING");
        if (!conf_tuple.value1) break;

        char *module_cmdline = conf_tuple.value2;
        if (!module_cmdline) module_cmdline = "";
        modules_cmdlines_size += ALIGN_UP(strlen(module_cmdline) + 1, 16);
    }

    size_t mb1_info_size = get_multiboot1_info_size(
        cmdline,
        n_modules,
        modules_cmdlines_size,
        section_hdr_info ? section_hdr_info->section_entry_size : 0,
        section_hdr_info ? section_hdr_info->num : 0
    );

    // GRUB allocates boot info at 0x10000, *except* if the kernel happens
    // to overlap this region, then it gets moved to right after the
    // kernel, or whichever PHDR happens to sit at 0x10000.
    // Allocate it wherever, then move it to where GRUB puts it
    // afterwards.
    void *mb1_info_raw = ext_mem_alloc(mb1_info_size);
    uint64_t mb1_info_final_loc = 0x10000;
retry_mb1_info_reloc:
    for (size_t i = 0; i < elf_ranges_count; i++) {
        uint64_t mb1_info_top = mb1_info_final_loc + mb1_info_size;

        uint64_t base = elf_ranges[i].base - slide;
        uint64_t length = elf_ranges[i].length;
        uint64_t top = base + length;

        // Do they overlap?
        if (mb1_overlap_check(base, top, mb1_info_final_loc, mb1_info_top)) {
            mb1_info_final_loc = top;
            goto retry_mb1_info_reloc;
        }

        // Make sure it is memory that actually exists.
        if (!memmap_alloc_range(mb1_info_final_loc, mb1_info_size, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                MEMMAP_USABLE, false, true, false)) {
            if (!memmap_alloc_range(mb1_info_final_loc, mb1_info_size, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                    MEMMAP_BOOTLOADER_RECLAIMABLE, false, true, false)) {
                mb1_info_final_loc += 0x1000;
                goto retry_mb1_info_reloc;
            }
        }
    }

    size_t mb1_info_slide = (size_t)mb1_info_raw - mb1_info_final_loc;

    if (mb1_info_final_loc + mb1_info_size > kernel_top) {
        kernel_top = mb1_info_final_loc + mb1_info_size;
    }

    struct multiboot1_info *multiboot1_info =
        mb1_info_alloc(&mb1_info_raw, sizeof(struct multiboot1_info));

    if (section_hdr_info != NULL) {
        multiboot1_info->elf_sect.num = section_hdr_info->num;
        multiboot1_info->elf_sect.size = section_hdr_info->section_entry_size;
        multiboot1_info->elf_sect.shndx = section_hdr_info->str_section_idx;

        void *sections = mb1_info_alloc(&mb1_info_raw,
            section_hdr_info->section_entry_size * section_hdr_info->num);

        multiboot1_info->elf_sect.addr = (uintptr_t)sections - mb1_info_slide;

        memcpy(sections, kernel + section_hdr_info->section_offset, section_hdr_info->section_entry_size * section_hdr_info->num);

        for (size_t i = 0; i < section_hdr_info->num; i++) {
            struct elf64_shdr *shdr = (void *)sections + i * section_hdr_info->section_entry_size;

            if (shdr->sh_addr != 0 || shdr->sh_size == 0) {
                continue;
            }

            void *section = mb1_alloc(shdr->sh_size);
            memcpy(section, kernel + shdr->sh_offset, shdr->sh_size);

            shdr->sh_addr = (uintptr_t)section;
        }

        multiboot1_info->flags |= (1 << 5);
    }

    if (n_modules) {
        struct multiboot1_module *mods =
            mb1_info_alloc(&mb1_info_raw, sizeof(struct multiboot1_module) * n_modules);

        multiboot1_info->mods_count = n_modules;
        multiboot1_info->mods_addr = (size_t)mods - mb1_info_slide;

        for (size_t i = 0; i < n_modules; i++) {
            struct multiboot1_module *m = mods + i;

            struct conf_tuple conf_tuple = config_get_tuple(config, i, "MODULE_PATH", "MODULE_STRING");
            char *module_path = conf_tuple.value1;
            if (module_path == NULL)
                panic(true, "multiboot1: Module disappeared unexpectedly");

            print("multiboot1: Loading module `%s`...\n", module_path);

            struct file_handle *f;
            if ((f = uri_open(module_path)) == NULL)
                panic(true, "multiboot1: Failed to open module with path `%s`. Is the path correct?", module_path);

            char *module_cmdline = conf_tuple.value2;
            if (module_cmdline == NULL) {
                module_cmdline = "";
            }
            char *lowmem_modstr = mb1_info_alloc(&mb1_info_raw, strlen(module_cmdline) + 1);
            strcpy(lowmem_modstr, module_cmdline);

            void *module_addr = mb1_alloc(f->size);

            fread(f, module_addr, 0, f->size);

            m->begin   = (uint32_t)(size_t)module_addr;
            m->end     = m->begin + f->size;
            m->cmdline = (uint32_t)(size_t)lowmem_modstr - mb1_info_slide;
            m->pad     = 0;

            fclose(f);

            if (verbose) {
                print("multiboot1: Requested module %u:\n", i);
                print("            Path:   %s\n", module_path);
                print("            String: \"%s\"\n", module_cmdline ?: "");
                print("            Begin:  %x\n", m->begin);
                print("            End:    %x\n", m->end);
            }
        }

        multiboot1_info->flags |= (1 << 3);
    }

    char *lowmem_cmdline = mb1_info_alloc(&mb1_info_raw, strlen(cmdline) + 1);
    strcpy(lowmem_cmdline, cmdline);
    multiboot1_info->cmdline = (uint32_t)(size_t)lowmem_cmdline - mb1_info_slide;
    if (cmdline)
        multiboot1_info->flags |= (1 << 2);

    char *bootload_name = LIMINE_BRAND;
    char *lowmem_bootname = mb1_info_alloc(&mb1_info_raw, strlen(bootload_name) + 1);
    strcpy(lowmem_bootname, bootload_name);

    multiboot1_info->bootloader_name = (uint32_t)(size_t)lowmem_bootname - mb1_info_slide;
    multiboot1_info->flags |= (1 << 9);

    term_deinit();

    if (header.flags & (1 << 2)) {
        size_t req_width  = header.fb_width;
        size_t req_height = header.fb_height;
        size_t req_bpp    = header.fb_bpp;

        if (header.fb_mode == 0) {
            char *resolution = config_get_value(config, 0, "RESOLUTION");
            if (resolution != NULL)
                parse_resolution(&req_width, &req_height, &req_bpp, resolution);

            struct fb_info fbinfo;
            if (!fb_init(&fbinfo, req_width, req_height, req_bpp)) {
                goto nofb;
            }

            multiboot1_info->fb_addr    = (uint64_t)fbinfo.framebuffer_addr;
            multiboot1_info->fb_width   = fbinfo.framebuffer_width;
            multiboot1_info->fb_height  = fbinfo.framebuffer_height;
            multiboot1_info->fb_bpp     = fbinfo.framebuffer_bpp;
            multiboot1_info->fb_pitch   = fbinfo.framebuffer_pitch;
            multiboot1_info->fb_type    = 1;
            multiboot1_info->fb_red_mask_size    = fbinfo.red_mask_size;
            multiboot1_info->fb_red_mask_shift   = fbinfo.red_mask_shift;
            multiboot1_info->fb_green_mask_size  = fbinfo.green_mask_size;
            multiboot1_info->fb_green_mask_shift = fbinfo.green_mask_shift;
            multiboot1_info->fb_blue_mask_size   = fbinfo.blue_mask_size;
            multiboot1_info->fb_blue_mask_shift  = fbinfo.blue_mask_shift;
        } else if (header.fb_mode == 1) {
nofb:;
#if uefi == 1
            panic(true, "multiboot1: Cannot use text mode with UEFI.");
#elif bios == 1
            size_t rows, cols;
            init_vga_textmode(&rows, &cols, false);

            multiboot1_info->fb_addr    = 0xb8000;
            multiboot1_info->fb_width   = cols;
            multiboot1_info->fb_height  = rows;
            multiboot1_info->fb_bpp     = 16;
            multiboot1_info->fb_pitch   = 2 * cols;
            multiboot1_info->fb_type    = 2;
#endif
        } else {
            panic(true, "multiboot1: Illegal framebuffer type requested");
        }

        multiboot1_info->flags |= (1 << 12);
    } else {
#if uefi == 1
        panic(true, "multiboot1: Cannot use text mode with UEFI.");
#elif bios == 1
        size_t rows, cols;
        init_vga_textmode(&rows, &cols, false);
#endif
    }

    // Load relocation stub where it won't get overwritten
    size_t reloc_stub_size = (size_t)multiboot_reloc_stub_end - (size_t)multiboot_reloc_stub;
    void *reloc_stub = mb1_alloc(reloc_stub_size);
    memcpy(reloc_stub, multiboot_reloc_stub, reloc_stub_size);

#if uefi == 1
    efi_exit_boot_services();
#endif

    size_t mb_mmap_count;
    struct e820_entry_t *raw_memmap = get_raw_memmap(&mb_mmap_count);

    size_t mb_mmap_len = mb_mmap_count * sizeof(struct multiboot1_mmap_entry);
    struct multiboot1_mmap_entry *mmap = mb1_info_alloc(&mb1_info_raw, mb_mmap_len);

    // Multiboot is bad and passes raw memmap. We do the same to support it.
    for (size_t i = 0; i < mb_mmap_count; i++) {
        mmap[i].size = sizeof(struct multiboot1_mmap_entry) - 4;
        mmap[i].addr = raw_memmap[i].base;
        mmap[i].len  = raw_memmap[i].length;
        mmap[i].type = raw_memmap[i].type;
    }

    {
        struct meminfo memory_info = mmap_get_info(mb_mmap_count, raw_memmap);

        // Convert the uppermem and lowermem fields from bytes to
        // KiB.
        multiboot1_info->mem_lower = memory_info.lowermem / 1024;
        multiboot1_info->mem_upper = memory_info.uppermem / 1024;
    }

    multiboot1_info->mmap_length = mb_mmap_len;
    multiboot1_info->mmap_addr = (uint32_t)(size_t)mmap - mb1_info_slide;
    multiboot1_info->flags |= (1 << 0) | (1 << 6);

    irq_flush_type = IRQ_PIC_ONLY_FLUSH;

    common_spinup(multiboot1_spinup_32, 8,
                  entry_point,
                  (uint32_t)(uintptr_t)multiboot1_info, (uint32_t)mb1_info_final_loc,
                  (uint32_t)mb1_info_size,
                  (uint32_t)(uintptr_t)elf_ranges, (uint32_t)elf_ranges_count,
                  (uint32_t)slide,
                  (uint32_t)(uintptr_t)reloc_stub);
}
