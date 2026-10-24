/*
 * NXP i.MX RT6xx Flexcomm USART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RT6XX_FLEXCOMM_H
#define HW_CHAR_RT6XX_FLEXCOMM_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_FLEXCOMM "rt6xx-flexcomm"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxFlexcommState, RT6XX_FLEXCOMM)

#define RT6XX_FLEXCOMM_FIFO_SIZE 16

struct Rt6xxFlexcommState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    uint32_t cfg;
    uint32_t ctl;
    uint32_t stat;
    uint32_t intenset;
    uint32_t brg;
    uint32_t osr;
    uint32_t addr;
    uint32_t fifocfg;
    uint32_t fifotrig;
    uint32_t fifointenset;
    uint32_t pselid;
    uint16_t rx_fifo[RT6XX_FLEXCOMM_FIFO_SIZE];
    uint8_t rx_head;
    uint8_t rx_count;
    bool clock_enabled;
    bool reset_asserted;
};

#endif
