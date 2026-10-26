/*
 * WM8904 stereo codec (functional control-plane model)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_WM8904_H
#define HW_AUDIO_WM8904_H

#include "hw/audio/rt685_audio_link.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_WM8904 "wm8904"
OBJECT_DECLARE_SIMPLE_TYPE(WM8904State, WM8904)

#define WM8904_NUM_REGS 256

/* Registers referenced by the MCUXpresso WM8904 driver. */
#define WM8904_RESET              0
#define WM8904_WRT_SEQUENCER_0    46
#define WM8904_WRT_SEQUENCER_3    49
#define WM8904_WRT_SEQUENCER_4    50

struct WM8904State {
    I2CSlave parent_obj;

    uint16_t regs[WM8904_NUM_REGS];
    uint8_t reg_ptr;            /* selected register address        */
    uint8_t rx_index;           /* byte position within a write     */
    uint16_t rx_value;          /* value being assembled            */
    uint8_t tx_index;           /* byte position within a read      */

    /* Deterministic capture generator and playback diagnostics. */
    uint64_t capture_frames;
    uint64_t playback_frames;
    uint32_t last_playback_word;
};

#endif /* HW_AUDIO_WM8904_H */
