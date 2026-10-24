/*
 * NXP i.MX RT6xx clock and system control blocks
 *
 * Register definitions and reset values are derived from the
 * MIMXRT685S_cm33 CMSIS-SVD supplied with this local project.
 *
 * This is a deliberately functional model: it provides register storage,
 * masks, SET/CLR aliases, reset state and the M33 root clock. It does not
 * attempt cycle-accurate oscillator or PLL lock timing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rt6xx_syscon.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * CLKCTL1.PSCCTL0 controls the Flexcomm 0..7 clocks in bits 8..15 and
 * the dedicated Flexcomm 14/15 clocks in bits 22/23.  Model the ten
 * clocked Flexcomm blocks in SDK instance order (0..7, 14, 15); the
 * output index is the logical index used by the SoC, not the hardware
 * Flexcomm number.
 */
static const uint8_t rt6xx_flexcomm_ctl_bit[] = {
    8, 9, 10, 11, 12, 13, 14, 15, 22, 23,
};

#define RT6XX_SYSCON_IOSIZE 0x1000
#define RT6XX_NO_ALIAS 0xff

typedef enum Rt6xxWriteMode {
    RT6XX_WM_MASKED,
    RT6XX_WM_IGNORE,
    RT6XX_WM_SET,
    RT6XX_WM_CLR,
} Rt6xxWriteMode;

typedef struct Rt6xxRegInfo {
    uint16_t offset;
    uint32_t reset;
    uint32_t wmask;
    uint8_t mode;
    uint8_t alias;
} Rt6xxRegInfo;

#define REG(off, rst, mask, wm, a) \
    { (off), (rst), (mask), (wm), (a) }

/* CLKCTL0 */
static const Rt6xxRegInfo clkctl0_regs[] = {
    REG(0x010, 0x00000005, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x014, 0x00000000, 0x0101810c, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x018, 0x00000000, 0xfc000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x040, 0, 0, RT6XX_WM_SET, 0),
    REG(0x044, 0, 0, RT6XX_WM_SET, 1),
    REG(0x048, 0, 0, RT6XX_WM_SET, 2),
    REG(0x070, 0, 0, RT6XX_WM_CLR, 0),
    REG(0x074, 0, 0, RT6XX_WM_CLR, 1),
    REG(0x078, 0, 0, RT6XX_WM_CLR, 2),
    REG(0x100, 0x00020410, 0x000fffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x104, 0x00000000, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x160, 0x00000000, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x168, 0x00000000, 0x00000007, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x190, 0x807bc4d4, 0x807fffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x1c0, 0x00000000, 0x00000001, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x200, 0x00000007, 0x00000007, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x204, 0x00160002, 0x00ff387b, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x20c, 0x0000cafe, 0x0000ffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x210, 0x04dd2f15, 0x3fffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x214, 0x1fffffdb, 0x3fffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x218, 0x80808080, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x240, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x244, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x248, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x24c, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x400, 0, 0x800000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x430, 0, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x434, 0, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x500, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x504, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x620, 0x00000007, 0x00000007, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x624, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x760, 0x00000007, 0x00000007, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x764, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
};

/* CLKCTL1: peripheral gates plus DSP and Flexcomm clock selectors. */
static const Rt6xxRegInfo clkctl1_regs[] = {
    REG(0x010, 0, 0x09c0ff00, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x014, 0, 0xb18100ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x018, 0, 0xc001059f, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x040, 0, 0, RT6XX_WM_SET, 0),
    REG(0x044, 0, 0, RT6XX_WM_SET, 1),
    REG(0x048, 0, 0, RT6XX_WM_SET, 2),
    REG(0x070, 0, 0, RT6XX_WM_CLR, 0),
    REG(0x074, 0, 0, RT6XX_WM_CLR, 1),
    REG(0x078, 0, 0, RT6XX_WM_CLR, 2),
    /* Audio PLL block and FRG PLL divider (see PERI_CLKCTL1.h). */
    REG(0x200, 0x00000007, 0x00000007, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x204, 0x00160002, 0x00ff387b, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x20c, 0x0000cafe, 0x0000ffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x210, 0x04dd2f15, 0x3fffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x214, 0x1fffffdb, 0x3fffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x218, 0x80808080, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x240, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x400, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x404, 0x00000001, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x430, 0, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x434, 0, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x480, 0, 0x00000003, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x500, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x504, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x508, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x520, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x524, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x528, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x540, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x544, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x548, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x560, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x564, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x568, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x580, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x584, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x588, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5a0, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5a4, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5a8, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5c0, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5c4, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5c8, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5e0, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5e4, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x5e8, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6c0, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6c4, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6c8, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6e0, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6e4, 0xff, 0xffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6e8, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6fc, 0x40000000, 0xe00000ff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
};

/* SYSCTL0 */
static const Rt6xxRegInfo sysctl0_regs[] = {
    REG(0x00c, 1, 1, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x010, 0, 0x0003ffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x014, 7, 7, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x030, 0, 0x8000007f, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x034, 0, 0x03ffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x038, 0, 0x03ffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x060, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0x064, 0x000b0000, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0x068, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0x080, 0xffffffff, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x084, 0xffffffff, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x0a0, 0xffffffff, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x100, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x104, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x600, 0x03febc80, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x604, 0x0fffffff, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x608, 0xfffffffe, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x60c, 0xfffffffe, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x610, 0x03febc80, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x614, 0x0fffffff, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x618, 0xfffffffe, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x61c, 0xfffffffe, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x620, 0, 0, RT6XX_WM_SET, 18),
    REG(0x624, 0, 0, RT6XX_WM_SET, 19),
    REG(0x628, 0, 0, RT6XX_WM_SET, 20),
    REG(0x62c, 0, 0, RT6XX_WM_SET, 21),
    REG(0x630, 0, 0, RT6XX_WM_CLR, 18),
    REG(0x634, 0, 0, RT6XX_WM_CLR, 19),
    REG(0x638, 0, 0, RT6XX_WM_CLR, 20),
    REG(0x63c, 0, 0, RT6XX_WM_CLR, 21),
    REG(0x660, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x680, 0, 0xfb7fffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x684, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x6a0, 0, 0, RT6XX_WM_SET, 31),
    REG(0x6a4, 0, 0, RT6XX_WM_SET, 32),
    REG(0x6c0, 0, 0, RT6XX_WM_CLR, 31),
    REG(0x6c4, 0, 0, RT6XX_WM_CLR, 32),
    REG(0x710, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0xf50, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0xf54, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0xf58, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
    REG(0xf5c, 0, 0, RT6XX_WM_IGNORE, RT6XX_NO_ALIAS),
};

/* SYSCTL1 */
static const Rt6xxRegInfo sysctl1_regs[] = {
    REG(0x010, 0, 1, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x030, 5, 0x8000001f, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x040, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x044, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x048, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x04c, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x050, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x054, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x058, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x05c, 0, 0x03030303, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x080, 0, 0x00ff0777, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x084, 0, 0x00ff0777, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
    REG(0x200, 0, 0xffffffff, RT6XX_WM_MASKED, RT6XX_NO_ALIAS),
};

static const Rt6xxRegInfo *rt6xx_table(Rt6xxSysconState *s, size_t *count)
{
    switch (s->variant) {
    case RT6XX_CLKCTL0:
        *count = ARRAY_SIZE(clkctl0_regs);
        return clkctl0_regs;
    case RT6XX_CLKCTL1:
        *count = ARRAY_SIZE(clkctl1_regs);
        return clkctl1_regs;
    case RT6XX_SYSCTL0:
        *count = ARRAY_SIZE(sysctl0_regs);
        return sysctl0_regs;
    case RT6XX_SYSCTL1:
        *count = ARRAY_SIZE(sysctl1_regs);
        return sysctl1_regs;
    default:
        g_assert_not_reached();
    }
}

static int rt6xx_find_register(Rt6xxSysconState *s, hwaddr offset)
{
    size_t count;
    const Rt6xxRegInfo *table = rt6xx_table(s, &count);

    for (size_t i = 0; i < count; i++) {
        if (table[i].offset == offset) {
            return i;
        }
    }
    return -1;
}

static uint32_t rt6xx_reg_value(Rt6xxSysconState *s, hwaddr offset)
{
    int index = rt6xx_find_register(s, offset);

    return index < 0 ? 0 : s->regs[index];
}

static bool rt6xx_is_clock_divider(Rt6xxSysconState *s, hwaddr offset)
{
    if (s->variant == RT6XX_CLKCTL1) {
        return offset == 0x400;
    }
    if (s->variant != RT6XX_CLKCTL0) {
        return false;
    }

    switch (offset) {
    case 0x240:
    case 0x244:
    case 0x248:
    case 0x24c:
    case 0x400:
    case 0x500:
    case 0x504:
    case 0x764:
        return true;
    default:
        return false;
    }
}

static void rt6xx_update_cpuclk(Rt6xxSysconState *s)
{
    uint64_t ffro, first, main;
    unsigned range, sela, selb, div;

    if (s->variant != RT6XX_CLKCTL0) {
        return;
    }

    range = extract32(rt6xx_reg_value(s, 0x100), 18, 2);
    ffro = s->ffro_enabled ? (range == 3 ? 60000000 : 48000000) : 0;
    sela = rt6xx_reg_value(s, 0x430) & 3;
    switch (sela) {
    case 0:
        first = ffro / 4;
        break;
    case 1:
        first = 24000000;
        break;
    case 2:
        first = 1000000;
        break;
    case 3:
        first = ffro;
        break;
    default:
        g_assert_not_reached();
    }

    selb = rt6xx_reg_value(s, 0x434) & 3;
    switch (selb) {
    case 0:
        main = first;
        break;
    case 1:
        main = 16000000;
        break;
    case 2:
        /* Lock timing is not modeled; expose the configured integer PLL. */
        main = 24000000ULL * extract32(rt6xx_reg_value(s, 0x204), 16, 8);
        break;
    case 3:
        main = 32768;
        break;
    default:
        g_assert_not_reached();
    }

    div = extract32(rt6xx_reg_value(s, 0x400), 0, 8) + 1;
    clock_update_hz(s->cpuclk, main / div);
}

static void rt6xx_update_outputs(Rt6xxSysconState *s)
{
    if (s->variant == RT6XX_SYSCTL0) {
        qemu_set_irq(s->ffro_enable_out,
                     !(rt6xx_reg_value(s, 0x610) & BIT(16)));
    } else if (s->variant == RT6XX_CLKCTL1) {
        uint32_t gates = rt6xx_reg_value(s, 0x010);

        for (unsigned i = 0; i < ARRAY_SIZE(s->flexcomm_clock); i++) {
            qemu_set_irq(s->flexcomm_clock[i],
                         extract32(gates, rt6xx_flexcomm_ctl_bit[i], 1));
        }
        gates = rt6xx_reg_value(s, 0x014);
        for (unsigned i = 0; i < ARRAY_SIZE(s->gpio_clock); i++) {
            qemu_set_irq(s->gpio_clock[i], extract32(gates, i, 1));
        }
    }
    rt6xx_update_cpuclk(s);
}

static uint64_t rt6xx_syscon_read(void *opaque, hwaddr offset, unsigned size)
{
    Rt6xxSysconState *s = RT6XX_SYSCON(opaque);
    size_t count;
    const Rt6xxRegInfo *table = rt6xx_table(s, &count);
    int index = rt6xx_find_register(s, offset);

    if (index < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%03" HWADDR_PRIx
                      "\n", TYPE_RT6XX_SYSCON, offset);
        return 0;
    }
    if (table[index].mode == RT6XX_WM_SET ||
        table[index].mode == RT6XX_WM_CLR) {
        return 0;
    }
    return s->regs[index];
}

static void rt6xx_syscon_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    Rt6xxSysconState *s = RT6XX_SYSCON(opaque);
    size_t count;
    const Rt6xxRegInfo *table = rt6xx_table(s, &count);
    int index = rt6xx_find_register(s, offset);
    const Rt6xxRegInfo *ri;
    uint32_t val = value;

    if (index < 0) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%03" HWADDR_PRIx
                      " value 0x%08x\n", TYPE_RT6XX_SYSCON, offset, val);
        return;
    }

    ri = &table[index];
    switch (ri->mode) {
    case RT6XX_WM_MASKED:
        s->regs[index] = (s->regs[index] & ~ri->wmask) | (val & ri->wmask);
        break;
    case RT6XX_WM_IGNORE:
        break;
    case RT6XX_WM_SET:
        g_assert(ri->alias < count);
        s->regs[ri->alias] |= val & table[ri->alias].wmask;
        break;
    case RT6XX_WM_CLR:
        g_assert(ri->alias < count);
        s->regs[ri->alias] &= ~(val & table[ri->alias].wmask);
        break;
    default:
        g_assert_not_reached();
    }

    /* Divider request/reset handshakes complete immediately. */
    if (rt6xx_is_clock_divider(s, offset)) {
        s->regs[index] &= ~(BIT(31) | BIT(29));
    }
    rt6xx_update_outputs(s);
}

static const MemoryRegionOps rt6xx_syscon_ops = {
    .read = rt6xx_syscon_read,
    .write = rt6xx_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rt6xx_ffro_enable(void *opaque, int n, int level)
{
    Rt6xxSysconState *s = RT6XX_SYSCON(opaque);

    s->ffro_enabled = level;
    rt6xx_update_cpuclk(s);
}

static void rt6xx_syscon_reset(DeviceState *dev)
{
    Rt6xxSysconState *s = RT6XX_SYSCON(dev);
    size_t count;
    const Rt6xxRegInfo *table = rt6xx_table(s, &count);

    memset(s->regs, 0, sizeof(s->regs));
    for (size_t i = 0; i < count; i++) {
        s->regs[i] = table[i].reset;
    }
    s->ffro_enabled = true;
    rt6xx_update_outputs(s);
}

static int rt6xx_syscon_post_load(void *opaque, int version_id)
{
    rt6xx_update_outputs(RT6XX_SYSCON(opaque));
    return 0;
}

static const VMStateDescription vmstate_rt6xx_syscon = {
    .name = TYPE_RT6XX_SYSCON,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt6xx_syscon_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Rt6xxSysconState, RT6XX_SYSCON_MAX_REGS),
        VMSTATE_BOOL(ffro_enabled, Rt6xxSysconState),
        VMSTATE_CLOCK(cpuclk, Rt6xxSysconState),
        VMSTATE_END_OF_LIST()
    },
};

static void rt6xx_syscon_init(Object *obj)
{
    Rt6xxSysconState *s = RT6XX_SYSCON(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rt6xx_syscon_ops, s,
                          TYPE_RT6XX_SYSCON, RT6XX_SYSCON_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(dev, &s->ffro_enable_out, "ffro-enabled", 1);
    qdev_init_gpio_out_named(dev, s->flexcomm_clock,
                             "flexcomm-clock",
                             ARRAY_SIZE(s->flexcomm_clock));
    qdev_init_gpio_out_named(dev, s->gpio_clock, "gpio-clock", 8);
    qdev_init_gpio_in_named(dev, rt6xx_ffro_enable, "ffro-enable-in", 1);
    s->cpuclk = qdev_init_clock_out(dev, "cpuclk");
}

static const Property rt6xx_syscon_properties[] = {
    DEFINE_PROP_UINT32("variant", Rt6xxSysconState, variant, RT6XX_CLKCTL0),
};

static void rt6xx_syscon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_rt6xx_syscon;
    device_class_set_legacy_reset(dc, rt6xx_syscon_reset);
    device_class_set_props(dc, rt6xx_syscon_properties);
}

static const TypeInfo rt6xx_syscon_info = {
    .name = TYPE_RT6XX_SYSCON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt6xxSysconState),
    .instance_init = rt6xx_syscon_init,
    .class_init = rt6xx_syscon_class_init,
};

static void rt6xx_syscon_register_types(void)
{
    type_register_static(&rt6xx_syscon_info);
}

type_init(rt6xx_syscon_register_types)
