/*
 * NXP MIMXRT685 HiFi4 DSP machine
 *
 * This deliberately small, project-local machine is intended to execute the
 * bare-metal DSP USART example.  It models the RT600 Xtensa core's local
 * memories, the DSP-visible SRAM window, and only the clock/reset/pin/USART
 * devices touched by that example.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/char/rt6xx_flexcomm.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/rt6xx_iopctl.h"
#include "hw/misc/rt6xx_rstctl.h"
#include "hw/misc/rt6xx_syscon.h"
#include "qemu/error-report.h"
#include "target/xtensa/cpu.h"
#include "target/xtensa/cpu-qom.h"
#include "xtensa_memory.h"
#include "xtensa_sim.h"

#define TYPE_MIMXRT685_HIFI4_MACHINE \
    MACHINE_TYPE_NAME("mimxrt685-evk-hifi4")
OBJECT_DECLARE_SIMPLE_TYPE(MIMXRT685HiFi4MachineState,
                           MIMXRT685_HIFI4_MACHINE)

#define RT685_DSP_SRAM_BASE       0x00200000ULL
#define RT685_DSP_SRAM_SIZE       0x00280000ULL
#define RT685_CLKCTL0_BASE        0x40001000ULL
#define RT685_RSTCTL1_BASE        0x40020000ULL
#define RT685_CLKCTL1_BASE        0x40021000ULL
#define RT685_IOPCTL_BASE         0x40004000ULL
#define RT685_FLEXCOMM0_BASE      0x40106000ULL

struct MIMXRT685HiFi4MachineState {
    MachineState parent_obj;

    XtensaCPU *cpu;
    MemoryRegion sram;
    Rt6xxSysconState clkctl0;
    Rt6xxSysconState clkctl1;
    Rt6xxRstctlState rstctl1;
    Rt6xxIopctlState iopctl;
    Rt6xxFlexcommState flexcomm0;
};

static void mimxrt685_hifi4_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void mimxrt685_hifi4_init(MachineState *machine)
{
    MIMXRT685HiFi4MachineState *s = MIMXRT685_HIFI4_MACHINE(machine);
    CPUXtensaState *env;
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *fc;

    object_initialize_child(OBJECT(machine), "clkctl0", &s->clkctl0,
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(OBJECT(machine), "clkctl1", &s->clkctl1,
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(OBJECT(machine), "rstctl1", &s->rstctl1,
                            TYPE_RT6XX_RSTCTL);
    object_initialize_child(OBJECT(machine), "iopctl", &s->iopctl,
                            TYPE_RT6XX_IOPCTL);
    object_initialize_child(OBJECT(machine), "flexcomm0", &s->flexcomm0,
                            TYPE_RT6XX_FLEXCOMM);
    fc = DEVICE(&s->flexcomm0);

    s->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));
    env = &s->cpu->env;
    qemu_register_reset(mimxrt685_hifi4_cpu_reset, s->cpu);
    cpu_reset(CPU(s->cpu));

    xtensa_create_memory_regions(&env->config->instram, "rt685-dsp.instram",
                                 system_memory);
    xtensa_create_memory_regions(&env->config->dataram, "rt685-dsp.dataram",
                                 system_memory);

    if (!memory_region_init_ram(&s->sram, NULL, "rt685-dsp.sram",
                                machine->ram_size, &error_fatal)) {
        return;
    }
    memory_region_add_subregion(system_memory, RT685_DSP_SRAM_BASE, &s->sram);

    qdev_prop_set_uint32(DEVICE(&s->clkctl0), "variant", RT6XX_CLKCTL0);
    sysbus_realize(SYS_BUS_DEVICE(&s->clkctl0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clkctl0), 0, RT685_CLKCTL0_BASE);

    qdev_prop_set_uint32(DEVICE(&s->clkctl1), "variant", RT6XX_CLKCTL1);
    sysbus_realize(SYS_BUS_DEVICE(&s->clkctl1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clkctl1), 0, RT685_CLKCTL1_BASE);

    qdev_prop_set_uint32(DEVICE(&s->rstctl1), "variant", RT6XX_RSTCTL1);
    sysbus_realize(SYS_BUS_DEVICE(&s->rstctl1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rstctl1), 0, RT685_RSTCTL1_BASE);

    sysbus_realize(SYS_BUS_DEVICE(&s->iopctl), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iopctl), 0, RT685_IOPCTL_BASE);

    qdev_prop_set_chr(fc, "chardev", serial_hd(0));
    qdev_prop_set_uint32(fc, "capabilities", RT6XX_FLEXCOMM_CAP_ALL);
    sysbus_realize(SYS_BUS_DEVICE(fc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(fc), 0, RT685_FLEXCOMM0_BASE);
    qdev_connect_gpio_out_named(DEVICE(&s->clkctl1), "flexcomm-clock", 0,
                                qdev_get_gpio_in_named(fc, "clock-enabled", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->rstctl1), "flexcomm-reset", 0,
                                qdev_get_gpio_in_named(fc, "reset", 0));

    if (!machine->kernel_filename) {
        error_report("mimxrt685-evk-hifi4 requires an Xtensa ELF "
                     "with -kernel");
        return;
    }
    xtensa_sim_load_kernel(s->cpu, machine);
}

static void mimxrt685_hifi4_machine_class_init(ObjectClass *oc,
                                                const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        XTENSA_CPU_TYPE_NAME("nxp_rt600"),
        NULL,
    };

    mc->desc = "NXP MIMXRT685 HiFi4 DSP (minimal local model)";
    mc->init = mimxrt685_hifi4_init;
    mc->default_cpu_type = XTENSA_CPU_TYPE_NAME("nxp_rt600");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = RT685_DSP_SRAM_SIZE;
}

static const TypeInfo mimxrt685_hifi4_machine_info = {
    .name = TYPE_MIMXRT685_HIFI4_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MIMXRT685HiFi4MachineState),
    .class_init = mimxrt685_hifi4_machine_class_init,
};

static void mimxrt685_hifi4_machine_register_types(void)
{
    type_register_static(&mimxrt685_hifi4_machine_info);
}

type_init(mimxrt685_hifi4_machine_register_types)
