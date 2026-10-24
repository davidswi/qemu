/*
 * NXP MIMXRT685S SoC
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_IMXRT685_H
#define HW_ARM_IMXRT685_H

#include "hw/arm/armv7m.h"
#include "hw/char/rt6xx_flexcomm.h"
#include "hw/gpio/rt6xx_gpio.h"
#include "hw/gpio/rt6xx_iopctl.h"
#include "hw/misc/rt6xx_rstctl.h"
#include "hw/misc/rt6xx_syscon.h"
#include "qom/object.h"

#define TYPE_IMXRT685_SOC "imxrt685-soc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT685State, IMXRT685_SOC)

#define IMXRT685_NUM_IRQS       60
#define IMXRT685_NVIC_PRIO_BITS 3

#define IMXRT685_SRAM_CODE_BASE 0x00000000ULL
#define IMXRT685_SRAM_DATA_BASE 0x20000000ULL
#define IMXRT685_SRAM_SIZE      0x00480000ULL

#define IMXRT685_ROM_BASE       0x03000000ULL
#define IMXRT685_ROM_ALIAS_BASE 0x13000000ULL
#define IMXRT685_ROM_SIZE       0x00020000ULL

#define IMXRT685_CLKCTL0_BASE   0x40001000ULL
#define IMXRT685_RSTCTL0_BASE   0x40000000ULL
#define IMXRT685_IOPCTL_BASE    0x40004000ULL
#define IMXRT685_SYSCTL0_BASE   0x40002000ULL
#define IMXRT685_RSTCTL1_BASE   0x40020000ULL
#define IMXRT685_CLKCTL1_BASE   0x40021000ULL
#define IMXRT685_SYSCTL1_BASE   0x40022000ULL
#define IMXRT685_GPIO_BASE      0x40100000ULL

struct IMXRT685State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    MemoryRegion sram;
    MemoryRegion sram_code_alias;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    Rt6xxSysconState clkctl[2];
    Rt6xxSysconState sysctl[2];
    Rt6xxRstctlState rstctl[2];
    Rt6xxIopctlState iopctl;
    Rt6xxGpioState gpio;
    Rt6xxFlexcommState flexcomm[10];
    char *cpu_type;
};

#endif
