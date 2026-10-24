/*
 * NXP i.MX RT6xx I/O pin control
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RT6XX_IOPCTL_H
#define HW_GPIO_RT6XX_IOPCTL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_IOPCTL "rt6xx-iopctl"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxIopctlState, RT6XX_IOPCTL)

#define RT6XX_IOPCTL_NUM_REGS 258

struct Rt6xxIopctlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t pin[RT6XX_IOPCTL_NUM_REGS];
};

#endif
