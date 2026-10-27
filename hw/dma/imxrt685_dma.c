/*
 * NXP MIMXRT685 DMA0 Controller (SDK-compatible model)
 *
 * Implements the descriptor-driven, DREQ-paced peripheral DMA controller
 * used by fsl_dma.c / fsl_i2s_dma.c for I2S audio transfers.  This is NOT
 * eDMA; it models NXP's older peripheral DMA at base address 0x40104000
 * (non-secure).
 *
 * Key behaviors:
 *   - Descriptor table loaded from SRAMBASE; linked descriptors follow.
 *   - XFERCFG encodes count-minus-one, width, increments, and control bits.
 *   - DREQ pacing for peripheral transfers when PERIPHREQEN is set.
 *   - INTA/INTB completion flags with proper pending/enabled IRQ derivation.
 *   - Software trigger (SWTRIG) sets the channel TRIG latch; HWTRIGEN is
 *     a separate hardware-trigger-input path.
 *
 * This is a local hardware-enablement device and must not be submitted
 * upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"
#include "hw/dma/imxrt685_dma.h"

/* MMIO aperture. */
#define DMA_MMIO_SIZE  0x1000

/* Channel register base offset within the window. */
#define DMA_CHN_BASE   0x400
#define DMA_CHN_STRIDE 0x10

/* Common register offsets. */
#define REG_CTRL        0x000
#define REG_INTSTAT     0x004
#define REG_SRAMBASE    0x008
#define REG_ENABLESET   0x020
#define REG_ENABLECLR   0x028
#define REG_ACTIVE      0x030
#define REG_BUSY        0x038
#define REG_ERRINT      0x040
#define REG_INTENSET    0x048
#define REG_INTENCLR    0x050
#define REG_INTA        0x058
#define REG_INTB        0x060
#define REG_SETVALID    0x068
#define REG_SETTRIG     0x070
#define REG_ABORT       0x078

/* Channel register offsets within the channel block. */
#define CHN_CFG        0x00
#define CHN_CTLSTAT    0x04
#define CHN_XFERCFG    0x08

/* Maximum beats per service pass to prevent runaway loops. */
#define MAX_BEATS_PER_PASS 256

/* Forward declaration for deferred service callback. */
static void dma_service_bh(void *opaque);

static void dma_schedule(Imxrt685DmaState *s)
{
    if (!s->service_pending) {
        s->service_pending = true;
        qemu_bh_schedule(s->service_bh);
    }
}

/* ----------------------------------------------------------------------- */
/* IRQ derivation                                                          */
/* ----------------------------------------------------------------------- */

static void imxrt685_dma_update_irq(Imxrt685DmaState *s)
{
    uint32_t normal = (s->inta | s->intb) & s->inten;
    uint32_t error  = s->errint & s->inten;

    bool new_state = (normal != 0 || error != 0);

    if (new_state != s->irq_state) {
        trace_dma_irq_transition(new_state ? "raise" : "lower",
                                 normal, error);
        qemu_set_irq(s->irq, new_state);
        s->irq_state = new_state;
    }
}

/* ----------------------------------------------------------------------- */
/* Descriptor loading                                                      */
/* ----------------------------------------------------------------------- */

static bool dma_fetch_descriptor(Imxrt685DmaState *s, hwaddr addr,
                                 DMADescriptor *desc)
{
    AddressSpace *as = &address_space_memory;

    if (address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED,
                         (uint8_t *)desc, sizeof(*desc), false) != MEMTX_OK) {
        trace_dma_descriptor_fetch_error(addr);
        return false;
    }

    desc->xfercfg = le32_to_cpu(desc->xfercfg);
    desc->src_end = le32_to_cpu(desc->src_end);
    desc->dst_end = le32_to_cpu(desc->dst_end);
    desc->link = le32_to_cpu(desc->link);

    /* Validate link alignment (must be 16-byte aligned). */
    if (desc->link && (desc->link & 0xf)) {
        trace_dma_descriptor_link_misaligned(desc->link);
        s->errint |= BIT(0);
        return false;
    }

    trace_dma_descriptor_loaded(addr, desc->xfercfg, desc->src_end,
                                desc->dst_end, desc->link);
    return true;
}

static void dma_load_channel_from_desc(Imxrt685DmaState *s, unsigned chn,
                                       const DMADescriptor *desc)
{
    Imxrt685DmaChn *c = &s->chn[chn];
    unsigned width_bytes;

    c->xfercfg  = desc->xfercfg;
    c->next_desc = desc->link;
    c->remaining = DMA_XFER_COUNT(desc->xfercfg) + 1; /* N = count+1 */

    /* Decode transfer parameters. */
    unsigned width = DMA_XFER_WIDTH(desc->xfercfg);
    if (width > 2) {
        trace_dma_error_invalid_width(chn, width);
        s->errint |= BIT(chn);
        return;
    }
    width_bytes = dma_width_bytes(width);

    unsigned src_inc = DMA_XFER_SRCINC(desc->xfercfg);
    unsigned dst_inc = DMA_XFER_DSTINC(desc->xfercfg);

    /* The encoding 3 means four times the transfer width. */
    src_inc = src_inc == 3 ? 4 : src_inc;
    dst_inc = dst_inc == 3 ? 4 : dst_inc;

    /* NXP descriptors store END addresses, not start. */
    c->current_src = desc->src_end - (c->remaining - 1) * width_bytes * src_inc;
    c->current_dst = desc->dst_end - (c->remaining - 1) * width_bytes * dst_inc;

    trace_dma_channel_loaded(chn, c->xfercfg, c->current_src,
                             c->current_dst, c->remaining);
}

static void dma_load_head_descriptor(Imxrt685DmaState *s, unsigned chn)
{
    Imxrt685DmaChn *c = &s->chn[chn];
    hwaddr desc_addr;
    DMADescriptor desc;

    if (!s->srambase || !(s->ctrl & DMA_CTRL_ENABLE) || !c->enabled) {
        return;
    }

    desc_addr = s->srambase + chn * 16;

    if (dma_fetch_descriptor(s, desc_addr, &desc)) {
        if (!(desc.xfercfg & DMA_XFER_CFGVALID)) {
            c->valid_pending = false;
            c->busy = false;
            return;
        }
        dma_load_channel_from_desc(s, chn, &desc);
        c->next_desc = desc.link;
        c->valid_pending = true;
        c->active = false;
        c->busy = true;
    } else {
        trace_dma_error_descriptor_fetch(chn);
        s->errint |= BIT(chn);
        c->valid_pending = false;
        c->active = false;
        c->busy = false;
    }
}

/* ----------------------------------------------------------------------- */
/* Transfer beat                                                           */
/* ----------------------------------------------------------------------- */

static bool dma_do_beat(Imxrt685DmaState *s, unsigned chn)
{
    Imxrt685DmaChn *c = &s->chn[chn];
    uint32_t data = 0;
    AddressSpace *as = &address_space_memory;

    /* Decode width and increments from the live XFERCFG. */
    unsigned width = DMA_XFER_WIDTH(c->xfercfg);
    if (width > 2) {
        trace_dma_error_invalid_width(chn, width);
        return false;
    }
    unsigned bytes = dma_width_bytes(width);

    unsigned src_inc = DMA_XFER_SRCINC(c->xfercfg);
    unsigned dst_inc = DMA_XFER_DSTINC(c->xfercfg);

    src_inc = src_inc == 3 ? 4 : src_inc;
    dst_inc = dst_inc == 3 ? 4 : dst_inc;
    hwaddr old_src = c->current_src;
    hwaddr old_dst = c->current_dst;

    /* Read from source. */
    if (address_space_rw(as, c->current_src, MEMTXATTRS_UNSPECIFIED,
                         (uint8_t *)&data, bytes, false) != MEMTX_OK) {
        trace_dma_error_bus_fault(chn, "read src", c->current_src);
        s->errint |= BIT(chn);
        return false;
    }

    /* Mask to actual width. */
    switch (bytes) {
    case 1: data &= 0xffU; break;
    case 2: data &= 0xffffU; break;
    default: break;
    }

    /* Write to destination. */
    if (address_space_rw(as, c->current_dst, MEMTXATTRS_UNSPECIFIED,
                         (uint8_t *)&data, bytes, true) != MEMTX_OK) {
        trace_dma_error_bus_fault(chn, "write dst", c->current_dst);
        s->errint |= BIT(chn);
        return false;
    }

    /* Advance addresses. */
    if (src_inc) {
        c->current_src += bytes * src_inc;
    }
    if (dst_inc) {
        c->current_dst += bytes * dst_inc;
    }

    /* Decrement remaining count. */
    c->remaining--;
    trace_dma_beat(chn, c->xfercfg, data,
                   old_src, old_dst);

    return true;
}

/* ----------------------------------------------------------------------- */
/* Descriptor completion                                                   */
/* ----------------------------------------------------------------------- */

static void dma_channel_complete(Imxrt685DmaState *s, unsigned chn)
{
    Imxrt685DmaChn *c = &s->chn[chn];
    bool reload = false;

    c->active = false;
    c->busy = false;

    /* Determine which interrupts to set. */
    if (c->xfercfg & DMA_XFER_INTA) {
        s->inta |= BIT(chn);
    }
    if (c->xfercfg & DMA_XFER_INTB) {
        s->intb |= BIT(chn);
    }

    /* Process CLRTRIG. */
    if (c->xfercfg & DMA_XFER_CLRTRIG) {
        c->ctlstat &= ~DMA_CTLSTAT_TRIG;
    }

    trace_dma_descriptor_complete(chn, s->inta, s->intb);

    /* Check for reload. */
    if ((c->xfercfg & DMA_XFER_RELOAD) && c->next_desc) {
        DMADescriptor desc;

        if (dma_fetch_descriptor(s, c->next_desc, &desc)) {
            dma_load_channel_from_desc(s, chn, &desc);
            c->active = true;
            reload = true;
            trace_dma_descriptor_reload(chn, c->next_desc);
        } else {
            s->errint |= BIT(chn);
        }
    }

    if (!reload) {
        c->valid_pending = false;
        trace_dma_channel_stopped(chn);
    }

    imxrt685_dma_update_irq(s);
}

/* ----------------------------------------------------------------------- */
/* Deferred service                                                        */
/* ----------------------------------------------------------------------- */

static void dma_service_bh(void *opaque)
{
    Imxrt685DmaState *s = opaque;
    unsigned pri = UINT_MAX, chn_pri, best_chn = DMA_CHANNEL_COUNT;
    int i;

    s->service_pending = false;

    /* Find highest-priority channel (lowest numeric priority). */
    for (i = 0; i < DMA_CHANNEL_COUNT; i++) {
        Imxrt685DmaChn *c = &s->chn[i];

        if (!c->enabled || !c->valid_pending ||
            !(c->ctlstat & DMA_CTLSTAT_TRIG)) {
            continue;
        }
        if ((c->cfg & DMA_CFG_PERIPHREQEN) && !c->dreq_level) {
            continue;
        }
        if (!(s->ctrl & DMA_CTRL_ENABLE)) {
            break;
        }

        chn_pri = (c->cfg >> 16) & 7;

        if (best_chn == DMA_CHANNEL_COUNT || chn_pri < pri) {
            best_chn = i;
            pri = chn_pri;
        } else if (chn_pri == pri && i < best_chn) {
            best_chn = i;
        }
    }

    if (best_chn != DMA_CHANNEL_COUNT) {
        Imxrt685DmaChn *c = &s->chn[best_chn];
        unsigned beats = 0;

        while (beats < MAX_BEATS_PER_PASS && c->remaining > 0) {
            bool dreq_required = c->cfg & DMA_CFG_PERIPHREQEN;

            /* If DREQ is required, only proceed if asserted. */
            if (dreq_required && !c->dreq_level) {
                break;
            }

            c->active = true;
            c->busy = true;
            c->ctlstat |= DMA_CTLSTAT_TRIG;

            if (!dma_do_beat(s, best_chn)) {
                /* Beat failed — channel is faulted. */
                dma_channel_complete(s, best_chn);
                return;
            }

            beats++;

            if (c->remaining == 0) {
                dma_channel_complete(s, best_chn);
                c->busy = false;
                break;
            }

            /* For peripheral requests, re-check DREQ after each beat. */
            if (dreq_required && !c->dreq_level) {
                c->active = false;
                break;
            }
        }

        if (!c->busy || beats == MAX_BEATS_PER_PASS) {
            c->active = false;
        }

        if (beats == MAX_BEATS_PER_PASS && c->remaining > 0) {
            dma_schedule(s);
        }

        imxrt685_dma_update_irq(s);
    }
}

/* ----------------------------------------------------------------------- */
/* MMIO handlers                                                           */
/* ----------------------------------------------------------------------- */

static uint64_t dma_read(void *opaque, hwaddr offset, unsigned size)
{
    Imxrt685DmaState *s = IMXRT685_DMA(opaque);
    unsigned chn;
    uint32_t val;

    switch (offset) {
    case REG_CTRL:
        return s->ctrl;

    case REG_INTSTAT: {
        uint32_t normal = (s->inta | s->intb) & s->inten;
        uint32_t error  = s->errint & s->inten;
        val = (normal != 0);  /* ACTIVEINT summary */
        if (error) {
            val |= BIT(1);      /* ACTIVEERRINT */
        }
        return val;
    }

    case REG_SRAMBASE:
        return s->srambase;

    case REG_ENABLESET:
        /* Read as CTRL mask for enabled channels. */
        val = 0;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (s->chn[i].enabled) {
                val |= BIT(i);
            }
        }
        return val;

    case REG_ACTIVE:
        /* Show channels currently active. */
        val = 0;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (s->chn[i].active) {
                val |= BIT(i);
            }
        }
        return val;

    case REG_BUSY:
        /* Show channels with in-flight work. */
        val = 0;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (s->chn[i].busy) {
                val |= BIT(i);
            }
        }
        return val;

    case REG_ENABLECLR:
        /* Read as zero — write-only. */
        return 0;

    case REG_ERRINT:
        val = s->errint;
        trace_dma_read(REG_ERRINT, val);
        return val;

    case REG_INTENSET:
        return s->inten;

    case REG_INTA: {
        val = s->inta;
        trace_dma_read(REG_INTA, val);
        return val;
    }

    case REG_INTB: {
        val = s->intb;
        trace_dma_read(REG_INTB, val);
        return val;
    }

    default:
        /* Channel register access. */
        if (offset >= DMA_CHN_BASE &&
            offset < DMA_CHN_BASE + DMA_CHANNEL_COUNT * DMA_CHN_STRIDE) {
            chn = (offset - DMA_CHN_BASE) / DMA_CHN_STRIDE;
            hwaddr ch_offset = (offset - DMA_CHN_BASE) % DMA_CHN_STRIDE;

            if (chn >= DMA_CHANNEL_COUNT) {
                trace_dma_read_chn_oob(offset);
                return 0;
            }

            Imxrt685DmaChn *c = &s->chn[chn];
            val = 0;
            switch (ch_offset) {
            case CHN_CFG:
                val = c->cfg;
                break;
            case CHN_CTLSTAT:
                val = c->ctlstat | (c->valid_pending ? DMA_CTLSTAT_VALIDPENDING : 0);
                break;
            case CHN_XFERCFG:
                val = c->xfercfg;
                break;
            }
            trace_dma_read_chn(chn, ch_offset, val);
            return val;
        }

        trace_dma_read_unimplemented(offset);
        return 0;
    }
}

static void dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Imxrt685DmaState *s = IMXRT685_DMA(opaque);
    unsigned chn;
    hwaddr ch_offset;

    switch (offset) {
    case REG_CTRL:
        if (value & DMA_CTRL_ENABLE) {
            s->ctrl |= DMA_CTRL_ENABLE;
            trace_dma_ctrl_enable();
        } else {
            s->ctrl &= ~DMA_CTRL_ENABLE;
            trace_dma_ctrl_disable();
        }
        break;

    case REG_INTSTAT:
        /* Write to clear ACTIVEINT and ACTIVEERRINT (read-only in practice). */
        break;

    case REG_SRAMBASE:
        s->srambase = value & ~0x1FFULL;  /* 512-byte alignment */
        trace_dma_srambase(s->srambase);
        break;

    case REG_ENABLESET: {
        uint32_t mask = value & 0x1FFFFFFFL;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (mask & BIT(i)) {
                s->chn[i].enabled = true;
                trace_dma_enable_set(i);

                /* Schedule deferred service if not already pending. */
                dma_schedule(s);
            }
        }
        break;
    }

    case REG_ENABLECLR: {
        uint32_t mask = value & 0x1FFFFFFFL;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (mask & BIT(i)) {
                Imxrt685DmaChn *c = &s->chn[i];

                c->enabled = false;
                c->ctlstat &= ~DMA_CTLSTAT_TRIG;
                trace_dma_enable_clear(i);

                /* If busy, complete and stop. */
                if (c->busy) {
                    c->active = false;
                    c->busy = false;
                    dma_channel_complete(s, i);
                }

                imxrt685_dma_update_irq(s);
            }
        }
        break;
    }

    case REG_ERRINT:
        /* W1C — clear error flags. */
        s->errint &= ~value;
        trace_dma_errint_clear(value);
        imxrt685_dma_update_irq(s);
        break;

    case REG_INTENSET: {
        uint32_t mask = value & 0x1FFFFFFFL;
        s->inten |= mask;
        trace_dma_inten_set(mask);
        imxrt685_dma_update_irq(s);
        break;
    }

    case REG_INTENCLR: {
        uint32_t mask = value & 0x1FFFFFFFL;
        s->inten &= ~mask;
        trace_dma_inten_clear(mask);
        imxrt685_dma_update_irq(s);
        break;
    }

    case REG_INTA:
        /* W1C — clear INTA flags. */
        s->inta &= ~value;
        trace_dma_inta_clear(value);
        imxrt685_dma_update_irq(s);
        break;

    case REG_INTB:
        /* W1C — clear INTB flags. */
        s->intb &= ~value;
        trace_dma_intb_clear(value);
        imxrt685_dma_update_irq(s);
        break;

    case REG_SETVALID: {
        uint32_t mask = value & 0x1FFFFFFFL;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (mask & BIT(i)) {
                s->chn[i].valid_pending = true;
                trace_dma_setvalid(i);

                /* If head descriptor hasn't been loaded, load it now. */
                Imxrt685DmaChn *c = &s->chn[i];
                if (!c->busy && !c->next_desc) {
                    dma_load_head_descriptor(s, i);
                }

                /* Schedule deferred service if not already pending. */
                dma_schedule(s);
            }
        }
        break;
    }

    case REG_SETTRIG: {
        uint32_t mask = value & 0x1FFFFFFFL;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (mask & BIT(i)) {
                s->chn[i].ctlstat |= DMA_CTLSTAT_TRIG;
                trace_dma_settrig(i);

                /* Schedule deferred service if valid-pending is set and not pending. */
                if (s->chn[i].valid_pending) {
                    dma_schedule(s);
                }
            }
        }
        break;
    }

    case REG_ABORT: {
        uint32_t mask = value & 0x1FFFFFFFL;
        for (int i = 0; i < DMA_CHANNEL_COUNT; i++) {
            if (mask & BIT(i)) {
                Imxrt685DmaChn *c = &s->chn[i];

                trace_dma_abort(i);
                c->valid_pending = false;
                c->ctlstat &= ~DMA_CTLSTAT_TRIG;
                c->active = false;
                c->busy = false;
            }
        }
        break;
    }

    default:
        /* Channel register access. */
        if (offset >= DMA_CHN_BASE &&
            offset < DMA_CHN_BASE + DMA_CHANNEL_COUNT * DMA_CHN_STRIDE) {
            chn = (offset - DMA_CHN_BASE) / DMA_CHN_STRIDE;
            ch_offset = (offset - DMA_CHN_BASE) % DMA_CHN_STRIDE;

            if (chn >= DMA_CHANNEL_COUNT) {
                trace_dma_write_chn_oob(chn);
                return;
            }

            Imxrt685DmaChn *c = &s->chn[chn];

            switch (ch_offset) {
            case CHN_CFG:
                c->cfg = value;
                trace_dma_chn_cfg(chn, c->cfg);
                break;

            case CHN_CTLSTAT:
                /* CTLSTAT is read-only. */
                trace_dma_chn_write_unimplemented(chn, CHN_CTLSTAT, value);
                break;

            case CHN_XFERCFG: {
                c->xfercfg = value & DMA_XFER_VALID_MASK & ~DMA_XFER_SWTRIG;
                trace_dma_chn_xfercfg(chn, c->xfercfg);

                if ((c->xfercfg & DMA_XFER_CFGVALID) && !c->busy) {
                    dma_load_head_descriptor(s, chn);
                }

                if (value & DMA_XFER_SWTRIG) {
                    c->ctlstat |= DMA_CTLSTAT_TRIG;
                    trace_dma_swtrig(chn);
                }

                dma_schedule(s);
                break;
            }

            default:
                trace_dma_chn_write_unimplemented(chn, ch_offset, value);
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

/* ----------------------------------------------------------------------- */
/* DREQ GPIO input                                                         */
/* ----------------------------------------------------------------------- */

static void imxrt685_dma_dreq(void *opaque, int n, int level)
{
    Imxrt685DmaState *s = opaque;
    unsigned chn = n;

    if (chn >= DMA_CHANNEL_COUNT) {
        return;
    }

    s->chn[chn].dreq_level = !!level;
    trace_dma_dreq(chn, level);

    /* If the channel is valid-pending and not already being serviced, schedule. */
    if (s->chn[chn].valid_pending) {
        dma_schedule(s);
    }
}

/* ----------------------------------------------------------------------- */
/* Reset                                                                   */
/* ----------------------------------------------------------------------- */

static void dma_reset(DeviceState *dev)
{
    Imxrt685DmaState *s = IMXRT685_DMA(dev);
    unsigned i;

    s->ctrl = 0;
    s->srambase = 0;
    s->irq_state = false;
    s->inta = 0;
    s->intb = 0;
    s->errint = 0;
    s->inten = 0;

    for (i = 0; i < DMA_CHANNEL_COUNT; i++) {
        Imxrt685DmaChn *c = &s->chn[i];

        c->cfg = 0;
        c->ctlstat = 0;
        c->xfercfg = 0;
        c->current_src = 0;
        c->current_dst = 0;
        c->next_desc = 0;
        c->remaining = 0;

        c->dreq_level = false;
        c->enabled = false;
        c->valid_pending = false;
        c->active = false;
        c->busy = false;
    }

    s->service_pending = false;
    qemu_bh_cancel(s->service_bh);

    imxrt685_dma_update_irq(s);
}

/* ----------------------------------------------------------------------- */
/* Migration state                                                         */
/* ----------------------------------------------------------------------- */

static const VMStateDescription vmstate_dma_ch = {
    .name = "imxrt685-dma-channel",
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cfg, Imxrt685DmaChn),
        VMSTATE_UINT32(ctlstat, Imxrt685DmaChn),
        VMSTATE_UINT32(xfercfg, Imxrt685DmaChn),
        VMSTATE_UINT32(current_src, Imxrt685DmaChn),
        VMSTATE_UINT32(current_dst, Imxrt685DmaChn),
        VMSTATE_UINT32(next_desc, Imxrt685DmaChn),
        VMSTATE_UINT32(remaining, Imxrt685DmaChn),
        VMSTATE_BOOL(dreq_level, Imxrt685DmaChn),
        VMSTATE_BOOL(enabled, Imxrt685DmaChn),
        VMSTATE_BOOL(valid_pending, Imxrt685DmaChn),
        VMSTATE_BOOL(active, Imxrt685DmaChn),
        VMSTATE_BOOL(busy, Imxrt685DmaChn),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_imxrt685_dma = {
    .name = TYPE_IMXRT685_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, Imxrt685DmaState),
        VMSTATE_UINT64(srambase, Imxrt685DmaState),
        VMSTATE_BOOL(irq_state, Imxrt685DmaState),
        VMSTATE_UINT32(inta, Imxrt685DmaState),
        VMSTATE_UINT32(intb, Imxrt685DmaState),
        VMSTATE_UINT32(errint, Imxrt685DmaState),
        VMSTATE_UINT32(inten, Imxrt685DmaState),
        VMSTATE_STRUCT_ARRAY(chn, Imxrt685DmaState, DMA_CHANNEL_COUNT, 0,
                             vmstate_dma_ch, Imxrt685DmaChn),
        VMSTATE_END_OF_LIST()
    },
};

/* ----------------------------------------------------------------------- */
/* Init / Class                                                            */
/* ----------------------------------------------------------------------- */

static void dma_init(Object *obj)
{
    Imxrt685DmaState *s = IMXRT685_DMA(obj);

    s->service_bh = qemu_bh_new_guarded(dma_service_bh, s,
                                        &DEVICE(s)->mem_reentrancy_guard);

    memory_region_init_io(&s->mmio, OBJECT(s), &dma_ops, s,
                          "imxrt685-dma", DMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);

    /* Create 33 DREQ GPIO inputs. */
    for (unsigned i = 0; i < DMA_CHANNEL_COUNT; i++) {
        qdev_init_gpio_in_named(DEVICE(s), imxrt685_dma_dreq,
                                "dma-dreq", i);
    }
}

static void dma_finalize(Object *obj)
{
    Imxrt685DmaState *s = IMXRT685_DMA(obj);

    qemu_bh_delete(s->service_bh);
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
    .instance_finalize = dma_finalize,
    .class_init = dma_class_init,
};

static void dma_register_types(void)
{
    type_register_static(&dma_info);
}

type_init(dma_register_types)
