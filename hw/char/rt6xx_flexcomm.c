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
#include "trace.h"

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

/* I2C block (register offsets are absolute in the Flexcomm aperture) */
#define I2C_CFG_MSTEN           BIT(0)
#define I2C_CFG_SLAVEMON_MASK   (BIT(1) | BIT(2))
#define I2C_CFG_MASK            0x3f
#define I2C_STAT_RESET          0x00000801
#define I2C_STAT_W1C_MASK       (BIT(4) | BIT(6) | BIT(15) | BIT(17) | \
                                 BIT(19) | BIT(24) | BIT(25))
#define I2C_INTEN_MASK          0x030b8951
/*
 * Status sources that can drive the shared IRQ in this master-only
 * model: master pending, master arbitration loss, master STOP error
 * and the (not yet generated) timeout flags.  Slave and monitor
 * sources stay quiet while those modes are deferred, even though
 * their INTENSET plumbing is correct for later work.
 */
#define I2C_IRQ_STAT_MASK       (BIT(0) | BIT(4) | BIT(6) | BIT(24) | BIT(25))
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

/*
 * I2C master model.  Transfers execute synchronously inside the MMIO
 * write that commands them, matching the QEMU I2C core semantics and
 * the existing USART TX approach; baud and delay registers are stored
 * but do not advance virtual time.
 */
static uint32_t rt6xx_flexcomm_i2c_stat(Rt6xxFlexcommState *s)
{
    /*
     * MSTPENDING is set whenever the (single) master state machine is
     * not mid-bit, which in this synchronous model means always; the
     * reset value 0x00000801 already reflects idle pending and
     * SLVNOTSTR.
     */
    return I2C_STAT_RESET | ((uint32_t)s->i2c.phase << 1) | s->i2c.stat;
}

static uint32_t rt6xx_flexcomm_i2c_effective_stat(Rt6xxFlexcommState *s)
{
    uint32_t stat = rt6xx_flexcomm_i2c_stat(s);

    if (!(s->i2c.cfg & I2C_CFG_MSTEN)) {
        stat &= ~BIT(0);
    }
    return stat & I2C_IRQ_STAT_MASK;
}

/* Leave the bus in a safe state and return the master to idle. */
static void rt6xx_flexcomm_i2c_abort(Rt6xxFlexcommState *s)
{
    if (s->i2c.bus_active && s->i2c_bus) {
        i2c_end_transfer(s->i2c_bus);
        s->i2c.bus_active = false;
    }
    s->i2c.phase = RT6XX_I2C_IDLE;
}

static void rt6xx_flexcomm_i2c_command(Rt6xxFlexcommState *s, uint32_t val)
{
    Rt6xxFlexcommI2cState *i = &s->i2c;

    if (!s->clock_enabled || s->reset_asserted || !(i->cfg & I2C_CFG_MSTEN)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: I2C master command while the "
                      "master is not enabled\n", TYPE_RT6XX_FLEXCOMM);
        return;
    }

    if (val & BIT(1)) {     /* MSTSTART */
        uint8_t address = extract32(i->mstdat, 1, 7);
        bool recv = i->mstdat & 1;

        /*
         * QEMU treats a start while the bus is already owned as a
         * repeated start, which is exactly the sequence the SDK's
         * I2C_MasterTransferBlocking() performs between the write and
         * read phases.
         */
        if (i2c_start_transfer(s->i2c_bus, address, recv)) {
            i->phase = RT6XX_I2C_NACK_ADDR;
            i->bus_active = false;
            trace_rt6xx_flexcomm_i2c_start(DEVICE(s)->canonical_path,
                                           address, recv, false);
        } else {
            i->bus_active = true;
            if (recv) {
                i->mstdat = i2c_recv(s->i2c_bus);
                i->phase = RT6XX_I2C_RX_READY;
            } else {
                i->phase = RT6XX_I2C_TX_READY;
            }
            trace_rt6xx_flexcomm_i2c_start(DEVICE(s)->canonical_path,
                                           address, recv, true);
        }
    }

    if (val & BIT(0)) {     /* MSTCONTINUE */
        switch (i->phase) {
        case RT6XX_I2C_TX_READY: {
            int ret = i2c_send(s->i2c_bus, i->mstdat & 0xff);

            trace_rt6xx_flexcomm_i2c_byte(DEVICE(s)->canonical_path, false,
                                          i->mstdat & 0xff, !ret);
            i->phase = ret ? RT6XX_I2C_NACK_DATA : RT6XX_I2C_TX_READY;
            break;
        }
        case RT6XX_I2C_RX_READY:
            i->mstdat = i2c_recv(s->i2c_bus);
            trace_rt6xx_flexcomm_i2c_byte(DEVICE(s)->canonical_path, true,
                                          i->mstdat & 0xff, true);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: I2C CONTINUE in state %u\n",
                          TYPE_RT6XX_FLEXCOMM, i->phase);
            i->stat |= BIT(6);    /* MSTSTSTPERR */
            break;
        }
    }

    if (val & BIT(2)) {     /* MSTSTOP */
        if (i->bus_active) {
            if (i->phase == RT6XX_I2C_RX_READY) {
                /* NACK the byte held in MSTDAT before releasing. */
                i2c_nack(s->i2c_bus);
            }
            i2c_end_transfer(s->i2c_bus);
            i->bus_active = false;
        }
        trace_rt6xx_flexcomm_i2c_stop(DEVICE(s)->canonical_path, i->phase);
        i->phase = RT6XX_I2C_IDLE;
    }
}

static uint64_t rt6xx_flexcomm_i2c_read(Rt6xxFlexcommState *s, hwaddr offset)
{
    Rt6xxFlexcommI2cState *i = &s->i2c;

    switch (offset) {
    case 0x800:
        return i->cfg;
    case 0x804:
        return rt6xx_flexcomm_i2c_stat(s);
    case 0x808:
        return i->inten;
    case 0x80c:
        return 0;
    case 0x810:
        /* TOMIN (bits 3:0) reads as 0xf; the timeout itself is stored. */
        return i->timeout | 0xf;
    case 0x814:
        return i->clkdiv;
    case 0x818:
        return rt6xx_flexcomm_i2c_effective_stat(s) & i->inten;
    case 0x820:
        /* Command bits self-clear; only MSTDMA persists. */
        return i->mstctl;
    case 0x824:
        return i->mstime;
    case 0x828:
        return i->mstdat;
    case 0x840:
        return i->slvctl;
    case 0x844:
        return i->slvdat;
    case 0x848:
    case 0x84c:
    case 0x850:
    case 0x854:
        return i->slvadr[(offset - 0x848) / 4];
    case 0x858:
        return i->slvqual0;
    case 0x880:
        return 0;   /* monitor mode deferred */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented I2C read at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_FLEXCOMM, offset);
        return 0;
    }
}

static void rt6xx_flexcomm_i2c_write(Rt6xxFlexcommState *s, hwaddr offset,
                                     uint32_t val)
{
    Rt6xxFlexcommI2cState *i = &s->i2c;

    switch (offset) {
    case 0x800:
        if (val & I2C_CFG_SLAVEMON_MASK) {
            qemu_log_mask(LOG_UNIMP, "%s: I2C slave/monitor modes are "
                          "not implemented\n", TYPE_RT6XX_FLEXCOMM);
        }
        if ((i->cfg & I2C_CFG_MSTEN) && !(val & I2C_CFG_MSTEN)) {
            rt6xx_flexcomm_i2c_abort(s);
        }
        i->cfg = val & I2C_CFG_MASK;
        break;
    case 0x804:
        i->stat &= ~(val & I2C_STAT_W1C_MASK);
        break;
    case 0x808:
        i->inten |= val & I2C_INTEN_MASK;
        break;
    case 0x80c:
        i->inten &= ~(val & I2C_INTEN_MASK);
        break;
    case 0x810:
        i->timeout = val & 0xfff0;
        break;
    case 0x814:
        i->clkdiv = val & 0xffff;
        break;
    case 0x820:
        if (val & BIT(3)) {
            qemu_log_mask(LOG_UNIMP, "%s: I2C DMA requests are not "
                          "implemented\n", TYPE_RT6XX_FLEXCOMM);
        }
        i->mstctl = val & BIT(3);
        rt6xx_flexcomm_i2c_command(s, val);
        break;
    case 0x824:
        i->mstime = val & 0x77;
        break;
    case 0x828:
        i->mstdat = val & 0xff;
        break;
    case 0x840:
        if (val & 0x30b) {
            qemu_log_mask(LOG_UNIMP, "%s: I2C slave operation is not "
                          "implemented\n", TYPE_RT6XX_FLEXCOMM);
        }
        i->slvctl = val & 0x30b;
        break;
    case 0x844:
        i->slvdat = val & 0xff;
        break;
    case 0x848:
    case 0x84c:
    case 0x850:
    case 0x854:
        i->slvadr[(offset - 0x848) / 4] = val & 0xff;
        break;
    case 0x858:
        i->slvqual0 = val & 0xff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented I2C write at 0x%03"
                      HWADDR_PRIx " value 0x%08x\n",
                      TYPE_RT6XX_FLEXCOMM, offset, val);
        break;
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
    uint32_t sources = 0;
    bool level = false;

    /*
     * USART, I2C and SPI share one NVIC line per instance; only the
     * currently selected function may drive it, and only while the
     * block is clocked and out of reset.
     */
    if (s->clock_enabled && !s->reset_asserted) {
        switch (rt6xx_flexcomm_selected(s)) {
        case RT6XX_FLEXCOMM_PERIPH_USART:
            if (u->cfg & CFG_ENABLE) {
                sources = (u->stat & u->intenset & INTEN_MASK) |
                          (rt6xx_flexcomm_fifointstat(s) & u->fifointenset);
                level = sources != 0;
            }
            break;
        case RT6XX_FLEXCOMM_PERIPH_I2C:
            sources = rt6xx_flexcomm_i2c_effective_stat(s) & s->i2c.inten;
            level = sources != 0;
            break;
        case RT6XX_FLEXCOMM_PERIPH_SPI:
            /* Added with the SPI master model. */
            break;
        default:
            break;
        }
    }
    trace_rt6xx_flexcomm_irq(DEVICE(s)->canonical_path,
                             rt6xx_flexcomm_selected(s), level, sources);
    qemu_set_irq(s->irq, level);
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
    Rt6xxFlexcommI2cState *i = &s->i2c;
    int k;

    rt6xx_flexcomm_i2c_abort(s);

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

    i->cfg = 0;
    i->stat = 0;
    i->inten = 0;
    i->timeout = 0xfff0;
    i->clkdiv = 0;
    i->mstctl = 0;
    i->mstime = 0x77;
    i->mstdat = 0;
    i->slvctl = 0;
    i->slvdat = 0;
    for (k = 0; k < 4; k++) {
        i->slvadr[k] = 1;
    }
    i->slvqual0 = 0;
    i->phase = RT6XX_I2C_IDLE;

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

/*
 * Dispatch an access to the function window it belongs to.  Accesses
 * to an inactive window read as zero and ignore writes, logging a
 * guest error; the common registers are always accessible.
 */
static bool rt6xx_flexcomm_check_access(Rt6xxFlexcommState *s, hwaddr offset)
{
    Rt6xxFlexcommPeriph sel = rt6xx_flexcomm_selected(s);
    bool allowed;

    if (offset >= 0xff8) {
        return true;
    }
    switch (offset) {
    case 0x000 ... 0x3ff:
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_USART;
        break;
    case 0x400 ... 0x7ff:
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_SPI;
        break;
    case 0x800 ... 0xbff:
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_I2C;
        break;
    case 0xc00 ... 0xdff:
        allowed = false;      /* I2S windows, deferred */
        break;
    default:                 /* 0xe00..0xff7 shared FIFO windows */
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_USART ||
                  sel == RT6XX_FLEXCOMM_PERIPH_SPI;
        break;
    }
    if (allowed) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: access at 0x%03" HWADDR_PRIx
                  " while Flexcomm function is not selected for it\n",
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
    }

    if (offset >= 0x800 && offset <= 0xbff) {
        return rt6xx_flexcomm_i2c_read(s, offset);
    }

    switch (offset) {
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

    if (offset >= 0x800 && offset <= 0xbff) {
        rt6xx_flexcomm_i2c_write(s, offset, val);
        rt6xx_flexcomm_update_irq(s);
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
            Rt6xxFlexcommPeriph old_sel = rt6xx_flexcomm_selected(s);

            if (!rt6xx_flexcomm_have_periph(s, sel)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: PSELID selects absent function %u\n",
                              TYPE_RT6XX_FLEXCOMM, sel);
                sel = rt6xx_flexcomm_selected(s);
            }
            s->pselid = (s->pselid & ~0x0f) | sel | (val & PSELID_LOCK);
            if (sel != old_sel) {
                if (old_sel == RT6XX_FLEXCOMM_PERIPH_I2C) {
                    rt6xx_flexcomm_i2c_abort(s);
                }
                trace_rt6xx_flexcomm_sel(DEVICE(s)->canonical_path, sel);
            }
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

    /* An in-flight bus transaction is not migrated by this device. */
    s->i2c.bus_active = false;
    if (version_id < 2) {
        Rt6xxFlexcommI2cState *i = &s->i2c;

        i->cfg = 0;
        i->stat = 0;
        i->inten = 0;
        i->timeout = 0xfff0;
        i->clkdiv = 0;
        i->mstctl = 0;
        i->mstime = 0x77;
        i->mstdat = 0;
        i->slvctl = 0;
        i->slvdat = 0;
        for (int k = 0; k < 4; k++) {
            i->slvadr[k] = 1;
        }
        i->slvqual0 = 0;
        i->phase = RT6XX_I2C_IDLE;
    }
    rt6xx_flexcomm_update_irq(s);
    qemu_chr_fe_accept_input(&s->chr);
    return 0;
}

static int rt6xx_flexcomm_pre_save(void *opaque)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);

    /* Do not migrate a controller halfway through an I2C transaction. */
    rt6xx_flexcomm_i2c_abort(s);
    return 0;
}

static const VMStateDescription vmstate_rt6xx_flexcomm = {
    .name = TYPE_RT6XX_FLEXCOMM,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = rt6xx_flexcomm_pre_save,
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
        VMSTATE_UINT32_V(i2c.cfg, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.stat, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.inten, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.timeout, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.clkdiv, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.mstctl, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.mstime, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.mstdat, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.slvctl, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_V(i2c.slvdat, Rt6xxFlexcommState, 2),
        VMSTATE_UINT32_ARRAY_V(i2c.slvadr, Rt6xxFlexcommState, 4, 2),
        VMSTATE_UINT32_V(i2c.slvqual0, Rt6xxFlexcommState, 2),
        VMSTATE_UINT8_V(i2c.phase, Rt6xxFlexcommState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_flexcomm_realize(DeviceState *dev, Error **errp)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(dev);
    g_autofree char *bus_name = NULL;

    if (s->capabilities & RT6XX_FLEXCOMM_CAP_I2C) {
        bus_name = g_strdup_printf("%s.i2c",
                    object_get_canonical_path_component(OBJECT(dev)));
        s->i2c_bus = i2c_init_bus(dev, bus_name);
    }
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
