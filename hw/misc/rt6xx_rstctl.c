/*
 * NXP i.MX RT6xx reset controller
 *
 * Register layout and reset values are from the project-local
 * MIMXRT685S_cm33 CMSIS-SVD.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rt6xx_rstctl.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RT6XX_RSTCTL_IOSIZE 0x80

/*
 * RSTCTL1.PRSTCTL0 resets the Flexcomm 0..7 blocks in bits 8..15 and
 * the dedicated Flexcomm 14/15 blocks in bits 22/23.  Output index is
 * the logical SoC index (Flexcomm 0..7, 14, 15 in SDK instance order).
 */
static const uint8_t rt6xx_flexcomm_ctl_bit[] = {
    8, 9, 10, 11, 12, 13, 14, 15, 22, 23,
};

static const uint32_t reset_values[2][3] = {
    [RT6XX_RSTCTL0] = { 0x01f11f02, 0x0101810c, 0x1c000001 },
    [RT6XX_RSTCTL1] = { 0x01c0ff00, 0xb18100ff, 0xc001011f },
};

static const uint32_t write_masks[2][3] = {
    [RT6XX_RSTCTL0] = { 0x01f11f02, 0xffffffff, 0x1c000003 },
    [RT6XX_RSTCTL1] = { 0x09c0ff00, 0xb18100ff, 0xc001051f },
};

static void rt6xx_rstctl_update_outputs(Rt6xxRstctlState *s)
{
    if (s->variant != RT6XX_RSTCTL1) {
        return;
    }

    for (unsigned i = 0; i < ARRAY_SIZE(s->flexcomm_reset); i++) {
        qemu_set_irq(s->flexcomm_reset[i],
                     extract32(s->prstctl[0], rt6xx_flexcomm_ctl_bit[i], 1));
    }
    qemu_set_irq(s->ostimer_reset, extract32(s->prstctl[0], 27, 1));
    for (unsigned i = 0; i < ARRAY_SIZE(s->gpio_reset); i++) {
        qemu_set_irq(s->gpio_reset[i], extract32(s->prstctl[1], i, 1));
    }
}

static uint64_t rt6xx_rstctl_read(void *opaque, hwaddr offset, unsigned size)
{
    Rt6xxRstctlState *s = RT6XX_RSTCTL(opaque);

    switch (offset) {
    case 0x00:
        return s->sysrststat;
    case 0x10:
    case 0x14:
    case 0x18:
        return s->prstctl[(offset - 0x10) / 4];
    case 0x40:
    case 0x44:
    case 0x48:
    case 0x70:
    case 0x74:
    case 0x78:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%02"
                      HWADDR_PRIx "\n", TYPE_RT6XX_RSTCTL, offset);
        return 0;
    }
}

static void rt6xx_rstctl_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    Rt6xxRstctlState *s = RT6XX_RSTCTL(opaque);
    unsigned index;
    uint32_t val = value;

    switch (offset) {
    case 0x00:
        /* SYSRSTSTAT is write-one-to-clear. POR is reasserted on reset. */
        s->sysrststat &= ~(val & 0xf1);
        break;
    case 0x10:
    case 0x14:
    case 0x18:
        index = (offset - 0x10) / 4;
        s->prstctl[index] =
            (s->prstctl[index] & ~write_masks[s->variant][index]) |
                            (val & write_masks[s->variant][index]);
        break;
    case 0x40:
    case 0x44:
    case 0x48:
        index = (offset - 0x40) / 4;
        s->prstctl[index] |= val & write_masks[s->variant][index];
        break;
    case 0x70:
    case 0x74:
    case 0x78:
        index = (offset - 0x70) / 4;
        s->prstctl[index] &= ~(val & write_masks[s->variant][index]);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%02"
                      HWADDR_PRIx " value 0x%08x\n",
                      TYPE_RT6XX_RSTCTL, offset, val);
        return;
    }

    rt6xx_rstctl_update_outputs(s);
}

static const MemoryRegionOps rt6xx_rstctl_ops = {
    .read = rt6xx_rstctl_read,
    .write = rt6xx_rstctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_rstctl_reset(DeviceState *dev)
{
    Rt6xxRstctlState *s = RT6XX_RSTCTL(dev);

    s->sysrststat = 1;
    memcpy(s->prstctl, reset_values[s->variant], sizeof(s->prstctl));
    rt6xx_rstctl_update_outputs(s);
}

static int rt6xx_rstctl_post_load(void *opaque, int version_id)
{
    rt6xx_rstctl_update_outputs(RT6XX_RSTCTL(opaque));
    return 0;
}

static const VMStateDescription vmstate_rt6xx_rstctl = {
    .name = TYPE_RT6XX_RSTCTL,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt6xx_rstctl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sysrststat, Rt6xxRstctlState),
        VMSTATE_UINT32_ARRAY(prstctl, Rt6xxRstctlState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_rstctl_init(Object *obj)
{
    Rt6xxRstctlState *s = RT6XX_RSTCTL(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_rstctl_ops, s,
                          TYPE_RT6XX_RSTCTL, RT6XX_RSTCTL_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(dev, s->flexcomm_reset, "flexcomm-reset",
                             ARRAY_SIZE(s->flexcomm_reset));
    qdev_init_gpio_out_named(dev, s->gpio_reset, "gpio-reset", 8);
    qdev_init_gpio_out_named(dev, &s->ostimer_reset, "ostimer-reset", 1);
}

static const Property rt6xx_rstctl_properties[] = {
    DEFINE_PROP_UINT32("variant", Rt6xxRstctlState, variant, RT6XX_RSTCTL0),
};

static void rt6xx_rstctl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_rt6xx_rstctl;
    device_class_set_legacy_reset(dc, rt6xx_rstctl_reset);
    device_class_set_props(dc, rt6xx_rstctl_properties);
}

static const TypeInfo rt6xx_rstctl_info = {
    .name = TYPE_RT6XX_RSTCTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxRstctlState),
    .instance_init = rt6xx_rstctl_init,
    .class_init = rt6xx_rstctl_class_init,
};

static void rt6xx_rstctl_register_types(void)
{
    type_register_static(&rt6xx_rstctl_info);
}

type_init(rt6xx_rstctl_register_types)
