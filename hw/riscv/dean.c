/*
 * QEMU RISC-V My Board
 *
 * Copyright (c) 2025 zhangdi, Inc.
 *
 * RISC-V machine with 16550a UART and VirtIO MMIO
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "target/riscv/cpu.h"
#include "hw/core/sysbus-fdt.h"
#include "target/riscv/pmu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/iommu.h"
#include "hw/riscv/riscv-iommu-bits.h"
#include "hw/riscv/dean.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "system/kvm.h"
#include "hw/firmware/smbios.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/sifive_plic.h"
#include "hw/misc/sifive_test.h"
#include "hw/platform-bus.h"
#include "chardev/char.h"
#include "system/device_tree.h"
#include "system/tcg.h"
#include "system/kvm.h"
#include "system/tpm.h"
#include "system/qtest.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/gpex.h"
#include "hw/display/ramfb.h"
#include "hw/acpi/aml-build.h"
#include "qapi/qapi-visit-common.h"
#include "hw/virtio/virtio-iommu.h"
#include "hw/ssi/sifive_spi.h"


/*
 * 定义内存相关资源"
 * 一个cpu内部的一片maskrom：用于cpu启动时固定执行其内部的代码
 * 一片sram：早期启动代码时数据存放空间
 * 一片ddr内存： 
 * Copyright (c) 2025 zhangdi, Inc.
 * 后续按需添加
 */
static const MemMapEntry dean_memmap[] = {
    [DEAN_MROM]  = {        0x0,        0x8000 },
    [DEAN_SRAM]  = {     0x8000,        0x8000 },
    [DEAN_UART0] = { 0x10000000,         0x100 },
    [DEAN_DRAM]  = { 0x80000000,           0x0 },
};


static void dean_setup_rom_reset_vec(MachineState *machine, RISCVHartArrayState *harts,
                               hwaddr start_addr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint64_t kernel_entry,
                               uint32_t fdt_load_addr)
{
    int i;
    uint32_t start_addr_hi32 = 0x00000000;

    if (!riscv_is_32bit(harts)) {
        start_addr_hi32 = start_addr >> 32;
    }
    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
        reset_vec[3] = 0x0202a583;   /*     lw     a1, 32(t0) */
        reset_vec[4] = 0x0182a283;   /*     lw     t0, 24(t0) */
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) */
    }

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }

    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);
}


static void dean_machine_init(MachineState *machine)
{
    const MemMapEntry *memmap = dean_memmap;
    RISCVVirtState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    int i, base_hartid, hart_count;
    char *soc_name;

    if (DEAN_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            DEAN_SOCKETS_MAX);
        exit(1);
    }

    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
    }

    memory_region_init_ram(main_mem, NULL, "riscv_dean_board.dram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[DEAN_DRAM].base,
        main_mem);

    memory_region_init_ram(sram_mem, NULL, "riscv_dean_board.sram",
                           memmap[DEAN_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[DEAN_SRAM].base,
        sram_mem);

    memory_region_init_rom(mask_rom, NULL, "riscv_dean_board.mrom",
                           memmap[DEAN_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[DEAN_MROM].base,
                                mask_rom);

    dean_setup_rom_reset_vec(machine, &s->soc[0], memmap[DEAN_MROM].base,
                              dean_memmap[DEAN_MROM].base,
                              dean_memmap[DEAN_MROM].size,
                              0x0, 0x0);
}

static void dean_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V DEAN board";
    
    //注册板卡资源初始化函数
    mc->init = dean_machine_init;
    
    //最大支持的smp核心数
    mc->max_cpus = DEAN_CPUS_MAX;

    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->pci_allow_0_address = true;
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;


    /*
    每一组object_class_property_add_bool和object_class_property_set_description
    对应qemu-system-riscv64 -machine dean,help中显示的一行。
    */
    //向对象系统注册一个布尔属性，revb
#if 0    
    object_class_property_add_bool(oc, "revb", dean_machine_get_revb,
                                   dean_machine_set_revb);

    //给已注册的属性写说明文字
    object_class_property_set_description(oc, "revb",
                                          "Set on to tell QEMU that it should model "
                                          "the revB HiFive1 board");
#endif

}

static void dean_machine_instance_init(Object *obj)
{
}

static const TypeInfo dean_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("dean-soc"),
    .parent     = TYPE_MACHINE,
    .class_init = dean_machine_class_init,
    .instance_init = dean_machine_instance_init,
    .instance_size = sizeof(RISCVVirtState),
};

static void dean_machine_init_register_types(void)
{
    type_register_static(&dean_machine_typeinfo);
}

type_init(dean_machine_init_register_types)