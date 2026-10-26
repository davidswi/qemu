/*
 * NXP MIMXRT685S SoC
 *
 * This model is intended for local hardware enablement experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/imxrt685.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"

/*
 * The RT685S integrates ten Flexcomm blocks.  Entries are in SDK
 * instance order; the array index is the logical index used for the
 * CLKCTL1/RSTCTL1 control outputs (PSCCTL1/PRSTCTL1 bits 8..15,
 * 22 and 23), while hwnum is the hardware Flexcomm number used for
 * naming and USART chardev assignment.
 */
static const struct Rt6xxFlexcommDesc {
    unsigned hwnum;
    hwaddr base;
    unsigned irq;
    uint32_t caps;
} flexcomm_desc[] = {
    {  0, 0x40106000, 14, RT6XX_FLEXCOMM_CAP_ALL },
    {  1, 0x40107000, 15, RT6XX_FLEXCOMM_CAP_ALL },
    {  2, 0x40108000, 16, RT6XX_FLEXCOMM_CAP_ALL },
    {  3, 0x40109000, 17, RT6XX_FLEXCOMM_CAP_ALL },
    {  4, 0x40122000, 18, RT6XX_FLEXCOMM_CAP_ALL },
    {  5, 0x40123000, 19, RT6XX_FLEXCOMM_CAP_ALL },
    {  6, 0x40124000, 43, RT6XX_FLEXCOMM_CAP_ALL },
    {  7, 0x40125000, 44, RT6XX_FLEXCOMM_CAP_ALL },
    { 14, 0x40126000, 20, RT6XX_FLEXCOMM_CAP_SPI },
    { 15, 0x40127000, 21, RT6XX_FLEXCOMM_CAP_I2C },
};

static_assert(ARRAY_SIZE(flexcomm_desc) ==
              sizeof_field(IMXRT685State, flexcomm) /
              sizeof_field(IMXRT685State, flexcomm[0]),
              "Flexcomm descriptor table and SoC state must match");

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
    object_initialize_child(obj, "ostimer", &s->ostimer,
                            TYPE_RT6XX_OSTIMER);
    for (unsigned i = 0; i < ARRAY_SIZE(flexcomm_desc); i++) {
        g_autofree char *name =
            g_strdup_printf("flexcomm%u", flexcomm_desc[i].hwnum);

        object_initialize_child(obj, name, &s->flexcomm[i],
                                TYPE_RT6XX_FLEXCOMM);
    }
    object_initialize_child(obj, "audio-link", &s->audio_link,
                            TYPE_RT685_AUDIO_LINK);
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

/*
 * The RT685S boot ROM is proprietary mask ROM.  The MCUXpresso SDK
 * clock driver calls three of its documented OTP API entry points
 * (see FSL_ROM_OTP_* in MIMXRT685S_cm33_COMMON.h):
 *
 *   void otp_init(uint32_t src_clk_freq)                  @ +0x9ff8
 *   void otp_deinit(void)                                 @ +0xa046
 *   void otp_fuse_read(uint32_t fuse_idx, uint32_t *out)  @ +0xa056
 *
 * Model them with ABI-compatible code returning "unprogrammed" (zero)
 * OTP contents, which is the reset state of the OTP controller model.
 * Every other ROM address executes BKPT #0 so that a call into an
 * unmodeled ROM entry faults visibly instead of running zeroes.
 */
static void imxrt685_populate_rom(void)
{
    g_autofree uint16_t *image = g_new(uint16_t, IMXRT685_ROM_SIZE / 2);
    static const struct {
        hwaddr offset;
        uint16_t code[3];
        unsigned len;
    } entries[] = {
        { 0x9ff8, { GUINT16_TO_LE(0x4770) }, 1 },         /* bx lr */
        { 0xa046, { GUINT16_TO_LE(0x4770) }, 1 },         /* bx lr */
        { 0xa056, { GUINT16_TO_LE(0x2200),   /* movs r2, #0 */
                    GUINT16_TO_LE(0x600A),   /* str r2, [r1] */
                    GUINT16_TO_LE(0x4770) }, /* bx lr */
          3 },
    };

    for (unsigned i = 0; i < IMXRT685_ROM_SIZE / 2; i++) {
        image[i] = GUINT16_TO_LE(0xbe00); /* bkpt #0 */
    }
    for (unsigned e = 0; e < ARRAY_SIZE(entries); e++) {
        for (unsigned j = 0; j < entries[e].len; j++) {
            image[entries[e].offset / 2 + j] = entries[e].code[j];
        }
    }

    rom_add_blob_fixed("imxrt685.rom", image, IMXRT685_ROM_SIZE,
                       IMXRT685_ROM_BASE);
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

    if (!memory_region_init_rom(&s->rom, OBJECT(s), "imxrt685.rom",
                                IMXRT685_ROM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(system_memory, IMXRT685_ROM_BASE, &s->rom);
    memory_region_init_alias(&s->rom_alias, OBJECT(s), "imxrt685.rom-alias",
                             &s->rom, 0, IMXRT685_ROM_SIZE);
    memory_region_add_subregion(system_memory, IMXRT685_ROM_ALIAS_BASE,
                                &s->rom_alias);
    imxrt685_populate_rom();

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

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ostimer), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ostimer), 0,
                    IMXRT685_OSTIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ostimer), 0,
                       qdev_get_gpio_in(armv7m, 41));
    qdev_connect_gpio_out_named(DEVICE(&s->clkctl[1]), "ostimer-clock", 0,
                                qdev_get_gpio_in_named(
                                    DEVICE(&s->ostimer),
                                    "clock-enabled", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->rstctl[1]), "ostimer-reset", 0,
                                qdev_get_gpio_in_named(
                                    DEVICE(&s->ostimer), "reset", 0));

    /*
     * EVK digital audio wiring: Flexcomm 1 (I2S RX, shared SCK/WS
     * clock master) and Flexcomm 3 (I2S TX, clock slave) exchange
     * frames through the on-board WM8904 under a single board-owned
     * 48 kHz cadence.  The codec control plane uses the SDK I2C
     * fallback on Flexcomm 4 at address 0x1a; the RT685 I3C
     * controller is not modeled here.
     */
    {
        Object *link = OBJECT(&s->audio_link);

        s->wm8904 = i2c_slave_new(TYPE_WM8904, 0x1a);
        /*
         * Forward audiodev to the codec.  Priority: explicit property >
         * first named backend in the global queue (e.g. "rt685" from
         * -audiodev wav,id=rt685,...).
         */
        if (s->audiodev) {
            object_property_set_str(OBJECT(s->wm8904), "audiodev",
                                    s->audiodev, &error_abort);
        } else {
            /*
             * No explicit property: try to get the first available
             * backend by name from the global audiodevs queue.
             */
            Error *local_err = NULL;
            AudioBackend *be = audio_be_by_name("rt685", &local_err);

            if (be) {
                object_property_set_str(OBJECT(s->wm8904), "audiodev",
                                        audio_be_get_id(be), &error_abort);
            }
        }
        object_property_set_link(link, "rx", OBJECT(&s->flexcomm[1]),
                                 &error_abort);
        object_property_set_link(link, "tx", OBJECT(&s->flexcomm[3]),
                                 &error_abort);
        /* Set before realize: the link property is realize-locked. */
        object_property_set_link(link, "codec", OBJECT(s->wm8904),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(link), errp)) {
            return;
        }
    }

    for (unsigned i = 0; i < ARRAY_SIZE(flexcomm_desc); i++) {
        const struct Rt6xxFlexcommDesc *d = &flexcomm_desc[i];
        DeviceState *fc = DEVICE(&s->flexcomm[i]);

        if (i == 1 || i == 3) {
            object_property_set_link(OBJECT(fc), "audio-link",
                                     OBJECT(&s->audio_link),
                                     &error_abort);
        }
        /* Only Flexcomm 0..7 contain a USART; do not consume serial
         * backends for the SPI-only and I2C-only instances. */
        if (d->caps & RT6XX_FLEXCOMM_CAP_USART) {
            qdev_prop_set_chr(fc, "chardev", serial_hd(d->hwnum));
        }
        qdev_prop_set_uint32(fc, "capabilities", d->caps);
        if (!sysbus_realize(SYS_BUS_DEVICE(fc), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(fc), 0, d->base);
        sysbus_connect_irq(SYS_BUS_DEVICE(fc), 0,
                           qdev_get_gpio_in(armv7m, d->irq));
        qdev_connect_gpio_out_named(DEVICE(&s->clkctl[1]),
                                    "flexcomm-clock", i,
                                    qdev_get_gpio_in_named(fc,
                                        "clock-enabled", 0));
        qdev_connect_gpio_out_named(DEVICE(&s->rstctl[1]),
                                    "flexcomm-reset", i,
                                    qdev_get_gpio_in_named(fc, "reset", 0));
    }

    for (unsigned i = 0; i < 8; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->clkctl[1]), "gpio-clock", i,
                                    qdev_get_gpio_in_named(DEVICE(&s->gpio),
                                        "clock-enabled", i));
        qdev_connect_gpio_out_named(DEVICE(&s->rstctl[1]), "gpio-reset", i,
                                    qdev_get_gpio_in_named(DEVICE(&s->gpio),
                                        "reset", i));
    }

    if (!i2c_slave_realize_and_unref(s->wm8904, s->flexcomm[4].i2c_bus,
                                     errp)) {
        return;
    }
}

static const Property imxrt685_properties[] = {
    DEFINE_PROP_STRING("audiodev", IMXRT685State, audiodev),
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
