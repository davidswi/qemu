/*
 * NXP MIMXRT685-EVK board
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/imxrt685.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"

#define TYPE_MIMXRT685_EVK_MACHINE MACHINE_TYPE_NAME("mimxrt685-evk")
OBJECT_DECLARE_SIMPLE_TYPE(MIMXRT685EVKMachineState,
                           MIMXRT685_EVK_MACHINE)

struct MIMXRT685EVKMachineState {
    MachineState parent_obj;
    IMXRT685State soc;
};

static void mimxrt685_evk_init(MachineState *machine)
{
    MIMXRT685EVKMachineState *s = MIMXRT685_EVK_MACHINE(machine);

    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_IMXRT685_SOC);
    qdev_prop_set_string(DEVICE(&s->soc), "cpu-type", machine->cpu_type);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    armv7m_load_kernel(s->soc.armv7m.cpu, machine->kernel_filename,
                       IMXRT685_SRAM_CODE_BASE, IMXRT685_SRAM_SIZE);
}

static void mimxrt685_evk_machine_class_init(ObjectClass *oc,
                                              const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"),
        NULL,
    };

    mc->desc = "NXP MIMXRT685-EVK (Cortex-M33, local experimental model)";
    mc->init = mimxrt685_evk_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
}

static const TypeInfo mimxrt685_evk_machine_info = {
    .name = TYPE_MIMXRT685_EVK_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MIMXRT685EVKMachineState),
    .class_init = mimxrt685_evk_machine_class_init,
};

static void mimxrt685_evk_machine_register_types(void)
{
    type_register_static(&mimxrt685_evk_machine_info);
}

type_init(mimxrt685_evk_machine_register_types)
