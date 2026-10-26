/*
 * NXP i.MX RT6xx Flexcomm
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RT6XX_FLEXCOMM_H
#define HW_CHAR_RT6XX_FLEXCOMM_H

#include "chardev/char-fe.h"
#include "hw/audio/rt685_audio_link.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_RT6XX_FLEXCOMM "rt6xx-flexcomm"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxFlexcommState, RT6XX_FLEXCOMM)

#define RT6XX_FLEXCOMM_FIFO_SIZE 16
/* The Flexcomm I2S FIFOs are eight 32-bit words deep. */
#define RT6XX_FLEXCOMM_I2S_FIFO_SIZE 8
/*
 * Slave-side wiring uses physical SSEL levels and one frame-level data GPIO.
 * Driving SPI_SLAVE_FRAME transfers its low 16 bits as MOSI; the synchronous
 * MISO result is driven on SPI_SLAVE_MISO as the GPIO level.
 */
#define RT6XX_FLEXCOMM_SPI_SLAVE_SSEL "spi-slave-ssel"
#define RT6XX_FLEXCOMM_SPI_SLAVE_FRAME "spi-slave-frame"
#define RT6XX_FLEXCOMM_SPI_SLAVE_MISO "spi-slave-miso"

/*
 * Protocol blocks physically present in one Flexcomm instance.  The
 * RT685S integrates USART/SPI/I2C/I2S in Flexcomm 0..7, SPI only in
 * Flexcomm 14 and I2C only in Flexcomm 15.
 */
#define RT6XX_FLEXCOMM_CAP_USART  BIT(0)
#define RT6XX_FLEXCOMM_CAP_SPI    BIT(1)
#define RT6XX_FLEXCOMM_CAP_I2C    BIT(2)
#define RT6XX_FLEXCOMM_CAP_I2S    BIT(3)
#define RT6XX_FLEXCOMM_CAP_ALL \
    (RT6XX_FLEXCOMM_CAP_USART | RT6XX_FLEXCOMM_CAP_SPI | \
     RT6XX_FLEXCOMM_CAP_I2C | RT6XX_FLEXCOMM_CAP_I2S)

/* PSELID.PERSEL values. */
typedef enum Rt6xxFlexcommPeriph {
    RT6XX_FLEXCOMM_PERIPH_NONE = 0,
    RT6XX_FLEXCOMM_PERIPH_USART = 1,
    RT6XX_FLEXCOMM_PERIPH_SPI = 2,
    RT6XX_FLEXCOMM_PERIPH_I2C = 3,
    RT6XX_FLEXCOMM_PERIPH_I2S_TX = 4,
    RT6XX_FLEXCOMM_PERIPH_I2S_RX = 5,
} Rt6xxFlexcommPeriph;

/* I2C master state machine phase; matches STAT.MSTSTATE encoding. */
typedef enum Rt6xxFlexcommI2cPhase {
    RT6XX_I2C_IDLE = 0,
    RT6XX_I2C_RX_READY = 1,
    RT6XX_I2C_TX_READY = 2,
    RT6XX_I2C_NACK_ADDR = 3,
    RT6XX_I2C_NACK_DATA = 4,
} Rt6xxFlexcommI2cPhase;

typedef struct Rt6xxFlexcommI2cState {
    uint32_t cfg;
    uint32_t stat;          /* sticky write-one-to-clear flags only */
    uint32_t inten;
    uint32_t timeout;       /* TO field; TOMIN always reads as 0xf */
    uint32_t clkdiv;
    uint32_t mstctl;        /* MSTDMA storage; command bits self-clear */
    uint32_t mstime;
    uint32_t mstdat;
    uint32_t slvctl;
    uint32_t slvdat;
    uint32_t slvadr[4];
    uint32_t slvqual0;
    uint8_t phase;
    bool bus_active;        /* this master owns an open QEMU transfer */
} Rt6xxFlexcommI2cState;

/*
 * SPI state.  Master frames complete synchronously.  In slave mode the TX
 * FIFO is filled by FIFOWR and consumed by externally supplied frames.  The
 * RX FIFO stores complete FIFORD words so received data and its SSEL/SOT
 * metadata travel together.
 */
typedef struct Rt6xxFlexcommSpiState {
    uint32_t cfg;
    uint32_t dly;
    uint32_t stat;          /* sticky SSA/SSD flags only */
    uint32_t inten;
    uint32_t div;
    uint32_t fifocfg;
    uint32_t fifoerr;       /* sticky TXERR/RXERR flags */
    uint32_t fifotrig;
    uint32_t fifointenset;
    uint32_t slave_control; /* most recently consumed slave frame control */
    uint32_t tx_fifo[RT6XX_FLEXCOMM_FIFO_SIZE];
    uint32_t rx_fifo[RT6XX_FLEXCOMM_FIFO_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
    uint8_t ssel;           /* mask of currently asserted SSELs */
    uint8_t ssel_input;     /* physical levels on slave SSEL inputs */
    bool slave_sot;         /* next received frame starts a transfer */
} Rt6xxFlexcommSpiState;

/*
 * I2S state.  One instance is wired as TX (PERSEL 4) or RX (PERSEL 5);
 * the TX and RX FIFOs are maintained independently, like the SPI
 * personality, so selection changes never alias data between the two
 * directions.  The audio link pops the TX FIFO and pushes the RX FIFO
 * once per board audio frame.
 */
typedef struct Rt6xxFlexcommI2sState {
    uint32_t cfg1;
    uint32_t cfg2;
    uint32_t stat;          /* sticky SLVFRMERR flag only */
    uint32_t div;
    uint32_t fifocfg;
    uint32_t fifoerr;       /* sticky TXERR/RXERR flags */
    uint32_t fifotrig;
    uint32_t fifointenset;
    uint32_t tx_fifo[RT6XX_FLEXCOMM_I2S_FIFO_SIZE];
    uint32_t rx_fifo[RT6XX_FLEXCOMM_I2S_FIFO_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
} Rt6xxFlexcommI2sState;

typedef struct Rt6xxFlexcommUsartState {
    uint32_t cfg;
    uint32_t ctl;
    uint32_t stat;
    uint32_t intenset;
    uint32_t brg;
    uint32_t osr;
    uint32_t addr;
    uint32_t fifocfg;
    uint32_t fifotrig;
    uint32_t fifointenset;
    uint16_t rx_fifo[RT6XX_FLEXCOMM_FIFO_SIZE];
    uint8_t rx_head;
    uint8_t rx_count;
} Rt6xxFlexcommUsartState;

struct Rt6xxFlexcommState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    uint32_t capabilities;
    uint32_t pselid;
    bool clock_enabled;
    bool reset_asserted;

    I2CBus *i2c_bus;
    SSIBus *spi_bus;
    qemu_irq spi_slave_miso;

    Rt6xxFlexcommUsartState usart;
    Rt6xxFlexcommI2cState i2c;
    Rt6xxFlexcommSpiState spi;
    Rt6xxFlexcommI2sState i2s;

    /* EVK board audio link (only wired for the EVK I2S instances). */
    Rt685AudioLink *audio_link;
};

#endif
