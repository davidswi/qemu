/*
 * NXP i.MX RT6xx Flexcomm USART
 *
 * Register layout and reset values are from the project-local
 * MIMXRT685S_cm33 CMSIS-SVD. Transmission is synchronous from QEMU's point
 * of view; the TX FIFO therefore remains empty/not-full.
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
#define PSELID_USART        1
#define PSELID_RESET        0x00101000

static bool rt6xx_flexcomm_usart_selected(Rt6xxFlexcommState *s)
{
    return (s->pselid & PSELID_PERSEL_MASK) == PSELID_USART;
}

static bool rt6xx_flexcomm_active(Rt6xxFlexcommState *s)
{
    return s->clock_enabled && !s->reset_asserted &&
           rt6xx_flexcomm_usart_selected(s) && (s->cfg & CFG_ENABLE);
}

static uint32_t rt6xx_flexcomm_fifostat(Rt6xxFlexcommState *s)
{
    uint32_t value = BIT(4) | BIT(5);

    if (s->rx_count) {
        value |= BIT(6);
    }
    if (s->rx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
        value |= BIT(7);
    }
    value |= (uint32_t)s->rx_count << 16;
    return value;
}

static uint32_t rt6xx_flexcomm_fifointstat(Rt6xxFlexcommState *s)
{
    uint32_t status = 0;
    unsigned rx_trigger = extract32(s->fifotrig, 16, 4);

    if (s->fifotrig & BIT(0)) {
        status |= FIFOINT_TXLVL;
    }
    if ((s->fifotrig & BIT(1)) && s->rx_count > rx_trigger) {
        status |= FIFOINT_RXLVL;
    }
    return status;
}

static void rt6xx_flexcomm_update_irq(Rt6xxFlexcommState *s)
{
    uint32_t peripheral = s->stat & s->intenset & INTEN_MASK;
    uint32_t fifo = rt6xx_flexcomm_fifointstat(s) & s->fifointenset;

    qemu_set_irq(s->irq, rt6xx_flexcomm_active(s) && (peripheral || fifo));
}

static int rt6xx_flexcomm_can_receive(void *opaque)
{
    Rt6xxFlexcommState *s = opaque;

    return rt6xx_flexcomm_active(s) &&
           (s->fifocfg & FIFOCFG_ENABLERX) &&
           s->rx_count < RT6XX_FLEXCOMM_FIFO_SIZE;
}

static void rt6xx_flexcomm_receive(void *opaque, const uint8_t *buf, int size)
{
    Rt6xxFlexcommState *s = opaque;

    for (int i = 0; i < size &&
         s->rx_count < RT6XX_FLEXCOMM_FIFO_SIZE; i++) {
        unsigned tail = (s->rx_head + s->rx_count) %
                        RT6XX_FLEXCOMM_FIFO_SIZE;
        s->rx_fifo[tail] = buf[i];
        s->rx_count++;
    }
    rt6xx_flexcomm_update_irq(s);
}

static void rt6xx_flexcomm_reset_registers(Rt6xxFlexcommState *s)
{
    s->cfg = 0;
    s->ctl = 0;
    s->stat = STAT_RESET;
    s->intenset = 0;
    s->brg = 0;
    s->osr = 0xf;
    s->addr = 0;
    s->fifocfg = 0;
    s->fifotrig = 0;
    s->fifointenset = 0;
    s->pselid = PSELID_RESET;
    s->rx_head = 0;
    s->rx_count = 0;
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
    uint16_t value;

    if (!rt6xx_flexcomm_check_access(s, offset)) {
        return 0;
    }

    switch (offset) {
    case 0x000:
        return s->cfg;
    case 0x004:
        return s->ctl;
    case 0x008:
        return s->stat;
    case 0x00c:
        return s->intenset;
    case 0x010:
        return 0;
    case 0x020:
        return s->brg;
    case 0x024:
        return s->stat & s->intenset;
    case 0x028:
        return s->osr;
    case 0x02c:
        return s->addr;
    case 0xe00:
        return s->fifocfg;
    case 0xe04:
        return rt6xx_flexcomm_fifostat(s);
    case 0xe08:
        return s->fifotrig;
    case 0xe10:
        return s->fifointenset;
    case 0xe14:
        return 0;
    case 0xe18:
        return rt6xx_flexcomm_fifointstat(s);
    case 0xe30:
    case 0xe40:
        if (!s->rx_count) {
            return 0;
        }
        value = s->rx_fifo[s->rx_head];
        if (offset == 0xe30) {
            s->rx_head = (s->rx_head + 1) % RT6XX_FLEXCOMM_FIFO_SIZE;
            s->rx_count--;
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
    uint32_t val = value;
    uint8_t ch;

    if (!rt6xx_flexcomm_check_access(s, offset)) {
        return;
    }

    switch (offset) {
    case 0x000:
        s->cfg = val & CFG_MASK;
        break;
    case 0x004:
        s->ctl = val & 0x00010346;
        break;
    case 0x008:
        s->stat &= ~(val & STAT_MASK & ~STAT_RESET);
        break;
    case 0x00c:
        s->intenset |= val & INTEN_MASK;
        break;
    case 0x010:
        s->intenset &= ~(val & INTEN_MASK);
        break;
    case 0x020:
        s->brg = val & 0xffff;
        break;
    case 0x028:
        s->osr = val & 0xf;
        break;
    case 0x02c:
        s->addr = val & 0xff;
        break;
    case 0xe00:
        s->fifocfg = val & (FIFOCFG_MASK &
                            ~(FIFOCFG_EMPTYTX | FIFOCFG_EMPTYRX));
        if (val & FIFOCFG_EMPTYRX) {
            s->rx_head = 0;
            s->rx_count = 0;
        }
        break;
    case 0xe04:
        /* TXERR and RXERR are write-one-to-clear; neither is sticky here. */
        break;
    case 0xe08:
        s->fifotrig = val & 0x000f0f03;
        break;
    case 0xe10:
        s->fifointenset |= val & FIFOINT_MASK;
        break;
    case 0xe14:
        s->fifointenset &= ~(val & FIFOINT_MASK);
        break;
    case 0xe20:
        if (rt6xx_flexcomm_active(s) &&
            (s->fifocfg & FIFOCFG_ENABLETX)) {
            ch = val;
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        break;
    case 0xff8:
        if (!(s->pselid & PSELID_LOCK)) {
            s->pselid = (s->pselid & ~0x0f) | (val & 0x0f);
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
        VMSTATE_UINT32(cfg, Rt6xxFlexcommState),
        VMSTATE_UINT32(ctl, Rt6xxFlexcommState),
        VMSTATE_UINT32(stat, Rt6xxFlexcommState),
        VMSTATE_UINT32(intenset, Rt6xxFlexcommState),
        VMSTATE_UINT32(brg, Rt6xxFlexcommState),
        VMSTATE_UINT32(osr, Rt6xxFlexcommState),
        VMSTATE_UINT32(addr, Rt6xxFlexcommState),
        VMSTATE_UINT32(fifocfg, Rt6xxFlexcommState),
        VMSTATE_UINT32(fifotrig, Rt6xxFlexcommState),
        VMSTATE_UINT32(fifointenset, Rt6xxFlexcommState),
        VMSTATE_UINT32(pselid, Rt6xxFlexcommState),
        VMSTATE_UINT16_ARRAY(rx_fifo, Rt6xxFlexcommState,
                             RT6XX_FLEXCOMM_FIFO_SIZE),
        VMSTATE_UINT8(rx_head, Rt6xxFlexcommState),
        VMSTATE_UINT8(rx_count, Rt6xxFlexcommState),
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
