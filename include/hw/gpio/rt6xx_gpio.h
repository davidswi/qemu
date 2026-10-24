/*
 * NXP i.MX RT6xx GPIO
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RT6XX_GPIO_H
#define HW_GPIO_RT6XX_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_GPIO "rt6xx-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxGpioState, RT6XX_GPIO)

#define RT6XX_GPIO_PORTS 8
#define RT6XX_GPIO_PINS  32
#define RT6XX_GPIO_LINES (RT6XX_GPIO_PORTS * RT6XX_GPIO_PINS)

struct Rt6xxGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t dir[RT6XX_GPIO_PORTS];
    uint32_t mask[RT6XX_GPIO_PORTS];
    uint32_t output[RT6XX_GPIO_PORTS];
    uint32_t input[RT6XX_GPIO_PORTS];
    uint32_t intena[RT6XX_GPIO_PORTS];
    uint32_t intenb[RT6XX_GPIO_PORTS];
    uint32_t intpol[RT6XX_GPIO_PORTS];
    uint32_t intedg[RT6XX_GPIO_PORTS];
    uint32_t intstata[RT6XX_GPIO_PORTS];
    uint32_t intstatb[RT6XX_GPIO_PORTS];
    bool clock_enabled[RT6XX_GPIO_PORTS];
    bool reset_asserted[RT6XX_GPIO_PORTS];
    qemu_irq pins[RT6XX_GPIO_LINES];
    qemu_irq irq[2];
};

#endif
