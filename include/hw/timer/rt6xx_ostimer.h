/*
 * NXP i.MX RT6xx OS/Event timer
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RT6XX_OSTIMER_H
#define HW_TIMER_RT6XX_OSTIMER_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RT6XX_OSTIMER "rt6xx-ostimer"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxOSTimerState, RT6XX_OSTIMER)

struct Rt6xxOSTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *match_timer;

    uint64_t count_base;
    int64_t count_base_ns;
    uint64_t match;
    uint64_t capture;
    uint32_t control;
    bool match_valid;
    bool clock_enabled;
    bool reset_asserted;
};

#endif
