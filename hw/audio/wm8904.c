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
 * This is a local hardware-enablement model and must not be
 * submitted upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/wm8904.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

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

/*
 * Deterministic digital capture source: frame n yields left sample n
 * and right sample n + 1.  Playback words are sink-only: the last
 * word and cumulative counts are exposed as QOM diagnostics.
 */
static bool wm8904_endpoint_push(Rt685AudioEndpoint *ep, uint32_t word)
{
    WM8904State *wm = WM8904(ep);

    wm->last_playback_word = word;
    wm->playback_frames++;
    return true;
}

static bool wm8904_endpoint_pop(Rt685AudioEndpoint *ep, uint32_t *word)
{
    WM8904State *wm = WM8904(ep);
    uint16_t left = (uint16_t)wm->capture_frames;
    uint16_t right = (uint16_t)(wm->capture_frames + 1);

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
}

static const VMStateDescription vmstate_wm8904 = {
    .name = TYPE_WM8904,
    .version_id = 1,
    .minimum_version_id = 1,
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
        VMSTATE_END_OF_LIST()
    },
};

static void wm8904_init(Object *obj)
{
    object_property_add_uint64_ptr(obj, "capture-frames",
                                   &WM8904(obj)->capture_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "playback-frames",
                                   &WM8904(obj)->playback_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "last-playback-word",
                                   &WM8904(obj)->last_playback_word,
                                   OBJ_PROP_FLAG_READ);
}

static void wm8904_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    Rt685AudioEndpointClass *ec = RT685_AUDIO_ENDPOINT_CLASS(klass);

    dc->desc = "WM8904 stereo codec (functional model)";
    dc->vmsd = &vmstate_wm8904;
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
        .class_init = wm8904_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_RT685_AUDIO_ENDPOINT },
            { }
        },
    },
};

DEFINE_TYPES(wm8904_types)
