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
#include "hw/core/loader.h"
#include "qemu/error-report.h"
#include "elf.h"

/*
 * The official MCUXpresso RAM linker configurations place the vector
 * table at the base of the RAM alias window (0x00080000 on real
 * silicon), not at address zero.  On hardware that image is started by
 * a debug probe or boot loader which programs the reset vector fetch
 * address; do the equivalent here by taking the initial vector base
 * from the loaded ELF's lowest load address.
 */
static void mimxrt685_evk_set_vector_base(MachineState *machine,
                                          ARMCPU *cpu)
{
    uint64_t low = 0, high = 0, entry = 0;

    if (!machine->kernel_filename) {
        return;
    }

    /*
     * Only used to discover the ELF load range; the authoritative load
     * and CPU reset wiring is done by armv7m_load_kernel().
     */
    if (load_elf(machine->kernel_filename, NULL, NULL, NULL, &entry,
                 &low, &high, NULL, ELFDATA2LSB, EM_ARM, 1, 0) < 0) {
        /* Not an ELF (raw binary): keep the SoC default vector base. */
        return;
    }

    object_property_set_uint(OBJECT(cpu), "init-svtor", low, &error_abort);
    object_property_set_uint(OBJECT(cpu), "init-nsvtor", low, &error_abort);
    info_report("mimxrt685-evk: vector base set to 0x%" PRIx64 " (entry 0x%"
                PRIx64 ")", low, entry);
}

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
    mimxrt685_evk_set_vector_base(machine, s->soc.armv7m.cpu);
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
