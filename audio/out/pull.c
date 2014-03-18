/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <inttypes.h>
#include <assert.h>
#include <stdatomic.h>

#include "ao.h"
#include "internal.h"
#include "audio/format.h"

#include "common/msg.h"
#include "common/common.h"

#include "osdep/timer.h"
#include "osdep/threads.h"
#include "misc/ring.h"

enum {
    AO_STATE_NONE,  // idle (e.g. before playback started, or after playback
                    // finished, but device is open)
    AO_STATE_PLAY,  // play the buffer
    AO_STATE_PAUSE, // pause playback
};

struct ao_pull_state {
    // Be very careful with the order when accessing planes.
    struct mp_ring *buffers[MP_NUM_CHANNELS];

    // AO_STATE_*
    int state;

    // Whether buffers[] can be accessed.
    int ready;

    // Device delay of the last written sample, in realtime.
    int64_t end_time_us;
};

static int get_space(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;
    // Since the reader will read the last plane last, its free space is the
    // minimum free space across all planes.
    return mp_ring_available(p->buffers[ao->num_planes - 1]) / ao->sstride;
}

static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct ao_pull_state *p = ao->api_priv;

    int write_samples = get_space(ao);
    write_samples = MPMIN(write_samples, samples);

    // Write starting from the last plane - this way, the first plane will
    // always contain the minimum amount of data readable across all planes
    // (assumes the reader starts with the first plane).
    int write_bytes = write_samples * ao->sstride;
    for (int n = ao->num_planes - 1; n >= 0; n--) {
        int r = mp_ring_write(p->buffers[n], data[n], write_bytes);
        assert(r == write_bytes);
    }
    if (p->state != AO_STATE_PLAY) {
        p->end_time_us = 0;
        p->state = AO_STATE_PLAY;
        atomic_thread_fence(memory_order_seq_cst);
        if (ao->driver->resume)
            ao->driver->resume(ao);
    }

    return write_samples;
}

// Read the given amount of samples in the user-provided data buffer. Returns
// the number of samples copied. If there is not enough data (buffer underrun
// or EOF), return the number of samples that could be copied, and fill the
// rest of the user-provided buffer with silence.
// This basically assumes that the audio device doesn't care about underruns.
// If this is called in paused mode, it will always return 0.
// The caller should set out_time_us to the expected delay the last sample
// reaches the speakers, in microseconds, using mp_time_us() as reference.
int ao_read_data(struct ao *ao, void **data, int samples, int64_t out_time_us)
{
    assert(ao->api == &ao_api_pull);

    struct ao_pull_state *p = ao->api_priv;
    int full_bytes = samples * ao->sstride;

    atomic_thread_fence(memory_order_seq_cst);
    if (!p->ready) {
        for (int n = 0; n < ao->num_planes; n++)
            af_fill_silence(data[n], full_bytes, ao->format);
        return 0;
    }

    // Since the writer will write the first plane last, its buffered amount
    // of data is the minimum amount across all planes.
    int bytes = mp_ring_buffered(p->buffers[0]);
    bytes = MPMIN(bytes, full_bytes);

    if (bytes > 0)
        p->end_time_us = out_time_us;

    atomic_thread_fence(memory_order_seq_cst);
    if (p->state == AO_STATE_PAUSE)
        bytes = 0;

    for (int n = 0; n < ao->num_planes; n++) {
        int r = mp_ring_read(p->buffers[n], data[n], bytes);
        assert(r == bytes);
        // pad with silence (underflow/paused/eof)
        int silence = full_bytes - bytes;
        if (silence)
            af_fill_silence((char *)data[n] + bytes, silence, ao->format);
    }
    return bytes / ao->sstride;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    if (ao->driver->control)
        return ao->driver->control(ao, cmd, arg);
    return CONTROL_UNKNOWN;
}

// Return size of the buffered data in seconds. Can include the device latency.
// Basically, this returns how much data there is still to play, and how long
// it takes until the last sample in the buffer reaches the speakers. This is
// used for audio/video synchronization, so it's very important to implement
// this correctly.
static float get_delay(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;

    atomic_thread_fence(memory_order_seq_cst);
    int64_t end = p->end_time_us;
    int64_t now = mp_time_us();
    double driver_delay = MPMAX(0, (end - now) / (1000.0 * 1000.0));
    return mp_ring_buffered(p->buffers[0]) / (double)ao->bps + driver_delay;
}

static void reset(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;
    if (ao->driver->reset)
        ao->driver->reset(ao);
    // Not like this is race-condition free. It will work if ->reset
    // stops the audio callback, though.
    p->ready = 0;
    p->state = AO_STATE_NONE;
    atomic_thread_fence(memory_order_seq_cst);
    for (int n = 0; n < ao->num_planes; n++)
        mp_ring_reset(p->buffers[n]);
    p->end_time_us = 0;
    atomic_thread_fence(memory_order_seq_cst);
    p->ready = 1;
    atomic_thread_fence(memory_order_seq_cst);
}

static void pause(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;
    if (ao->driver->pause)
        ao->driver->pause(ao);
    p->state = AO_STATE_PAUSE;
    atomic_thread_fence(memory_order_seq_cst);
}

static void resume(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;
    p->state = AO_STATE_PLAY;
    atomic_thread_fence(memory_order_seq_cst);
    if (ao->driver->resume)
        ao->driver->resume(ao);
}

static void uninit(struct ao *ao)
{
    ao->driver->uninit(ao);
}

static int init(struct ao *ao)
{
    struct ao_pull_state *p = ao->api_priv;
    for (int n = 0; n < ao->num_planes; n++)
            p->buffers[n] = mp_ring_new(ao, ao->buffer * ao->sstride);
    p->ready = 1;
    atomic_thread_fence(memory_order_seq_cst);
    return 0;
}

const struct ao_driver ao_api_pull = {
    .init = init,
    .control = control,
    .uninit = uninit,
    .reset = reset,
    .get_space = get_space,
    .play = play,
    .get_delay = get_delay,
    .pause = pause,
    .resume = resume,
    .priv_size = sizeof(struct ao_pull_state),
};
