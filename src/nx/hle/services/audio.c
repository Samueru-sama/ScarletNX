/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* audio services: audout sends guest PCM to the host, audren renders the game's voices (audren.c, audren_dsp.c),
   and hwopus decodes the game's Opus packets with FFmpeg */
#include "nx/hle/services/services_int.h"
#include "hle/services/audio.swipc.inc"
#ifndef SNX_NO_H264
#include "nx/gpu/video/host1x.h"
#include <libavcodec/avcodec.h>
#include <math.h>
#endif
/* IAudioOut: appended PCM is copied to the host output and buffers come back at 48 kHz speed like on the console.
   Players time video against the released buffers (CRI movies): never releasing them stalls the video.
   Buffers are laid out on a play clock that runs only while the audout is started: a buffer is released once the
   clock passes its end, or when Stop or FlushAudioOutBuffers hands the queued ones back, and then waits in rel[] for
   GetReleasedAudioOutBuffers */
typedef struct AudOut {
    struct AudOut* next; /* audio.outs */
    u64 tag[64], start[64], end[64], frames[64];
    u32 head, n;
    u64 rel[64];
    u32 rhead, rn;
    u64 last_end, played;
    u64 base, anchor; /* the play clock: base, plus the time since anchor while started */
    int started;
    u32 channels;
    float volume;
    PAudio* output; /* paused until Start. The release clock keeps running even without a host audio device. */
    KEvent* ev;     /* RegisterBufferEvent */
    PMutex lk;
} AudOut;

static struct {
    KEvent* ev; /* the audio renderer's event, signaled every 5ms by audio_clock */
    /* the open IAudioOuts, which audio_clock looks at to signal their buffer events */
    AudOut* outs;
    PMutex outs_lk; /* static: the first audout can open before audio_clock runs */
} audio = {.outs_lk = PMUTEX_INIT};
void audio_init(void) { audio.ev = owned_event("audio renderer"); }

/* the rest of audout's helpers take q->lk held */
static u64 audout_clock(AudOut* q, u64 now) { return q->started ? q->base + (now - q->anchor) : q->base; }
/* buffers the clock has passed move to rel[] */
static void audout_release(AudOut* q, u64 now) {
    u64 t = audout_clock(q, now);
    while (q->n && q->end[q->head] <= t) {
        q->rel[(q->rhead + q->rn) & 63] = q->tag[q->head];
        q->rn++;
        q->played += q->frames[q->head];
        q->head = (q->head + 1) & 63;
        q->n--;
    }
}

/* renderer event fires every 5ms (240 samples @ 48k) like HOS. Each IAudioOut's buffer event is signaled while it
   has released buffers the game hasn't taken: the SDK clears it after each wait */
void audio_clock(void* arg) {
    (void)arg;
    u64 next = plat_ns();
    for (;;) {
        next += 5000000ull;
        u64 now = plat_ns();
        if (next > now)
            plat_sleep_ns((i64)(next - now));
        else if (now - next > 50000000ull)
            next = now;
        audren_tick(); /* the frame first: a game waiting on the event reads what it rendered */
        kevent_signal(audio.ev);
        now = plat_ns();
        pm_lock(&audio.outs_lk);
        for (AudOut* q = audio.outs; q; q = q->next) {
            pm_lock(&q->lk);
            audout_release(q, now);
            int any = q->rn != 0;
            pm_unlock(&q->lk);
            if (any) kevent_signal(q->ev);
        }
        pm_unlock(&audio.outs_lk);
    }
}

/* IAudioRenderer: u[0..2] sample rate, sample count and mix buffer count, u[3] state (0 started, 1 stopped),
   u[4] audren.c's renderer, u[6] the rendering time limit */
static int nn_audio_detail_IAudioRenderer_GetSampleRate(SvcObj* self, u32* sample_rate) {
    *sample_rate = (u32)self->u[0];
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetSampleCount(SvcObj* self, u32* sample_count) {
    *sample_count = (u32)self->u[1];
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetMixBufferCount(SvcObj* self, u32* mix_buffer_count) {
    *mix_buffer_count = (u32)self->u[2];
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetState(SvcObj* self, u32* state) {
    *state = (u32)__atomic_load_n(&self->u[3], __ATOMIC_ACQUIRE);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_RequestUpdate(SvcObj* self, IpcBuf input, IpcBuf output, IpcBuf performance) {
    return audren_update(self, input, output);
}
static int nn_audio_detail_IAudioRenderer_RequestUpdateAuto(SvcObj* self, IpcBuf input, IpcBuf output,
                                                            IpcBuf performance) {
    return audren_update(self, input, output);
}
static int nn_audio_detail_IAudioRenderer_Start(SvcObj* self) { /* audren_tick reads it on the audio clock */
    __atomic_store_n(&self->u[3], 0, __ATOMIC_RELEASE);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_Stop(SvcObj* self) {
    __atomic_store_n(&self->u[3], 1, __ATOMIC_RELEASE);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_QuerySystemEvent(SvcObj* self, u32* event) {
    *event = ev_handle(audio.ev);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_SetRenderingTimeLimit(SvcObj* self, u32 percent) {
    self->u[6] = percent;
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetRenderingTimeLimit(SvcObj* self, u32* percent) {
    *percent = (u32)self->u[6];
    return 0;
}
/* IAudioDevice: its system, input and output events in u[0..2], made with the object and kept by it. They start
   signaled, as on the console */
static void auddev_destroy(SvcObj* o) {
    for (int i = 0; i < 3; i++)
        if (o->u[i]) kobj_unref(&((KEvent*)(size_t)o->u[i])->o);
}
static const char g_device_name[] = "AudioTvOutput";
/* a device name into the game's out buffer, cut to fit and zero padded */
static u32 put_name(IpcBuf b, const char* nm) {
    if (!b.addr || !b.size) return 0;
    u64 n = b.size < 0x100 ? b.size : 0x100;
    memset(GP(b.addr), 0, n);
    memcpy(GP(b.addr), nm, strlen(nm) < n ? strlen(nm) : n - 1);
    return 1;
}
/* the one device, in a list of names */
static i32 list_device_names(IpcBuf names) {
    if (!names.addr || names.size < 0x100) return 0;
    memset(GP(names.addr), 0, 0x100);
    strcpy((char*)GP(names.addr), g_device_name);
    return 1;
}
static int nn_audio_detail_IAudioDevice_ListAudioDeviceName(SvcObj* self, IpcBuf names, i32* count) {
    *count = list_device_names(names);
    return 0;
}
static int nn_audio_detail_IAudioDevice_ListAudioDeviceNameAuto(SvcObj* self, IpcBuf names, i32* count) {
    *count = list_device_names(names);
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolume(SvcObj* self, IpcBuf name, float* volume) {
    *volume = 1.0f;
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolumeAuto(SvcObj* self, IpcBuf name, float* volume) {
    *volume = 1.0f;
    return 0;
}
/* the active device's name, cut to fit */
static int active_device_name(IpcBuf name) {
    put_name(name, g_device_name);
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(SvcObj* self, IpcBuf name) {
    return active_device_name(name);
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceNameAuto(SvcObj* self, IpcBuf name) {
    return active_device_name(name);
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioOutputDeviceName(SvcObj* self, IpcBuf name) {
    return active_device_name(name);
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceSystemEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceInputEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[1]);
    return 0;
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceOutputEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[2]);
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetActiveChannelCount(SvcObj* self, u32* count) {
    *count = 2;
    return 0;
}
static SvcObj* new_auddev(void) {
    static const char* const names[3] = {"audio device system", "audio device input", "audio device output"};
    SvcObj* d = nn_audio_detail_IAudioDevice_new(0);
    for (int i = 0; i < 3; i++) {
        KEvent* e = owned_event(names[i]);
        kevent_signal(e);
        d->u[i] = (u64)(size_t)e;
    }
    d->destroy = auddev_destroy;
    return d;
}
/* OpenAudioRenderer and OpenAudioRendererForManualExecution: the same parameters. Parameters the renderer can't be
   made with fail the open (OperationFailed), rather than give a renderer every update of which fails */
static int open_renderer(const nn_audio_detail_AudioRendererParameterInternal* params, SvcObj** renderer) {
    SvcObj* r = nn_audio_detail_IAudioRenderer_new(0);
    r->u[0] = params->sample_rate;
    r->u[1] = params->sample_count;
    r->u[2] = params->mix_buffer_count;
    r->u[3] = 1;
    r->u[6] = 100; /* the rendering time limit starts at 100% */
    if (!audren_open(r, (const u8*)params)) {
        svcobj_unref(r);
        return RC(153, 2);
    }
    *renderer = r;
    return 0;
}
static int nn_audio_detail_IAudioRendererManager_OpenAudioRenderer(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64 work_buffer_size, u64 aruid,
    u32 work_buffer, u32 process, SvcObj** renderer) {
    return open_renderer(params, renderer);
}
static int nn_audio_detail_IAudioRendererManager_OpenAudioRendererForManualExecution(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64 work_buffer_address, u32 process,
    u64 work_buffer_size, u64 aruid, SvcObj** renderer) {
    return open_renderer(params, renderer);
}
static int nn_audio_detail_IAudioRendererManager_GetWorkBufferSize(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64* size) {
    u64 sz =
        0x4000 + (u64)params->voice_count * 0x400 + (u64)params->sink_count * 0x200 + (u64)params->effect_count * 0x800;
    /* The SDK allocates this beside its renderer config. An oversized estimate can place that config
       outside the pages it mapped. Transfer memory requires whole pages. */
    *size = (sz + 0xFFF) & ~0xFFFull;
    return 0;
}
static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceService(SvcObj* self,
                                                                       nn_applet_AppletResourceUserId aruid,
                                                                       SvcObj** device) {
    *device = new_auddev();
    return 0;
}
static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceServiceWithRevisionInfo(
    SvcObj* self, nn_applet_AppletResourceUserId aruid, u32 revision, SvcObj** device) {
    *device = new_auddev();
    return 0;
}
SvcObj* new_audren(void) { return nn_audio_detail_IAudioRendererManager_new(0); }

static void audout_destroy(SvcObj* o) {
    AudOut* q = (AudOut*)o->data;
    pm_lock(&audio.outs_lk);
    for (AudOut** p = &audio.outs; *p; p = &(*p)->next)
        if (*p == q) {
            *p = q->next;
            break;
        }
    pm_unlock(&audio.outs_lk);
    plat_audio_close(q->output);
    kobj_unref(&q->ev->o);
}

/* AudioOutBuffer (libnx audout.h): next, sample pointer, capacity, data size, offset, all u64. IPC only maps the
   descriptor, so validate the separate sample range before the backend copies it. */
static int audout_pcm_range(u64 addr, u64 bytes) {
    if (!addr || addr >= AS_SIZE || bytes > AS_SIZE - addr) return 0;
    u64 end = addr + bytes;
    while (addr < end) {
        MemBlock b;
        mem_query(addr, &b);
        if (!(b.perm & PERM_R) || b.addr > addr || b.size <= addr - b.addr) return 0;
        u64 n = b.size - (addr - b.addr);
        if (n >= end - addr) return 1;
        addr += n;
    }
    return 1;
}

static int nn_audio_detail_IAudioOut_GetAudioOutState(SvcObj* self, u32* state) { /* 0 started, 1 stopped */
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    *state = q->started ? 0 : 1;
    pm_unlock(&q->lk);
    return 0;
}
static int nn_audio_detail_IAudioOut_Start(SvcObj* self) { /* the play clock runs from now */
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    if (!q->started) {
        plat_audio_start(q->output, 1);
        q->anchor = plat_ns();
        q->started = 1;
    }
    pm_unlock(&q->lk);
    return 0;
}
/* the frames of the head buffer the play clock has passed, at time t */
static u64 audout_head_played(AudOut* q, u64 t) {
    if (!q->n || t <= q->start[q->head]) return 0;
    u64 part = (t - q->start[q->head]) * 48000 / 1000000000ull;
    return part < q->frames[q->head] ? part : q->frames[q->head];
}
/* every queued buffer comes back, and the host drops what it holds of them. What the clock had played of the head
   buffer stays counted (the played count never goes back). 1 when the game has buffers to take */
static int audout_flush(AudOut* q, u64 now) {
    q->played += audout_head_played(q, audout_clock(q, now));
    plat_audio_clear(q->output);
    while (q->n) {
        q->rel[(q->rhead + q->rn) & 63] = q->tag[q->head];
        q->rn++;
        q->head = (q->head + 1) & 63;
        q->n--;
    }
    q->last_end = 0;
    return q->rn != 0;
}
/* the play clock stops where it is, and the buffers not played yet come back, as on the console (yuzu and Ryujinx
   release them too) */
static int nn_audio_detail_IAudioOut_Stop(SvcObj* self) {
    AudOut* q = (AudOut*)self->data;
    u64 now = plat_ns();
    int any = 0;
    pm_lock(&q->lk);
    if (q->started) {
        audout_release(q, now);
        any = audout_flush(q, now);
        q->base = audout_clock(q, now);
        q->started = 0;
        plat_audio_start(q->output, 0);
    }
    pm_unlock(&q->lk);
    if (any) kevent_signal(q->ev);
    return 0;
}
static int append_buffer(AudOut* q, u64 tag, IpcBuf b) {
    nn_audio_AudioOutBuffer ob;
    const u64 no_offset = offsetof(nn_audio_AudioOutBuffer, data_offset); /* a 0x20 byte buffer has no data_offset */
    memset(&ob, 0, sizeof ob);
    if (b.addr && b.size >= no_offset) memcpy(&ob, GP(b.addr), b.size < sizeof ob ? no_offset : sizeof ob);
    u64 bytes = ob.data_size, samples = ob.buffer, capacity = ob.buffer_capacity, offset = ob.data_offset;
    if (bytes > 0xFFFFFFFFull) bytes = 0xFFFFFFFFull; /* keeps frames * 1e9 in 64 bits */
    u64 now = plat_ns();
    pm_lock(&q->lk);
    u64 frames = bytes / (2ull * q->channels);
    audout_release(q, now);
    if (q->n + q->rn >= 64) {
        /* the SDK's AppendAudioOutBuffer returns false for BufferRingFull (0x1099) */
        pm_unlock(&q->lk);
        return RC(153, 8);
    }
    u64 pcm_bytes = frames * 2ull * q->channels;
    if (pcm_bytes && q->output) {
        if (offset <= capacity && pcm_bytes <= capacity - offset && samples <= AS_SIZE && offset <= AS_SIZE - samples &&
            audout_pcm_range(samples + offset, pcm_bytes))
            plat_audio_queue(q->output, GP(samples + offset), (u32)pcm_bytes);
        else
            WARN_LIMIT(4, "audout: PCM buffer is outside its readable guest allocation\n");
    }
    u64 t = audout_clock(q, now), st = q->last_end > t ? q->last_end : t;
    u32 k = (q->head + q->n) & 63;
    q->tag[k] = tag;
    q->start[k] = st;
    q->end[k] = st + frames * 1000000000ull / 48000;
    q->frames[k] = frames;
    q->n++;
    q->last_end = q->end[k];
    pm_unlock(&q->lk);
    return 0;
}
static int nn_audio_detail_IAudioOut_AppendAudioOutBuffer(SvcObj* self, u64 tag, IpcBuf buffer) {
    return append_buffer((AudOut*)self->data, tag, buffer);
}
static int nn_audio_detail_IAudioOut_AppendAudioOutBufferAuto(SvcObj* self, u64 tag, IpcBuf buffer) {
    return append_buffer((AudOut*)self->data, tag, buffer);
}
static int nn_audio_detail_IAudioOut_RegisterBufferEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AudOut*)self->data)->ev);
    return 0;
}
/* the tags of the released buffers, as many as fit: unused slots read as null */
static u32 released_buffers(AudOut* q, IpcBuf tags) {
    u32 cnt = 0;
    u64 room = tags.addr ? tags.size : 0; /* no buffer takes none: they stay released for the next call */
    if (room) memset(GP(tags.addr), 0, room);
    u64 now = plat_ns();
    pm_lock(&q->lk);
    audout_release(q, now);
    while (q->rn && (u64)(cnt + 1) * 8 <= room) {
        memcpy(GP(tags.addr + (u64)cnt * 8), &q->rel[q->rhead], 8);
        q->rhead = (q->rhead + 1) & 63;
        q->rn--;
        cnt++;
    }
    pm_unlock(&q->lk);
    return cnt;
}
static int nn_audio_detail_IAudioOut_GetReleasedAudioOutBuffers(SvcObj* self, IpcBuf tags, u32* count) {
    *count = released_buffers((AudOut*)self->data, tags);
    return 0;
}
static int nn_audio_detail_IAudioOut_GetReleasedAudioOutBuffersAuto(SvcObj* self, IpcBuf tags, u32* count) {
    *count = released_buffers((AudOut*)self->data, tags);
    return 0;
}
/* appended and not yet taken back by the game */
static int nn_audio_detail_IAudioOut_ContainsAudioOutBuffer(SvcObj* self, u64 tag, u8* contains) {
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    audout_release(q, plat_ns());
    for (u32 k = 0; k < q->n; k++)
        if (q->tag[(q->head + k) & 63] == tag) *contains = 1;
    for (u32 k = 0; k < q->rn; k++)
        if (q->rel[(q->rhead + k) & 63] == tag) *contains = 1;
    pm_unlock(&q->lk);
    return 0;
}
/* appended and not played yet */
static int nn_audio_detail_IAudioOut_GetAudioOutBufferCount(SvcObj* self, u32* count) {
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    audout_release(q, plat_ns());
    *count = q->n;
    pm_unlock(&q->lk);
    return 0;
}
/* the finished buffers and what the clock played of the next */
static int nn_audio_detail_IAudioOut_GetAudioOutPlayedSampleCount(SvcObj* self, u64* count) {
    AudOut* q = (AudOut*)self->data;
    u64 now = plat_ns();
    pm_lock(&q->lk);
    audout_release(q, now);
    u64 played = q->played + audout_head_played(q, audout_clock(q, now));
    pm_unlock(&q->lk);
    *count = played;
    return 0;
}
/* the queued buffers come back, and what the clock played of the head stays counted (audout_flush) */
static int nn_audio_detail_IAudioOut_FlushAudioOutBuffers(SvcObj* self, u8* released) {
    AudOut* q = (AudOut*)self->data;
    u64 now = plat_ns();
    pm_lock(&q->lk);
    audout_release(q, now);
    int any = audout_flush(q, now);
    pm_unlock(&q->lk);
    if (any) kevent_signal(q->ev);
    *released = 1;
    return 0;
}
static int nn_audio_detail_IAudioOut_SetAudioOutVolume(SvcObj* self, float volume) {
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    q->volume = volume;
    plat_audio_volume(q->output, q->volume);
    pm_unlock(&q->lk);
    return 0;
}
static int nn_audio_detail_IAudioOut_GetAudioOutVolume(SvcObj* self, float* volume) {
    AudOut* q = (AudOut*)self->data;
    pm_lock(&q->lk);
    *volume = q->volume;
    pm_unlock(&q->lk);
    return 0;
}
static int nn_audio_detail_IAudioOutManager_ListAudioOuts(SvcObj* self, IpcBuf names, u32* count) {
    *count = put_name(names, "DeviceOut");
    return 0;
}
static int nn_audio_detail_IAudioOutManager_ListAudioOutsAuto(SvcObj* self, IpcBuf names, u32* count) {
    *count = put_name(names, "DeviceOut");
    return 0;
}
/* OpenAudioOut(Auto): 48 kHz s16, with the channel count the game asked for */
static int open_audio_out(const nn_audio_AudioOutParameter* params, IpcBuf name_out, u32* sample_rate,
                          u32* channel_count, u32* sample_format, u32* state, SvcObj** out) {
    SvcObj* r = nn_audio_detail_IAudioOut_new(sizeof(AudOut));
    AudOut* q = (AudOut*)r->data;
    pm_init(&q->lk);
    u32 channels = params->channel_count;
    if (channels != 1 && channels != 2 && channels != 4 && channels != 6) channels = 2;
    q->channels = channels; /* what the reply tells the game it gets */
    q->volume = 1.0f;
    q->output = plat_audio_open(48000, q->channels);
    q->ev = owned_event("audio out buffers");
    r->destroy = audout_destroy;
    pm_lock(&audio.outs_lk);
    q->next = audio.outs;
    audio.outs = q;
    pm_unlock(&audio.outs_lk);
    put_name(name_out, "DeviceOut");
    LOG_LIMIT(1, "audout: opened (the game asked for %u channels, it gets %u)\n", params->channel_count,
              q->channels);
    *sample_rate = 48000;
    *channel_count = q->channels;
    *sample_format = 2; /* PCM s16 */
    *state = 1;         /* stopped */
    *out = r;
    return 0;
}
static int nn_audio_detail_IAudioOutManager_OpenAudioOut(SvcObj* self, const nn_audio_AudioOutParameter* params,
                                                         u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                         u32* sample_rate, u32* channel_count, u32* sample_format,
                                                         u32* state, SvcObj** out) {
    return open_audio_out(params, name_out, sample_rate, channel_count, sample_format, state, out);
}
static int nn_audio_detail_IAudioOutManager_OpenAudioOutAuto(SvcObj* self, const nn_audio_AudioOutParameter* params,
                                                             u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                             u32* sample_rate, u32* channel_count, u32* sample_format,
                                                             u32* state, SvcObj** out) {
    return open_audio_out(params, name_out, sample_rate, channel_count, sample_format, state, out);
}
SvcObj* new_audout(void) { return nn_audio_detail_IAudioOutManager_new(0); }

/* hwopus: Opus packets decoded with FFmpeg's Opus decoder, required by every NX runtime build.
   The commands are the ones the SDK's nn::codec client in the game sends (SDK 6.5: manager 0 to 3, decoder 8 and 9),
   read from its disassembly, and Switchbrew's hwopus page and Ryujinx (MIT, Ryujinx.HLE Services/Audio
   HardwareOpusDecoderManager) for the older decode commands and the ones newer SDKs add.
   A packet starts with an 8 byte header, big-endian: the payload's size, then the encoder's final range (not checked:
   FFmpeg doesn't give its own). The reply is the bytes consumed, the samples per channel (interleaved s16 at the
   decoder's rate) and, from the decoder's command 4 on, the time the decode took in microseconds.
   Results: module 111 description 17 is the one the SDK reports as an invalid packet, 385 the one it reports as a
   failed open. It treats any other result as fatal.
   The console decodes with libopus (opus_decode at the decoder's rate), which gives each packet exactly its duration
   in samples, or fails before touching its state when the output buffer is too small. FFmpeg's decoder differs, and
   what is between it and the reply makes up for that:
   - it decodes at 48 kHz only, to planar float. Lower rates go through a low-pass FIR that keeps every step-th
     sample, and the samples are clipped to s16 and interleaved.
   - its frames aren't one packet's duration: SILK goes through libswresample (opus/dec.c), which holds back a few
     samples when it starts (OPUS_DELAY_48K at most) and gives them back, on top of the packet's own, when the
     mode or the SILK rate changes. Its frames go into a FIFO that starts with OPUS_DELAY_48K samples of silence, and
     each reply takes exactly the packet's duration from it (read from the packet's TOC, RFC 6716 3.1).
   - it conceals a frame it can't decode with silence instead of failing (without AV_EF_EXPLODE), which is kept:
     libopus decodes about anything that parses, and what doesn't parse FFmpeg still refuses */
#define OPUS_BAD_PACKET  RC(111, 17)
#define OPUS_OPEN_FAILED RC(111, 385)
#define OPUS_MAX_48K     5760 /* samples per channel in the longest packet, 120 ms at 48 kHz */
/* the FIFO's silence: the most libswresample holds back. It keeps 8 input samples of its 16 taps and FFmpeg primes
   it with 4 (narrowband, 8 kHz: 4 short, 24 at 48 kHz), 8 or 11 (medium and wide band, none short) */
#define OPUS_DELAY_48K 24
/* the FIFO's room per channel: what is left after a reply, and a packet with the samples a resampler flush adds */
#define OPUS_FIFO_48K (OPUS_MAX_48K + 960)
typedef struct {
    u32 rate, channels;
    PMutex lk;
#ifndef SNX_NO_H264
    AVCodecContext* dec;
    AVPacket* pkt;
    AVFrame* frame;
    u8* in; /* the payload, with the padding FFmpeg reads past its end */
    u32 incap;
    /* the FIR's taps (step > 1), then each channel's samples at 48 kHz, OPUS_FIFO_48K + taps - 1 apart: the FIR's
       last taps - 1 inputs, then the fifo samples FFmpeg gave that no reply has taken yet */
    u32 step, taps, fifo;
    float* fir;
    float* pcm;
#endif
} HwOpus;
#ifndef SNX_NO_H264
/* windowed sinc (Blackman), cut off at 90% of the output rate's Nyquist frequency, unity gain */
static void opus_fir(float* h, u32 n, u32 step) {
    double fc = 0.45 / step, sum = 0, m = (n - 1) / 2.0;
    for (u32 i = 0; i < n; i++) {
        double t = i - m, s = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (u32 i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
}
static i16 opus_s16(float v) {
    v *= 32768.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (i16)lrintf(v);
}
/* the samples per channel at 48 kHz in the packet p (the first stream's, in a multistream one: its TOC comes first
   too), from its TOC byte and, for code 3, its frame count byte. 0 when it isn't a packet libopus decodes */
static u32 opus_packet_48k(const u8* p, u32 len) {
    static const u16 silk[4] = {480, 960, 1920, 2880}, celt[4] = {120, 240, 480, 960};
    if (!len) return 0;
    u32 config = p[0] >> 3, frames = (p[0] & 3) == 0 ? 1 : (p[0] & 3) < 3 ? 2 : len > 1 ? p[1] & 63 : 0;
    u32 n = frames * (config < 12 ? silk[config & 3] : config < 16 ? silk[config & 1] : celt[config & 3]);
    return n <= OPUS_MAX_48K ? n : 0;
}
static float* opus_channel(HwOpus* h, u32 c) { return h->pcm + (size_t)c * (OPUS_FIFO_48K + h->taps - 1); }
/* back to the state of a new decoder: FFmpeg's, no FIR history, the FIFO's silence */
static void hwopus_reset(HwOpus* h) {
    avcodec_flush_buffers(h->dec);
    for (u32 c = 0; c < h->channels; c++) memset(opus_channel(h, c), 0, (h->taps - 1 + OPUS_DELAY_48K) * sizeof(float));
    h->fifo = OPUS_DELAY_48K;
}
#endif
static void hwopus_destroy(SvcObj* o) {
    HwOpus* h = (HwOpus*)o->data;
#ifndef SNX_NO_H264
    avcodec_free_context(&h->dec);
    av_packet_free(&h->pkt);
    av_frame_free(&h->frame);
    free(h->in);
    free(h->fir);
#else
    (void)h;
#endif
}
/* the decoder for rate and channels: streams Opus streams, stereo of them coupled, map[c] the stream channel of
   output channel c (as libopus' multistream decoder takes them). 0 when it can't be made */
static int hwopus_open(HwOpus* h, u32 rate, u32 channels, u32 streams, u32 stereo, const u8* map) {
    h->rate = rate;
    h->channels = channels;
    pm_init(&h->lk);
#ifdef SNX_NO_H264
    (void)streams;
    (void)stereo;
    (void)map;
    LOG_LIMIT(1, "hwopus: this test fixture has no Opus decoder\n");
    return 0;
#else
    if (rate != 48000 && rate != 24000 && rate != 16000 && rate != 12000 && rate != 8000) return 0;
    if (!channels || channels > 255 || !streams || stereo > streams || streams + stereo > 255) return 0;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return 0;
    ffmpeg_log_init();
    h->dec = avcodec_alloc_context3(codec);
    h->pkt = av_packet_alloc();
    h->frame = av_frame_alloc();
    h->step = 48000 / rate;
    h->taps = h->step > 1 ? 24 * h->step + 1 : 1;
    /* one block: the filter, then each channel's history and FIFO */
    h->fir = calloc(h->taps + (size_t)channels * (OPUS_FIFO_48K + h->taps - 1), sizeof(float));
    if (!h->dec || !h->pkt || !h->frame || !h->fir) return 0;
    h->pcm = h->fir + h->taps;
    h->fifo = OPUS_DELAY_48K;
    if (h->step > 1) opus_fir(h->fir, h->taps, h->step);
    /* the OpusHead FFmpeg takes the layout from (RFC 7845): no pre-skip, the game trims that itself. Mapping family 0
       is one mono or stereo stream, 255 any number of streams with the mapping as given */
    int multi = streams != 1 || channels > 2 || map;
    u32 n = multi ? 21 + channels : 19;
    u8* eh = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!eh) return 0;
    memcpy(eh, "OpusHead", 8);
    eh[8] = 1;
    eh[9] = (u8)channels;
    eh[12] = 0x80; /* the input rate, 48000, little-endian */
    eh[13] = 0xBB;
    eh[18] = multi ? 255 : 0;
    if (multi) {
        eh[19] = (u8)streams;
        eh[20] = (u8)stereo;
        for (u32 i = 0; i < channels; i++) eh[21 + i] = map ? map[i] : (u8)i;
    }
    h->dec->extradata = eh;
    h->dec->extradata_size = (int)n;
    h->dec->ch_layout.nb_channels = (int)channels;
    if (avcodec_open2(h->dec, codec, NULL) < 0 || h->dec->ch_layout.nb_channels != (int)channels) return 0;
    return 1;
#endif
}
#ifndef SNX_NO_H264
/* the frames FFmpeg has for the packet just sent, onto the FIFO. 0 when one doesn't fit or isn't FFmpeg's format */
static int hwopus_queue(HwOpus* h) {
    int ok = 1;
    while (avcodec_receive_frame(h->dec, h->frame) == 0) {
        u32 n = (u32)h->frame->nb_samples;
        if (h->frame->format != AV_SAMPLE_FMT_FLTP || n > OPUS_FIFO_48K - h->fifo) ok = 0;
        for (u32 c = 0; ok && c < h->channels; c++)
            memcpy(opus_channel(h, c) + h->taps - 1 + h->fifo, h->frame->extended_data[c], n * sizeof(float));
        if (ok) h->fifo += n;
        av_frame_unref(h->frame);
    }
    return ok;
}
/* n 48 kHz samples off the FIFO into out, interleaved s16 at the decoder's rate (n / step of them) */
static void hwopus_render(HwOpus* h, u32 n, i16* out) {
    u32 ch = h->channels, step = h->step, keep = h->taps - 1;
    if (h->fifo < n) {
        /* FFmpeg held back more than the FIFO's silence covers: silence in front keeps the reply's length */
        LOG_LIMIT(4, "hwopus: %u samples short of the packet, silence put in front\n", n - h->fifo);
        for (u32 c = 0; c < ch; c++) {
            float* x = opus_channel(h, c) + keep;
            memmove(x + n - h->fifo, x, h->fifo * sizeof(float));
            memset(x, 0, (n - h->fifo) * sizeof(float));
        }
        h->fifo = n;
    }
    for (u32 c = 0; c < ch; c++) {
        float* x = opus_channel(h, c);
        i16* d = out + c;
        if (step == 1)
            for (u32 i = 0; i < n; i++) d[(u64)i * ch] = opus_s16(x[i]);
        else
            for (u32 k = 0; k < n / step; k++) {
                const float* xp = x + (u64)k * step;
                float acc = 0;
                for (u32 j = 0; j < h->taps; j++) acc += h->fir[j] * xp[j];
                d[(u64)k * ch] = opus_s16(acc);
            }
        /* the last keep samples taken are the next reply's history, the ones after them its FIFO */
        memmove(x, x + n, (keep + h->fifo - n) * sizeof(float));
    }
    h->fifo -= n;
}
#endif
/* one packet (header and payload) into out, interleaved s16 */
static u32 hwopus_decode(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
#ifdef SNX_NO_H264
    (void)h;
    (void)in;
    (void)in_size;
    (void)out;
    (void)out_size;
    (void)reset;
    (void)consumed;
    (void)samples;
    return OPUS_BAD_PACKET;
#else
    if (reset) hwopus_reset(h);
    if (in_size < 8) return OPUS_BAD_PACKET;
    u32 len = ((u32)in[0] << 24) | ((u32)in[1] << 16) | ((u32)in[2] << 8) | in[3];
    if (!len || len > in_size - 8) return OPUS_BAD_PACKET;
    /* refused before FFmpeg sees it, like libopus refuses a buffer too small before decoding */
    u32 n = opus_packet_48k(in + 8, len);
    if (!n || (u64)(n / h->step) * h->channels * sizeof(i16) > out_size) return OPUS_BAD_PACKET;
    if (h->incap < len) {
        u8* nb = realloc(h->in, (size_t)len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nb) return OPUS_BAD_PACKET;
        h->in = nb;
        h->incap = len;
    }
    memcpy(h->in, in + 8, len);
    memset(h->in + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    h->pkt->data = h->in;
    h->pkt->size = (int)len;
    if (avcodec_send_packet(h->dec, h->pkt) < 0 || !hwopus_queue(h)) return OPUS_BAD_PACKET;
    hwopus_render(h, n, out);
    *consumed = len + 8;
    *samples = n / h->step;
    return 0;
#endif
}
/* the decode commands: the reset flag comes first from DecodeInterleavedWithPerfAndResetOld on, the time the decode
   took from DecodeInterleavedWithPerfOld on */
static int decode(SvcObj* self, int reset, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    HwOpus* h = (HwOpus*)self->data;
    if (!input.addr || !output.addr) return OPUS_BAD_PACKET;
    u64 t0 = plat_ns();
    pm_lock(&h->lk);
    u32 rc = hwopus_decode(h, GP(input.addr), input.size, (i16*)GP(output.addr), output.size, reset, consumed, samples);
    pm_unlock(&h->lk);
    if (rc) {
        LOG_LIMIT(8, "hwopus: a %llu byte packet didn't decode, the game is told it's invalid\n",
                  (unsigned long long)input.size);
        return (int)rc;
    }
    if (perf_us) *perf_us = (plat_ns() - t0) / 1000;
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedOld(SvcObj* self, IpcBuf input, IpcBuf output,
                                                                     u32* consumed, u32* samples) {
    return decode(self, 0, input, output, consumed, samples, NULL);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamOld(SvcObj* self, IpcBuf input,
                                                                                   IpcBuf output, u32* consumed,
                                                                                   u32* samples) {
    return decode(self, 0, input, output, consumed, samples, NULL);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedWithPerfOld(SvcObj* self, IpcBuf input, IpcBuf output,
                                                                             u32* consumed, u32* samples,
                                                                             u64* perf_us) {
    return decode(self, 0, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamWithPerfOld(SvcObj* self, IpcBuf input,
                                                                                           IpcBuf output, u32* consumed,
                                                                                           u32* samples, u64* perf_us) {
    return decode(self, 0, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedWithPerfAndResetOld(SvcObj* self, u8 reset,
                                                                                     IpcBuf input, IpcBuf output,
                                                                                     u32* consumed, u32* samples,
                                                                                     u64* perf_us) {
    return decode(self, reset & 1, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamWithPerfAndResetOld(
    SvcObj* self, u8 reset, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    return decode(self, reset & 1, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleaved(SvcObj* self, u8 reset, IpcBuf input, IpcBuf output,
                                                                  u32* consumed, u32* samples, u64* perf_us) {
    return decode(self, reset & 1, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStream(SvcObj* self, u8 reset, IpcBuf input,
                                                                                IpcBuf output, u32* consumed,
                                                                                u32* samples, u64* perf_us) {
    return decode(self, reset & 1, input, output, consumed, samples, perf_us);
}
/* a work buffer the game allocates and passes as transfer memory, which the decoder here never touches: about what a
   libopus decoder needs per stream */
static u32 hwopus_work_size(u32 streams) { return 0x8000 + 0x8000 * (streams ? streams : 1); }
/* OpusMultiStreamParameters(Ex), the X buffer of the multistream commands: rate, channels, streams, coupled streams,
   (Ex: flags and padding,) then the channel mapping, 0x100 bytes */
static int ms_params(IpcBuf b, u32 v[4], const u8** map, int ex) {
    typedef nn_codec_detail_HardwareOpusMultiStreamDecoderParameterInternal Params;
    typedef nn_codec_detail_HardwareOpusMultiStreamDecoderParameterInternalEx ParamsEx;
    if (!b.addr || b.size < (ex ? sizeof(ParamsEx) : sizeof(Params))) return 0;
    memcpy(v, GP(b.addr), 16); /* the same four u32s first in both */
    *map = GP(b.addr) + (ex ? offsetof(ParamsEx, channel_mappings) : offsetof(Params, channel_mappings));
    return 1;
}
static int open_decoder(u32 rate, u32 ch, SvcObj** decoder) {
    SvcObj* r = nn_codec_detail_IHardwareOpusDecoder_new(sizeof(HwOpus));
    r->destroy = hwopus_destroy;
    if (ch > 2 || !hwopus_open((HwOpus*)r->data, rate, ch, 1, ch == 2, NULL)) {
        LOG_LIMIT(4, "hwopus: no decoder for %u Hz, %u channels\n", rate, ch);
        svcobj_unref(r);
        return OPUS_OPEN_FAILED;
    }
    *decoder = r;
    return 0;
}
static int open_ms_decoder(IpcBuf params, int ex, SvcObj** decoder) {
    u32 v[4];
    const u8* map;
    if (!ms_params(params, v, &map, ex)) return OPUS_OPEN_FAILED;
    SvcObj* r = nn_codec_detail_IHardwareOpusDecoder_new(sizeof(HwOpus));
    r->destroy = hwopus_destroy;
    if (!hwopus_open((HwOpus*)r->data, v[0], v[1], v[2], v[3], map)) {
        LOG_LIMIT(4, "hwopus: no decoder for %u Hz, %u channels in %u streams (%u stereo)\n", v[0], v[1], v[2], v[3]);
        svcobj_unref(r);
        return OPUS_OPEN_FAILED;
    }
    *decoder = r;
    return 0;
}
static u32 ms_work_size(IpcBuf params, int ex) {
    u32 v[4];
    const u8* map;
    return hwopus_work_size(ms_params(params, v, &map, ex) ? v[2] : 1);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoder(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternal* params, u32 work_buffer_size,
    u32 work_buffer, SvcObj** decoder) {
    return open_decoder(params->sample_rate, params->channel_count, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32 work_buffer_size,
    u32 work_buffer, SvcObj** decoder) {
    return open_decoder(params->sample_rate, params->channel_count, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderForMultiStream(
    SvcObj* self, IpcBuf params, u32 work_buffer_size, u32 work_buffer, SvcObj** decoder) {
    return open_ms_decoder(params, 0, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderForMultiStreamEx(
    SvcObj* self, IpcBuf params, u32 work_buffer_size, u32 work_buffer, SvcObj** decoder) {
    return open_ms_decoder(params, 1, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSize(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternal* params, u32* size) {
    *size = hwopus_work_size(1);
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    *size = hwopus_work_size(1);
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeExEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    *size = hwopus_work_size(1);
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(SvcObj* self, IpcBuf params,
                                                                                       u32* size) {
    *size = ms_work_size(params, 0);
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamEx(SvcObj* self, IpcBuf params,
                                                                                         u32* size) {
    *size = ms_work_size(params, 1);
    return 0;
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamExEx(SvcObj* self, IpcBuf params,
                                                                                           u32* size) {
    *size = ms_work_size(params, 1);
    return 0;
}
SvcObj* new_hwopus(void) { return nn_codec_detail_IHardwareOpusDecoderManager_new(0); }
