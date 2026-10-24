/*
 * NXP i.MX RT6xx reset controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RT6XX_RSTCTL_H
#define HW_MISC_RT6XX_RSTCTL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_RSTCTL "rt6xx-rstctl"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxRstctlState, RT6XX_RSTCTL)

typedef enum Rt6xxRstctlVariant {
    RT6XX_RSTCTL0,
    RT6XX_RSTCTL1,
} Rt6xxRstctlVariant;

struct Rt6xxRstctlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t sysrststat;
    uint32_t prstctl[3];
    uint32_t variant;
    qemu_irq flexcomm_reset[8];
    qemu_irq gpio_reset[8];
};

#endif
