/*
 * NXP i.MX RT6xx I/O pin control
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/rt6xx_iopctl.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RT6XX_IOPCTL_IOSIZE 0x408
#define RT6XX_IOPCTL_MASK   0x00000fff

static bool rt6xx_iopctl_valid_offset(hwaddr offset)
{
    return offset < RT6XX_IOPCTL_IOSIZE && !(offset & 3);
}

static uint64_t rt6xx_iopctl_read(void *opaque, hwaddr offset, unsigned size)
{
    Rt6xxIopctlState *s = RT6XX_IOPCTL(opaque);

    if (!rt6xx_iopctl_valid_offset(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid read at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_IOPCTL, offset);
        return 0;
    }
    return s->pin[offset / 4];
}

static void rt6xx_iopctl_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    Rt6xxIopctlState *s = RT6XX_IOPCTL(opaque);

    if (!rt6xx_iopctl_valid_offset(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid write at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_IOPCTL, offset);
        return;
    }
    s->pin[offset / 4] = value & RT6XX_IOPCTL_MASK;
}

static const MemoryRegionOps rt6xx_iopctl_ops = {
    .read = rt6xx_iopctl_read,
    .write = rt6xx_iopctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_iopctl_reset(DeviceState *dev)
{
    Rt6xxIopctlState *s = RT6XX_IOPCTL(dev);

    memset(s->pin, 0, sizeof(s->pin));
    s->pin[(2 * 32 + 25)] = 0x41;
    s->pin[(2 * 32 + 26)] = 0x41;
}

static const VMStateDescription vmstate_rt6xx_iopctl = {
    .name = TYPE_RT6XX_IOPCTL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pin, Rt6xxIopctlState,
                             RT6XX_IOPCTL_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_iopctl_init(Object *obj)
{
    Rt6xxIopctlState *s = RT6XX_IOPCTL(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_iopctl_ops, s,
                          TYPE_RT6XX_IOPCTL, RT6XX_IOPCTL_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void rt6xx_iopctl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_rt6xx_iopctl;
    device_class_set_legacy_reset(dc, rt6xx_iopctl_reset);
}

static const TypeInfo rt6xx_iopctl_info = {
    .name = TYPE_RT6XX_IOPCTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxIopctlState),
    .instance_init = rt6xx_iopctl_init,
    .class_init = rt6xx_iopctl_class_init,
};

static void rt6xx_iopctl_register_types(void)
{
    type_register_static(&rt6xx_iopctl_info);
}

type_init(rt6xx_iopctl_register_types)
