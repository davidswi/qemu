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

#ifndef HW_DMA_IMXRT685_DMA_H
#define HW_DMA_IMXRT685_DMA_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"

/* Base addresses for the two DMA controllers. */
#define IMXRT685_DMA0_BASE_NS   0x40104000ULL
#define IMXRT685_DMA0_BASE      0x50104000ULL
#define IMXRT685_DMA1_BASE_NS   0x40105000ULL
#define IMXRT685_DMA1_BASE      0x50105000ULL

/* Channel count (from CMSIS header). */
#define DMA_CHANNEL_COUNT       33

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

/* DREQ callback: called when a Flexcomm I2S peripheral requests DMA transfer. */
void imxrt685_dma_handle_dreq(Imxrt685DmaState *s, unsigned ch, int level);

#endif /* HW_DMA_IMXRT685_DMA_H */
