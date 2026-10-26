/*
 * WM8904 stereo codec (functional control-plane model)
 *
 * Implements the WM8904 I2C control protocol (register-select byte
 * followed by one big-endian 16-bit value, with readback of the
 * selected register) and a deterministic digital audio endpoint for
 * the EVK audio link.  The register map is functional storage: all
 * documented WM8904 reset values are 0x0000 and the write sequencer
 * completes immediately so the SDK driver's polling loops terminate.
 *
 * Host output path: I2S playback words from the guest are converted to
 * signed-16 stereo PCM, buffered in a bounded ring, and drained by
 * QEMU's callback-driven audio subsystem (CoreAudio, PipeWire/PulseAudio/
 * ALSA, or WAV).  The capture endpoint produces a deterministic 440 Hz
 * sine wave by default so the SDK record/playback example generates
 * audible output.
 *
 * This is a local hardware-enablement model and must not be submitted
 * upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/audio/wm8904.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/system.h"

/*
 * Control protocol.  Byte 0 of a write selects an 8-bit register;
 * bytes 1 and 2 form the big-endian register value.  A read returns
 * the selected register MSB first; register selection persists across
 * a repeated START.
 */
static int wm8904_event(I2CSlave *s, enum i2c_event event)
{
    WM8904State *wm = WM8904(s);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
        wm->rx_index = 0;
        wm->rx_value = 0;
        break;
    case I2C_START_RECV:
        wm->tx_index = 0;
        break;
    case I2C_FINISH:
    case I2C_NACK:
        break;
    default:
        break;
    }
    return 0;
}

static void wm8904_commit(WM8904State *wm)
{
    if (wm->reg_ptr == WM8904_RESET && wm->rx_value == 0) {
        /* Software reset restores the (all-zero) documented reset map. */
        memset(wm->regs, 0, sizeof(wm->regs));
        return;
    }
    /*
     * The write sequencer completes without delay: WRSEQ_BUSY_STATUS
     * (WRT_SEQUENCER_4 bit 0) always reads clear, otherwise
     * WM8904_Init() would spin in WM8904_WaitOnWriteSequencer().
     */
    if (wm->reg_ptr == WM8904_WRT_SEQUENCER_4) {
        wm->regs[wm->reg_ptr] = wm->rx_value & ~1U;
    } else {
        wm->regs[wm->reg_ptr] = wm->rx_value;
    }
}

static int wm8904_send(I2CSlave *s, uint8_t data)
{
    WM8904State *wm = WM8904(s);

    if (wm->rx_index == 0) {
        wm->reg_ptr = data;
        wm->rx_index++;
        return 0;
    }
    if (wm->rx_index <= 2) {
        wm->rx_value = (wm->rx_value << 8) | data;
        wm->rx_index++;
        if (wm->rx_index == 3) {
            wm8904_commit(wm);
        }
        return 0;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write byte beyond register value\n", TYPE_WM8904);
    return 0;
}

static uint8_t wm8904_recv(I2CSlave *s)
{
    WM8904State *wm = WM8904(s);
    uint16_t value = wm->regs[wm->reg_ptr];

    /* Reads always return the selected register, MSB first. */
    return wm->tx_index++ == 0 ? (value >> 8) & 0xff : value & 0xff;
}

/* ---------------------------------------------------------------------------
 * Host audio output: ring buffer + callback
 * ---------------------------------------------------------------------------*/

static const int16_t tone_table[] = {
        0,   201,   402,   603,
      803,  1003,  1202,  1401,
     1598,  1795,  1990,  2185,
     2378,  2570,  2760,  2948,
     3135,  3320,  3503,  3683,
     3862,  4038,  4212,  4383,
     4551,  4717,  4880,  5040,
     5197,  5351,  5501,  5649,
     5793,  5933,  6070,  6203,
     6333,  6458,  6580,  6698,
     6811,  6921,  7027,  7128,
     7225,  7317,  7405,  7489,
     7568,  7643,  7713,  7779,
     7839,  7895,  7946,  7993,
     8035,  8071,  8103,  8130,
     8153,  8170,  8182,  8190,
     8192,  8190,  8182,  8170,
     8153,  8130,  8103,  8071,
     8035,  7993,  7946,  7895,
     7839,  7779,  7713,  7643,
     7568,  7489,  7405,  7317,
     7225,  7128,  7027,  6921,
     6811,  6698,  6580,  6458,
     6333,  6203,  6070,  5933,
     5793,  5649,  5501,  5351,
     5197,  5040,  4880,  4717,
     4551,  4383,  4212,  4038,
     3862,  3683,  3503,  3320,
     3135,  2948,  2760,  2570,
     2378,  2185,  1990,  1795,
     1598,  1401,  1202,  1003,
      803,   603,   402,   201,
        0,  -200,  -401,  -602,
     -802, -1002, -1201, -1400,
    -1597, -1794, -1989, -2184,
    -2377, -2569, -2759, -2947,
    -3134, -3319, -3502, -3682,
    -3861, -4037, -4211, -4382,
    -4550, -4716, -4879, -5039,
    -5196, -5350, -5500, -5648,
    -5792, -5932, -6069, -6202,
    -6332, -6457, -6579, -6697,
    -6810, -6920, -7026, -7127,
    -7224, -7316, -7404, -7488,
    -7567, -7642, -7712, -7778,
    -7838, -7894, -7945, -7992,
    -8034, -8070, -8102, -8129,
    -8152, -8169, -8181, -8189,
    -8191, -8189, -8181, -8169,
    -8152, -8129, -8102, -8070,
    -8034, -7992, -7945, -7894,
    -7838, -7778, -7712, -7642,
    -7567, -7488, -7404, -7316,
    -7224, -7127, -7026, -6920,
    -6810, -6697, -6579, -6457,
    -6332, -6202, -6069, -5932,
    -5792, -5648, -5500, -5350,
    -5196, -5039, -4879, -4716,
    -4550, -4382, -4211, -4037,
    -3861, -3682, -3502, -3319,
    -3134, -2947, -2759, -2569,
    -2377, -2184, -1989, -1794,
    -1597, -1400, -1201, -1002,
     -802,  -602,  -401,  -200,
};

/* Audio callback: drain the ring buffer into the backend. */
static void wm8904_audio_out_callback(void *opaque, int avail_bytes)
{
    WM8904State *wm = opaque;
    unsigned read_pos = wm->playback_read;
    uint32_t count = wm->playback_count;
    bool was_empty = (count == 0);

    if (!wm->voice_active || !wm->voice) {
        return;
    }

    while (count >= 2 && avail_bytes >= 4) {
        unsigned samples = MIN(count, PLAYBACK_BUF_SIZE - read_pos);
        size_t written;

        samples = MIN(samples, (unsigned)avail_bytes / sizeof(int16_t));
        samples &= ~1U; /* submit complete stereo frames only */
        if (!samples) {
            break;
        }

        written = audio_be_write(wm->audio_be, wm->voice,
                                 &wm->playback_ring[read_pos],
                                 samples * sizeof(int16_t));
        written &= ~3U; /* advance only by complete stereo frames */
        if (written == 0) {
            break;   /* backend temporarily full */
        }
        read_pos = (read_pos + written / sizeof(int16_t)) % PLAYBACK_BUF_SIZE;
        count -= written / sizeof(int16_t);
        avail_bytes -= written;
    }

    wm->playback_read = read_pos;
    wm->playback_count = count;

    /* Track underruns: transition from non-empty to empty. */
    if (!was_empty && count == 0) {
        wm->playback_underruns++;
    }

    /* Deactivate voice when ring is truly empty and there's no new data
     * queued — but leave it active through temporary guest FIFO underruns,
     * as the design specifies. */
}

/* Ring-buffer push: called from wm8904_endpoint_push by the audio-link timer. */
static void wm8904_ring_push(WM8904State *wm, uint32_t word)
{
    int16_t left = (int16_t)(word & 0xffff);
    int16_t right = (int16_t)(word >> 16);

    if (wm->playback_count == PLAYBACK_BUF_SIZE) {
        /* Ring full: drop oldest frame to bound latency. */
        wm->playback_read = (wm->playback_read + 2) % PLAYBACK_BUF_SIZE;
        wm->playback_count -= 2;
        wm->playback_dropped++;
    }

    wm->playback_ring[wm->playback_write] = left;
    wm->playback_ring[(wm->playback_write + 1) % PLAYBACK_BUF_SIZE] = right;
    wm->playback_write = (wm->playback_write + 2) % PLAYBACK_BUF_SIZE;
    wm->playback_count += 2;

    /* Activate voice on first frame. */
    if (!wm->voice_active && wm->audio_be) {
        audio_be_set_active_out(wm->audio_be, wm->voice, true);
        wm->voice_active = true;
    }
}

/* ---------------------------------------------------------------------------
 * Deterministic capture source: tone / ramp / silence
 * ---------------------------------------------------------------------------*/

static bool wm8904_endpoint_push(Rt685AudioEndpoint *ep, uint32_t word)
{
    WM8904State *wm = WM8904(ep);

    wm->last_playback_word = word;
    wm->playback_frames++;

    if (wm->audio_be && wm->voice) {
        wm8904_ring_push(wm, word);
    }

    return true;
}

static bool wm8904_endpoint_pop(Rt685AudioEndpoint *ep, uint32_t *word)
{
    WM8904State *wm = WM8904(ep);
    uint16_t left, right;

    switch (wm->capture_mode) {
    case WM8904_CAPTURE_TONE: {
        unsigned table_index =
            ((uint64_t)wm->tone_idx * TONE_TABLE_SIZE) / 48000;
        int16_t sample = tone_table[table_index];

        /* I2S and the output voice both carry signed 16-bit PCM. */
        left = right = (uint16_t)sample;
        wm->tone_idx = (wm->tone_idx + 440) % 48000;
        break;
    }
    case WM8904_CAPTURE_RAMP: {
        left = (uint16_t)wm->capture_frames;
        right = (uint16_t)(wm->capture_frames + 1);
        break;
    }
    default: /* SILENCE */
        left = right = 0;
        break;
    }

    *word = left | ((uint32_t)right << 16);
    wm->capture_frames++;
    return true;
}

static bool wm8904_endpoint_is_active(Rt685AudioEndpoint *ep,
                                      G_GNUC_UNUSED Rt685AudioDirection dir)
{
    /* The codec model is always ready to exchange a frame. */
    return true;
}

/* ---------------------------------------------------------------------------
 * Lifecycle: realize, reset, post_load, properties
 * ---------------------------------------------------------------------------*/

static void wm8904_audio_close(WM8904State *wm)
{
    if (wm->voice && wm->audio_be) {
        audio_be_set_active_out(wm->audio_be, wm->voice, false);
        wm->voice_active = false;
        audio_be_close_out(wm->audio_be, wm->voice);
        wm->voice = NULL;
    }
}

static void wm8904_exit(Notifier *notifier, void *data)
{
    WM8904State *wm = container_of(notifier, WM8904State, exit_notifier);

    wm8904_audio_close(wm);
}

static int wm8904_audio_init(WM8904State *wm, Error **errp)
{
    struct audsettings as;

    if (!wm->audio_be) {
        /* No backend configured: nothing to open. */
        return 0;
    }

    if (wm->voice) {
        /* Already initialized (e.g. after migration). */
        return 0;
    }

    as.freq = 48000;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.big_endian = false;

    wm->voice = audio_be_open_out(wm->audio_be, wm->voice, "wm8904.out",
                                  wm, wm8904_audio_out_callback, &as);
    if (!wm->voice) {
        error_setg(errp, "%s: Could not open audio output voice", TYPE_WM8904);
        return -1;
    }

    return 0;
}

static void wm8904_reset(DeviceState *dev)
{
    WM8904State *wm = WM8904(dev);

    memset(wm->regs, 0, sizeof(wm->regs));
    wm->reg_ptr = 0;
    wm->rx_index = 0;
    wm->rx_value = 0;
    wm->tx_index = 0;
    wm->capture_frames = 0;
    wm->playback_frames = 0;
    wm->last_playback_word = 0;

    /* Flush ring buffer and deactivate, but retain the realized host voice. */
    memset(wm->playback_ring, 0, sizeof(wm->playback_ring));
    wm->playback_write = 0;
    wm->playback_read = 0;
    wm->playback_count = 0;
    if (wm->voice && wm->audio_be) {
        audio_be_set_active_out(wm->audio_be, wm->voice, false);
    }
    wm->voice_active = false;

    /* Reset tone state. */
    wm->tone_idx = 0;

    /* Clear diagnostics counters on reset (as the design specifies). */
    wm->playback_dropped = 0;
    wm->playback_underruns = 0;
}

static int wm8904_post_load(void *opaque, int version_id)
{
    WM8904State *wm = opaque;

    (void)version_id;

    /* Reopen the voice with current backend settings. */
    if (wm->audio_be && !wm->voice) {
        wm8904_audio_init(wm, NULL);
    }

    /* Validate ring indices are within bounds. */
    wm->playback_write %= PLAYBACK_BUF_SIZE;
    wm->playback_read %= PLAYBACK_BUF_SIZE;

    return 0;
}

static const VMStateDescription vmstate_wm8904 = {
    .name = TYPE_WM8904,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = wm8904_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, WM8904State, 0,
                       vmstate_i2c_slave, I2CSlave),
        VMSTATE_UINT16_ARRAY(regs, WM8904State, WM8904_NUM_REGS),
        VMSTATE_UINT8(reg_ptr, WM8904State),
        VMSTATE_UINT8(rx_index, WM8904State),
        VMSTATE_UINT16(rx_value, WM8904State),
        VMSTATE_UINT8(tx_index, WM8904State),
        VMSTATE_UINT64(capture_frames, WM8904State),
        VMSTATE_UINT64(playback_frames, WM8904State),
        VMSTATE_UINT32(last_playback_word, WM8904State),

        /* Ring buffer state — migrate emulated PCM data. */
        VMSTATE_INT16_ARRAY(playback_ring, WM8904State, PLAYBACK_BUF_SIZE),
        VMSTATE_UINT32(playback_write, WM8904State),
        VMSTATE_UINT32(playback_read, WM8904State),
        VMSTATE_UINT32(playback_count, WM8904State),

        /* Diagnostics. */
        VMSTATE_UINT64(playback_dropped, WM8904State),
        VMSTATE_UINT64(playback_underruns, WM8904State),

        /* Tone state — migrate phase for deterministic capture. */
        VMSTATE_UINT32(capture_mode, WM8904State),
        VMSTATE_UINT32(tone_idx, WM8904State),

        VMSTATE_I2C_SLAVE(parent_obj, WM8904State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8904_init(Object *obj)
{
    WM8904State *wm = WM8904(obj);

    /* Default capture mode: 440 Hz sine tone. */
    wm->capture_mode = WM8904_CAPTURE_TONE;
    wm->tone_idx = 0;

    object_property_add_uint64_ptr(obj, "capture-frames",
                                   &wm->capture_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "playback-frames",
                                   &wm->playback_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "last-playback-word",
                                   &wm->last_playback_word,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "playback-dropped-frames",
                                   &wm->playback_dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "playback-underruns",
                                   &wm->playback_underruns,
                                   OBJ_PROP_FLAG_READ);
}

static void wm8904_realize(DeviceState *dev, Error **errp)
{
    WM8904State *wm = WM8904(dev);

    if (wm->capture_mode > WM8904_CAPTURE_SILENCE) {
        error_setg(errp, "%s: invalid capture-mode %u", TYPE_WM8904,
                   wm->capture_mode);
        return;
    }

    if (!audio_be_check(&wm->audio_be, errp)) {
        return;
    }

    /* Open the output voice with default settings. */
    if (wm8904_audio_init(wm, errp) != 0) {
        return;
    }

    wm->exit_notifier.notify = wm8904_exit;
    qemu_add_exit_notifier(&wm->exit_notifier);
    wm->exit_notifier_registered = true;
}

static void wm8904_finalize(Object *obj)
{
    WM8904State *wm = WM8904(obj);

    if (wm->exit_notifier_registered) {
        qemu_remove_exit_notifier(&wm->exit_notifier);
        wm->exit_notifier_registered = false;
    }
    wm8904_audio_close(wm);
    wm->audio_be = NULL;
}

static const Property wm8904_properties[] = {
    DEFINE_AUDIO_PROPERTIES(WM8904State, audio_be),
    /* capture_mode: 0=tone, 1=ramp, 2=silence. Validated in realize(). */
    DEFINE_PROP_UINT32("capture-mode", WM8904State, capture_mode, 0),
};

static void wm8904_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    Rt685AudioEndpointClass *ec = RT685_AUDIO_ENDPOINT_CLASS(klass);

    dc->desc = "WM8904 stereo codec (functional model)";
    dc->realize = wm8904_realize;
    dc->vmsd = &vmstate_wm8904;
    device_class_set_props(dc, wm8904_properties);
    device_class_set_legacy_reset(dc, wm8904_reset);

    sc->event = wm8904_event;
    sc->send = wm8904_send;
    sc->recv = wm8904_recv;

    ec->push = wm8904_endpoint_push;
    ec->pop = wm8904_endpoint_pop;
    ec->is_active = wm8904_endpoint_is_active;
}

static const TypeInfo wm8904_types[] = {
    {
        .name = TYPE_WM8904,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(WM8904State),
        .instance_init = wm8904_init,
        .instance_finalize = wm8904_finalize,
        .class_init = wm8904_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_RT685_AUDIO_ENDPOINT },
            { }
        },
    },
};

DEFINE_TYPES(wm8904_types)
