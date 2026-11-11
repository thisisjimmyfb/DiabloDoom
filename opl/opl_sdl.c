//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     OPL SDL interface.
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "SDL.h"
#ifndef DISABLE_SDL2MIXER
#include "SDL_mixer.h"
#endif  // DISABLE_SDL2MIXER

#include "opl3.h"

#include "opl.h"
#include "opl_internal.h"

#include "opl_queue.h"


#ifndef DISABLE_SDL2MIXER


#define MAX_SOUND_SLICE_TIME 100 /* ms */

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;

// When the callback mutex is locked using OPL_Lock, callback functions
// are not invoked.

static SDL_mutex *callback_mutex = NULL;

// Queue of callbacks waiting to be invoked.

static opl_callback_queue_t *callback_queue;

// Mutex used to control access to the callback queue.

static SDL_mutex *callback_queue_mutex = NULL;

// Current time, in us since startup:

static uint64_t current_time;

// If non-zero, playback is currently paused.

static int opl_sdl_paused;

// Time offset (in us) due to the fact that callbacks
// were previously paused.

static uint64_t pause_offset;

// OPL software emulator structure.

static opl3_chip opl_chip;
static int opl_opl3mode;

// Temporary mixing buffer used by the mixing callback.

static uint8_t *mix_buffer = NULL;

// ---- Decoupled OPL rendering ----
// The OPL music render (OPL3_GenerateStream) used to run inside the SDL
// audio callback (OPL_Mix_Callback). That costs ~12% CPU in the callback,
// and the SDL dummy audio driver sleeps for the full buffer period after
// the callback returns, so the callback's CPU time directly reduces the
// effective audio write rate (measured ~73-78 int16/ms vs nominal 88.2).
// A dedicated render thread now renders OPL audio ahead of time into a
// ring buffer; the audio callback just mixes pre-rendered samples out of
// the ring (cheap, ~0% CPU), so the write rate stays full.
// Ring buffer: stereo int16, power-of-two frame count. The render thread
// is the sole writer, the audio callback the sole reader; indices are
// guarded by opl_ring_mutex (the callback uses SDL_TryLockMutex so it
// never blocks).
#define OPL_RING_FRAMES 16384
#define OPL_RENDER_CHUNK_FRAMES 1024
static int16_t *opl_ring_buf = NULL;
static unsigned int opl_ring_read = 0;   // frames consumed by callback
static unsigned int opl_ring_write = 0;  // frames produced by render thread
static SDL_mutex *opl_ring_mutex = NULL;
static unsigned int opl_ring_underruns = 0;
static SDL_Thread *opl_render_thread = NULL;
static SDL_atomic_t opl_render_running;

// Register number that was written.

static int register_num = 0;

// Timers; DBOPL does not do timer stuff itself.

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

// SDL parameters.

static int sdl_was_initialized = 0;
static int mixing_freq, mixing_channels;
static Uint16 mixing_format;

static int SDLIsInitialized(void)
{
    int freq, channels;
    Uint16 format;

    return Mix_QuerySpec(&freq, &format, &channels);
}

// Advance time by the specified number of samples, invoking any
// callback functions as appropriate.

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    SDL_LockMutex(callback_queue_mutex);

    // Advance time.

    us = ((uint64_t) nsamples * OPL_SECOND) / mixing_freq;
    current_time += us;

    if (opl_sdl_paused)
    {
        pause_offset += us;
    }

    // Are there callbacks to invoke now?  Keep invoking them
    // until there are no more left.

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        // Pop the callback from the queue to invoke it.

        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }

        // The mutex stuff here is a bit complicated.  We must
        // hold callback_mutex when we invoke the callback (so that
        // the control thread can use OPL_Lock() to prevent callbacks
        // from being invoked), but we must not be holding
        // callback_queue_mutex, as the callback must be able to
        // call OPL_SetCallback to schedule new callbacks.

        SDL_UnlockMutex(callback_queue_mutex);

        SDL_LockMutex(callback_mutex);
        callback(callback_data);
        SDL_UnlockMutex(callback_mutex);

        SDL_LockMutex(callback_queue_mutex);
    }

    SDL_UnlockMutex(callback_queue_mutex);
}

// Render up to max_frames of OPL audio into out (linear stereo int16).
// Advances MIDI time and invokes due callbacks, exactly like the old
// in-callback loop. Returns frames rendered. Called only by the render
// thread (and by OPL_Mix_Callback as a synchronous fallback when the
// render thread could not be started).
static unsigned int OPL_RenderChunk(int16_t *out, unsigned int max_frames)
{
    unsigned int filled = 0;

    while (filled < max_frames)
    {
        uint64_t next_callback_time;
        unsigned int nsamples;

        SDL_LockMutex(callback_queue_mutex);

        if (opl_sdl_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = max_frames - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

            nsamples = (next_callback_time - current_time) * mixing_freq;
            nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;

            if (nsamples > max_frames - filled)
            {
                nsamples = max_frames - filled;
            }
        }

        SDL_UnlockMutex(callback_queue_mutex);

        if (nsamples == 0)
        {
            // A callback is due right now; advance time (with 0 samples)
            // so it fires, then loop to recompute.
            AdvanceTime(0);
            continue;
        }

        OPL3_GenerateStream(&opl_chip, out + filled * 2, nsamples);
        filled += nsamples;

        AdvanceTime(nsamples);
    }

    return filled;
}

// Render thread: renders OPL audio ahead of time into the ring buffer.
// Renders a chunk into mix_buffer (render-thread-only temp), then copies
// the chunk into the ring. Sleeps briefly when the ring is full.
static int OPL_RenderThread(void *data)
{
    (void) data;

    while (SDL_AtomicGet(&opl_render_running))
    {
        unsigned int used, space, frames;
        unsigned int wpos, first;

        SDL_LockMutex(opl_ring_mutex);
        used = opl_ring_write - opl_ring_read;
        space = OPL_RING_FRAMES - used;
        SDL_UnlockMutex(opl_ring_mutex);

        if (space < OPL_RENDER_CHUNK_FRAMES)
        {
            SDL_Delay(1);
            continue;
        }

        frames = OPL_RenderChunk((int16_t *) mix_buffer,
                                 OPL_RENDER_CHUNK_FRAMES);

        SDL_LockMutex(opl_ring_mutex);
        // Space can only have grown since the check (the callback
        // consumes), and only this thread writes, so frames always fit.
        wpos = opl_ring_write & (OPL_RING_FRAMES - 1);
        first = OPL_RING_FRAMES - wpos;
        if (first > frames)
            first = frames;
        memcpy(opl_ring_buf + wpos * 2, mix_buffer, first * 4);
        if (first < frames)
        {
            memcpy(opl_ring_buf,
                   (uint8_t *) mix_buffer + first * 4,
                   (frames - first) * 4);
        }
        opl_ring_write += frames;
        SDL_UnlockMutex(opl_ring_mutex);
    }

    return 0;
}

// Callback function to mix OPL music into a new sound buffer.
// This runs in the SDL audio thread, so it must stay cheap: it only
// mixes pre-rendered samples out of the ring buffer (rendered ahead of
// time by OPL_RenderThread). It never blocks, never touches OPL state,
// and never waits on the render thread. If the ring is empty (underrun)
// the OPL contribution is silence for those frames; the stream already
// holds the SDL_mixer mix, so nothing is zeroed here.
static void OPL_Mix_Callback(int chan, void *stream, int len, void *udata)
{
    unsigned int filled, buffer_samples;
    Uint8 *buffer = (Uint8*)stream;
    (void) chan;
    (void) udata;

    filled = 0;
    buffer_samples = len / 4;

    if (opl_render_thread == NULL)
    {
        // No render thread (thread creation failed at init): render
        // synchronously, the old behaviour. Keeps music working.
        unsigned int frames = OPL_RenderChunk((int16_t *) mix_buffer,
                                              buffer_samples);
        SDL_MixAudioFormat(buffer, mix_buffer, AUDIO_S16SYS, frames * 4,
                           SDL_MIX_MAXVOLUME);
        return;
    }

    while (filled < buffer_samples)
    {
        unsigned int avail, nframes, rpos, first;

        if (SDL_TryLockMutex(opl_ring_mutex) != 0)
            break;

        avail = opl_ring_write - opl_ring_read;
        if (avail == 0)
        {
            SDL_UnlockMutex(opl_ring_mutex);
            break;
        }
        nframes = avail;
        if (nframes > buffer_samples - filled)
            nframes = buffer_samples - filled;

        rpos = opl_ring_read & (OPL_RING_FRAMES - 1);
        first = OPL_RING_FRAMES - rpos;
        if (first > nframes)
            first = nframes;
        SDL_MixAudioFormat(buffer + filled * 4,
                           (Uint8 *) (opl_ring_buf + rpos * 2),
                           AUDIO_S16SYS, first * 4, SDL_MIX_MAXVOLUME);
        if (first < nframes)
        {
            SDL_MixAudioFormat(buffer + (filled + first) * 4,
                               (Uint8 *) opl_ring_buf,
                               AUDIO_S16SYS, (nframes - first) * 4,
                               SDL_MIX_MAXVOLUME);
        }
        opl_ring_read += nframes;
        SDL_UnlockMutex(opl_ring_mutex);
        filled += nframes;
    }

    if (filled < buffer_samples)
    {
        // Ring underrun: OPL contributes silence for the missing
        // frames (the stream already holds the SDL_mixer mix).
        ++opl_ring_underruns;
        if (opl_ring_underruns <= 3 || (opl_ring_underruns % 200) == 0)
        {
            fprintf(stderr,
                    "OPL_Mix_Callback: ring underrun #%u (%u frames silent)\n",
                    opl_ring_underruns, buffer_samples - filled);
        }
    }
}

static void OPL_SDL_Shutdown(void)
{
    // Stop the render thread first: it uses the OPL chip, the callback
    // queue and the mutexes, which are destroyed below.
    if (opl_render_thread != NULL)
    {
        SDL_AtomicSet(&opl_render_running, 0);
        SDL_WaitThread(opl_render_thread, NULL);
        opl_render_thread = NULL;
    }

    // Unregister the postmix effect (the symmetric call to the
    // Mix_RegisterEffect in OPL_SDL_Init).
    Mix_UnregisterEffect(MIX_CHANNEL_POST, OPL_Mix_Callback);
    Mix_HookMusic(NULL, NULL);

    if (opl_ring_underruns > 0)
    {
        fprintf(stderr, "OPL_SDL: %u ring underruns total\n",
                opl_ring_underruns);
    }

    if (sdl_was_initialized)
    {
        Mix_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        OPL_Queue_Destroy(callback_queue);
        free(mix_buffer);
        sdl_was_initialized = 0;
    }

/*
    if (opl_chip != NULL)
    {
        OPLDestroy(opl_chip);
        opl_chip = NULL;
    }
    */

    if (callback_mutex != NULL)
    {
        SDL_DestroyMutex(callback_mutex);
        callback_mutex = NULL;
    }

    if (callback_queue_mutex != NULL)
    {
        SDL_DestroyMutex(callback_queue_mutex);
        callback_queue_mutex = NULL;
    }

    if (opl_ring_mutex != NULL)
    {
        SDL_DestroyMutex(opl_ring_mutex);
        opl_ring_mutex = NULL;
    }

    if (opl_ring_buf != NULL)
    {
        free(opl_ring_buf);
        opl_ring_buf = NULL;
    }
}

static unsigned int GetSliceSize(void)
{
    int limit;
    int n;

    limit = (opl_sample_rate * MAX_SOUND_SLICE_TIME) / 1000;

    // Try all powers of two, not exceeding the limit.

    for (n=0;; ++n)
    {
        // 2^n <= limit < 2^n+1 ?

        if ((1 << (n + 1)) > limit)
        {
            return (1 << n);
        }
    }

    // Should never happen?

    return 1024;
}

static int OPL_SDL_Init(unsigned int port_base)
{
    // Check if SDL_mixer has been opened already
    // If not, we must initialize it now

    if (!SDLIsInitialized())
    {
        if (SDL_Init(SDL_INIT_AUDIO) < 0)
        {
            fprintf(stderr, "Unable to set up sound.\n");
            return 0;
        }

        if (Mix_OpenAudioDevice(opl_sample_rate, AUDIO_S16SYS, 2, GetSliceSize(), NULL, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) < 0)
        {
            fprintf(stderr, "Error initialising SDL_mixer: %s\n", Mix_GetError());

            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return 0;
        }

        SDL_PauseAudio(0);

        // When this module shuts down, it has the responsibility to 
        // shut down SDL.

        sdl_was_initialized = 1;
    }
    else
    {
        sdl_was_initialized = 0;
    }

    opl_sdl_paused = 0;
    pause_offset = 0;

    // Queue structure of callbacks to invoke.

    callback_queue = OPL_Queue_Create();
    current_time = 0;

    // Get the mixer frequency, format and number of channels.

    Mix_QuerySpec(&mixing_freq, &mixing_format, &mixing_channels);

    // Only supports AUDIO_S16SYS

    if (mixing_format != AUDIO_S16SYS || mixing_channels != 2)
    {
        fprintf(stderr, 
                "OPL_SDL only supports native signed 16-bit LSB, "
                "stereo format!\n");

        OPL_SDL_Shutdown();
        return 0;
    }

    // Mix buffer: four bytes per sample (16 bits * 2 channels):
    mix_buffer = malloc(mixing_freq * 4);

    // Create the emulator structure:

    OPL3_Reset(&opl_chip, mixing_freq);
    opl_opl3mode = 0;

    callback_mutex = SDL_CreateMutex();
    callback_queue_mutex = SDL_CreateMutex();

    // Ring buffer + render thread for decoupled OPL rendering.
    // The render thread renders OPL audio ahead of time into the ring;
    // the audio callback (OPL_Mix_Callback) just mixes pre-rendered
    // samples out of it, so the callback stays cheap and the dummy
    // audio driver's rate stays full. If the ring or thread cannot be
    // set up, the callback renders synchronously (old behaviour).
    opl_ring_buf = malloc(OPL_RING_FRAMES * 4);
    opl_ring_mutex = SDL_CreateMutex();
    if (opl_ring_buf == NULL || opl_ring_mutex == NULL)
    {
        fprintf(stderr, "OPL_SDL: failed to allocate ring buffer\n");
        free(opl_ring_buf);
        opl_ring_buf = NULL;
        if (opl_ring_mutex != NULL)
        {
            SDL_DestroyMutex(opl_ring_mutex);
            opl_ring_mutex = NULL;
        }
    }
    else
    {
        opl_ring_read = 0;
        opl_ring_write = 0;
        opl_ring_underruns = 0;
        SDL_AtomicSet(&opl_render_running, 1);
        opl_render_thread = SDL_CreateThread(OPL_RenderThread,
                                             "OPLRender", NULL);
        if (opl_render_thread == NULL)
        {
            fprintf(stderr, "OPL_SDL: failed to start render thread (%s); "
                            "falling back to in-callback rendering\n",
                    SDL_GetError());
            SDL_DestroyMutex(opl_ring_mutex);
            opl_ring_mutex = NULL;
            free(opl_ring_buf);
            opl_ring_buf = NULL;
        }
        else
        {
            printf("OPL_SDL: decoupled render thread started "
                   "(ring %u frames)\n", OPL_RING_FRAMES);
        }
    }

    // Set postmix that adds the OPL music. This is deliberately done
    // as a postmix and not using Mix_HookMusic() as the latter disables
    // normal SDL_mixer music mixing.
    Mix_RegisterEffect(MIX_CHANNEL_POST, OPL_Mix_Callback, NULL, NULL);

    return 1;
}

static unsigned int OPL_SDL_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    // If the timer is enabled, calculate the time when the timer
    // will expire.

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;

        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;

        default:
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;
    }
}

static void OPL_SDL_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_SDL_SetCallback(uint64_t us, opl_callback_t callback,
                                void *data)
{
    SDL_LockMutex(callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
    SDL_UnlockMutex(callback_queue_mutex);
}

static void OPL_SDL_ClearCallbacks(void)
{
    SDL_LockMutex(callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    SDL_UnlockMutex(callback_queue_mutex);
}

static void OPL_SDL_Lock(void)
{
    SDL_LockMutex(callback_mutex);
}

static void OPL_SDL_Unlock(void)
{
    SDL_UnlockMutex(callback_mutex);
}

static void OPL_SDL_SetPaused(int paused)
{
    opl_sdl_paused = paused;
}

static void OPL_SDL_AdjustCallbacks(float factor)
{
    SDL_LockMutex(callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    SDL_UnlockMutex(callback_queue_mutex);
}

opl_driver_t opl_sdl_driver =
{
    "SDL",
    OPL_SDL_Init,
    OPL_SDL_Shutdown,
    OPL_SDL_PortRead,
    OPL_SDL_PortWrite,
    OPL_SDL_SetCallback,
    OPL_SDL_ClearCallbacks,
    OPL_SDL_Lock,
    OPL_SDL_Unlock,
    OPL_SDL_SetPaused,
    OPL_SDL_AdjustCallbacks,
};


#endif // DISABLE_SDL2MIXER
