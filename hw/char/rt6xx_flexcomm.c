/*
 * NXP i.MX RT6xx Flexcomm
 *
 * Register layout and reset values are from the project-local
 * MIMXRT685S_cm33 CMSIS-SVD.  Transmission is synchronous from QEMU's
 * point of view; the TX FIFO therefore remains empty/not-full.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/rt6xx_flexcomm.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RT6XX_FLEXCOMM_IOSIZE 0x1000

#define CFG_ENABLE          BIT(0)
#define CFG_MASK            0x00fddbfd
#define STAT_RESET          0x0000000a
#define STAT_MASK           0x0000045a
#define INTEN_MASK          0x0001f868
#define FIFOCFG_ENABLETX    BIT(0)
#define FIFOCFG_ENABLERX    BIT(1)
#define FIFOCFG_EMPTYTX     BIT(16)
#define FIFOCFG_EMPTYRX     BIT(17)
#define FIFOCFG_MASK        0x0007f033
#define FIFOINT_TXERR       BIT(0)
#define FIFOINT_RXERR       BIT(1)
#define FIFOINT_TXLVL       BIT(2)
#define FIFOINT_RXLVL       BIT(3)
#define FIFOINT_MASK        0x0f
#define PSELID_PERSEL_MASK  0x07
#define PSELID_LOCK         BIT(3)
#define PSELID_ID           0x00101000
/*
 * The shared FLEXCOMM SVD node masks the capability indicators to zero
 * (it models the I2S-only Flexcomm 11), but the NXP feature header and
 * the SDK instance tables establish the real contents: USART/SPI/I2C/
 * I2S in Flexcomm 0..7, SPI only in Flexcomm 14 and I2C only in
 * Flexcomm 15.  The read-only presence bits are derived from the
 * instance "capabilities" property so that the SDK's
 * FLEXCOMM_PeripheralIsPresent() (PSELID bit 3 + PERIPH, with I2S RX
 * sharing bit 7) succeeds for every block an instance really contains.
 * Bits 31..12 are the read-only FLEXCOMM_ID field.
 */

static Rt6xxFlexcommPeriph rt6xx_flexcomm_selected(Rt6xxFlexcommState *s)
{
    return s->pselid & PSELID_PERSEL_MASK;
}

static bool rt6xx_flexcomm_usart_selected(Rt6xxFlexcommState *s)
{
    return rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_USART;
}

static uint32_t rt6xx_flexcomm_caps_present(Rt6xxFlexcommState *s)
{
    uint32_t value = PSELID_ID;

    if (s->capabilities & RT6XX_FLEXCOMM_CAP_USART) {
        value |= BIT(4);
    }
    if (s->capabilities & RT6XX_FLEXCOMM_CAP_SPI) {
        value |= BIT(5);
    }
    if (s->capabilities & RT6XX_FLEXCOMM_CAP_I2C) {
        value |= BIT(6);
    }
    if (s->capabilities & RT6XX_FLEXCOMM_CAP_I2S) {
        value |= BIT(7);
    }
    return value;
}

static bool rt6xx_flexcomm_have_periph(Rt6xxFlexcommState *s,
                                       Rt6xxFlexcommPeriph periph)
{
    switch (periph) {
    case RT6XX_FLEXCOMM_PERIPH_NONE:
        return true;
    case RT6XX_FLEXCOMM_PERIPH_USART:
        return s->capabilities & RT6XX_FLEXCOMM_CAP_USART;
    case RT6XX_FLEXCOMM_PERIPH_SPI:
        return s->capabilities & RT6XX_FLEXCOMM_CAP_SPI;
    case RT6XX_FLEXCOMM_PERIPH_I2C:
        return s->capabilities & RT6XX_FLEXCOMM_CAP_I2C;
    default:
        return false;
    }
}



static bool rt6xx_flexcomm_active(Rt6xxFlexcommState *s)
{
    return s->clock_enabled && !s->reset_asserted &&
           rt6xx_flexcomm_usart_selected(s) &&
           (s->usart.cfg & CFG_ENABLE);
}

static uint32_t rt6xx_flexcomm_fifostat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommUsartState *u = &s->usart;
    uint32_t value = BIT(4) | BIT(5);

    if (u->rx_count) {
        value |= BIT(6);
    }
    if (u->rx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
        value |= BIT(7);
    }
    value |= (uint32_t)u->rx_count << 16;
    return value;
}

static uint32_t rt6xx_flexcomm_fifointstat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommUsartState *u = &s->usart;
    uint32_t status = 0;
    unsigned rx_trigger = extract32(u->fifotrig, 16, 4);

    if (u->fifotrig & BIT(0)) {
        status |= FIFOINT_TXLVL;
    }
    if ((u->fifotrig & BIT(1)) && u->rx_count > rx_trigger) {
        status |= FIFOINT_RXLVL;
    }
    return status;
}

static void rt6xx_flexcomm_update_irq(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommUsartState *u = &s->usart;
    uint32_t peripheral = u->stat & u->intenset & INTEN_MASK;
    uint32_t fifo = rt6xx_flexcomm_fifointstat(s) & u->fifointenset;

    /*
     * USART, I2C and SPI share one NVIC line per instance; only the
     * currently selected function may drive it.
     */
    qemu_set_irq(s->irq, rt6xx_flexcomm_active(s) && (peripheral || fifo));
}

static int rt6xx_flexcomm_can_receive(void *opaque)
{
    Rt6xxFlexcommState *s = opaque;

    return rt6xx_flexcomm_active(s) &&
           (s->usart.fifocfg & FIFOCFG_ENABLERX) &&
           s->usart.rx_count < RT6XX_FLEXCOMM_FIFO_SIZE;
}

static void rt6xx_flexcomm_receive(void *opaque, const uint8_t *buf, int size)
{
    Rt6xxFlexcommState *s = opaque;
    Rt6xxFlexcommUsartState *u = &s->usart;

    for (int i = 0; i < size && u->rx_count < RT6XX_FLEXCOMM_FIFO_SIZE; i++) {
        unsigned tail = (u->rx_head + u->rx_count) % RT6XX_FLEXCOMM_FIFO_SIZE;
        u->rx_fifo[tail] = buf[i];
        u->rx_count++;
    }
    rt6xx_flexcomm_update_irq(s);
}

static void rt6xx_flexcomm_reset_registers(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommUsartState *u = &s->usart;

    u->cfg = 0;
    u->ctl = 0;
    u->stat = STAT_RESET;
    u->intenset = 0;
    u->brg = 0;
    u->osr = 0xf;
    u->addr = 0;
    u->fifocfg = 0;
    u->fifotrig = 0;
    u->fifointenset = 0;
    u->rx_head = 0;
    u->rx_count = 0;
    s->pselid = rt6xx_flexcomm_caps_present(s);
}

static void rt6xx_flexcomm_set_clock(void *opaque, int n, int level)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);

    s->clock_enabled = level;
    rt6xx_flexcomm_update_irq(s);
    qemu_chr_fe_accept_input(&s->chr);
}

static void rt6xx_flexcomm_set_reset(void *opaque, int n, int level)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);

    if (level && !s->reset_asserted) {
        rt6xx_flexcomm_reset_registers(s);
    }
    s->reset_asserted = level;
    rt6xx_flexcomm_update_irq(s);
}

static bool rt6xx_flexcomm_check_access(Rt6xxFlexcommState *s, hwaddr offset)
{
    if (offset >= 0xff8 || rt6xx_flexcomm_usart_selected(s)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: USART access at 0x%03" HWADDR_PRIx
                  " while Flexcomm function is not USART\n",
                  TYPE_RT6XX_FLEXCOMM, offset);
    return false;
}

static uint64_t rt6xx_flexcomm_read(void *opaque, hwaddr offset, unsigned size)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);
    Rt6xxFlexcommUsartState *u = &s->usart;
    uint16_t value;

    if (!rt6xx_flexcomm_check_access(s, offset)) {
        return 0;
    }

    switch (offset) {
    case 0x000:
        return u->cfg;
    case 0x004:
        return u->ctl;
    case 0x008:
        return u->stat;
    case 0x00c:
        return u->intenset;
    case 0x010:
        return 0;
    case 0x020:
        return u->brg;
    case 0x024:
        return u->stat & u->intenset;
    case 0x028:
        return u->osr;
    case 0x02c:
        return u->addr;
    case 0xe00:
        return u->fifocfg;
    case 0xe04:
        return rt6xx_flexcomm_fifostat(s);
    case 0xe08:
        return u->fifotrig;
    case 0xe10:
        return u->fifointenset;
    case 0xe14:
        return 0;
    case 0xe18:
        return rt6xx_flexcomm_fifointstat(s);
    case 0xe30:
    case 0xe40:
        if (!u->rx_count) {
            return 0;
        }
        value = u->rx_fifo[u->rx_head];
        if (offset == 0xe30) {
            u->rx_head = (u->rx_head + 1) % RT6XX_FLEXCOMM_FIFO_SIZE;
            u->rx_count--;
            qemu_chr_fe_accept_input(&s->chr);
            rt6xx_flexcomm_update_irq(s);
        }
        return value;
    case 0xe48:
        return RT6XX_FLEXCOMM_FIFO_SIZE;
    case 0xff8:
        return s->pselid;
    case 0xffc:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_FLEXCOMM, offset);
        return 0;
    }
}

static void rt6xx_flexcomm_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);
    Rt6xxFlexcommUsartState *u = &s->usart;
    uint32_t val = value;
    uint8_t ch;

    if (!rt6xx_flexcomm_check_access(s, offset)) {
        return;
    }

    switch (offset) {
    case 0x000:
        u->cfg = val & CFG_MASK;
        break;
    case 0x004:
        u->ctl = val & 0x00010346;
        break;
    case 0x008:
        u->stat &= ~(val & STAT_MASK & ~STAT_RESET);
        break;
    case 0x00c:
        u->intenset |= val & INTEN_MASK;
        break;
    case 0x010:
        u->intenset &= ~(val & INTEN_MASK);
        break;
    case 0x020:
        u->brg = val & 0xffff;
        break;
    case 0x028:
        u->osr = val & 0xf;
        break;
    case 0x02c:
        u->addr = val & 0xff;
        break;
    case 0xe00:
        u->fifocfg = val & (FIFOCFG_MASK &
                            ~(FIFOCFG_EMPTYTX | FIFOCFG_EMPTYRX));
        if (val & FIFOCFG_EMPTYRX) {
            u->rx_head = 0;
            u->rx_count = 0;
        }
        break;
    case 0xe04:
        /* TXERR and RXERR are write-one-to-clear; neither is sticky here. */
        break;
    case 0xe08:
        u->fifotrig = val & 0x000f0f03;
        break;
    case 0xe10:
        u->fifointenset |= val & FIFOINT_MASK;
        break;
    case 0xe14:
        u->fifointenset &= ~(val & FIFOINT_MASK);
        break;
    case 0xe20:
        if (rt6xx_flexcomm_active(s) && (u->fifocfg & FIFOCFG_ENABLETX)) {
            ch = val;
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        break;
    case 0xff8:
        if (!(s->pselid & PSELID_LOCK)) {
            Rt6xxFlexcommPeriph sel = val & PSELID_PERSEL_MASK;

            if (!rt6xx_flexcomm_have_periph(s, sel)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: PSELID selects absent function %u\n",
                              TYPE_RT6XX_FLEXCOMM, sel);
                sel = rt6xx_flexcomm_selected(s);
            }
            s->pselid = (s->pselid & ~0x0f) | sel | (val & PSELID_LOCK);
        }
        break;
    case 0x024:
    case 0xe18:
    case 0xe30:
    case 0xe40:
    case 0xe48:
    case 0xffc:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%03"
                      HWADDR_PRIx " value 0x%08x\n",
                      TYPE_RT6XX_FLEXCOMM, offset, val);
        return;
    }
    rt6xx_flexcomm_update_irq(s);
    qemu_chr_fe_accept_input(&s->chr);
}

static const MemoryRegionOps rt6xx_flexcomm_ops = {
    .read = rt6xx_flexcomm_read,
    .write = rt6xx_flexcomm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_flexcomm_reset(DeviceState *dev)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(dev);

    rt6xx_flexcomm_reset_registers(s);
    s->clock_enabled = false;
    s->reset_asserted = true;
    rt6xx_flexcomm_update_irq(s);
}

static int rt6xx_flexcomm_post_load(void *opaque, int version_id)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);

    rt6xx_flexcomm_update_irq(s);
    qemu_chr_fe_accept_input(&s->chr);
    return 0;
}

static const VMStateDescription vmstate_rt6xx_flexcomm = {
    .name = TYPE_RT6XX_FLEXCOMM,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt6xx_flexcomm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(usart.cfg, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.ctl, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.stat, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.intenset, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.brg, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.osr, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.addr, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.fifocfg, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.fifotrig, Rt6xxFlexcommState),
        VMSTATE_UINT32(usart.fifointenset, Rt6xxFlexcommState),
        VMSTATE_UINT32(pselid, Rt6xxFlexcommState),
        VMSTATE_UINT16_ARRAY(usart.rx_fifo, Rt6xxFlexcommState,
                             RT6XX_FLEXCOMM_FIFO_SIZE),
        VMSTATE_UINT8(usart.rx_head, Rt6xxFlexcommState),
        VMSTATE_UINT8(usart.rx_count, Rt6xxFlexcommState),
        VMSTATE_BOOL(clock_enabled, Rt6xxFlexcommState),
        VMSTATE_BOOL(reset_asserted, Rt6xxFlexcommState),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_flexcomm_realize(DeviceState *dev, Error **errp)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(dev);

    qemu_chr_fe_set_handlers(&s->chr, rt6xx_flexcomm_can_receive,
                             rt6xx_flexcomm_receive, NULL, NULL,
                             s, NULL, true);
}

static void rt6xx_flexcomm_init(Object *obj)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_flexcomm_ops, s,
                          TYPE_RT6XX_FLEXCOMM, RT6XX_FLEXCOMM_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(dev, rt6xx_flexcomm_set_clock,
                            "clock-enabled", 1);
    qdev_init_gpio_in_named(dev, rt6xx_flexcomm_set_reset,
                            "reset", 1);
}

static const Property rt6xx_flexcomm_properties[] = {
    DEFINE_PROP_CHR("chardev", Rt6xxFlexcommState, chr),
    DEFINE_PROP_UINT32("capabilities", Rt6xxFlexcommState, capabilities,
                       RT6XX_FLEXCOMM_CAP_ALL),
};

static void rt6xx_flexcomm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rt6xx_flexcomm_realize;
    dc->vmsd = &vmstate_rt6xx_flexcomm;
    device_class_set_legacy_reset(dc, rt6xx_flexcomm_reset);
    device_class_set_props(dc, rt6xx_flexcomm_properties);
}

static const TypeInfo rt6xx_flexcomm_info = {
    .name = TYPE_RT6XX_FLEXCOMM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxFlexcommState),
    .instance_init = rt6xx_flexcomm_init,
    .class_init = rt6xx_flexcomm_class_init,
};

static void rt6xx_flexcomm_register_types(void)
{
    type_register_static(&rt6xx_flexcomm_info);
}

type_init(rt6xx_flexcomm_register_types)
