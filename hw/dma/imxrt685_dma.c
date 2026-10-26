/*
 * NXP MIMXRT685 DMA Controller (SDK-compatible lightweight model)
 *
 * Models the subset of registers that the MCUXpresso SDK's fsl_dma.c /
 * fsl_i2s_dma.c driver accesses for I2S audio transfers.  This is NOT eDMA;
 * it is NXP's older peripheral DMA controller found at base address 0x40104000
 * (non-secure) / 0x50104000 (secure).
 *
 * Register layout per channel n:
 *   CFG     at offset 0x400 + n*0x10 — PERIPHREQEN(0), HWTRIGEN(1)
 *   XFERCFG at offset 0x408 + n*0x10 — transfer count (bits 0:9), SWTRIG(12)
 *
 * This is a local hardware-enablement device and must not be submitted upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "trace.h"

/* Per-channel register stride. */
#define DMA_CHN_STRIDE   0x10

/* Channel CFG offsets within the MMIO window. */
#define DMA_CHN_CFG       0x400
#define DMA_CHN_XFERCFG   0x408

/* CTRL bits. */
#define DMA_CTRL_ENABLE   BIT(0)

/* XFERCFG bits — SDK uses these for transfer count and software trigger. */
#define DMA_XFER_COUNT    0x3FFU          /* bits 0:9 - number of transfers */
#define DMA_XFER_SWTRIG   BIT(12)         /* bit 12 - software trigger */

/* CFG bits. */
#define DMA_CFG_PERIPHREQEN  BIT(0)      /* hardware request enabled */
#define DMA_CFG_HWTRIGEN     BIT(1)      /* hardware triggering enabled */

/* Channel count (from CMSIS header). */
#define DMA_CHANNEL_COUNT   33

/* Per-channel state. */
typedef struct Imxrt685DmaChn {
    uint32_t cfg;          /* Channel configuration        */
    uint32_t xfercfg;      /* Transfer configuration       */
} Imxrt685DmaChn;

#define TYPE_IMXRT685_DMA "imxrt685-dma"
OBJECT_DECLARE_SIMPLE_TYPE(Imxrt685DmaState, IMXRT685_DMA)

struct Imxrt685DmaState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;     /* DMA register space             */
    qemu_irq irq;          /* Completion interrupt to NVIC   */
    QEMUTimer *timer[DMA_CHANNEL_COUNT];  /* Per-channel timer for cadence */

    uint32_t ctrl;         /* CTRL register (enable bit)     */
    hwaddr srambase;       /* SRAMBASE (descriptor table addr) */

    Imxrt685DmaChn chn[DMA_CHANNEL_COUNT];  /* Per-channel state */

    uint32_t sample_rate;  /* Audio cadence for DREQ timing  */
};

/* Handle a software trigger on the given channel. */
static void dma_sw_trigger(Imxrt685DmaState *s, unsigned ch)
{
    Imxrt685DmaChn *chn = &s->chn[ch];

    if (!(chn->cfg & DMA_CFG_PERIPHREQEN)) {
        return;  /* Only hardware-triggered channels matter for I2S */
    }

    trace_dma_sw_trigger(ch);
}

/* Handle hardware/peripheral request on channel. */
static void dma_periph_request(Imxrt685DmaState *s, unsigned ch)
{
    Imxrt685DmaChn *chn = &s->chn[ch];
    uint32_t count;

    if (!(chn->cfg & DMA_CFG_HWTRIGEN)) {
        return;  /* Hardware triggering disabled for this channel */
    }
    if (chn->xfercfg == 0) {
        return;  /* No transfer configured */
    }

    trace_dma_periph_request(ch, chn->xfercfg);

    /* Read the transfer count from XFERCFG. */
    count = chn->xfercfg & DMA_XFER_COUNT;
    if (count == 0) {
        return;
    }

    /* Decrement and check for completion. */
    uint32_t new_count = (chn->xfercfg & ~DMA_XFER_COUNT) | (count - 1);
    chn->xfercfg = new_count;

    trace_dma_transfer_active(ch, count - 1);

    if ((chn->xfercfg & DMA_XFER_SWTRIG)) {
        /* Transfer complete — assert interrupt. */
        qemu_set_irq(s->irq, true);
        trace_imxrt685_dma_ch_complete(ch);
    } else if (count > 1) {
        /* More transfers pending — schedule next at audio cadence. */
        int64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           NANOSECONDS_PER_SECOND / s->sample_rate;

        timer_mod(s->timer[ch], deadline);
    } else {
        trace_imxrt685_dma_ch_done(ch);
    }
}

/* MMIO read handler. */
static uint64_t dma_read(void *opaque, hwaddr offset, unsigned size)
{
    Imxrt685DmaState *s = IMXRT685_DMA(opaque);
    unsigned ch;
    uint32_t val;

    switch (offset) {
    case 0x000:     /* CTRL */
        return s->ctrl;
    case 0x008:     /* SRAMBASE */
        return s->srambase;
    default:
        /* Channel register access. */
        if (offset >= DMA_CHN_CFG && offset < DMA_CHN_CFG + DMA_CHANNEL_COUNT * DMA_CHN_STRIDE) {
            ch = (offset - DMA_CHN_CFG) / DMA_CHN_STRIDE;
            hwaddr ch_offset = (offset - DMA_CHN_CFG) % DMA_CHN_STRIDE;

            if (ch < DMA_CHANNEL_COUNT) {
                val = 0;
                switch (ch_offset) {
                case 0x0:     /* CFG */
                    val = s->chn[ch].cfg;
                    break;
                case 0x8:     /* XFERCFG */
                    val = s->chn[ch].xfercfg;
                    break;
                }
                trace_dma_read(ch, ch_offset, offset, val);
                return val;
            }
        }
        trace_dma_read_unimplemented(offset);
        return 0;
    }
}

/* MMIO write handler. */
static void dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Imxrt685DmaState *s = IMXRT685_DMA(opaque);
    unsigned ch;
    hwaddr ch_offset;

    switch (offset) {
    case 0x000:     /* CTRL — write enable to set bit */
        if (value & DMA_CTRL_ENABLE) {
            s->ctrl |= DMA_CTRL_ENABLE;
            trace_dma_ctrl_enable();
        } else {
            s->ctrl &= ~DMA_CTRL_ENABLE;
            trace_dma_ctrl_disable();
        }
        break;
    case 0x008:     /* SRAMBASE */
        s->srambase = value & ~0x1FFULL;
        trace_dma_srambase(s->srambase);
        break;
    default:
        if (offset >= DMA_CHN_CFG && offset < DMA_CHN_CFG + DMA_CHANNEL_COUNT * DMA_CHN_STRIDE) {
            ch = (offset - DMA_CHN_CFG) / DMA_CHN_STRIDE;
            ch_offset = (offset - DMA_CHN_CFG) % DMA_CHN_STRIDE;

            if (ch >= DMA_CHANNEL_COUNT) {
                trace_dma_write_chn_oob(ch);
                return;
            }

            Imxrt685DmaChn *chn = &s->chn[ch];

            switch (ch_offset) {
            case 0x0:     /* CFG */
                chn->cfg = value & 0xfU;  /* Only bits 0-3 used by SDK */
                trace_dma_chn_cfg(ch, chn->cfg);
                break;
            case 0x8:     /* XFERCFG */
                if (value & DMA_XFER_SWTRIG) {
                    dma_sw_trigger(s, ch);
                } else {
                    chn->xfercfg = value & 0xfU;
                    trace_dma_chn_xfercfg(ch, chn->xfercfg);
                }
                break;
            default:
                /* Ignore writes to reserved/other register bits. */
                break;
            }
        } else {
            trace_dma_write_unimplemented(offset, value);
        }
    }
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read,
    .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

/* Handle DREQ from a Flexcomm I2S peripheral — triggers DMA transfer on ch. */
void imxrt685_dma_handle_dreq(Imxrt685DmaState *s, unsigned ch, int level)
{
    if (level && (ch < DMA_CHANNEL_COUNT) &&
        (s->chn[ch].cfg & DMA_CFG_HWTRIGEN)) {
        dma_periph_request(s, ch);
    }
}

static void dma_reset(DeviceState *dev)
{
    Imxrt685DmaState *s = IMXRT685_DMA(dev);
    unsigned i;

    s->ctrl = 0;
    s->srambase = 0;
    for (i = 0; i < DMA_CHANNEL_COUNT; i++) {
        s->chn[i].cfg = 0;
        s->chn[i].xfercfg = 0;
        timer_del(s->timer[i]);
    }
    qemu_set_irq(s->irq, false);
}

static const VMStateDescription vmstate_imxrt685_dma = {
    .name = TYPE_IMXRT685_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, Imxrt685DmaState),
        VMSTATE_END_OF_LIST()
    }
};

static void dma_init(Object *obj)
{
    Imxrt685DmaState *s = IMXRT685_DMA(obj);
    unsigned i;

    s->sample_rate = 48000;

    for (i = 0; i < DMA_CHANNEL_COUNT; i++) {
        s->timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL, NULL, NULL);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &dma_ops, s,
                          "imxrt685-dma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dma_reset);
    dc->vmsd = &vmstate_imxrt685_dma;
    dc->user_creatable = false;
}

static const TypeInfo dma_info = {
    .name = TYPE_IMXRT685_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Imxrt685DmaState),
    .instance_init = dma_init,
    .class_init = dma_class_init,
};

static void dma_register_types(void)
{
    type_register_static(&dma_info);
}

type_init(dma_register_types)
