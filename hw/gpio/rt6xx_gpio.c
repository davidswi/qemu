/*
 * NXP i.MX RT6xx GPIO
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/rt6xx_gpio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RT6XX_GPIO_IOSIZE 0x2800

static uint32_t rt6xx_gpio_pin_value(Rt6xxGpioState *s, unsigned port)
{
    return (s->output[port] & s->dir[port]) |
           (s->input[port] & ~s->dir[port]);
}

static void rt6xx_gpio_reset_port(Rt6xxGpioState *s, unsigned port)
{
    s->dir[port] = 0;
    s->mask[port] = 0;
    s->output[port] = 0;
    s->input[port] = 0;
    s->intena[port] = 0;
    s->intenb[port] = 0;
    s->intpol[port] = 0;
    s->intedg[port] = 0;
    s->intstata[port] = 0;
    s->intstatb[port] = 0;
}

static void rt6xx_gpio_update_outputs(Rt6xxGpioState *s, unsigned port)
{
    uint32_t value = rt6xx_gpio_pin_value(s, port);

    for (unsigned pin = 0; pin < RT6XX_GPIO_PINS; pin++) {
        qemu_set_irq(s->pins[port * RT6XX_GPIO_PINS + pin],
                     extract32(value, pin, 1));
    }
}

static void rt6xx_gpio_update_irq(Rt6xxGpioState *s)
{
    bool irqa = false;
    bool irqb = false;

    for (unsigned port = 0; port < RT6XX_GPIO_PORTS; port++) {
        if (!s->clock_enabled[port] || s->reset_asserted[port]) {
            continue;
        }
        uint32_t levels = rt6xx_gpio_pin_value(s, port);
        uint32_t level_hits = ~(levels ^ s->intpol[port]) & ~s->intedg[port];

        irqa |= (s->intstata[port] | level_hits) & s->intena[port];
        irqb |= (s->intstatb[port] | level_hits) & s->intenb[port];
    }
    qemu_set_irq(s->irq[0], irqa);
    qemu_set_irq(s->irq[1], irqb);
}

static void rt6xx_gpio_set_input(void *opaque, int line, int level)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);
    unsigned port = line / RT6XX_GPIO_PINS;
    unsigned pin = line % RT6XX_GPIO_PINS;
    uint32_t bit = BIT(pin);
    bool old_level = s->input[port] & bit;

    if (level) {
        s->input[port] |= bit;
    } else {
        s->input[port] &= ~bit;
    }

    if ((s->intedg[port] & bit) && old_level != level &&
        (!!(s->intpol[port] & bit) == !!level)) {
        s->intstata[port] |= bit;
        s->intstatb[port] |= bit;
    }
    rt6xx_gpio_update_outputs(s, port);
    rt6xx_gpio_update_irq(s);
}

static void rt6xx_gpio_set_clock(void *opaque, int line, int level)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);

    s->clock_enabled[line] = level;
    rt6xx_gpio_update_irq(s);
}

static void rt6xx_gpio_set_reset(void *opaque, int line, int level)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);

    if (level && !s->reset_asserted[line]) {
        rt6xx_gpio_reset_port(s, line);
        rt6xx_gpio_update_outputs(s, line);
    }
    s->reset_asserted[line] = level;
    rt6xx_gpio_update_irq(s);
}

static bool rt6xx_gpio_port_active(Rt6xxGpioState *s, unsigned port)
{
    return s->clock_enabled[port] && !s->reset_asserted[port];
}

static uint64_t rt6xx_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);
    unsigned port;
    unsigned pin;
    unsigned group;
    uint32_t value;

    if (offset < 0x100) {
        port = offset / 0x20;
        pin = offset % 0x20;
        if (size != 1 || !rt6xx_gpio_port_active(s, port)) {
            return 0;
        }
        return extract32(rt6xx_gpio_pin_value(s, port), pin, 1);
    }
    if (offset >= 0x1000 && offset < 0x1400 && !(offset & 3)) {
        port = (offset - 0x1000) / 0x80;
        pin = ((offset - 0x1000) % 0x80) / 4;
        if (size != 4 || !rt6xx_gpio_port_active(s, port)) {
            return 0;
        }
        return extract32(rt6xx_gpio_pin_value(s, port), pin, 1);
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid %u-byte read at 0x%04"
                      HWADDR_PRIx "\n", TYPE_RT6XX_GPIO, size, offset);
        return 0;
    }
    if (offset < 0x2000 || offset >= RT6XX_GPIO_IOSIZE ||
        ((offset - 0x2000) % 0x80) >= RT6XX_GPIO_PORTS * 4 ||
        (offset & 3)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%04"
                      HWADDR_PRIx "\n", TYPE_RT6XX_GPIO, offset);
        return 0;
    }
    group = (offset - 0x2000) / 0x80;
    port = ((offset - 0x2000) % 0x80) / 4;
    if (!rt6xx_gpio_port_active(s, port)) {
        return 0;
    }

    switch (group) {
    case 0:
        value = s->dir[port];
        break;
    case 1:
        value = s->mask[port];
        break;
    case 2:
        value = rt6xx_gpio_pin_value(s, port);
        break;
    case 3:
        value = rt6xx_gpio_pin_value(s, port) & ~s->mask[port];
        break;
    case 4:
        value = s->output[port];
        break;
    case 10:
        value = s->intena[port];
        break;
    case 11:
        value = s->intenb[port];
        break;
    case 12:
        value = s->intpol[port];
        break;
    case 13:
        value = s->intedg[port];
        break;
    case 14:
        value = s->intstata[port];
        break;
    case 15:
        value = s->intstatb[port];
        break;
    default:
        value = 0;
        break;
    }
    return value;
}

static void rt6xx_gpio_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);
    unsigned port;
    unsigned pin;
    unsigned group;
    uint32_t val = value;
    uint32_t bit;

    if (offset < 0x100) {
        port = offset / 0x20;
        pin = offset % 0x20;
        if (size != 1 || !rt6xx_gpio_port_active(s, port)) {
            return;
        }
        bit = BIT(pin);
        s->output[port] = (s->output[port] & ~bit) |
                          ((val & 1) << pin);
        rt6xx_gpio_update_outputs(s, port);
        return;
    }
    if (offset >= 0x1000 && offset < 0x1400 && !(offset & 3)) {
        port = (offset - 0x1000) / 0x80;
        pin = ((offset - 0x1000) % 0x80) / 4;
        if (size != 4 || !rt6xx_gpio_port_active(s, port)) {
            return;
        }
        bit = BIT(pin);
        s->output[port] = (s->output[port] & ~bit) |
                          ((val & 1) << pin);
        rt6xx_gpio_update_outputs(s, port);
        return;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid %u-byte write at 0x%04"
                      HWADDR_PRIx "\n", TYPE_RT6XX_GPIO, size, offset);
        return;
    }
    if (offset < 0x2000 || offset >= RT6XX_GPIO_IOSIZE ||
        ((offset - 0x2000) % 0x80) >= RT6XX_GPIO_PORTS * 4 ||
        (offset & 3)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%04"
                      HWADDR_PRIx " value 0x%08x\n",
                      TYPE_RT6XX_GPIO, offset, val);
        return;
    }
    group = (offset - 0x2000) / 0x80;
    port = ((offset - 0x2000) % 0x80) / 4;
    if (!rt6xx_gpio_port_active(s, port)) {
        return;
    }

    switch (group) {
    case 0:
        s->dir[port] = val;
        break;
    case 1:
        s->mask[port] = val;
        break;
    case 2:
        s->output[port] = val;
        break;
    case 3:
        s->output[port] = (s->output[port] & s->mask[port]) |
                          (val & ~s->mask[port]);
        break;
    case 4:
        s->output[port] |= val;
        break;
    case 5:
        s->output[port] &= ~val;
        break;
    case 6:
        s->output[port] ^= val;
        break;
    case 7:
        s->dir[port] |= val;
        break;
    case 8:
        s->dir[port] &= ~val;
        break;
    case 9:
        s->dir[port] ^= val;
        break;
    case 10:
        s->intena[port] = val;
        break;
    case 11:
        s->intenb[port] = val;
        break;
    case 12:
        s->intpol[port] = val;
        break;
    case 13:
        s->intedg[port] = val;
        break;
    case 14:
        s->intstata[port] &= ~val;
        break;
    case 15:
        s->intstatb[port] &= ~val;
        break;
    default:
        g_assert_not_reached();
    }
    rt6xx_gpio_update_outputs(s, port);
    rt6xx_gpio_update_irq(s);
}

static const MemoryRegionOps rt6xx_gpio_ops = {
    .read = rt6xx_gpio_read,
    .write = rt6xx_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_gpio_reset(DeviceState *dev)
{
    Rt6xxGpioState *s = RT6XX_GPIO(dev);

    for (unsigned port = 0; port < RT6XX_GPIO_PORTS; port++) {
        rt6xx_gpio_reset_port(s, port);
        s->clock_enabled[port] = false;
        s->reset_asserted[port] = true;
        rt6xx_gpio_update_outputs(s, port);
    }
    rt6xx_gpio_update_irq(s);
}

static int rt6xx_gpio_post_load(void *opaque, int version_id)
{
    Rt6xxGpioState *s = RT6XX_GPIO(opaque);

    for (unsigned port = 0; port < RT6XX_GPIO_PORTS; port++) {
        rt6xx_gpio_update_outputs(s, port);
    }
    rt6xx_gpio_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rt6xx_gpio = {
    .name = TYPE_RT6XX_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt6xx_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(dir, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(mask, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(output, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(input, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intena, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intenb, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intpol, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intedg, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intstata, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(intstatb, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_BOOL_ARRAY(clock_enabled, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_BOOL_ARRAY(reset_asserted, Rt6xxGpioState, RT6XX_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_gpio_init(Object *obj)
{
    Rt6xxGpioState *s = RT6XX_GPIO(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_gpio_ops, s,
                          TYPE_RT6XX_GPIO, RT6XX_GPIO_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[1]);
    qdev_init_gpio_in(dev, rt6xx_gpio_set_input, RT6XX_GPIO_LINES);
    qdev_init_gpio_out(dev, s->pins, RT6XX_GPIO_LINES);
    qdev_init_gpio_in_named(dev, rt6xx_gpio_set_clock, "clock-enabled",
                            RT6XX_GPIO_PORTS);
    qdev_init_gpio_in_named(dev, rt6xx_gpio_set_reset, "reset",
                            RT6XX_GPIO_PORTS);
}

static void rt6xx_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_rt6xx_gpio;
    device_class_set_legacy_reset(dc, rt6xx_gpio_reset);
}

static const TypeInfo rt6xx_gpio_info = {
    .name = TYPE_RT6XX_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxGpioState),
    .instance_init = rt6xx_gpio_init,
    .class_init = rt6xx_gpio_class_init,
};

static void rt6xx_gpio_register_types(void)
{
    type_register_static(&rt6xx_gpio_info);
}

type_init(rt6xx_gpio_register_types)
