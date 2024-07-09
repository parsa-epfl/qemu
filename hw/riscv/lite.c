#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/char/sifive_uart.h"
#include "hw/riscv/lite.h"
#include "sysemu/sysemu.h"


enum {
    LITE_BS,
    LITE_ROM,
    LITE_MMU,
    LITE_QSPI,
    LITE_CLINT,
    LITE_PLIC,
    LITE_UART,
    LITE_RAM
};

enum {
    LITE_UART_IRQ = 7,
    LITE_QSPI_IRQ = 8
};

static MemMapEntry map[] = {
    [LITE_BS   ] = { 0x80000000lu, 0xffffffff80000000lu },
    [LITE_ROM  ] = { 0x10000000lu, 0x000000000000f000lu },
    [LITE_MMU  ] = { 0x20000000lu, 0x0000000000001000lu },
    [LITE_QSPI ] = { 0x30000000lu, 0x0000000000001000lu },
    [LITE_CLINT] = { 0x38000000lu, 0x0000000000010000lu },
    [LITE_PLIC ] = { 0x3c000000lu, 0x0000000004000000lu },
    [LITE_UART ] = { 0x40600000lu, 0x0000000000001000lu },
    [LITE_RAM  ] = { 0x80000000lu, 0x0000000100000000lu }
};


static void init(MachineState *m) {
    LiteDevice  *dev  = LITE_MACHINE(m);
    DeviceState *plic = NULL;

    // sockets
    dev->soc = g_new0(RISCVHartArrayState, riscv_socket_count(m));

    for (int i = 0; i < riscv_socket_count(m); i++) {
        object_initialize_child(OBJECT(m), "socket", &dev->soc[i], TYPE_RISCV_HART_ARRAY);

        int hart_num = riscv_socket_hart_count  (m, i);
        int hart_id  = riscv_socket_first_hartid(m, i);

        qdev_prop_set_string(DEVICE(&dev->soc[i]), "cpu-type",    m->cpu_type);
        qdev_prop_set_uint32(DEVICE(&dev->soc[i]), "num-harts",   hart_num);
        qdev_prop_set_uint32(DEVICE(&dev->soc[i]), "hartid-base", hart_id);
        qdev_prop_set_uint64(DEVICE(&dev->soc[i]), "resetvec",    map[LITE_ROM].base);

        sysbus_realize(SYS_BUS_DEVICE(&dev->soc[i]), &error_abort);

        riscv_aclint_swi_create   (map[LITE_CLINT].base + map[LITE_CLINT].size * i,
                                   hart_id, hart_num, false);

        riscv_aclint_mtimer_create(map[LITE_CLINT].base + map[LITE_CLINT].size * i + RISCV_ACLINT_SWI_SIZE,
                                   RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                                   hart_id,
                                   hart_num,
                                   RISCV_ACLINT_DEFAULT_MTIMECMP,
                                   RISCV_ACLINT_DEFAULT_MTIME,
                                   RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ,
                                   false);

        plic = sifive_plic_create (map[LITE_PLIC].base,
                                   riscv_plic_hart_config_string(hart_num),
                                   m->smp.cpus, 0,
                                   127,      7,
                                   0x04,     0x1000,
                                   0x2000,   0x80,
                                   0x200000, 0x1000,
                                   map[LITE_PLIC].size);
    }

    map[LITE_RAM].size = m->ram_size;

    // boot rom
    MemoryRegion *rom = g_new0(MemoryRegion, 1);
    MemoryRegion *ram = g_new0(MemoryRegion, 1);

    // TODO: rom/ram must be instantiated in the same order as they are placed in the as...
    memory_region_init_rom(rom, NULL, "lite.rom", map[LITE_ROM].size, &error_fatal);
    memory_region_init_ram(ram, NULL, "lite.ram", map[LITE_RAM].size, &error_fatal);

    memory_region_add_subregion(get_system_memory(), map[LITE_ROM].base, rom);
    memory_region_add_subregion(get_system_memory(), map[LITE_RAM].base, ram);

    // load
    hwaddr kernel = map[LITE_RAM].base;
    hwaddr initrd = map[LITE_RAM].base + 0x2000000lu;
    hwaddr dtb    = map[LITE_ROM].base + 0x200lu;

    // real physical as
    if (m->kernel_filename)
        load_image_targphys_as(m->kernel_filename, kernel,
                               map[LITE_RAM].size, NULL);
    if (m->initrd_filename)
        load_image_targphys_as(m->initrd_filename, initrd,
                               map[LITE_RAM].size, NULL);

    // original physical (now lite) as
    if (m->dtb)
        load_image_targphys_as(m->dtb, dtb,
                               map[LITE_ROM].size, NULL);

    // syms
    if (m->sym)
        load_sym(m->sym);

    riscv_setup_rom_reset_vec(m, dev->soc, kernel,
                              map[LITE_ROM].base,
                              map[LITE_ROM].size,
                              kernel, dtb);

    // uart
    sifive_uart_create(get_system_memory(),
                       map[LITE_UART].base,
                       serial_hd(0),
                       qdev_get_gpio_in(DEVICE(plic), LITE_UART_IRQ));

    // qspi
    DeviceState  *qspi = qdev_new("sifive.spi");
    SysBusDevice *bus  = SYS_BUS_DEVICE(qspi);

    sysbus_realize_and_unref(bus, &error_fatal);
    sysbus_connect_irq      (bus, 0, qdev_get_gpio_in(DEVICE(plic), LITE_QSPI_IRQ));

    memory_region_add_subregion(get_system_memory(),
                                map[LITE_QSPI].base,
                                sysbus_mmio_get_region(bus, 0));
}

static void class_init(ObjectClass *klass, void *data) {
    MachineClass *m = MACHINE_CLASS(klass);

    m->desc                        = "RISC-V Lite";
    m->init                        =  init;
    m->max_cpus                    =  32;
    m->is_default                  =  true;
    m->default_cpu_type            =  TYPE_RISCV_CPU_BASE;
    m->possible_cpu_arch_ids       =  riscv_numa_possible_cpu_arch_ids;
    m->cpu_index_to_instance_props =  riscv_numa_cpu_index_to_props;
    m->get_default_cpu_node_id     =  riscv_numa_get_default_cpu_node_id;
    m->numa_mem_supported          =  true;
}

static const TypeInfo type_info = {
    .name          = TYPE_LITE_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(LiteDevice),
    .class_init    = class_init
};

static void type_init_fn(void) {
    type_register(&type_info);
}

type_init(type_init_fn)
