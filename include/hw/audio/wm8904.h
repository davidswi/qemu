/*
 * WM8904 stereo codec (functional control-plane model)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_WM8904_H
#define HW_AUDIO_WM8904_H

#include "hw/audio/rt685_audio_link.h"
#include "hw/i2c/i2c.h"
#include "qemu/audio.h"
#include "qemu/notify.h"
#include "qom/object.h"

#define TYPE_WM8904 "wm8904"
OBJECT_DECLARE_SIMPLE_TYPE(WM8904State, WM8904)

#define WM8904_NUM_REGS 256

/* Ring buffer: power-of-two capacity in int16_t samples.
 * 4096 stereo frames × 2 channels = 8192 int16_t entries,
 * ~85 ms at 48 kHz. */
#define PLAYBACK_BUF_SIZE 8192
/* Number of sine-table entries for capture tone (440 Hz, 48 kHz).
 * Period ≈ 109 samples; round up to a power of two for clean wrapping. */
#define TONE_TABLE_SIZE 256

/* Registers referenced by the MCUXpresso WM8904 driver. */
#define WM8904_RESET              0
#define WM8904_WRT_SEQUENCER_0    46
#define WM8904_WRT_SEQUENCER_3    49
#define WM8904_WRT_SEQUENCER_4    50

#define WM8904_CAPTURE_TONE      0
#define WM8904_CAPTURE_RAMP      1
#define WM8904_CAPTURE_SILENCE   2

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

    /* Host audio output (owned by this device, not migrated). */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    Notifier exit_notifier;
    bool exit_notifier_registered;
    int16_t playback_ring[PLAYBACK_BUF_SIZE];
    unsigned playback_write;   /* next ring slot to fill          */
    unsigned playback_read;    /* next ring slot to drain         */
    uint32_t playback_count;   /* buffered int16_t samples       */
    uint64_t playback_dropped; /* frames dropped on overflow      */
    uint64_t playback_underruns;
    bool voice_active;

    /* Deterministic capture tone.
     * Stored as uint32 for property/migration compatibility.
     * 0=tone, 1=ramp, 2=silence. */
    uint32_t capture_mode;      /* raw integer value              */
    unsigned tone_idx;         /* phase in 1/48000-cycle units    */
};

#endif /* HW_AUDIO_WM8904_H */
