/*
 * NXP i.MX RT6xx OS/Event timer
 *
 * Register definitions and reset values are derived from the project-local
 * MIMXRT685S CMSIS-SVD and MCUXpresso SDK.  The RT685 counter and match
 * registers are Gray encoded.
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/rt6xx_ostimer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define RT6XX_OSTIMER_IOSIZE 0x1000
#define RT6XX_OSTIMER_HZ     1000000ULL

#define EVTIMERL    0x00
#define EVTIMERH    0x04
#define CAPTURE_L   0x08
#define CAPTURE_H   0x0c
#define MATCH_L     0x10
#define MATCH_H     0x14
#define OSEVENT_CTRL 0x1c

#define CTRL_INTRFLAG BIT(0)
#define CTRL_INTENA   BIT(1)

static uint64_t rt6xx_ostimer_gray_to_binary(uint64_t gray)
{
    uint64_t binary = gray;

    for (unsigned shift = 1; shift < 64; shift <<= 1) {
        binary ^= binary >> shift;
    }
    return binary;
}

static uint64_t rt6xx_ostimer_binary_to_gray(uint64_t binary)
{
    return binary ^ (binary >> 1);
}

static bool rt6xx_ostimer_running(Rt6xxOSTimerState *s)
{
    return s->clock_enabled && !s->reset_asserted;
}

static uint64_t rt6xx_ostimer_count(Rt6xxOSTimerState *s)
{
    int64_t now;

    if (!rt6xx_ostimer_running(s)) {
        return s->count_base;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return s->count_base + (now - s->count_base_ns) /
                           (NANOSECONDS_PER_SECOND / RT6XX_OSTIMER_HZ);
}

static void rt6xx_ostimer_update_irq(Rt6xxOSTimerState *s)
{
    qemu_set_irq(s->irq, rt6xx_ostimer_running(s) &&
                 (s->control & CTRL_INTRFLAG) &&
                 (s->control & CTRL_INTENA));
}

static void rt6xx_ostimer_match(void *opaque)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);

    if (!rt6xx_ostimer_running(s) || !s->match_valid) {
        return;
    }
    s->control |= CTRL_INTRFLAG;
    rt6xx_ostimer_update_irq(s);
}

static void rt6xx_ostimer_schedule_match(Rt6xxOSTimerState *s)
{
    uint64_t count, target, delta, delta_ns;
    int64_t now;

    timer_del(s->match_timer);
    if (!rt6xx_ostimer_running(s) || !s->match_valid) {
        return;
    }

    count = rt6xx_ostimer_count(s);
    target = rt6xx_ostimer_gray_to_binary(s->match);
    if (target < count) {
        return;
    }
    if (target == count) {
        s->control |= CTRL_INTRFLAG;
        rt6xx_ostimer_update_irq(s);
        return;
    }

    delta = target - count;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (delta <= (INT64_MAX - now) /
                 (NANOSECONDS_PER_SECOND / RT6XX_OSTIMER_HZ)) {
        delta_ns = delta *
                   (NANOSECONDS_PER_SECOND / RT6XX_OSTIMER_HZ);
        timer_mod_ns(s->match_timer, now + delta_ns);
    }
}

static uint64_t rt6xx_ostimer_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);
    uint64_t value;

    switch (offset) {
    case EVTIMERL:
        value = rt6xx_ostimer_binary_to_gray(rt6xx_ostimer_count(s));
        return value & UINT32_MAX;
    case EVTIMERH:
        value = rt6xx_ostimer_binary_to_gray(rt6xx_ostimer_count(s));
        return value >> 32;
    case CAPTURE_L:
        return s->capture & UINT32_MAX;
    case CAPTURE_H:
        return s->capture >> 32;
    case MATCH_L:
        return s->match & UINT32_MAX;
    case MATCH_H:
        return s->match >> 32;
    case OSEVENT_CTRL:
        /* MATCH_WR_RDY is zero: synchronization completes immediately. */
        return s->control;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read at 0x%03" HWADDR_PRIx "\n",
                      TYPE_RT6XX_OSTIMER, offset);
        return 0;
    }
}

static void rt6xx_ostimer_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);
    uint32_t val = value;

    switch (offset) {
    case MATCH_L:
        s->match = deposit64(s->match, 0, 32, val);
        break;
    case MATCH_H:
        s->match = deposit64(s->match, 32, 32, val);
        /* The SDK writes low then high; commit the pair on the high write. */
        s->match_valid = true;
        rt6xx_ostimer_schedule_match(s);
        break;
    case OSEVENT_CTRL:
        if (val & CTRL_INTRFLAG) {
            s->control &= ~CTRL_INTRFLAG;
        }
        s->control = (s->control & ~CTRL_INTENA) | (val & CTRL_INTENA);
        rt6xx_ostimer_update_irq(s);
        break;
    case EVTIMERL:
    case EVTIMERH:
    case CAPTURE_L:
    case CAPTURE_H:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_OSTIMER, offset);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write at 0x%03" HWADDR_PRIx
                      " value 0x%08x\n", TYPE_RT6XX_OSTIMER, offset, val);
        break;
    }
}

static const MemoryRegionOps rt6xx_ostimer_ops = {
    .read = rt6xx_ostimer_read,
    .write = rt6xx_ostimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_ostimer_set_clock(void *opaque, int n, int level)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);

    level = !!level;
    if (s->clock_enabled == level) {
        return;
    }
    if (rt6xx_ostimer_running(s)) {
        s->count_base = rt6xx_ostimer_count(s);
    }
    s->clock_enabled = level;
    s->count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    rt6xx_ostimer_schedule_match(s);
    rt6xx_ostimer_update_irq(s);
}

static void rt6xx_ostimer_reset_state(Rt6xxOSTimerState *s)
{
    timer_del(s->match_timer);
    s->count_base = 0;
    s->count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->match = 0;
    s->capture = 0;
    s->control = 0;
    s->match_valid = false;
    rt6xx_ostimer_update_irq(s);
}

static void rt6xx_ostimer_set_reset(void *opaque, int n, int level)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);

    level = !!level;
    if (level && !s->reset_asserted) {
        s->reset_asserted = true;
        rt6xx_ostimer_reset_state(s);
    } else if (!level && s->reset_asserted) {
        s->reset_asserted = false;
        s->count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        rt6xx_ostimer_schedule_match(s);
        rt6xx_ostimer_update_irq(s);
    }
}

static void rt6xx_ostimer_reset(DeviceState *dev)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(dev);

    rt6xx_ostimer_reset_state(s);
}

static int rt6xx_ostimer_post_load(void *opaque, int version_id)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(opaque);

    rt6xx_ostimer_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rt6xx_ostimer = {
    .name = TYPE_RT6XX_OSTIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt6xx_ostimer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(count_base, Rt6xxOSTimerState),
        VMSTATE_INT64(count_base_ns, Rt6xxOSTimerState),
        VMSTATE_UINT64(match, Rt6xxOSTimerState),
        VMSTATE_UINT64(capture, Rt6xxOSTimerState),
        VMSTATE_UINT32(control, Rt6xxOSTimerState),
        VMSTATE_BOOL(match_valid, Rt6xxOSTimerState),
        VMSTATE_BOOL(clock_enabled, Rt6xxOSTimerState),
        VMSTATE_BOOL(reset_asserted, Rt6xxOSTimerState),
        VMSTATE_TIMER_PTR(match_timer, Rt6xxOSTimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_ostimer_init(Object *obj)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_ostimer_ops, s,
                          TYPE_RT6XX_OSTIMER, RT6XX_OSTIMER_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(dev, rt6xx_ostimer_set_clock,
                            "clock-enabled", 1);
    qdev_init_gpio_in_named(dev, rt6xx_ostimer_set_reset, "reset", 1);
    s->match_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  rt6xx_ostimer_match, s);
}

static void rt6xx_ostimer_finalize(Object *obj)
{
    Rt6xxOSTimerState *s = RT6XX_OSTIMER(obj);

    timer_free(s->match_timer);
}

static void rt6xx_ostimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_rt6xx_ostimer;
    device_class_set_legacy_reset(dc, rt6xx_ostimer_reset);
}

static const TypeInfo rt6xx_ostimer_info = {
    .name = TYPE_RT6XX_OSTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxOSTimerState),
    .instance_init = rt6xx_ostimer_init,
    .instance_finalize = rt6xx_ostimer_finalize,
    .class_init = rt6xx_ostimer_class_init,
};

static void rt6xx_ostimer_register_types(void)
{
    type_register_static(&rt6xx_ostimer_info);
}

type_init(rt6xx_ostimer_register_types)
