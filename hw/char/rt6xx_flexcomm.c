/*
 * NXP i.MX RT6xx Flexcomm
 *
 * Register layout and reset values are from the project-local
 * MIMXRT685S_cm33 CMSIS-SVD.  Transmission is synchronous from QEMU's
 * point of view; the TX FIFO therefore remains empty/not-full.
 *
 * DMA DREQ outputs: one per direction (TX/RX), wired to DMA channel inputs.
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

/* I2S FIFO DMA request bits in FIFOCFG register. */
#define I2S_FIFOCFG_TXDMA   BIT(12)
#define I2S_FIFOCFG_RXDMA   BIT(13)

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
 * I2S block (register offsets are absolute in the Flexcomm aperture).
 * CFG1..DIV live at 0xc00..0xc1c; the FIFO window 0xe00..0xff8 is
 * shared with the other personalities and dispatches on PERSEL.
 */
#define I2S_CFG1_MAINENABLE   BIT(0)
#define I2S_CFG1_DATAPAUSE    BIT(1)
#define I2S_CFG1_MASK         0x001f3fff
#define I2S_CFG2_MASK         0x07ff07ff
#define I2S_STAT_SLVFRMERR    BIT(1)
#define I2S_DIV_MASK          0xfff
#define I2S_FIFOCFG_TXI2SE0   BIT(2)
#define I2S_FIFOCFG_PACK48    BIT(3)
#define I2S_FIFOCFG_SIZE      (3U << 4) /* 8 entries (log2 encoding) */
#define I2S_FIFOCFG_MASK      0x0000f00f
#define I2S_FIFOSTAT_TXERR    BIT(0)
#define I2S_FIFOSTAT_RXERR    BIT(1)
#define I2S_FIFOSTAT_TXEMPTY  BIT(4)
#define I2S_FIFOSTAT_TXNOTFULL BIT(5)
#define I2S_FIFOSTAT_RXNOTEMPTY BIT(6)
#define I2S_FIFOSTAT_RXFULL   BIT(7)
#define I2S_FIFOTRIG_TXLVLENA BIT(0)
#define I2S_FIFOTRIG_RXLVLENA BIT(1)
#define I2S_FIFOTRIG_MASK     0x000f0f03
#define I2S_FIFOINT_TXERR     BIT(0)
#define I2S_FIFOINT_RXERR     BIT(1)
#define I2S_FIFOINT_TXLVL     BIT(2)
#define I2S_FIFOINT_RXLVL     BIT(3)
#define I2S_FIFOINT_MASK      0x0f

/* SPI block (register offsets are absolute in the Flexcomm aperture) */
#define SPI_CFG_ENABLE          BIT(0)
#define SPI_CFG_MASTER          BIT(2)
#define SPI_CFG_LOOP            BIT(7)
#define SPI_CFG_SPOL(n)         BIT(8 + (n))
#define SPI_CFG_MASK            0x0fbd
#define SPI_STAT_SSA            BIT(4)
#define SPI_STAT_SSD            BIT(5)
#define SPI_STAT_ENDTRANSFER    BIT(7)
#define SPI_STAT_MSTIDLE        BIT(8)
#define SPI_STAT_W1C_MASK       (SPI_STAT_SSA | SPI_STAT_SSD)
#define SPI_INTEN_MASK          0x130
#define SPI_FIFOWR_EOT          BIT(20)
#define SPI_FIFOWR_EOF          BIT(21)
#define SPI_FIFOWR_RXIGNORE     BIT(22)
#define SPI_FIFOWR_TXIGNORE     BIT(23)
#define SPI_FIFOWR_LEN(v)       extract32((v), 24, 4)
#define SPI_FIFORD_SOT          BIT(20)
#define SPI_FIFOSTAT_TXERR      BIT(0)
#define SPI_FIFOSTAT_RXERR      BIT(1)
#define SPI_FIFOSTAT_PERINT     BIT(3)
#define SPI_FIFOINT_PERINT      BIT(4)
#define SPI_FIFOCFG_SIZE        (2U << 4) /* 16 entries */
#define SPI_SSEL_COUNT          4
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

static void rt6xx_flexcomm_update_irq(Rt6xxFlexcommState *s);

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
    case RT6XX_FLEXCOMM_PERIPH_I2S_TX:
    case RT6XX_FLEXCOMM_PERIPH_I2S_RX:
        /* One silicon I2S capability serves both PERSEL directions. */
        return s->capabilities & RT6XX_FLEXCOMM_CAP_I2S;
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

/*
 * SPI master model.  One FIFOWR write completes one frame synchronously,
 * so the TX FIFO is always empty and only received frames need queuing.
 * DIV and DLY are stored but do not advance virtual time.
 */
static uint32_t rt6xx_flexcomm_spi_stat(Rt6xxFlexcommState *s)
{
    return (s->spi.stat & SPI_STAT_W1C_MASK) | SPI_STAT_MSTIDLE;
}

static uint32_t rt6xx_flexcomm_spi_peripheral_irq(Rt6xxFlexcommState *s)
{
    return rt6xx_flexcomm_spi_stat(s) & s->spi.inten & SPI_INTEN_MASK;
}

static int rt6xx_flexcomm_spi_cs_level(Rt6xxFlexcommState *s,
                                       unsigned cs, bool active)
{
    bool active_high = s->spi.cfg & SPI_CFG_SPOL(cs);

    return active ? active_high : !active_high;
}

static void rt6xx_flexcomm_spi_update_cs(Rt6xxFlexcommState *s)
{
    if (!s->spi_bus) {
        return;
    }

    for (unsigned cs = 0; cs < SPI_SSEL_COUNT; cs++) {
        DeviceState *dev = ssi_get_cs(s->spi_bus, cs);

        if (!dev || !object_property_find(OBJECT(dev), SSI_GPIO_CS "[0]")) {
            continue;
        }
        qemu_set_irq(qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0),
                     rt6xx_flexcomm_spi_cs_level(
                         s, cs, (s->spi.cfg & SPI_CFG_MASTER) &&
                                (s->spi.ssel & BIT(cs))));
    }
}

static void rt6xx_flexcomm_spi_set_ssel(Rt6xxFlexcommState *s, uint8_t ssel)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;
    uint8_t old = sp->ssel;

    ssel &= MAKE_64BIT_MASK(0, SPI_SSEL_COUNT);
    if (ssel & ~old) {
        sp->stat |= SPI_STAT_SSA;
        sp->slave_sot = true;
    }
    if (old & ~ssel) {
        sp->stat |= SPI_STAT_SSD;
    }
    sp->ssel = ssel;
    rt6xx_flexcomm_spi_update_cs(s);
    if (old != ssel) {
        trace_rt6xx_flexcomm_spi_cs(DEVICE(s)->canonical_path, old, ssel);
    }
}

static uint32_t rt6xx_flexcomm_spi_fifostat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;
    uint32_t value = sp->fifoerr;

    if (!sp->tx_count) {
        value |= BIT(4);
    }
    if (sp->tx_count < RT6XX_FLEXCOMM_FIFO_SIZE) {
        value |= BIT(5);
    }

    if (rt6xx_flexcomm_spi_peripheral_irq(s)) {
        value |= SPI_FIFOSTAT_PERINT;
    }
    if (sp->rx_count) {
        value |= BIT(6);
    }
    if (sp->rx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
        value |= BIT(7);
    }
    value |= (uint32_t)sp->tx_count << 8;
    value |= (uint32_t)sp->rx_count << 16;
    return value;
}

static uint32_t rt6xx_flexcomm_spi_fifointstat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;
    uint32_t status = sp->fifoerr;
    unsigned tx_trigger = extract32(sp->fifotrig, 8, 4);
    unsigned rx_trigger = extract32(sp->fifotrig, 16, 4);

    if ((sp->fifotrig & BIT(0)) && sp->tx_count <= tx_trigger) {
        status |= FIFOINT_TXLVL;
    }
    if ((sp->fifotrig & BIT(1)) && sp->rx_count > rx_trigger) {
        status |= FIFOINT_RXLVL;
    }
    if (rt6xx_flexcomm_spi_peripheral_irq(s)) {
        status |= SPI_FIFOINT_PERINT;
    }
    return status;
}

static bool rt6xx_flexcomm_spi_slave_active(Rt6xxFlexcommState *s)
{
    return s->clock_enabled && !s->reset_asserted &&
           rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_SPI &&
           (s->spi.cfg & SPI_CFG_ENABLE) &&
           !(s->spi.cfg & SPI_CFG_MASTER);
}

static void rt6xx_flexcomm_spi_recompute_slave_ssel(Rt6xxFlexcommState *s)
{
    uint8_t active = 0;

    for (unsigned cs = 0; cs < SPI_SSEL_COUNT; cs++) {
        bool level = s->spi.ssel_input & BIT(cs);
        bool active_high = s->spi.cfg & SPI_CFG_SPOL(cs);

        if (level == active_high) {
            active |= BIT(cs);
        }
    }
    rt6xx_flexcomm_spi_set_ssel(s, active);
}

static void rt6xx_flexcomm_spi_slave_ssel(void *opaque, int n, int level)
{
    Rt6xxFlexcommState *s = opaque;

    s->spi.ssel_input = deposit32(s->spi.ssel_input, n, 1, !!level);
    if (rt6xx_flexcomm_spi_slave_active(s)) {
        rt6xx_flexcomm_spi_recompute_slave_ssel(s);
        if (!s->spi.ssel) {
            qemu_set_irq(s->spi_slave_miso, 0);
        }
    }
    rt6xx_flexcomm_update_irq(s);
}

static void rt6xx_flexcomm_spi_slave_frame(void *opaque, int n, int level)
{
    Rt6xxFlexcommState *s = opaque;
    Rt6xxFlexcommSpiState *sp = &s->spi;
    uint32_t control = sp->slave_control;
    uint32_t tx = 0;
    uint32_t rx;
    uint32_t mask;
    unsigned bits;

    if (!rt6xx_flexcomm_spi_slave_active(s) || !sp->ssel) {
        qemu_set_irq(s->spi_slave_miso, 0);
        return;
    }

    if ((sp->fifocfg & FIFOCFG_ENABLETX) && sp->tx_count) {
        control = sp->tx_fifo[sp->tx_head];
        sp->tx_head = (sp->tx_head + 1) % RT6XX_FLEXCOMM_FIFO_SIZE;
        sp->tx_count--;
        sp->slave_control = control;
        if (!(control & SPI_FIFOWR_TXIGNORE)) {
            tx = control & 0xffff;
        }
    } else if (!(control & SPI_FIFOWR_TXIGNORE)) {
        sp->fifoerr |= SPI_FIFOSTAT_TXERR;
    }

    bits = SPI_FIFOWR_LEN(control) + 1;
    mask = MAKE_64BIT_MASK(0, bits);
    tx &= mask;
    rx = (uint32_t)level & mask;
    qemu_set_irq(s->spi_slave_miso, tx);

    trace_rt6xx_flexcomm_spi_frame(DEVICE(s)->canonical_path, tx, rx, bits,
                                   sp->ssel, false, false, false);

    if (!(control & SPI_FIFOWR_RXIGNORE) &&
        (sp->fifocfg & FIFOCFG_ENABLERX)) {
        uint32_t entry = rx | ((uint32_t)(~sp->ssel & 0xf) << 16);

        if (sp->slave_sot) {
            entry |= SPI_FIFORD_SOT;
        }
        if (sp->rx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
            sp->fifoerr |= SPI_FIFOSTAT_RXERR;
        } else {
            unsigned tail = (sp->rx_head + sp->rx_count) %
                            RT6XX_FLEXCOMM_FIFO_SIZE;

            sp->rx_fifo[tail] = entry;
            sp->rx_count++;
        }
    }
    sp->slave_sot = false;
    rt6xx_flexcomm_update_irq(s);
}

static void rt6xx_flexcomm_spi_slave_queue(Rt6xxFlexcommState *s,
                                            uint32_t val)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;

    if (!s->clock_enabled || s->reset_asserted ||
        !(sp->cfg & SPI_CFG_ENABLE) ||
        !(sp->fifocfg & FIFOCFG_ENABLETX)) {
        sp->fifoerr |= SPI_FIFOSTAT_TXERR;
        return;
    }

    /* TXIGNORE is a persistent receive-only setup, not a FIFO entry. */
    if (val & SPI_FIFOWR_TXIGNORE) {
        sp->slave_control = val;
        return;
    }
    if (sp->tx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
        sp->fifoerr |= SPI_FIFOSTAT_TXERR;
        return;
    }

    sp->tx_fifo[(sp->tx_head + sp->tx_count) % RT6XX_FLEXCOMM_FIFO_SIZE] = val;
    sp->tx_count++;
}

static void rt6xx_flexcomm_spi_frame(Rt6xxFlexcommState *s, uint32_t val)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;
    unsigned bits = SPI_FIFOWR_LEN(val) + 1;
    uint32_t data_mask = MAKE_64BIT_MASK(0, bits);
    uint32_t tx = val & data_mask;
    uint8_t ssel = ~extract32(val, 16, 4) & 0xf;
    bool sot = (ssel & ~sp->ssel) != 0;
    uint32_t rx;

    if (!s->clock_enabled || s->reset_asserted) {
        return;
    }
    if (!(sp->cfg & SPI_CFG_ENABLE) || !(sp->cfg & SPI_CFG_MASTER) ||
        !(sp->fifocfg & FIFOCFG_ENABLETX)) {
        sp->fifoerr |= SPI_FIFOSTAT_TXERR;
        if (!(sp->cfg & SPI_CFG_MASTER)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: frame written while SPI master is disabled\n",
                          TYPE_RT6XX_FLEXCOMM);
        }
        return;
    }

    if (val & SPI_FIFOWR_TXIGNORE) {
        qemu_log_mask(LOG_UNIMP, "%s: SPI TXIGNORE drives zero data\n",
                      TYPE_RT6XX_FLEXCOMM);
        tx = 0;
    }

    rt6xx_flexcomm_spi_set_ssel(s, ssel);
    rx = (sp->cfg & SPI_CFG_LOOP) ? tx : ssi_transfer(s->spi_bus, tx);
    rx &= data_mask;

    trace_rt6xx_flexcomm_spi_frame(DEVICE(s)->canonical_path, tx, rx, bits,
                                   ssel, !!(val & SPI_FIFOWR_EOT),
                                   !!(val & SPI_FIFOWR_EOF),
                                   !!(sp->cfg & SPI_CFG_LOOP));

    if (!(val & SPI_FIFOWR_RXIGNORE) &&
        (sp->fifocfg & FIFOCFG_ENABLERX)) {
        uint32_t entry = rx | ((uint32_t)(~ssel & 0xf) << 16);

        if (sot) {
            entry |= SPI_FIFORD_SOT;
        }
        if (sp->rx_count == RT6XX_FLEXCOMM_FIFO_SIZE) {
            sp->fifoerr |= SPI_FIFOSTAT_RXERR;
        } else {
            unsigned tail = (sp->rx_head + sp->rx_count) %
                            RT6XX_FLEXCOMM_FIFO_SIZE;

            sp->rx_fifo[tail] = entry;
            sp->rx_count++;
        }
    }

    if (val & SPI_FIFOWR_EOT) {
        rt6xx_flexcomm_spi_set_ssel(s, 0);
    }
}

static uint64_t rt6xx_flexcomm_spi_read(Rt6xxFlexcommState *s, hwaddr offset)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;
    uint32_t value;

    switch (offset) {
    case 0x400:
        return sp->cfg;
    case 0x404:
        return sp->dly;
    case 0x408:
        return rt6xx_flexcomm_spi_stat(s);
    case 0x40c:
    case 0x410:
        return sp->inten;
    case 0x424:
        return sp->div;
    case 0x428:
        return rt6xx_flexcomm_spi_peripheral_irq(s);
    case 0xe00:
        return sp->fifocfg | SPI_FIFOCFG_SIZE;
    case 0xe04:
        return rt6xx_flexcomm_spi_fifostat(s);
    case 0xe08:
        return sp->fifotrig;
    case 0xe10:
    case 0xe14:
        return sp->fifointenset;
    case 0xe18:
        return rt6xx_flexcomm_spi_fifointstat(s);
    case 0xe20:
        return 0;
    case 0xe30:
    case 0xe40:
        if (!sp->rx_count) {
            sp->fifoerr |= SPI_FIFOSTAT_RXERR;
            return 0;
        }
        value = sp->rx_fifo[sp->rx_head];
        if (offset == 0xe30) {
            sp->rx_head = (sp->rx_head + 1) % RT6XX_FLEXCOMM_FIFO_SIZE;
            sp->rx_count--;
        }
        return value;
    case 0xe48:
        return RT6XX_FLEXCOMM_FIFO_SIZE;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented SPI read at 0x%03"
                      HWADDR_PRIx "\n", TYPE_RT6XX_FLEXCOMM, offset);
        return 0;
    }
}

static void rt6xx_flexcomm_spi_write(Rt6xxFlexcommState *s, hwaddr offset,
                                     uint32_t val)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;

    switch (offset) {
    case 0x400: {
        uint32_t cfg = val & SPI_CFG_MASK;

        if (!(cfg & SPI_CFG_ENABLE) || !(cfg & SPI_CFG_MASTER) ||
            !(sp->cfg & SPI_CFG_MASTER)) {
            rt6xx_flexcomm_spi_set_ssel(s, 0);
        }
        sp->cfg = cfg;
        if ((cfg & SPI_CFG_ENABLE) && !(cfg & SPI_CFG_MASTER)) {
            rt6xx_flexcomm_spi_recompute_slave_ssel(s);
        } else {
            qemu_set_irq(s->spi_slave_miso, 0);
        }
        rt6xx_flexcomm_spi_update_cs(s);
        break;
    }
    case 0x404:
        sp->dly = val & 0xffff;
        break;
    case 0x408:
        sp->stat &= ~(val & SPI_STAT_W1C_MASK);
        if (val & SPI_STAT_ENDTRANSFER) {
            rt6xx_flexcomm_spi_set_ssel(s, 0);
        }
        break;
    case 0x40c:
        sp->inten |= val & SPI_INTEN_MASK;
        break;
    case 0x410:
        sp->inten &= ~(val & SPI_INTEN_MASK);
        break;
    case 0x424:
        sp->div = val & 0xffff;
        break;
    case 0xe00:
        if (val & (BIT(12) | BIT(13))) {
            qemu_log_mask(LOG_UNIMP, "%s: SPI DMA is not implemented\n",
                          TYPE_RT6XX_FLEXCOMM);
        }
        sp->fifocfg = val & (FIFOCFG_MASK &
                             ~(FIFOCFG_EMPTYTX | FIFOCFG_EMPTYRX | 0x30));
        if (val & FIFOCFG_EMPTYTX) {
            sp->tx_head = 0;
            sp->tx_count = 0;
        }
        if (val & FIFOCFG_EMPTYRX) {
            sp->rx_head = 0;
            sp->rx_count = 0;
        }
        break;
    case 0xe04:
        sp->fifoerr &= ~(val & (SPI_FIFOSTAT_TXERR | SPI_FIFOSTAT_RXERR));
        break;
    case 0xe08:
        sp->fifotrig = val & 0x000f0f03;
        break;
    case 0xe10:
        sp->fifointenset |= val & FIFOINT_MASK;
        break;
    case 0xe14:
        sp->fifointenset &= ~(val & FIFOINT_MASK);
        break;
    case 0xe20:
        if (sp->cfg & SPI_CFG_MASTER) {
            rt6xx_flexcomm_spi_frame(s, val);
        } else {
            rt6xx_flexcomm_spi_slave_queue(s, val);
        }
        break;
    case 0x428:
    case 0xe18:
    case 0xe30:
    case 0xe40:
    case 0xe48:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented SPI write at 0x%03"
                      HWADDR_PRIx " value 0x%08x\n",
                      TYPE_RT6XX_FLEXCOMM, offset, val);
        break;
    }
}

static void rt6xx_flexcomm_spi_reset_state(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommSpiState *sp = &s->spi;

    sp->cfg = 0;
    sp->dly = 0;
    sp->stat = 0;
    sp->inten = 0;
    sp->div = 0;
    sp->fifocfg = 0;
    sp->fifoerr = 0;
    sp->fifotrig = 0;
    sp->fifointenset = 0;
    sp->slave_control = 7U << 24;
    memset(sp->tx_fifo, 0, sizeof(sp->tx_fifo));
    memset(sp->rx_fifo, 0, sizeof(sp->rx_fifo));
    sp->tx_head = 0;
    sp->tx_count = 0;
    sp->rx_head = 0;
    sp->rx_count = 0;
    sp->ssel = 0;
    sp->ssel_input = 0xf;
    sp->slave_sot = false;
    qemu_set_irq(s->spi_slave_miso, 0);
    rt6xx_flexcomm_spi_update_cs(s);
}

/*
 * I2S model.  The board audio link (hw/audio/rt685_audio_link) moves
 * one 32-bit stereo FIFO word per direction per audio frame through
 * the endpoint interface implemented at the bottom of this file; the
 * MMIO side below provides registers, FIFOs, watermark triggers and
 * the level-sensitive shared IRQ.  FIFOINTSTAT reports the raw
 * conditions AND the interrupt enable mask, matching fsl_i2s.c, and
 * the level conditions are always derived from the current FIFO
 * counts, so a W1C write to FIFOSTAT acknowledges the sampled
 * condition but it immediately reasserts while still true.
 */
static bool rt6xx_flexcomm_i2s_selected(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommPeriph sel = rt6xx_flexcomm_selected(s);

    return sel == RT6XX_FLEXCOMM_PERIPH_I2S_TX ||
           sel == RT6XX_FLEXCOMM_PERIPH_I2S_RX;
}

static bool rt6xx_flexcomm_i2s_tx_active(Rt6xxFlexcommState *s)
{
    return s->clock_enabled && !s->reset_asserted &&
           rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_I2S_TX &&
           (s->i2s.cfg1 & I2S_CFG1_MAINENABLE) &&
           (s->i2s.fifocfg & FIFOCFG_ENABLETX);
}

static bool rt6xx_flexcomm_i2s_rx_active(Rt6xxFlexcommState *s)
{
    return s->clock_enabled && !s->reset_asserted &&
           rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_I2S_RX &&
           (s->i2s.cfg1 & I2S_CFG1_MAINENABLE) &&
           (s->i2s.fifocfg & FIFOCFG_ENABLERX);
}

static void rt6xx_flexcomm_i2s_link_update(Rt6xxFlexcommState *s)
{
    rt685_audio_link_endpoint_changed(s->audio_link);
}

static uint32_t rt6xx_flexcomm_i2s_fifointstat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;
    uint32_t raw = i->fifoerr;
    unsigned tx_trigger = extract32(i->fifotrig, 8, 4);
    unsigned rx_trigger = extract32(i->fifotrig, 16, 4);

    if ((i->fifotrig & I2S_FIFOTRIG_TXLVLENA) &&
        i->tx_count <= tx_trigger) {
        raw |= I2S_FIFOINT_TXLVL;
    }
    if ((i->fifotrig & I2S_FIFOTRIG_RXLVLENA) &&
        i->rx_count > rx_trigger) {
        raw |= I2S_FIFOINT_RXLVL;
    }
    return raw & i->fifointenset;
}

static uint32_t rt6xx_flexcomm_i2s_fifostat(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;
    uint32_t value = i->fifoerr;

    if (!i->tx_count) {
        value |= I2S_FIFOSTAT_TXEMPTY;
    }
    if (i->tx_count < RT6XX_FLEXCOMM_I2S_FIFO_SIZE) {
        value |= I2S_FIFOSTAT_TXNOTFULL;
    }
    if (i->rx_count) {
        value |= I2S_FIFOSTAT_RXNOTEMPTY;
    }
    if (i->rx_count == RT6XX_FLEXCOMM_I2S_FIFO_SIZE) {
        value |= I2S_FIFOSTAT_RXFULL;
    }
    value |= (uint32_t)i->tx_count << 8;
    value |= (uint32_t)i->rx_count << 16;
    return value;
}

static void rt6xx_flexcomm_i2s_update_dreq(Rt6xxFlexcommState *s);

static uint64_t rt6xx_flexcomm_i2s_read(Rt6xxFlexcommState *s, hwaddr offset)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;

    switch (offset) {
    case 0xc00:
        return i->cfg1;
    case 0xc04:
        return i->cfg2;
    case 0xc08: {
        uint32_t value = i->stat & I2S_STAT_SLVFRMERR;

        if (s->clock_enabled && !s->reset_asserted &&
            (i->cfg1 & I2S_CFG1_MAINENABLE)) {
            value |= BIT(0);     /* BUSY */
        }
        if (i->cfg1 & I2S_CFG1_DATAPAUSE) {
            value |= BIT(3);
        }
        return value;
    }
    case 0xc1c:
        return i->div;
    case 0xe00:
        return i->fifocfg | I2S_FIFOCFG_SIZE;
    case 0xe04:
        return rt6xx_flexcomm_i2s_fifostat(s);
    case 0xe08:
        return i->fifotrig;
    case 0xe10:
    case 0xe14:
        return i->fifointenset;
    case 0xe18:
        return rt6xx_flexcomm_i2s_fifointstat(s);
    case 0xe30:
    case 0xe40: {
        uint32_t value;

        if (!i->rx_count) {
            return 0;
        }
        value = i->rx_fifo[i->rx_head];
        if (offset == 0xe30) {
            i->rx_head = (i->rx_head + 1) % RT6XX_FLEXCOMM_I2S_FIFO_SIZE;
            i->rx_count--;
            rt6xx_flexcomm_update_irq(s);
            rt6xx_flexcomm_i2s_update_dreq(s);
        }
        return value;
    }
    case 0xe48:
        return RT6XX_FLEXCOMM_I2S_FIFO_SIZE;
    default:
        if (offset >= 0xc20 && offset <= 0xdf3) {
            qemu_log_mask(LOG_UNIMP, "%s: I2S secondary channels are not "
                          "implemented (read at 0x%03" HWADDR_PRIx ")\n",
                          TYPE_RT6XX_FLEXCOMM, offset);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented I2S read at 0x%03"
                          HWADDR_PRIx "\n", TYPE_RT6XX_FLEXCOMM, offset);
        }
        return 0;
    }
}

static void rt6xx_flexcomm_i2s_write(Rt6xxFlexcommState *s, hwaddr offset,
                                     uint32_t val)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;
    bool activity_changed = false;

    switch (offset) {
    case 0xc00: {
        uint32_t cfg1 = val & I2S_CFG1_MASK;

        activity_changed = (cfg1 & I2S_CFG1_MAINENABLE) !=
                           (i->cfg1 & I2S_CFG1_MAINENABLE);
        i->cfg1 = cfg1;
        rt6xx_flexcomm_i2s_update_dreq(s);
        break;
    }
    case 0xc04:
        i->cfg2 = val & I2S_CFG2_MASK;
        break;
    case 0xc08:
        i->stat &= ~(val & I2S_STAT_SLVFRMERR);
        break;
    case 0xc1c:
        i->div = val & I2S_DIV_MASK;
        break;
    case 0xe00:
        /* BIT(12)=TXDMA, BIT(13)=RXDMA wire to DMA channel DREQ inputs. */
        if (val & I2S_FIFOCFG_PACK48) {
            qemu_log_mask(LOG_UNIMP, "%s: I2S 48-bit packing is not "
                          "implemented\n", TYPE_RT6XX_FLEXCOMM);
        }
        activity_changed =
            (val & (FIFOCFG_ENABLETX | FIFOCFG_ENABLERX)) !=
            (i->fifocfg & (FIFOCFG_ENABLETX | FIFOCFG_ENABLERX));
        i->fifocfg = val & I2S_FIFOCFG_MASK;
        if (val & FIFOCFG_EMPTYTX) {
            i->tx_head = 0;
            i->tx_count = 0;
        }
        if (val & FIFOCFG_EMPTYRX) {
            i->rx_head = 0;
            i->rx_count = 0;
        }
        rt6xx_flexcomm_i2s_update_dreq(s);
        break;
    case 0xe04:
        /* TXERR and RXERR are write-one-to-clear; the level bits
         * (including the driver's FIFOSTAT = TXLVL/RXLVL(1) writes)
         * are derived from the FIFO counts and ignore writes.
         */
        i->fifoerr &= ~(val & (I2S_FIFOSTAT_TXERR | I2S_FIFOSTAT_RXERR));
        break;
    case 0xe08:
        i->fifotrig = val & I2S_FIFOTRIG_MASK;
        rt6xx_flexcomm_i2s_update_dreq(s);
        break;
    case 0xe10:
        i->fifointenset |= val & I2S_FIFOINT_MASK;
        break;
    case 0xe14:
        i->fifointenset &= ~(val & I2S_FIFOINT_MASK);
        break;
    case 0xe20:
        if (i->tx_count == RT6XX_FLEXCOMM_I2S_FIFO_SIZE) {
            i->fifoerr |= I2S_FIFOSTAT_TXERR;   /* TX overflow */
        } else {
            unsigned tail = (i->tx_head + i->tx_count) %
                            RT6XX_FLEXCOMM_I2S_FIFO_SIZE;

            i->tx_fifo[tail] = val;
            i->tx_count++;
        }
        rt6xx_flexcomm_i2s_update_dreq(s);
        break;
    case 0xe24:
    case 0xe34:
    case 0xe44:
        qemu_log_mask(LOG_UNIMP, "%s: I2S 48-bit FIFO access at 0x%03"
                      HWADDR_PRIx " is not implemented\n",
                      TYPE_RT6XX_FLEXCOMM, offset);
        break;
    default:
        if (offset >= 0xc20 && offset <= 0xdf3) {
            qemu_log_mask(LOG_UNIMP, "%s: I2S secondary channels are not "
                          "implemented (write 0x%08x at 0x%03" HWADDR_PRIx
                          ")\n", TYPE_RT6XX_FLEXCOMM, val, offset);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented I2S write at 0x%03"
                          HWADDR_PRIx " value 0x%08x\n",
                          TYPE_RT6XX_FLEXCOMM, offset, val);
        }
        break;
    }
    if (activity_changed) {
        rt6xx_flexcomm_i2s_link_update(s);
    }
    /*
     * FIFO level sources are derived from the current register and FIFO
     * state.  In particular, enabling TXLVL while the FIFO is empty must
     * assert the interrupt immediately, and filling the FIFO in the ISR
     * must deassert it once the watermark is exceeded.
     */
    rt6xx_flexcomm_update_irq(s);
}

/*
 * Recompute and output DMA DREQ levels for both TX and RX directions.
 *
 * TX DREQ: asserted when I2S + DMATX + TXLVLENA enabled AND
 *          tx_count <= TXLVL (TXLVL=0 means "empty" triggers immediately).
 * RX DREQ: asserted when I2S + DMARX + RXLVLENA enabled AND
 *          rx_count > RXLVL (RXLVL=0 means "1 entry" triggers).
 *
 * Called whenever FIFO counts or any enabling register changes.
 */
static void rt6xx_flexcomm_i2s_update_dreq(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;
    bool tx_active, rx_active;
    unsigned tx_trigger, rx_trigger;

    if (!rt6xx_flexcomm_i2s_selected(s)) {
        /* Not I2S mode — deassert both DREQs. */
        qemu_set_irq(s->i2s.dreq_tx, 0);
        qemu_set_irq(s->i2s.dreq_rx, 0);
        return;
    }

    if (!(i->cfg1 & I2S_CFG1_MAINENABLE)) {
        /* I2S main not enabled — deassert both. */
        qemu_set_irq(s->i2s.dreq_tx, 0);
        qemu_set_irq(s->i2s.dreq_rx, 0);
        return;
    }

    tx_trigger = extract32(i->fifotrig, 8, 4);  /* TXLVL field */
    rx_trigger = extract32(i->fifotrig, 16, 4); /* RXLVL field */

    /* TX DREQ: active when I2S + DMATX + TXLVLENA and tx_count <= TXLVL. */
    tx_active = (i->fifocfg & FIFOCFG_ENABLETX) &&
                (i->fifocfg & I2S_FIFOCFG_TXDMA) &&
                (i->fifotrig & I2S_FIFOTRIG_TXLVLENA) &&
                (i->tx_count <= tx_trigger);
    qemu_set_irq(s->i2s.dreq_tx, tx_active ? 1 : 0);

    /* RX DREQ: active when I2S + DMARX + RXLVLENA and rx_count > RXLVL. */
    rx_active = (i->fifocfg & FIFOCFG_ENABLERX) &&
                (i->fifocfg & I2S_FIFOCFG_RXDMA) &&
                (i->fifotrig & I2S_FIFOTRIG_RXLVLENA) &&
                (i->rx_count > rx_trigger);
    qemu_set_irq(s->i2s.dreq_rx, rx_active ? 1 : 0);

    trace_rt6xx_flexcomm_i2s_dreq(DEVICE(s)->canonical_path,
                                  tx_active || rx_active);
}

/*
 * Board audio endpoint.  The link only calls these while the
 * corresponding direction reports itself active.
 */
static bool rt6xx_flexcomm_endpoint_push(Rt685AudioEndpoint *ep,
                                         uint32_t word)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(ep);
    Rt6xxFlexcommI2sState *i = &s->i2s;

    if (rt6xx_flexcomm_selected(s) != RT6XX_FLEXCOMM_PERIPH_I2S_RX) {
        return false;
    }
    if (i->rx_count == RT6XX_FLEXCOMM_I2S_FIFO_SIZE) {
        i->fifoerr |= I2S_FIFOSTAT_RXERR;       /* RX overflow */
        rt6xx_flexcomm_update_irq(s);
        return false;
    }
    i->rx_fifo[(i->rx_head + i->rx_count) % RT6XX_FLEXCOMM_I2S_FIFO_SIZE] =
        word;
    i->rx_count++;
    rt6xx_flexcomm_update_irq(s);
    rt6xx_flexcomm_i2s_update_dreq(s);
    return true;
}

static bool rt6xx_flexcomm_endpoint_pop(Rt685AudioEndpoint *ep,
                                        uint32_t *word)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(ep);
    Rt6xxFlexcommI2sState *i = &s->i2s;

    if (rt6xx_flexcomm_selected(s) != RT6XX_FLEXCOMM_PERIPH_I2S_TX) {
        *word = 0;
        return false;
    }
    if (!i->tx_count) {
        *word = 0;
        if (!(i->fifocfg & I2S_FIFOCFG_TXI2SE0)) {
            i->fifoerr |= I2S_FIFOSTAT_TXERR;   /* TX underrun */
        }
        rt6xx_flexcomm_update_irq(s);
        return false;
    }
    *word = i->tx_fifo[i->tx_head];
    i->tx_head = (i->tx_head + 1) % RT6XX_FLEXCOMM_I2S_FIFO_SIZE;
    i->tx_count--;
    rt6xx_flexcomm_update_irq(s);

    /* Recompute TX DREQ after sample consumption. */
    if (i->fifocfg & I2S_FIFOCFG_TXDMA) {
        rt6xx_flexcomm_i2s_update_dreq(s);
    }
    return true;
}

static bool rt6xx_flexcomm_endpoint_is_active(Rt685AudioEndpoint *ep,
                                              Rt685AudioDirection dir)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(ep);

    return dir == RT685_AUDIO_DIR_RX ? rt6xx_flexcomm_i2s_rx_active(s) :
                                       rt6xx_flexcomm_i2s_tx_active(s);
}

/*
 * Push a sample into the Flexcomm I2S TX FIFO.  Called by the DMA
 * model when DOFF==0 (fixed destination write to FIFO).
 */
bool rt6xx_flexcomm_i2s_fifo_push(Rt6xxFlexcommState *s, uint32_t sample)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;

    if (rt6xx_flexcomm_selected(s) != RT6XX_FLEXCOMM_PERIPH_I2S_TX) {
        return false;
    }
    if (!rt6xx_flexcomm_i2s_tx_active(s)) {
        return false;
    }
    if (i->tx_count == RT6XX_FLEXCOMM_I2S_FIFO_SIZE) {
        i->fifoerr |= I2S_FIFOSTAT_TXERR;   /* TX overflow */
        rt6xx_flexcomm_update_irq(s);
        return false;
    }
    unsigned tail = (i->tx_head + i->tx_count) % RT6XX_FLEXCOMM_I2S_FIFO_SIZE;

    i->tx_fifo[tail] = sample;
    i->tx_count++;
    trace_rt6xx_flexcomm_i2s_dma_push(DEVICE(s)->canonical_path, sample);
    rt6xx_flexcomm_update_irq(s);

    /* Recompute DREQ after FIFO count changes. */
    rt6xx_flexcomm_i2s_update_dreq(s);
    return true;
}



static void rt6xx_flexcomm_i2s_reset_state(Rt6xxFlexcommState *s)
{
    Rt6xxFlexcommI2sState *i = &s->i2s;

    i->cfg1 = 0;
    i->cfg2 = 0;
    i->stat = 0;
    i->div = 0;
    i->fifocfg = 0;
    i->fifoerr = 0;
    i->fifotrig = 0;
    i->fifointenset = 0;
    memset(i->tx_fifo, 0, sizeof(i->tx_fifo));
    memset(i->rx_fifo, 0, sizeof(i->rx_fifo));
    i->tx_head = 0;
    i->tx_count = 0;
    i->rx_head = 0;
    i->rx_count = 0;
    rt6xx_flexcomm_i2s_update_dreq(s);
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
            if (s->spi.cfg & SPI_CFG_ENABLE) {
                uint32_t peripheral =
                    rt6xx_flexcomm_spi_peripheral_irq(s);
                uint32_t fifo = rt6xx_flexcomm_spi_fifointstat(s) &
                                s->spi.fifointenset & FIFOINT_MASK;

                sources = peripheral | fifo;
                level = sources != 0;
            }
            break;
        case RT6XX_FLEXCOMM_PERIPH_I2S_TX:
        case RT6XX_FLEXCOMM_PERIPH_I2S_RX:
            if (s->i2s.cfg1 & I2S_CFG1_MAINENABLE) {
                sources = rt6xx_flexcomm_i2s_fifointstat(s);
                level = sources != 0;
            }
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

    rt6xx_flexcomm_spi_reset_state(s);
    rt6xx_flexcomm_i2s_reset_state(s);

    s->pselid = rt6xx_flexcomm_caps_present(s);
}

static void rt6xx_flexcomm_set_clock(void *opaque, int n, int level)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(opaque);

    s->clock_enabled = level;
    if (rt6xx_flexcomm_spi_slave_active(s)) {
        rt6xx_flexcomm_spi_recompute_slave_ssel(s);
    } else if (!level) {
        rt6xx_flexcomm_spi_set_ssel(s, 0);
        qemu_set_irq(s->spi_slave_miso, 0);
    }
    rt6xx_flexcomm_i2s_link_update(s);
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
    rt6xx_flexcomm_i2s_link_update(s);
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
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_I2S_TX ||
                  sel == RT6XX_FLEXCOMM_PERIPH_I2S_RX;
        break;
    default:                 /* 0xe00..0xff7 shared FIFO windows */
        allowed = sel == RT6XX_FLEXCOMM_PERIPH_USART ||
                  sel == RT6XX_FLEXCOMM_PERIPH_SPI ||
                  rt6xx_flexcomm_i2s_selected(s);
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

    if (rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_SPI &&
        ((offset >= 0x400 && offset <= 0x7ff) ||
         (offset >= 0xe00 && offset < 0xff8))) {
        uint64_t result = rt6xx_flexcomm_spi_read(s, offset);

        rt6xx_flexcomm_update_irq(s);
        return result;
    }

    if (rt6xx_flexcomm_i2s_selected(s) &&
        ((offset >= 0xc00 && offset <= 0xdff) ||
         (offset >= 0xe00 && offset < 0xff8))) {
        uint64_t result = rt6xx_flexcomm_i2s_read(s, offset);

        rt6xx_flexcomm_update_irq(s);
        return result;
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

    if (rt6xx_flexcomm_selected(s) == RT6XX_FLEXCOMM_PERIPH_SPI &&
        ((offset >= 0x400 && offset <= 0x7ff) ||
         (offset >= 0xe00 && offset < 0xff8))) {
        rt6xx_flexcomm_spi_write(s, offset, val);
        rt6xx_flexcomm_update_irq(s);
        return;
    }

    if (rt6xx_flexcomm_i2s_selected(s) &&
        ((offset >= 0xc00 && offset <= 0xdff) ||
         (offset >= 0xe00 && offset < 0xff8))) {
        rt6xx_flexcomm_i2s_write(s, offset, val);
        rt6xx_flexcomm_update_irq(s);
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
                if (old_sel == RT6XX_FLEXCOMM_PERIPH_SPI) {
                    rt6xx_flexcomm_spi_set_ssel(s, 0);
                    qemu_set_irq(s->spi_slave_miso, 0);
                }
                if (sel == RT6XX_FLEXCOMM_PERIPH_SPI &&
                    rt6xx_flexcomm_spi_slave_active(s)) {
                    rt6xx_flexcomm_spi_recompute_slave_ssel(s);
                }
                rt6xx_flexcomm_i2s_link_update(s);
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
    if (version_id < 3) {
        rt6xx_flexcomm_spi_reset_state(s);
    } else {
        rt6xx_flexcomm_spi_update_cs(s);
    }
    if (version_id < 5) {
        rt6xx_flexcomm_i2s_reset_state(s);
    }
    rt6xx_flexcomm_update_irq(s);
    rt6xx_flexcomm_i2s_link_update(s);
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
    .version_id = 5,
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
        VMSTATE_UINT32_V(spi.cfg, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.dly, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.stat, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.inten, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.div, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.fifocfg, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.fifoerr, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.fifotrig, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.fifointenset, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_ARRAY_V(spi.rx_fifo, Rt6xxFlexcommState,
                               RT6XX_FLEXCOMM_FIFO_SIZE, 3),
        VMSTATE_UINT8_V(spi.rx_head, Rt6xxFlexcommState, 3),
        VMSTATE_UINT8_V(spi.rx_count, Rt6xxFlexcommState, 3),
        VMSTATE_UINT8_V(spi.ssel, Rt6xxFlexcommState, 3),
        VMSTATE_UINT32_V(spi.slave_control, Rt6xxFlexcommState, 4),
        VMSTATE_UINT32_ARRAY_V(spi.tx_fifo, Rt6xxFlexcommState,
                               RT6XX_FLEXCOMM_FIFO_SIZE, 4),
        VMSTATE_UINT8_V(spi.tx_head, Rt6xxFlexcommState, 4),
        VMSTATE_UINT8_V(spi.tx_count, Rt6xxFlexcommState, 4),
        VMSTATE_UINT8_V(spi.ssel_input, Rt6xxFlexcommState, 4),
        VMSTATE_BOOL_V(spi.slave_sot, Rt6xxFlexcommState, 4),
        VMSTATE_UINT32_V(i2s.cfg1, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.cfg2, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.stat, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.div, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.fifocfg, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.fifoerr, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.fifotrig, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_V(i2s.fifointenset, Rt6xxFlexcommState, 5),
        VMSTATE_UINT32_ARRAY_V(i2s.tx_fifo, Rt6xxFlexcommState,
                               RT6XX_FLEXCOMM_I2S_FIFO_SIZE, 5),
        VMSTATE_UINT32_ARRAY_V(i2s.rx_fifo, Rt6xxFlexcommState,
                               RT6XX_FLEXCOMM_I2S_FIFO_SIZE, 5),
        VMSTATE_UINT8_V(i2s.tx_head, Rt6xxFlexcommState, 5),
        VMSTATE_UINT8_V(i2s.tx_count, Rt6xxFlexcommState, 5),
        VMSTATE_UINT8_V(i2s.rx_head, Rt6xxFlexcommState, 5),
        VMSTATE_UINT8_V(i2s.rx_count, Rt6xxFlexcommState, 5),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_flexcomm_realize(DeviceState *dev, Error **errp)
{
    Rt6xxFlexcommState *s = RT6XX_FLEXCOMM(dev);
    const char *component = object_get_canonical_path_component(OBJECT(dev));
    g_autofree char *i2c_bus_name = NULL;
    g_autofree char *spi_bus_name = NULL;

    if (s->capabilities & RT6XX_FLEXCOMM_CAP_I2C) {
        i2c_bus_name = g_strdup_printf("%s.i2c", component);
        s->i2c_bus = i2c_init_bus(dev, i2c_bus_name);
    }
    if (s->capabilities & RT6XX_FLEXCOMM_CAP_SPI) {
        spi_bus_name = g_strdup_printf("%s.spi", component);
        s->spi_bus = ssi_create_bus(dev, spi_bus_name);
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
    /* DMA DREQ outputs: TX and RX direction. */
    qdev_init_gpio_out_named(dev, &s->i2s.dreq_tx, "dma-dreq-tx", 1);
    qdev_init_gpio_out_named(dev, &s->i2s.dreq_rx, "dma-dreq-rx", 1);
    qdev_init_gpio_in_named(dev, rt6xx_flexcomm_spi_slave_ssel,
                            RT6XX_FLEXCOMM_SPI_SLAVE_SSEL,
                            SPI_SSEL_COUNT);
    qdev_init_gpio_in_named(dev, rt6xx_flexcomm_spi_slave_frame,
                            RT6XX_FLEXCOMM_SPI_SLAVE_FRAME, 1);
    qdev_init_gpio_out_named(dev, &s->spi_slave_miso,
                             RT6XX_FLEXCOMM_SPI_SLAVE_MISO, 1);
}

static const Property rt6xx_flexcomm_properties[] = {
    DEFINE_PROP_CHR("chardev", Rt6xxFlexcommState, chr),
    DEFINE_PROP_UINT32("capabilities", Rt6xxFlexcommState, capabilities,
                       RT6XX_FLEXCOMM_CAP_ALL),
    DEFINE_PROP_LINK("audio-link", Rt6xxFlexcommState, audio_link,
                     TYPE_RT685_AUDIO_LINK, Rt685AudioLink *),
};

static void rt6xx_flexcomm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    Rt685AudioEndpointClass *ec = RT685_AUDIO_ENDPOINT_CLASS(klass);

    dc->realize = rt6xx_flexcomm_realize;
    dc->vmsd = &vmstate_rt6xx_flexcomm;
    device_class_set_legacy_reset(dc, rt6xx_flexcomm_reset);
    device_class_set_props(dc, rt6xx_flexcomm_properties);

    ec->push = rt6xx_flexcomm_endpoint_push;
    ec->pop = rt6xx_flexcomm_endpoint_pop;
    ec->is_active = rt6xx_flexcomm_endpoint_is_active;
}

static const TypeInfo rt6xx_flexcomm_info = {
    .name = TYPE_RT6XX_FLEXCOMM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxFlexcommState),
    .instance_init = rt6xx_flexcomm_init,
    .class_init = rt6xx_flexcomm_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_RT685_AUDIO_ENDPOINT },
        { }
    },
};

static void rt6xx_flexcomm_register_types(void)
{
    type_register_static(&rt6xx_flexcomm_info);
}

type_init(rt6xx_flexcomm_register_types)
