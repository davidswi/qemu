/*
 * NXP MIMXRT685-EVK board I2S audio link
 *
 * Owns the virtual-time frame cadence for the EVK's shared I2S
 * wiring (Flexcomm 1 RX master -> WM8904 -> Flexcomm 3 TX slave).
 * One frame (one 32-bit stereo FIFO word in each direction) moves
 * per timer tick; a whole application buffer is never transferred
 * inside a single callback.  This is a local hardware-enablement
 * device and must not be submitted upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/audio/rt685_audio_link.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

static int64_t rt685_audio_link_period(Rt685AudioLink *s)
{
    return NANOSECONDS_PER_SECOND / s->sample_rate;
}

static bool rt685_audio_link_running(Rt685AudioLink *s)
{
    return s->codec &&
           rt685_audio_endpoint_is_active(s->rx, RT685_AUDIO_DIR_RX) &&
           rt685_audio_endpoint_is_active(s->tx, RT685_AUDIO_DIR_TX);
}

static void rt685_audio_link_stop(Rt685AudioLink *s)
{
    timer_del(s->frame_timer);
    s->next_deadline = 0;
}

static void rt685_audio_link_tick(void *opaque)
{
    Rt685AudioLink *s = RT685_AUDIO_LINK(opaque);
    uint32_t word = 0;
    uint32_t capture = 0;

    if (!rt685_audio_link_running(s)) {
        rt685_audio_link_stop(s);
        return;
    }

    /* Playback: Flexcomm TX -> codec (TXI2SE0 semantics live in the
     * Flexcomm pop implementation).
     */
    rt685_audio_endpoint_pop(s->tx, &word);
    rt685_audio_endpoint_push(s->codec, word);

    /* Capture: codec -> Flexcomm RX.  RX overflow state is recorded
     * by the Flexcomm push implementation.
     */
    if (rt685_audio_endpoint_pop(s->codec, &capture)) {
        rt685_audio_endpoint_push(s->rx, capture);
    }

    s->frames++;

    /* Absolute scheduling keeps the long-term cadence drift-free. */
    s->next_deadline += s->frame_ns;
    timer_mod(s->frame_timer, s->next_deadline);
}

void rt685_audio_link_endpoint_changed(Rt685AudioLink *s)
{
    if (!s || !s->frame_timer) {
        return;
    }
    if (rt685_audio_link_running(s)) {
        if (!timer_pending(s->frame_timer)) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            s->next_deadline = now + s->frame_ns;
            timer_mod(s->frame_timer, s->next_deadline);
        }
    } else {
        rt685_audio_link_stop(s);
    }
}

static const Property rt685_audio_link_properties[] = {
    DEFINE_PROP_LINK("rx", Rt685AudioLink, rx, TYPE_RT685_AUDIO_ENDPOINT,
                     Rt685AudioEndpoint *),
    DEFINE_PROP_LINK("tx", Rt685AudioLink, tx, TYPE_RT685_AUDIO_ENDPOINT,
                     Rt685AudioEndpoint *),
    DEFINE_PROP_LINK("codec", Rt685AudioLink, codec, TYPE_RT685_AUDIO_ENDPOINT,
                     Rt685AudioEndpoint *),
    DEFINE_PROP_UINT32("sample-rate", Rt685AudioLink, sample_rate, 48000),
};

static void rt685_audio_link_realize(DeviceState *dev, Error **errp)
{
    Rt685AudioLink *s = RT685_AUDIO_LINK(dev);

    if (s->sample_rate == 0) {
        error_setg(errp, "%s: sample-rate must be nonzero",
                   TYPE_RT685_AUDIO_LINK);
        return;
    }
    s->frame_ns = rt685_audio_link_period(s);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  rt685_audio_link_tick, s);
}

static void rt685_audio_link_reset(DeviceState *dev)
{
    Rt685AudioLink *s = RT685_AUDIO_LINK(dev);

    rt685_audio_link_stop(s);
    s->frames = 0;
}

static int rt685_audio_link_post_load(void *opaque, int version_id)
{
    Rt685AudioLink *s = RT685_AUDIO_LINK(opaque);

    /* Restart from a quiet state; partial frame deadlines are not
     * migrated.  Endpoints recompute their own state in their own
     * post_load hooks.
     */
    rt685_audio_link_stop(s);
    rt685_audio_link_endpoint_changed(s);
    return 0;
}

static const VMStateDescription vmstate_rt685_audio_link = {
    .name = TYPE_RT685_AUDIO_LINK,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rt685_audio_link_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sample_rate, Rt685AudioLink),
        VMSTATE_UINT64(frames, Rt685AudioLink),
        VMSTATE_END_OF_LIST()
    },
};

static void rt685_audio_link_init(Object *obj)
{
    Rt685AudioLink *s = RT685_AUDIO_LINK(obj);

    object_property_add_uint64_ptr(obj, "frames", &s->frames,
                                   OBJ_PROP_FLAG_READ);
}

static void rt685_audio_link_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rt685_audio_link_realize;
    device_class_set_legacy_reset(dc, rt685_audio_link_reset);
    device_class_set_props(dc, rt685_audio_link_properties);
    dc->vmsd = &vmstate_rt685_audio_link;
    /* Board-internal wiring object; not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo rt685_audio_link_info = {
    .name = TYPE_RT685_AUDIO_LINK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rt685AudioLink),
    .instance_init = rt685_audio_link_init,
    .class_init = rt685_audio_link_class_init,
};

static const TypeInfo rt685_audio_endpoint_info = {
    .name = TYPE_RT685_AUDIO_ENDPOINT,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(Rt685AudioEndpointClass),
};

static void rt685_audio_link_register_types(void)
{
    type_register_static(&rt685_audio_link_info);
    type_register_static(&rt685_audio_endpoint_info);
}

type_init(rt685_audio_link_register_types)
