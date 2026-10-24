/*
 * NXP i.MX RT6xx clock and system control blocks
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RT6XX_SYSCON_H
#define HW_MISC_RT6XX_SYSCON_H

#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_SYSCON "rt6xx-syscon"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxSysconState, RT6XX_SYSCON)

typedef enum Rt6xxSysconVariant {
    RT6XX_CLKCTL0,
    RT6XX_CLKCTL1,
    RT6XX_SYSCTL0,
    RT6XX_SYSCTL1,
} Rt6xxSysconVariant;

#define RT6XX_SYSCON_MAX_REGS 86

struct Rt6xxSysconState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[RT6XX_SYSCON_MAX_REGS];
    uint32_t variant;
    bool ffro_enabled;
    qemu_irq ffro_enable_out;
    qemu_irq flexcomm_clock[10];
    qemu_irq gpio_clock[8];
    Clock *cpuclk;
};

#endif
