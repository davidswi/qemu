/*
 * NXP MIMXRT685S SoC
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/imxrt685.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"

static const hwaddr flexcomm_base[8] = {
    0x40106000, 0x40107000, 0x40108000, 0x40109000,
    0x40122000, 0x40123000, 0x40124000, 0x40125000,
};

static const unsigned flexcomm_irq[8] = { 14, 15, 16, 17, 18, 19, 43, 44 };

static void imxrt685_init(Object *obj)
{
    IMXRT685State *s = IMXRT685_SOC(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "clkctl0", &s->clkctl[0],
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(obj, "clkctl1", &s->clkctl[1],
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(obj, "sysctl0", &s->sysctl[0],
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(obj, "sysctl1", &s->sysctl[1],
                            TYPE_RT6XX_SYSCON);
    object_initialize_child(obj, "rstctl0", &s->rstctl[0],
                            TYPE_RT6XX_RSTCTL);
    object_initialize_child(obj, "rstctl1", &s->rstctl[1],
                            TYPE_RT6XX_RSTCTL);
    object_initialize_child(obj, "iopctl", &s->iopctl, TYPE_RT6XX_IOPCTL);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_RT6XX_GPIO);
    for (unsigned i = 0; i < ARRAY_SIZE(s->flexcomm); i++) {
        g_autofree char *name = g_strdup_printf("flexcomm%u", i);

        object_initialize_child(obj, name, &s->flexcomm[i],
                                TYPE_RT6XX_FLEXCOMM);
    }
}

static bool imxrt685_realize_syscon(Rt6xxSysconState *s, unsigned variant,
                                    hwaddr base, Error **errp)
{
    DeviceState *dev = DEVICE(s);

    qdev_prop_set_uint32(dev, "variant", variant);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return true;
}

static bool imxrt685_realize_rstctl(Rt6xxRstctlState *s, unsigned variant,
                                    hwaddr base, Error **errp)
{
    DeviceState *dev = DEVICE(s);

    qdev_prop_set_uint32(dev, "variant", variant);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return true;
}

static void imxrt685_realize(DeviceState *dev, Error **errp)
{
    IMXRT685State *s = IMXRT685_SOC(dev);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m = DEVICE(&s->armv7m);

    if (!s->cpu_type) {
        error_setg(errp, "MIMXRT685S CPU type was not configured");
        return;
    }

    if (!memory_region_init_ram(&s->sram, OBJECT(s), "imxrt685.sram",
                                IMXRT685_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(system_memory, IMXRT685_SRAM_DATA_BASE,
                                &s->sram);
    memory_region_init_alias(&s->sram_code_alias, OBJECT(s),
                             "imxrt685.sram-code-alias", &s->sram, 0,
                             IMXRT685_SRAM_SIZE);
    memory_region_add_subregion(system_memory, IMXRT685_SRAM_CODE_BASE,
                                &s->sram_code_alias);

    if (!imxrt685_realize_syscon(&s->clkctl[0], RT6XX_CLKCTL0,
                                 IMXRT685_CLKCTL0_BASE, errp) ||
        !imxrt685_realize_syscon(&s->clkctl[1], RT6XX_CLKCTL1,
                                 IMXRT685_CLKCTL1_BASE, errp) ||
        !imxrt685_realize_syscon(&s->sysctl[0], RT6XX_SYSCTL0,
                                 IMXRT685_SYSCTL0_BASE, errp) ||
        !imxrt685_realize_syscon(&s->sysctl[1], RT6XX_SYSCTL1,
                                 IMXRT685_SYSCTL1_BASE, errp)) {
        return;
    }

    qdev_connect_gpio_out_named(DEVICE(&s->sysctl[0]), "ffro-enabled", 0,
        qdev_get_gpio_in_named(DEVICE(&s->clkctl[0]), "ffro-enable-in", 0));

    qdev_prop_set_uint32(armv7m, "num-irq", IMXRT685_NUM_IRQS);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", IMXRT685_NVIC_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type", s->cpu_type);
    qdev_prop_set_uint32(armv7m, "init-svtor", IMXRT685_SRAM_CODE_BASE);
    qdev_prop_set_uint32(armv7m, "init-nsvtor", IMXRT685_SRAM_CODE_BASE);
    qdev_connect_clock_in(armv7m, "cpuclk",
                          qdev_get_clock_out(DEVICE(&s->clkctl[0]), "cpuclk"));
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    if (!imxrt685_realize_rstctl(&s->rstctl[0], RT6XX_RSTCTL0,
                                 IMXRT685_RSTCTL0_BASE, errp) ||
        !imxrt685_realize_rstctl(&s->rstctl[1], RT6XX_RSTCTL1,
                                 IMXRT685_RSTCTL1_BASE, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->iopctl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iopctl), 0, IMXRT685_IOPCTL_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, IMXRT685_GPIO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), 0,
                       qdev_get_gpio_in(armv7m, 2));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), 1,
                       qdev_get_gpio_in(armv7m, 3));

    for (unsigned i = 0; i < ARRAY_SIZE(s->flexcomm); i++) {
        DeviceState *fc = DEVICE(&s->flexcomm[i]);

        qdev_prop_set_chr(fc, "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(fc), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(fc), 0, flexcomm_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(fc), 0,
                           qdev_get_gpio_in(armv7m, flexcomm_irq[i]));
        qdev_connect_gpio_out_named(DEVICE(&s->clkctl[1]),
                                    "flexcomm-clock", i,
                                    qdev_get_gpio_in_named(fc,
                                        "clock-enabled", 0));
        qdev_connect_gpio_out_named(DEVICE(&s->rstctl[1]),
                                    "flexcomm-reset", i,
                                    qdev_get_gpio_in_named(fc, "reset", 0));
        qdev_connect_gpio_out_named(DEVICE(&s->clkctl[1]), "gpio-clock", i,
                                    qdev_get_gpio_in_named(DEVICE(&s->gpio),
                                        "clock-enabled", i));
        qdev_connect_gpio_out_named(DEVICE(&s->rstctl[1]), "gpio-reset", i,
                                    qdev_get_gpio_in_named(DEVICE(&s->gpio),
                                        "reset", i));
    }
}

static const Property imxrt685_properties[] = {
    DEFINE_PROP_STRING("cpu-type", IMXRT685State, cpu_type),
};

static void imxrt685_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt685_realize;
    device_class_set_props(dc, imxrt685_properties);
}

static const TypeInfo imxrt685_info = {
    .name = TYPE_IMXRT685_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT685State),
    .instance_init = imxrt685_init,
    .class_init = imxrt685_class_init,
};

static void imxrt685_register_types(void)
{
    type_register_static(&imxrt685_info);
}

type_init(imxrt685_register_types)
