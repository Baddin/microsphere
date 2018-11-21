
/*
 * Copyright (C) 2018-present Frederic Meyer. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "apic.h"
#include <stdint.h>
#include <stddef.h>
#include <arch/print.h>
#include <arch/x86_64/pc/pm/pm.h>
#include <arch/x86_64/pc/vm/vm.h>
#include <log.h>

#define MSR_APIC_BASE  (0x1B)
#define MSR_APIC_BASE_ENABLE (0x800)
#define MSR_APIC_BASE_BSP (0x100)

void _rdmsr(uint32_t reg, uint32_t* eax, uint32_t* edx);
void _wrmsr(uint32_t reg, uint32_t  eax, uint32_t  edx);

static void* lapic_mmio = NULL;

bool lapic_is_present(struct amd64_cpu* cpu) {
    return cpu->features & CPUID_FEAT_APIC;
}

static uint64_t lapic_get_base() {
    uint32_t lapic_base_edx;
    uint32_t lapic_base_eax;

    /* get current LAPIC base from MSR */
    _rdmsr(MSR_APIC_BASE, &lapic_base_eax, &lapic_base_edx);

    return ((uint64_t)lapic_base_edx<<32)|(lapic_base_eax&0xFFFFF000);
}

static void lapic_set_base(uint64_t base) {
    base = (base&0xFFFFF000)|MSR_APIC_BASE_ENABLE|MSR_APIC_BASE_BSP;
    _wrmsr(MSR_APIC_BASE, (base&0xFFFFFFFF), (base>>32));
}

/* TODO: place MPC code somewhere else? */

/* Multiprocessor Configuration Table */
struct mpc_table {
    uint32_t magic;
    uint16_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[8];
    char     product_id[12];
    uint32_t oem_tab;
    uint16_t oem_tab_sz;
    uint16_t entry_cnt;
    uint32_t lapic_base;
    uint16_t ext_tab_sz;
    uint8_t  ext_tab_chk;
} __attribute__((packed));

/* Multiprocessor Floating Pointer Structure */
struct mpc_pointer {
    uint32_t magic;
    uint32_t config_ptr;
    uint8_t  length;
    uint8_t  version;
    uint8_t  checksum;
    uint8_t  features[5];
} __attribute__((packed));

/* Multiprocessor Processor Entry */
struct mpc_cpu {
    uint8_t type;
    uint8_t lapic_id;
    uint8_t lapic_ver;
    unsigned int enabled : 1;
    unsigned int bsp : 1;
    unsigned int reserved : 6;
    uint32_t signature;
    uint32_t features;
} __attribute__((packed));

void delay(int ms);

static void wakeup(int apic_id) {
    volatile uint32_t* icr_hi = (void*)lapic_mmio + 0x310;
    volatile uint32_t* icr_lo = (void*)lapic_mmio + 0x300;
    volatile uint32_t* id = (void*)lapic_mmio + 0x20;

    *icr_hi = apic_id << 24;
    *icr_lo = 0x08 | (5 << 8) | (1 << 14) | (0 << 18);
    *id;
    delay(10);
    while (icr_lo[0] & (1<<12)) ;
    *icr_lo = 0x08 | (6 << 8) | (0 << 14) | (0 << 18);
    *id;
    delay(10);
    while (icr_lo[0] & (1<<12)) ;
}

static void find_mpc_table() {
    void* data = (void*)0xFFFF808000000000;
    struct mpc_pointer* ptr = NULL;
    struct mpc_table* config = NULL;

    /* TODO: atm we are scanning the entire first 1MiB.
     * However the specification allows us to narrow it down
     * to three specific memory areas.
     */
    for (int i = 0; i <= 0xFFFFC; i++) {
        if (*(uint32_t*)(data + i) == 0x5F504D5F) { /* _MP_ */
            ptr = data + i;
            klog(LL_DEBUG, "apic: MPC-pointer discovered (%p).", ptr);
            break;
        }
    }

    if (ptr == NULL) {
        klog(LL_WARN, "apic: Missing MPC-pointer. Not a multicore system?");
        return;
    }

    config = data + ptr->config_ptr;
    klog(LL_DEBUG, "apic: MPC-table found (%p).", config);

    if (config->magic != 0x504D4350) {
        klog(LL_WARN, "apic: MPC-table signature mismatch: %#08x", config->magic);
        return;
    }

    int cpu_id = 0;

    /* Why the fuck do we have to + 1?
     * The entries are said to _follow_ after the config table.
     * Our structure has exactly 43 bytes which matches the specified size.
     */
    void* entry = (void*)config + sizeof(struct mpc_table) + 1;

    /* Traverse configuration table. */
    for (int i = 0; i < config->entry_cnt; i++) {
        uint8_t type = *(uint8_t*)entry;
        switch (type) {
            case 0: {
                struct mpc_cpu* cpu = entry;
                klog(LL_INFO, "apic: cpu[%d]: lapic_id=%#x enabled=%u bsp=%u signature=%#x",
                    cpu_id++,
                    cpu->lapic_id,
                    cpu->enabled,
                    cpu->bsp,
                    cpu->signature
                );
                if (!cpu->bsp) {
                    wakeup(cpu->lapic_id);
                }
                entry += 20;
                break;
            }
            case 1:
            case 2:
            case 3:
            case 4:
                entry += 8;
                break;
            default:
                entry += 8;
                klog(LL_WARN, "apic: Unknown MPC-table entry type %#x.", type);
                break;
        }
    }
}

extern const void _wakeup_start;
extern const void _wakeup_end;
extern const void _wakeup_tab;

extern const struct vm_context vm_kctx;

void lapic_init() {
    uint64_t base = lapic_get_base();

    /* Enable the Local-APIC */
    lapic_set_base(base);

    /* Map its MMIO registers into memory */
    lapic_mmio = vm_alloc(1);
    vm_map_page(lapic_mmio, base / 4096);
    klog(LL_DEBUG, "apic: Mapped Local-APIC MMIO @ %p", lapic_mmio);

    volatile uint32_t* spivr = (void*)lapic_mmio + 0xF0;

    /* Actually enabling the Local-APIC by setting
     * Enable-bit in the SPIVR register.
     */
    *spivr |= 0x80;

    uint64_t* src = &_wakeup_start;
    uint64_t* dst = (void*)0xFFFF808000000000 + 0x8000;
    uint64_t* tab = (void*)dst + (&_wakeup_tab - &_wakeup_start);

    /* Copy payload to lower 1 MiB. */
    while (src < &_wakeup_end)
        *dst++ = *src++;

    /* TEST: Allocate stack. */
    void* stack_virt = vm_alloc(8);
    uint32_t stack_phys[8];

    /* TODO: check status code */
    pm_stack_alloc(8, stack_phys);
    vm_map_pages(stack_virt, stack_phys, 8);
    tab[0] = stack_virt + 32768;
    tab[1] = (void*)&vm_kctx.pml4[0] - VM_BASE_KERNEL_ELF;

    find_mpc_table();
}
