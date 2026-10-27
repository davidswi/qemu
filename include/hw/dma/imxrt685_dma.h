/*
 * NXP MIMXRT685 DMA0 Controller (SDK-compatible model)
 *
 * Models the non-secure NXP peripheral DMA controller found at base
 * address 0x40104000.  This is NOT eDMA; it is the older peripheral
 * DMA used by fsl_dma.c / fsl_i2s_dma.c for I2S audio transfers.
 *
 * Register layout (SDK-visible offsets):
 *   CTRL          0x000 — global enable
 *   INTSTAT       0x004 — active-interrupt summary
 *   SRAMBASE      0x008 — descriptor-table base address
 *   ENABLESET/CLR 0x020/0x028 — channel enable (W1S/W1C)
 *   ACTIVE        0x030 — channels currently active
 *   BUSY          0x038 — channels with in-flight transfers
 *   ERRINT        0x040 — error interrupt flags (W1C)
 *   INTENSET/CLR  0x048/0x050 — interrupt enable (W1S/W1C)
 *   INTA          0x058 — transfer-complete A flags (W1C)
 *   INTB          0x060 — transfer-complete B flags (W1C)
 *   SETVALID      0x068 — set channel valid-pending (W1S)
 *   SETTRIG       0x070 — set trigger latch (W1S)
 *   ABORT         0x078 — abort channel (W1C)
 *
 * Per-channel registers start at offset 0x400:
 *   CFG           +0x0 — transfer configuration bits
 *   CTLSTAT       +0x4 — control/status (TRIG, VALIDPENDING, etc.)
 *   XFERCFG       +0x8 — channel-specific transfer config
 *
 * Channel n = 0..32; bank 1 for channel 32 uses the common register set.
 *
 * This is a local hardware-enablement device and must not be submitted
 * upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_IMXRT685_DMA_H
#define HW_DMA_IMXRT685_DMA_H

#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"

/* Non-secure base address used by the SDK firmware. */
#define IMXRT685_DMA0_BASE_NS  0x40104000ULL

/* Channel count (from CMSIS header). */
#define DMA_CHANNEL_COUNT      33

/* CTRL bits. */
#define DMA_CTRL_ENABLE        BIT(0)

/* Per-channel CFG bits (compact storage). */
#define DMA_CFG_PERIPHREQEN    BIT(0)   /* accept peripheral DREQ       */
#define DMA_CFG_HWTRIGEN       BIT(1)   /* hardware trigger input active*/
#define DMA_CFG_PRIORITY(n)      ((n) & 7)   /* priority field (bits 3:5 in real HW,
                                                  stored compactly here; 0=high)*/
#define DMA_CFG_NCOINCR        BIT(6)   /* source increment disable       */
#define DMA_CFG_DOINCRRR       BIT(7)   /* destination increment disable  */

/* Channel CTLSTAT bits. */
#define DMA_CTLSTAT_TRIG       BIT(0)   /* trigger latch (set by SWTRIG/SETTRIG) */
#define DMA_CTLSTAT_VALIDPENDING BIT(2) /* valid-pending flag               */
#define DMA_CTLSTAT_BUSY       BIT(5)   /* channel is busy                  */

/* XFERCFG control bit masks (bits 0:5). */
#define DMA_XFER_CLRTRIG       BIT(0)   /* clear trigger on completion      */
#define DMA_XFER_INTA          BIT(1)   /* interrupt A on complete          */
#define DMA_XFER_INTB          BIT(2)   /* interrupt B on complete          */

/* XFERCFG field positions. */
#define DMA_XFER_WIDTH(n)        extract32((n), 8, 2)   /* width 9:8    */
#define DMA_XFER_SRCINC(n)       extract32((n), 12, 4)  /* src inc 15:12*/
#define DMA_XFER_DSTINC(n)       extract32((n), 16, 4)  /* dst inc 19:16*/
#define DMA_XFER_COUNT(n)        ((n) >> 10)            /* count 25:10  */

/* Width multiplier. */
static inline unsigned dma_width_bytes(unsigned width)
{
    return 1U << width;
}

#define TYPE_IMXRT685_DMA "imxrt685-dma"
OBJECT_DECLARE_SIMPLE_TYPE(Imxrt685DmaState, IMXRT685_DMA)

/* Descriptor layout in SRAM (little-endian words). */
typedef struct {
    uint32_t xfercfg;   /* word 0: transfer configuration         */
    uint32_t src_end;   /* word 1: source end address             */
    uint32_t dst_end;   /* word 2: destination end address        */
    uint32_t link;      /* word 3: link to next descriptor (0=end)*/
} DMADescriptor;

/* Per-channel state structure. */
typedef struct Imxrt685DmaChn {
    uint32_t cfg;
    uint32_t ctlstat;
    uint32_t xfercfg;
    uint32_t current_src;
    uint32_t current_dst;
    uint32_t next_desc;
    unsigned remaining;

    bool dreq_level;
    bool valid_pending;
    bool active;
    bool busy;
} Imxrt685DmaChn;

struct Imxrt685DmaState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;         /* DMA register space             */
    qemu_irq irq;              /* shared IRQ to NVIC             */

    /* Controller state. */
    uint32_t ctrl;             /* CTRL — global enable           */
    hwaddr srambase;           /* SRAMBASE — descriptor table    */
    bool irq_state;            /* current IRQ level (cached)     */

    /* Channel state. */
    Imxrt685DmaChn chn[DMA_CHANNEL_COUNT];

    /* Interrupt state. */
    uint32_t inta;
    uint32_t intb;
    uint32_t errint;
    uint32_t inten;

    /* Deferred DMA service flag (re-entrancy guard). */
    bool service_pending;
};

#endif /* HW_DMA_IMXRT685_DMA_H */
