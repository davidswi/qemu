/*
 * Minimal NXP RT600 Xtensa configuration for local RT685 experiments.
 *
 * The architectural configuration comes from the Zephyr SDK RT600
 * toolchain.  The generated ISA module comes from the matching Zephyr SDK
 * sdk-ng overlay (overlays/xtensa_nxp_rt600_adsp/binutils/bfd/xtensa-modules.c,
 * SHA-256 9e218b19033b93ad53b042f86a643034033a2322cfd09c138592c6255e53f372).
 * The GDB register description comes from gdb/gdb/xtensa-config.c in the
 * same overlay (Git blob b2ad61ddde1c6ac215ed24abad3c23f5a0150363).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"
#include "qemu/host-utils.h"

#include "core-nxp_rt600/core-isa.h"
#include "overlay_tool.h"

#define xtensa_modules xtensa_modules_nxp_rt600
#include "core-nxp_rt600/xtensa-modules.c.inc"

static XtensaConfig nxp_rt600 __attribute__((unused)) = {
    .name = "nxp_rt600",
    .gdb_regmap = {
        .reg = {
#include "core-nxp_rt600/gdb-config.c.inc"
        }
    },
    .isa_internal = &xtensa_modules,
    .clock_freq_khz = 600000,
    DEFAULT_SECTIONS
};

REGISTER_CORE(nxp_rt600)
