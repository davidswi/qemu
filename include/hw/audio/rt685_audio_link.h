/*
 * NXP MIMXRT685-EVK board I2S audio link
 *
 * Models the fixed digital-audio wiring between Flexcomm 1 (I2S RX,
 * clock master), the WM8904 codec and Flexcomm 3 (I2S TX, clock
 * slave).  This is a local hardware-enablement device and is not a
 * QEMU hardware model suitable for upstream submission.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_RT685_AUDIO_LINK_H
#define HW_AUDIO_RT685_AUDIO_LINK_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

/*
 * Audio endpoint interface.
 *
 * An endpoint is a device that either provides or consumes 32-bit
 * I2S FIFO words (one stereo sample pair per word) on the board's
 * shared I2S wires.  The audio link owns the only frame timer and
 * calls these methods once per audio frame; endpoints must recompute
 * their own FIFO status and IRQ levels as a side effect.
 */
#define TYPE_RT685_AUDIO_ENDPOINT "rt685-audio-endpoint"

typedef struct Rt685AudioEndpoint Rt685AudioEndpoint;
typedef struct Rt685AudioEndpointClass Rt685AudioEndpointClass;

DECLARE_OBJ_CHECKERS(Rt685AudioEndpoint, Rt685AudioEndpointClass,
                     RT685_AUDIO_ENDPOINT, TYPE_RT685_AUDIO_ENDPOINT)

typedef enum Rt685AudioDirection {
    RT685_AUDIO_DIR_TX, /* endpoint -> codec (playback) */
    RT685_AUDIO_DIR_RX, /* codec -> endpoint (capture) */
} Rt685AudioDirection;

/**
 * Rt685AudioEndpointClass:
 * @push: consume one FIFO word.  Returns %false when the endpoint
 *   cannot accept it (the endpoint records its own overflow state).
 * @pop: provide one FIFO word.  @word is always written (zero when
 *   nothing is available); returns %false when nothing was available
 *   (the endpoint applies its own empty-FIFO policy, such as the
 *   Flexcomm I2S TXI2SE0 zero-fill).
 * @is_active: whether this endpoint currently participates in frame
 *   transfer for the given direction (selected, clocked, out of
 *   reset and enabled).
 */
struct Rt685AudioEndpointClass {
    InterfaceClass parent;
    bool (*push)(Rt685AudioEndpoint *ep, uint32_t word);
    bool (*pop)(Rt685AudioEndpoint *ep, uint32_t *word);
    bool (*is_active)(Rt685AudioEndpoint *ep, Rt685AudioDirection dir);
};

static inline bool rt685_audio_endpoint_push(Rt685AudioEndpoint *ep,
                                             uint32_t word)
{
    Rt685AudioEndpointClass *c;

    if (!ep) {
        return false;
    }
    c = RT685_AUDIO_ENDPOINT_GET_CLASS(ep);
    return c->push ? c->push(ep, word) : false;
}

static inline bool rt685_audio_endpoint_pop(Rt685AudioEndpoint *ep,
                                            uint32_t *word)
{
    Rt685AudioEndpointClass *c;

    if (!ep) {
        *word = 0;
        return false;
    }
    c = RT685_AUDIO_ENDPOINT_GET_CLASS(ep);
    if (!c->pop) {
        *word = 0;
        return false;
    }
    return c->pop(ep, word);
}

static inline bool rt685_audio_endpoint_is_active(Rt685AudioEndpoint *ep,
                                                  Rt685AudioDirection dir)
{
    Rt685AudioEndpointClass *c;

    if (!ep) {
        return false;
    }
    c = RT685_AUDIO_ENDPOINT_GET_CLASS(ep);
    return c->is_active ? c->is_active(ep, dir) : false;
}

/*
 * Board audio link: the single owner of the I2S frame cadence.
 *
 * At each frame tick, in order:
 *   1. pop one word from the TX endpoint (zero when empty and the
 *      TX endpoint fills zeros);
 *   2. deliver that word to the codec playback sink;
 *   3. pop one deterministic capture word from the codec;
 *   4. push it into the RX endpoint.
 * The timer runs only while both Flexcomm endpoints are active and
 * stops as soon as either side is disabled, gated or reset.  Endpoints
 * call rt685_audio_link_endpoint_changed() whenever their activity
 * may have changed.
 */
#define TYPE_RT685_AUDIO_LINK "rt685-audio-link"
OBJECT_DECLARE_SIMPLE_TYPE(Rt685AudioLink, RT685_AUDIO_LINK)

struct Rt685AudioLink {
    SysBusDevice parent_obj;

    Rt685AudioEndpoint *rx;     /* Flexcomm configured as I2S RX  */
    Rt685AudioEndpoint *tx;     /* Flexcomm configured as I2S TX  */
    Rt685AudioEndpoint *codec;  /* WM8904 digital audio endpoint  */
    uint32_t sample_rate;

    QEMUTimer *frame_timer;     /* owned by this device           */
    int64_t frame_ns;
    int64_t next_deadline;
    uint64_t frames;
};

void rt685_audio_link_endpoint_changed(Rt685AudioLink *link);

#endif /* HW_AUDIO_RT685_AUDIO_LINK_H */
