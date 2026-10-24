/*
 * NXP i.MX RT6xx Flexcomm
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RT6XX_FLEXCOMM_H
#define HW_CHAR_RT6XX_FLEXCOMM_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RT6XX_FLEXCOMM "rt6xx-flexcomm"
OBJECT_DECLARE_SIMPLE_TYPE(Rt6xxFlexcommState, RT6XX_FLEXCOMM)

#define RT6XX_FLEXCOMM_FIFO_SIZE 16

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
} Rt6xxFlexcommPeriph;

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

    Rt6xxFlexcommUsartState usart;
};

#endif
