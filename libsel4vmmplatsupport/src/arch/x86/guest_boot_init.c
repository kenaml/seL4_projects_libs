/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

/* Guest specific booting to do bootinfo manipulation */

#include <autoconf.h>
#include <sel4vm/gen_config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cpio/cpio.h>
#include <sel4utils/mapping.h>
#include <vka/capops.h>

#include <sel4vm/guest_vm.h>
#include <sel4vm/guest_x86_context.h>
#include <sel4vm/guest_ram.h>
#include <sel4vm/guest_memory_util.h>
#include <sel4vm/processor/platfeature.h>
#include <sel4vm/platform/vmcs.h>
#include <sel4vm/guest_memory.h>

#include <sel4vmmplatsupport/guest_boot_init.h>
#include <sel4vmmplatsupport/guest_boot_info.h>
#include <sel4vmmplatsupport/e820.h>
#include <sel4vmmplatsupport/acpi.h>

#include <sel4/arch/bootinfo_types.h>

static int guest_elf_write_address(vm_t *vm, uintptr_t paddr, void *vaddr, size_t size, size_t offset,
        void *cookie) {
    memcpy(vaddr, cookie + offset, size);
    return 0;
}

static int guest_elf_read_address(vm_t *vm, uintptr_t paddr, void *vaddr, size_t size, size_t offset,
        void *cookie) {
    memcpy(cookie + offset, vaddr, size);
    return 0;
}

static inline uint32_t vmm_plat_vesa_fbuffer_size(seL4_VBEModeInfoBlock_t *block)
{
    assert(block);
    return ALIGN_UP(block->vbe_common.bytesPerScanLine * block->vbe12_part1.yRes, 65536);
}

static int make_guest_cmd_line_continued(vm_t *vm, uintptr_t phys, void *vaddr, size_t size, size_t offset, void *cookie) {
    /* Copy the string to this area. */
    const char *cmdline = (const char *)cookie;
    memcpy(vaddr, cmdline + offset, size);
    return 0;
}

static int make_guest_cmd_line(vm_t *vm, const char *cmdline, uintptr_t *guest_cmd_addr, size_t *guest_cmd_len) {
    /* Allocate command line from guest ram */
    int len = strlen(cmdline);
    uintptr_t cmd_addr = vm_ram_allocate(vm, len + 1);
    if (cmd_addr == 0) {
        ZF_LOGE("Failed to allocate guest cmdline (length %d)", len);
        return -1;
    }
    printf("Constructing guest cmdline at 0x%x of size %d\n", (unsigned int)cmd_addr, len);
    *guest_cmd_addr = cmd_addr;
    *guest_cmd_len = len;
    return vm_ram_touch(vm, cmd_addr, len + 1, make_guest_cmd_line_continued, (void*)cmdline);
}

static void make_guest_screen_info(vm_t *vm, struct screen_info *info) {
    /* VESA information */
    seL4_X86_BootInfo_VBE vbeinfo;
    ssize_t result;
    int error;
    result = simple_get_extended_bootinfo(vm->simple, SEL4_BOOTINFO_HEADER_X86_VBE, &vbeinfo, sizeof(seL4_X86_BootInfo_VBE));
    uintptr_t base = 0;
    size_t fbuffer_size;
    if (config_set(CONFIG_VMM_VESA_FRAMEBUFFER) && result != -1) {
        /* map the protected mode interface at the same location we are told about it at.
         * this is just to guarantee that it ends up within the segment addressable range */
        uintptr_t pm_base = (vbeinfo.vbeInterfaceSeg << 4) + vbeinfo.vbeInterfaceOff;
        error = 0;
        if (pm_base > 0xc000) {
            /* construct a page size and aligned region to map */
            uintptr_t aligned_pm = ROUND_DOWN(pm_base, PAGE_SIZE_4K);
            int size = vbeinfo.vbeInterfaceLen + (pm_base - aligned_pm);
            size = ROUND_UP(size, PAGE_SIZE_4K);
            vm_memory_reservation_t *reservation = vm_reserve_memory_at(vm, aligned_pm, size,
                    default_error_fault_callback, NULL);
            if (reservation) {
                error = map_ut_alloc_reservation(vm, reservation);
            } else {
                error = -1;
            }
        }
        if (error) {
            ZF_LOGE("Failed to map vbe protected mode interface for VESA frame buffer. Disabling");
        } else {
            fbuffer_size = vmm_plat_vesa_fbuffer_size(&vbeinfo.vbeModeInfoBlock);
            vm_memory_reservation_t *reservation = vm_reserve_anon_memory(vm, fbuffer_size,
                    default_error_fault_callback, NULL, &base);
            if (!reservation) {
                ZF_LOGE("Failed to reserve base pointer for VESA frame buffer. Disabling");
            } else {
                error = map_ut_alloc_reservation_with_base_paddr(vm, vbeinfo.vbeModeInfoBlock.vbe20.physBasePtr, reservation);
                if (error) {
                    ZF_LOGE("Failed to map base pointer for VESA frame buffer. Disabling");
                    base = 0;
                }
            }
        }
    }
    if (base) {
        info->orig_video_isVGA = 0x23; // Tell Linux it's a VESA mode
        info->lfb_width = vbeinfo.vbeModeInfoBlock.vbe12_part1.xRes;
        info->lfb_height = vbeinfo.vbeModeInfoBlock.vbe12_part1.yRes;
        info->lfb_depth = vbeinfo.vbeModeInfoBlock.vbe12_part1.bitsPerPixel;

        info->lfb_base = base;
        info->lfb_size = fbuffer_size >> 16;
        info->lfb_linelength = vbeinfo.vbeModeInfoBlock.vbe_common.bytesPerScanLine;

        info->red_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.redLen;
        info->red_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.redOff;
        info->green_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.greenLen;
        info->green_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.greenOff;
        info->blue_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.blueLen;
        info->blue_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.blueOff;
        info->rsvd_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.rsvdLen;
        info->rsvd_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.rsvdOff;
        info->vesapm_seg = vbeinfo.vbeInterfaceSeg;
        info->vesapm_off = vbeinfo.vbeInterfaceOff;
        info->pages = vbeinfo.vbeModeInfoBlock.vbe12_part1.planes;
    } else {
        memset(info, 0, sizeof(*info));
    }
}

static int make_guest_e820_map(struct e820entry *e820, vm_mem_t *guest_memory) {
    int i;
    int entry = 0;
    /* Create an initial entry at 0 that is reserved */
    e820[entry].addr = 0;
    e820[entry].size = 0;
    e820[entry].type = E820_RESERVED;
    assert(guest_memory->num_ram_regions > 0);
    for (i = 0; i < guest_memory->num_ram_regions; i++) {
        /* Check for discontinuity. We need this check since we can have multiple
         * regions that are contiguous if they have different allocation flags.
         * However we are reporting ALL of this memory to the guest */
        if (e820[entry].addr + e820[entry].size != guest_memory->ram_regions[i].start) {
            /* Finish region. Unless it was zero sized */
            if (e820[entry].size != 0) {
                entry++;
                assert(entry < E820MAX);
                e820[entry].addr = e820[entry - 1].addr + e820[entry - 1].size;
                e820[entry].type = E820_RESERVED;
            }
            /* Pad region */
            e820[entry].size = guest_memory->ram_regions[i].start - e820[entry].addr;
            /* Now start a new RAM region */
            entry++;
            assert(entry < E820MAX);
            e820[entry].addr = guest_memory->ram_regions[i].start;
            e820[entry].type = E820_RAM;
        }
        /* Increase region to size */
        e820[entry].size = guest_memory->ram_regions[i].start - e820[entry].addr + guest_memory->ram_regions[i].size;
    }
    /* Create empty region at the end */
    entry++;
    assert(entry < E820MAX);
    e820[entry].addr = e820[entry - 1].addr + e820[entry - 1].size;
    e820[entry].size = 0x100000000ull - e820[entry].addr;
    e820[entry].type = E820_RESERVED;
    printf("Final e820 map is:\n");
    for (i = 0; i <= entry; i++) {
        printf("\t0x%x - 0x%x type %d\n", (unsigned int)e820[i].addr, (unsigned int)(e820[i].addr + e820[i].size),
               e820[i].type);
        assert(e820[i].addr < e820[i].addr + e820[i].size);
    }
    return entry + 1;
}

static int make_guest_boot_info(vm_t *vm, uintptr_t guest_cmd_addr, size_t guest_cmd_len,
        uintptr_t guest_kernel_load_addr, size_t guest_kernel_alignment,
        uintptr_t guest_ramdisk_load_addr, size_t guest_ramdisk_size) {
    /* TODO: Bootinfo struct needs to be allocated in location accessable by real mode? */
    uintptr_t addr = vm_ram_allocate(vm, sizeof(struct boot_params));
    if (addr == 0) {
        ZF_LOGE("Failed to allocate %zu bytes for guest boot info struct", sizeof(struct boot_params));
        return -1;
    }
    printf("Guest boot info allocated at %p. Populating...\n", (void*)addr);
    vm->arch.guest_boot_info.boot_info = addr;

    /* Map in BIOS boot info structure. */
    struct boot_params boot_info;
    memset(&boot_info, 0, sizeof(struct boot_params));

    /* Initialise basic bootinfo structure. Src: Linux kernel Documentation/x86/boot.txt */
    boot_info.hdr.header = 0x53726448; /* Magic number 'HdrS' */
    boot_info.hdr.boot_flag = 0xAA55; /* Magic number for Linux. */
    boot_info.hdr.type_of_loader = 0xFF; /* Undefined loeader type. */
    boot_info.hdr.code32_start = guest_kernel_load_addr;
    boot_info.hdr.kernel_alignment = guest_kernel_alignment;
    boot_info.hdr.relocatable_kernel = true;

    /* Set up screen information. */
    /* Tell Guest OS about VESA mode. */
    make_guest_screen_info(vm, &boot_info.screen_info);

    /* Create e820 memory map */
    boot_info.e820_entries = make_guest_e820_map(boot_info.e820_map, &vm->mem);

    /* Pass in the command line string. */
    boot_info.hdr.cmd_line_ptr = guest_cmd_addr;
    boot_info.hdr.cmdline_size = guest_cmd_len;

    /* These are not needed to be precise, because Linux uses these values
     * only to raise an error when the decompression code cannot find good
     * space. ref: GRUB2 source code loader/i386/linux.c */
    boot_info.alt_mem_k = 0;//((32 * 0x100000) >> 10);

    /* Pass in initramfs. */
    if (guest_ramdisk_load_addr) {
        boot_info.hdr.ramdisk_image = (uint32_t) guest_ramdisk_load_addr;
        boot_info.hdr.ramdisk_size = guest_ramdisk_size;
        boot_info.hdr.root_dev = 0x0100;
        boot_info.hdr.version = 0x0204; /* Report version 2.04 in order to report ramdisk_image. */
    } else {
        boot_info.hdr.version = 0x0202;
    }
    return vm_ram_touch(vm, addr, sizeof(boot_info), guest_elf_write_address, &boot_info);
}

/* Init the guest page directory, cmd line args and boot info structures. */
void vmm_plat_init_guest_boot_structure(vm_t *vm, const char *cmdline,
        uintptr_t guest_kernel_load_addr, size_t guest_kernel_alignment,
        uintptr_t guest_ramdisk_load_addr, size_t guest_ramdisk_size) {
    int UNUSED err;
    uintptr_t guest_cmd_addr;
    size_t guest_cmd_size;

    err = make_guest_cmd_line(vm, cmdline, &guest_cmd_addr, &guest_cmd_size);
    assert(!err);

    err = make_guest_boot_info(vm, guest_cmd_addr, guest_cmd_size,
            guest_kernel_load_addr, guest_kernel_alignment,
            guest_ramdisk_load_addr, guest_ramdisk_size);
    assert(!err);

    err = make_guest_acpi_tables(vm);
    assert(!err);
}

void vmm_init_guest_thread_state(vm_vcpu_t *vcpu, uintptr_t guest_entry_addr) {
    vm_set_thread_context_reg(vcpu, VCPU_CONTEXT_EAX, 0);
    vm_set_thread_context_reg(vcpu, VCPU_CONTEXT_EBX, 0);
    vm_set_thread_context_reg(vcpu, VCPU_CONTEXT_ECX, 0);
    vm_set_thread_context_reg(vcpu, VCPU_CONTEXT_EDX, 0);

    /* Entry point. */
    printf("Initializing guest to start running at 0x%x\n", (unsigned int) guest_entry_addr);
    vm_set_vmcs_field(vcpu, VMX_GUEST_RIP, (unsigned int) guest_entry_addr);
    /* The boot_param structure. */
    vm_set_thread_context_reg(vcpu, VCPU_CONTEXT_ESI, vcpu->vm->arch.guest_boot_info.boot_info);
}
