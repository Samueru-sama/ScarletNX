/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the audio renderer's DSP: RequestUpdate's parameters kept in an ARState, and the frame the console renders every
   5 ms from them. A frame, as Eden's audio_core builds its command list (renderer/command/command_generator.cpp):
   each playing voice channel decoded and resampled, ramped from its last volume and sent to its mix's buffers; the
   submixes, farthest from the final mix first, each through its effects and then into the mix it feeds; the final
   mix through its effects and its volume; then the device sinks, then the circular buffer sinks. The mix buffers
   hold samples on the 16-bit scale, as floats */
#include "hle/services/audren_dsp.h"
#include "runtime/platform/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static u16 rd16(const u8* p) {
    u16 v;
    memcpy(&v, p, 2);
    return v;
}
static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u64 rd64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}
static float rdf(const u8* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}
static void wr32(u8* p, u32 v) { memcpy(p, &v, 4); }
static void wr64(u8* p, u64 v) { memcpy(p, &v, 8); }
static i16 sat16(float x) { return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (i16)lrintf(x); }
static i32 sat32(float x) {
    return x >= 2147483520.0f ? 2147483647 : x <= -2147483648.0f ? (i32)0x80000000u : (i32)lrintf(x);
}

/* features met and not rendered, each logged once */
enum {
    SNX_AUDREN_WARN_SPLITTER = 1,
    SNX_AUDREN_WARN_BIQUAD = 2,
    SNX_AUDREN_WARN_EFFECT = 4,
    SNX_AUDREN_WARN_FORMAT = 8,
    SNX_AUDREN_WARN_COUNTS = 16
};
static void warn_once(ARState* s, u32 bit, const char* what) {
    if (s->warned & bit) return;
    s->warned |= bit;
    LOG("audren: %s\n", what);
}

/* the resampler's polyphase tables: 128 phases of 4 taps (default quality) or 8 (high quality). Row p, tap k is
   sample n = 128 k + 127 - p of a windowed sinc of 128 * taps points, so the phase's fraction is (p + 0.5) / 128.
   Each table is a cutoff, a gain and a window: Hamming (0.54) or Kaiser (beta). These are fitted to the console's
   tables as Eden carries them (renderer/command/resample/resample.cpp) and give them to 1 LSB in Q15, 2 for the
   default 0.75 one; the high quality cutoffs come out as 0.5, 0.75 and 1 times 1024 / 1023 */
#define SNX_AUDREN_PI 3.14159265358979323846
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64 && term > 1e-17 * sum; k++) {
        term *= (x / 2) * (x / 2) / ((double)k * k);
        sum += term;
    }
    return sum;
}
static void make_lut(float* out, int taps, double cutoff, double gain, double hamming, double beta) {
    int n_all = 128 * taps;
    for (int p = 0; p < 128; p++)
        for (int k = 0; k < taps; k++) {
            int n = 128 * k + 127 - p;
            double t = (n - (n_all - 1) / 2.0) / 128.0, x = SNX_AUDREN_PI * cutoff * t, u = 2.0 * n / (n_all - 1) - 1.0,
                   w;
            if (hamming > 0)
                w = hamming - (1 - hamming) * cos(2 * SNX_AUDREN_PI * n / (n_all - 1));
            else
                w = bessel_i0(beta * sqrt(fmax(0.0, 1 - u * u))) / bessel_i0(beta);
            out[p * taps + k] = (float)(rint(gain * cutoff * (x == 0 ? 1.0 : sin(x) / x) * w) / 32768.0);
        }
}
static void make_luts(ARState* s) {
    make_lut(s->lut4[0], 4, 0.499991, 38852.97, 0.54, 0); /* ratios over 1.3 */
    make_lut(s->lut4[1], 4, 0.999998, 32640.38, 0.54, 0); /* up to 1: the pass-through one */
    make_lut(s->lut4[2], 4, 0.752909, 34914.86, 0, 7.02743);
    make_lut(s->lut8[0], 8, 0.5 * 1024 / 1023, 32740.59, 0, 5.9);
    make_lut(s->lut8[1], 8, 0.75 * 1024 / 1023, 32787.99, 0, 5.9);
    make_lut(s->lut8[2], 8, 1024.0 / 1023, 32735.86, 0, 5.9); /* up to 1 */
}

ARState* audren_state_new(const u8* params) {
    u32 rate = rd32(params), frame = rd32(params + 4), buffers = rd32(params + 8), submixes = rd32(params + 0xC);
    u32 voices = rd32(params + 0x10), sinks = rd32(params + 0x14), effects = rd32(params + 0x18);
    if (!rate || !frame || frame > 1024 || buffers > 0x400 || submixes > 0x100 || voices > 0x400 || sinks > 0x40 ||
        effects > 0x100)
        return NULL;
    ARState* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rate = rate;
    s->frame = frame;
    s->buffers = buffers;
    s->nvoices = s->nres = voices;
    s->nmix = submixes + 1;
    s->neffects = effects;
    s->nsinks = sinks;
    s->voice = calloc(voices ? voices : 1, sizeof *s->voice);
    s->chan = calloc(voices ? voices : 1, sizeof *s->chan);
    s->mix = calloc(s->nmix, sizeof *s->mix);
    s->effect = calloc(effects ? effects : 1, sizeof *s->effect);
    s->sink = calloc(sinks ? sinks : 1, sizeof *s->sink);
    s->buf = calloc((size_t)(buffers + SNX_AUDREN_CHANNELS) * frame, sizeof *s->buf);
    s->temp = calloc(SNX_AUDREN_TEMP + SNX_AUDREN_HISTORY, sizeof *s->temp);
    s->order = calloc(s->nmix, sizeof *s->order);
    if (!s->voice || !s->chan || !s->mix || !s->effect || !s->sink || !s->buf || !s->temp || !s->order) {
        audren_state_free(s);
        return NULL;
    }
    make_luts(s);
    return s;
}

void audren_state_free(ARState* s) {
    if (!s) return;
    for (u32 i = 0; s->effect && i < s->neffects; i++) free(s->effect[i].look);
    free(s->voice);
    free(s->chan);
    free(s->mix);
    free(s->effect);
    free(s->sink);
    free(s->buf);
    free(s->temp);
    free(s->order);
    free(s);
}

/* ------------------------------------------------------------ the update (Eden's renderer/behavior/info_updater.cpp) */

/* a wave buffer the game hands over, checked against its own size as Eden's VoiceInfo::UpdateWaveBuffer does */
static void take_wave(ARWave* w, const u8* wb, u8 format) {
    i32 st, en;
    memcpy(&st, wb + 0x10, 4);
    memcpy(&en, wb + 0x14, 4);
    u64 size = rd64(wb + 8);
    if (st < 0 || en < 0) return;
    if (format == SNX_AUDREN_ADPCM) {
        u64 a = (u64)(st / 14) * 8 + (st % 14 ? (u64)(st % 14) / 2 + 1 + (st % 14) % 2 : 0);
        u64 b = (u64)(en / 14) * 8 + (en % 14 ? (u64)(en % 14) / 2 + 1 + (en % 14) % 2 : 0);
        if (a > size || b > size) return;
    } else {
        u64 bytes = format == SNX_AUDREN_PCMF32 ? 4 : 2;
        if ((u64)st * bytes > size || (u64)en * bytes > size) return;
    }
    w->addr = rd64(wb);
    w->size = size;
    w->start = st;
    w->end = en;
    w->loop = wb[0x18];
    w->ended = wb[0x19];
    memcpy(&w->loop_count, wb + 0x1C, 4);
    w->ctx = rd64(wb + 0x20);
    w->ctx_size = rd64(wb + 0x28);
    memcpy(&w->loop_start, wb + 0x30, 4);
    memcpy(&w->loop_end, wb + 0x34, 4);
    w->sent = 0;
}

/* VoiceIn: id 0, is_new 8, in_use 9, play state 0xA, format 0xB, rate 0xC, channels 0x18, pitch 0x1C, volume 0x20,
   biquads 0x24, wave buffer count 0x3C and index 0x40, data 0x48 and size 0x50, mix 0x58, splitter 0x5C, wave
   buffers 0x60 (0x38 each), channel resources 0x140, flush count 0x159, flags 0x15C, conversion quality 0x15E */
static void update_voice(ARState* s, ARVoice* v, const u8* vi, u8* vo) {
    if (vi[8]) { /* is_new: its buffers start over, and the channels with the first frame */
        memset(v, 0, sizeof *v);
        v->fresh = 1;
        v->play = SNX_AUDREN_STOPPED;
        for (int w = 0; w < SNX_AUDREN_WAVES; w++) v->wave[w].sent = 1; /* empty slots: nothing to hand the DSP */
    }
    v->in_use = vi[9];
    if (v->in_use) {
        v->last_play = v->play;
        switch (vi[0xA]) {
            case 0: v->play = SNX_AUDREN_STARTED; break;
            case 1:
                if (v->play != SNX_AUDREN_STOPPED) v->play = SNX_AUDREN_STOPPING;
                break;
            case 2: v->play = SNX_AUDREN_PAUSED; break;
        }
        if (vi[0x15E] <= 2) v->quality = vi[0x15E];
        v->format = vi[0xB];
        v->rate = rd32(vi + 0xC);
        u32 ch = rd32(vi + 0x18);
        v->channels = (u8)(ch < 1 ? 1 : ch > SNX_AUDREN_CHANNELS ? SNX_AUDREN_CHANNELS : ch);
        v->pitch = rdf(vi + 0x1C);
        v->volume = rdf(vi + 0x20);
        if (vi[0x24] || vi[0x24 + 12]) warn_once(s, SNX_AUDREN_WARN_BIQUAD, "voice biquad filters are not rendered");
        v->wave_count = rd32(vi + 0x3C);
        v->wave_index = rd16(vi + 0x40) % SNX_AUDREN_WAVES;
        v->data = rd64(vi + 0x48);
        v->data_size = rd64(vi + 0x50);
        v->flush += vi[0x159];
        v->mix_id = rd32(vi + 0x58);
        v->splitter_id = rd32(vi + 0x5C);
        for (int c = 0; c < SNX_AUDREN_CHANNELS; c++) v->res[c] = rd32(vi + 0x140 + c * 4);
        v->flags = vi[0x15C];
        for (int w = 0; w < SNX_AUDREN_WAVES; w++) {
            const u8* wb = vi + 0x60 + w * 0x38;
            if (!wb[0x1A]) take_wave(&v->wave[w], wb, v->format); /* not yet sent to the DSP */
        }
    }
    /* VoiceOut: played samples, consumed buffers, dropped. A voice not yet rendered since is_new reads 0 */
    if (!v->fresh && v->res[0] < s->nres) {
        wr64(vo, s->chan[v->res[0]].played);
        wr32(vo + 8, s->chan[v->res[0]].consumed);
    }
}

int audren_state_update(ARState* s, const u8* in, u64 is, u8* out, u64 os) {
    if (is < 0x40) return -41; /* InvalidUpdateInfo, the caller makes it a result */
    u32 ih[16];
    memcpy(ih, in, 64);
    u32 rev = ih[0];
    int revn = (int)((rev >> 24) & 0xFF) - '0';
    if (revn < 0) revn = 0;
    u64 need = 0x40 + (u64)ih[1] + ih[2] + ih[3] + ih[4] + ih[5] + ih[6] + ih[7] + ih[8] + ih[9];
    if (need > is) {
        WARN_LIMIT(4, "audren: update sections (%llu bytes) don't fit in the %llu byte input\n",
                   (unsigned long long)need, (unsigned long long)is);
        return -41;
    }
    u32 npools = ih[2] / 0x20, nres = ih[4] / 0x70, nvoices = ih[3] / 0x170, neff = ih[5] / 0xC0, nmix = ih[6] / 0x930,
        nsinks = ih[7] / 0x140;
    int eff_v2 = revn >= 9;
    u32 o_mp = npools * 0x10, o_vo = nvoices * 0x10, o_ef = neff * (eff_v2 ? 0x90 : 0x10), o_sk = nsinks * 0x20,
        o_pf = 0x10, o_bh = 0xB0, o_ri = revn >= 5 ? 0x10 : 0;
    u64 total = 0x40 + (u64)o_mp + o_vo + o_ef + o_sk + o_pf + o_bh + o_ri;
    if (total > os) {
        WARN_LIMIT(4, "audren: output buffer too small (%llu > %llu)\n", (unsigned long long)total,
                   (unsigned long long)os);
        return -4;
    }
    if (nvoices > s->nvoices || nres > s->nres || neff > s->neffects || nmix > s->nmix || nsinks > s->nsinks)
        warn_once(s, SNX_AUDREN_WARN_COUNTS,
                  "an update holds more voices, effects, mixes or sinks than the renderer opened");
    s->rev = rev;
    const u8* p = in + 0x40 + ih[1];
    const u8* pools = p;
    p += ih[2];
    const u8* res = p;
    p += ih[4];
    const u8* voices = p;
    p += ih[3];
    const u8* effects = p;
    p += ih[5];
    p += ih[9]; /* splitters: not rendered */
    const u8* mixes = p;
    p += ih[6];
    const u8* sinks = p;

    memset(out, 0, total);
    u32 oh[16] = {0};
    oh[0] = rev;
    oh[1] = o_bh;
    oh[2] = o_mp;
    oh[3] = o_vo;
    oh[5] = o_ef;
    oh[7] = o_sk;
    oh[8] = o_pf;
    oh[10] = o_ri;
    oh[15] = (u32)total;
    memcpy(out, oh, 64);
    u8* q = out + 0x40;
    /* pools: RequestAttach(4)->Attached(5), RequestDetach(2)->Detached(3). Guest memory needs no mapping here */
    for (u32 i = 0; i < npools; i++) {
        u32 st = rd32(pools + i * 0x20 + 0x10);
        wr32(q + i * 0x10, st == 4 ? 5 : st == 2 ? 3 : st);
    }
    q += o_mp;
    /* voice channel resources: id 0, send volumes 4, in_use 0x64 */
    for (u32 i = 0; i < nres && i < s->nres; i++) {
        const u8* r = res + i * 0x70;
        ARChannel* c = &s->chan[i];
        c->in_use = r[0x64];
        if (c->in_use) memcpy(c->mix, r + 4, sizeof c->mix);
    }
    for (u32 i = 0; i < nvoices; i++)
        if (i < s->nvoices) update_voice(s, &s->voice[i], voices + i * 0x170, q + i * 0x10);
    q += o_vo;
    /* effects: type 0, is_new 1, enabled 2, mix 4, process order 0x18, the type's parameters from 0x20. Out:
       Used(3) while enabled, Removed(4) */
    for (u32 i = 0; i < neff; i++) {
        const u8* ei = effects + i * 0xC0;
        if (i < s->neffects) {
            AREffect* e = &s->effect[i];
            if (ei[1] || e->type != ei[0]) e->fresh = 1;
            e->type = ei[0];
            e->enabled = ei[2];
            e->mix_id = rd32(ei + 4);
            e->order = rd32(ei + 0x18);
            memcpy(e->p, ei + 0x20, sizeof e->p);
        }
        q[i * (eff_v2 ? 0x90 : 0x10)] = ei[2] ? 3 : 4;
    }
    q += o_ef;
    /* mixes, by their mix id: volume 0, buffers 8, in_use 0xC, mix id 0x10, send volumes 0x24, destination 0x924 */
    for (u32 i = 0; i < nmix; i++) {
        const u8* mi = mixes + i * 0x930;
        u32 id = rd32(mi + 0x10);
        if (id >= s->nmix) id = i;
        if (id >= s->nmix) continue;
        ARMix* m = &s->mix[id];
        m->in_use = mi[0xC];
        m->volume = rdf(mi);
        m->buffers = rd32(mi + 8);
        if (m->buffers > SNX_AUDREN_MIX_OUTS) m->buffers = SNX_AUDREN_MIX_OUTS;
        memcpy(m->vol, mi + 0x24, sizeof m->vol);
        memcpy(&m->dest, mi + 0x924, 4);
    }
    /* sinks: type 0, in_use 1, the type's parameters from 0x20. Out: a circular buffer sink's write offset, two
       frames behind where it writes, as Eden's CircularBufferSinkInfo reports last_pos2 */
    for (u32 i = 0; i < nsinks; i++) {
        const u8* si = sinks + i * 0x140;
        if (i < s->nsinks) {
            ARSink* k = &s->sink[i];
            k->type = si[0];
            k->in_use = si[1];
            memcpy(k->p, si + 0x20, sizeof k->p);
            wr32(q + i * 0x20, k->last2);
        }
    }
    q += o_sk;
    q += o_pf;
    q += o_bh;
    if (o_ri) wr64(q, s->frames);
    return 0;
}

/* ------------------------------------------------------------ voices (renderer/command/data_source/decode.cpp) */

static u32 decode_pcm(const ARVoice* v, const ARWave* w, int ch, i32 st, i32 en, u32 offset, u32 want, i16* out) {
    if (!w->addr || !w->size || st >= en) return 0;
    u32 n = (u32)(en - st) - offset;
    if (want < n) n = want;
    u64 bytes = v->format == SNX_AUDREN_PCMF32 ? 4 : 2, first = ((u64)(st + (i32)offset) * v->channels) * bytes;
    if (first + (u64)n * v->channels * bytes > w->size) return 0;
    const u8* src = GP(w->addr + first);
    for (u32 i = 0; i < n; i++) {
        const u8* at = src + ((u64)i * v->channels + ch) * bytes;
        if (v->format == SNX_AUDREN_PCMF32)
            out[i] = sat16(rdf(at) * 32767.0f);
        else
            out[i] = (i16)rd16(at);
    }
    return n;
}

/* DSP-ADPCM: 8-byte frames, a header (scale, coefficient pair) and 14 four-bit codes, predicted from the last two
   samples with the voice's 8 coefficient pairs (its data). The context carries the header and those two samples */
static u32 decode_adpcm(const ARVoice* v, ARChannel* c, const ARWave* w, i32 st, i32 en, u32 offset, u32 want,
                        i16* out) {
    if (!w->addr || !w->size || en < st || v->data_size < 32 || !v->data) return 0;
    u32 end = (u32)(en % 14) + 16 * (u32)(en / 14) + (en % 14 ? 3 : 1);
    if (w->size < end / 2) return 0;
    u32 start = (u32)st + offset;
    if ((u32)en <= start) return 0;
    u32 todo = (u32)en - start;
    if (want < todo) todo = want;
    u32 in_frame = start % 14, pos = (start / 14) * 16 + in_frame;
    if (in_frame) pos += 2;
    u64 avail = w->size - pos / 2;
    const u8* wb = GP(w->addr + pos / 2);
    i16 coef[16];
    memcpy(coef, GP(v->data), sizeof coef);
    u16 header = c->adpcm_header;
    i32 scale = header & 0xF, ci = (header >> 4) & 7, c0 = coef[ci * 2], c1 = coef[ci * 2 + 1];
    i32 yn0 = c->yn0, yn1 = c->yn1;
    u32 ri = 0, wi = 0, left = todo;
#define SNX_AUDREN_NIB(x) ((x) & 8 ? (i32)(x) - 16 : (i32)(x))
#define SNX_AUDREN_SAMPLE(code)                                                                                        \
    do {                                                                                                               \
        i32 smp = (((i32)(code) * (1 << scale)) * 2048 + 0x400 + c0 * yn0 + c1 * yn1) >> 11;                           \
        smp = smp > 0x7FFF ? 0x7FFF : smp < -0x8000 ? -0x8000 : smp;                                                   \
        yn1 = yn0;                                                                                                     \
        yn0 = smp;                                                                                                     \
        out[wi++] = (i16)smp;                                                                                          \
    } while (0)
    while (left > 0) {
        if (pos % 16 == 0) { /* a frame's header */
            if (ri >= avail) break;
            header = wb[ri++];
            scale = header & 0xF;
            ci = (header >> 4) & 7;
            c0 = coef[ci * 2];
            c1 = coef[ci * 2 + 1];
            pos += 2;
            if (left >= 14 && ri + 7 <= avail) {
                for (u32 k = 0; k < 7; k++) {
                    SNX_AUDREN_SAMPLE(SNX_AUDREN_NIB(wb[ri + k] >> 4));
                    SNX_AUDREN_SAMPLE(SNX_AUDREN_NIB(wb[ri + k] & 0xF));
                }
                ri += 7;
                pos += 14;
                left -= 14;
                continue;
            }
        }
        if (ri >= avail) break;
        u32 code = wb[ri];
        if (pos & 1) {
            code &= 0xF;
            ri++;
        } else
            code >>= 4;
        SNX_AUDREN_SAMPLE(SNX_AUDREN_NIB(code));
        pos++;
        left--;
    }
#undef SNX_AUDREN_SAMPLE
#undef SNX_AUDREN_NIB
    c->adpcm_header = header;
    c->yn0 = (i16)yn0;
    c->yn1 = (i16)yn1;
    return wi;
}

static void end_wave(ARChannel* c, const ARWave* w) {
    c->valid[c->index] = 0;
    c->loop_count = 0;
    if (w->ended) c->played = 0;
    c->index = (c->index + 1) % SNX_AUDREN_WAVES;
    c->consumed++;
}

/* polyphase resampling over 4 taps (default quality), 8 (high) or the nearest sample (low), the ratio and fraction
   in Q15 (renderer/command/resample/resample.cpp). Default quality uses lut1, the pass-through table, up to a
   ratio of 1 (rEden 21c3595309); high quality's pass-through table is lut2 */
static void resample(const ARState* s, float* out, const i16* in, i64 ratio, i64* frac, u32 n, u8 quality) {
    u32 ri = 0;
    i64 f = *frac;
    if (quality == 2) {
        for (u32 i = 0; i < n; i++) {
            out[i] = in[ri + (f >= 0x4000)];
            f += ratio;
            ri += (u32)(f >> 15);
            f &= 0x7FFF;
        }
    } else if (quality == 1) {
        const float* t = ratio <= 0x8000 ? s->lut8[2] : ratio < (i64)(1.3 * 0x8000) ? s->lut8[1] : s->lut8[0];
        for (u32 i = 0; i < n; i++) {
            const float* k = t + ((((u32)f >> 8) << 3) & 1023);
            float acc = 0;
            for (int j = 0; j < 8; j++) acc += in[ri + j] * k[j];
            out[i] = floorf(acc);
            f += ratio;
            ri += (u32)(f >> 15);
            f &= 0x7FFF;
        }
    } else {
        const float* t = ratio <= 0x8000 ? s->lut4[1] : ratio < (i64)(1.3 * 0x8000) ? s->lut4[2] : s->lut4[0];
        for (u32 i = 0; i < n; i++) {
            const float* k = t + ((((u32)f >> 8) << 2) & 511);
            out[i] = floorf(in[ri] * k[0] + in[ri + 1] * k[1] + in[ri + 2] * k[2] + in[ri + 3] * k[3]);
            f += ratio;
            ri += (u32)(f >> 15);
            f &= 0x7FFF;
        }
    }
    *frac = f;
}

/* one channel of a voice for a frame, into out: its wave buffers in ring order from the one playing, decoded and
   resampled to the renderer's rate (Eden's DecodeFromWaveBuffers). A buffer that ends is consumed; a looping one
   starts over until its loop count runs out */
static void voice_channel(ARState* s, ARVoice* v, int ch, float* out) {
    ARChannel* c = &s->chan[v->res[ch]];
    u32 n = s->frame, taps = v->quality == 1 ? 8 : 4;
    float pitch = v->pitch > 0.0f && v->pitch <= 8.0f ? v->pitch : 1.0f;
    u32 rate = v->rate && v->rate <= 192000 ? v->rate : s->rate;
    i64 ratio = (i64)((float)rate / (float)s->rate * pitch * 32768.0f);
    if (ratio <= 0) ratio = 0x8000;
    i64 need = (c->fraction + (i64)n * ratio) >> 15;
    if (need + taps > SNX_AUDREN_TEMP) {
        memset(out, 0, n * sizeof *out);
        return;
    }
    i16* t = s->temp;
    memcpy(t, c->history, taps * sizeof *t);
    u32 pos = taps, read = 0;
    while (read < (u32)need) {
        if (!c->valid[c->index]) break; /* starved */
        const ARWave* w = &v->wave[c->index];
        if (c->offset == 0 && v->format == SNX_AUDREN_ADPCM && w->ctx && w->ctx_size >= 6) {
            const u8* x = GP(w->ctx);
            c->adpcm_header = rd16(x);
            c->yn0 = (i16)rd16(x + 2);
            c->yn1 = (i16)rd16(x + 4);
        }
        i32 st = w->start, en = w->end;
        if (w->loop && c->loop_count > 0 && w->loop_start < w->loop_end) {
            st = w->loop_start;
            en = w->loop_end;
        }
        u32 got;
        if (v->format == SNX_AUDREN_PCM16 || v->format == SNX_AUDREN_PCMF32)
            got = decode_pcm(v, w, ch, st, en, c->offset, (u32)need - read, t + pos);
        else if (v->format == SNX_AUDREN_ADPCM)
            got = decode_adpcm(v, c, w, st, en, c->offset, (u32)need - read, t + pos);
        else {
            warn_once(s, SNX_AUDREN_WARN_FORMAT, "a voice's sample format is not rendered");
            got = 0;
        }
        c->played += got;
        read += got;
        pos += got;
        c->offset += got;
        if (got && c->offset < (u32)(en - st)) continue;
        c->offset = 0;
        if (w->loop) {
            c->loop_count++;
            if (w->loop_count >= 0 && (c->loop_count > w->loop_count || !got)) end_wave(c, w);
            if (!got) break;
            if (v->flags & 1) c->played = 0; /* the played count starts over at the loop point */
        } else
            end_wave(c, w);
    }
    memset(t + pos, 0, (taps + (u32)need - pos) * sizeof *t);
    resample(s, out, t, ratio, &c->fraction, n, v->quality);
    memcpy(c->history, t + need, taps * sizeof *t);
}

/* Eden's VoiceInfo::UpdateParametersForCommandGeneration: a flush drops buffers from the wave buffer index; a
   started voice hands its new buffers to the DSP; a stop consumes what was queued and starts the channels over.
   Returns 1 when the voice has something to play this frame */
static int voice_prepare(ARState* s, ARVoice* v) {
    for (int ch = 0; ch < v->channels; ch++)
        if (v->res[ch] >= s->nres) return 0;
    if (v->fresh) { /* the first frame since is_new: the channels start clean, the volumes without a ramp */
        for (int ch = 0; ch < v->channels; ch++) {
            ARChannel* c = &s->chan[v->res[ch]];
            float mix[SNX_AUDREN_MIX_OUTS];
            memcpy(mix, c->mix, sizeof mix);
            u8 in_use = c->in_use;
            memset(c, 0, sizeof *c);
            memcpy(c->mix, mix, sizeof mix);
            memcpy(c->prev_mix, mix, sizeof mix);
            c->in_use = in_use;
        }
        v->prev_volume = v->volume;
        v->fresh = 0;
    }
    if (v->flush) {
        u32 w = v->wave_index;
        for (u32 i = 0; i < v->flush && i < SNX_AUDREN_WAVES; i++, w = (w + 1) % SNX_AUDREN_WAVES) {
            v->wave[w].sent = 1;
            for (int ch = 0; ch < v->channels; ch++) {
                ARChannel* c = &s->chan[v->res[ch]];
                if (c->index == w) {
                    c->index = (c->index + 1) % SNX_AUDREN_WAVES;
                    c->consumed++;
                }
                c->valid[w] = 0;
            }
        }
        v->flush = 0;
    }
    switch (v->play) {
        case SNX_AUDREN_STARTED:
            for (int w = 0; w < SNX_AUDREN_WAVES; w++)
                if (!v->wave[w].sent) {
                    for (int ch = 0; ch < v->channels; ch++) s->chan[v->res[ch]].valid[w] = 1;
                    v->wave[w].sent = 1;
                }
            v->was_playing = 0;
            for (int w = 0; w < SNX_AUDREN_WAVES; w++)
                if (s->chan[v->res[0]].valid[w]) return 1;
            return 0;
        case SNX_AUDREN_STOPPING:
            for (int w = 0; w < SNX_AUDREN_WAVES; w++) {
                v->wave[w].sent = 1;
                for (int ch = 0; ch < v->channels; ch++) {
                    ARChannel* c = &s->chan[v->res[ch]];
                    if (c->valid[w]) {
                        c->index = (c->index + 1) % SNX_AUDREN_WAVES;
                        c->consumed++;
                    }
                    c->valid[w] = 0;
                }
            }
            for (int ch = 0; ch < v->channels; ch++) {
                ARChannel* c = &s->chan[v->res[ch]];
                c->offset = 0;
                c->played = 0;
                c->adpcm_header = 0;
                c->yn0 = c->yn1 = 0;
                memset(c->history, 0, sizeof c->history);
                c->fraction = 0;
            }
            v->play = SNX_AUDREN_STOPPED;
            v->was_playing = v->last_play == SNX_AUDREN_STARTED;
            return 0;
        default: v->was_playing = v->last_play == SNX_AUDREN_STARTED; return 0;
    }
}

/* ------------------------------------------------------------ effects (renderer/command/effect) */

static float* mixbuf(ARState* s, const ARMix* m, i32 index) {
    if (index < 0 || (u32)index >= m->buffers) return NULL;
    return s->buf + (size_t)(m->offset + index) * s->frame;
}

/* an aux ring in guest memory: its DSP-side offsets at info + 0x40 (read 0, write 4, lost 8, total 0xC), its samples,
   32-bit, from info + 0x80. A channel writes a frame at write offset + its place, and the last channel moves the
   offset past all of them (Eden's WriteAuxBufferDsp and ReadAuxBufferDsp) */
static u32 aux_ring(u64 info, u32 count_max, float* samples, u32 n, u32 place, u32 advance, int write) {
    if (!info || !count_max || n > count_max) return 0;
    u8* dsp = GP(info + 0x40);
    u8* ring = GP(info + 0x80);
    u32 at = rd32(dsp + (write ? 4 : 0)) + place;
    if (at > count_max) return 0;
    for (u32 left = n, k = 0; left > 0;) {
        u32 run = count_max - at < left ? count_max - at : left;
        for (u32 i = 0; i < run; i++, k++) {
            if (write)
                wr32(ring + (u64)(at + i) * 4, (u32)sat32(samples[k]));
            else
                samples[k] = (float)(i32)rd32(ring + (u64)(at + i) * 4);
        }
        at = (at + run) % count_max;
        left -= run;
    }
    if (advance) wr32(dsp + (write ? 4 : 0), (rd32(dsp + (write ? 4 : 0)) + advance) % count_max);
    return n;
}

static void effect_aux(ARState* s, ARMix* m, AREffect* e) {
    const u8* p = e->p; /* inputs 0, outputs 0x18, channels 0x30, ring samples 0x38, send info 0x40, return 0x50 */
    u32 count = rd32(p + 0x30), count_max = rd32(p + 0x38), n = s->frame;
    u64 send = rd64(p + 0x40), ret = rd64(p + 0x50);
    if (!send || !ret) return;
    if (count > SNX_AUDREN_MIX_OUTS) count = SNX_AUDREN_MIX_OUTS;
    if (!e->enabled) {
        for (int k = 0; k < 2; k++) memset(GP((k ? ret : send) + 0x40), 0, 0x10);
        for (u32 ch = 0; ch < count; ch++) {
            float *in = mixbuf(s, m, (i8)p[ch]), *out = mixbuf(s, m, (i8)p[0x18 + ch]);
            if (in && out && in != out) memcpy(out, in, n * sizeof *out);
        }
        return;
    }
    for (u32 ch = 0; ch < count; ch++) {
        float *in = mixbuf(s, m, (i8)p[ch]), *out = mixbuf(s, m, (i8)p[0x18 + ch]);
        u32 advance = ch + 1 == count ? (ch + 1) * n : 0;
        if (in) aux_ring(send, count_max, in, n, ch * n, advance, 1);
        if (out && aux_ring(ret, count_max, out, n, ch * n, advance, 0) != n) memset(out, 0, n * sizeof *out);
    }
}

/* Eden's reciprocal estimate: a in 1/512ths rounded down, its reciprocal from the middle of that step in 1/256ths */
static double recip_estimate(double a) {
    i32 q = (i32)(a * 512.0);
    double r = 1.0 / (((double)q + 0.5) / 512.0);
    return (double)(i32)(256.0 * r + 0.5) / 256.0;
}

/* the light limiter (renderer/command/effect/light_limiter.cpp): a detector averaging |x| with the attack or the
   release coefficient, a gain that follows threshold over the detector while the detector is above the threshold,
   applied to samples delayed by the look-ahead. Mode 1 takes the detector's reciprocal estimate; any other mode an
   exact reciprocal (Eden's mode 0 "refinement" is not one; what the console's mode 0 does is not known) */
static void effect_limiter(ARState* s, ARMix* m, AREffect* e) {
    const u8* p = e->p;
    u32 channels = rd16(p + 0xE), n = s->frame;
    if (channels > SNX_AUDREN_CHANNELS) channels = SNX_AUDREN_CHANNELS;
    i32 la_min, la_max;
    memcpy(&la_min, p + 0x38, 4);
    memcpy(&la_max, p + 0x3C, 4);
    if (la_max < 1) la_max = 1;
    if (la_max > 4800) la_max = 4800;
    if (la_min < 1) la_min = 1;
    if (la_min > la_max) la_min = la_max;
    if (e->fresh || e->look_len != (u32)la_max) {
        free(e->look);
        e->look = calloc((size_t)la_max * SNX_AUDREN_CHANNELS, sizeof *e->look);
        e->look_len = e->look ? (u32)la_max : 0;
        for (int c = 0; c < SNX_AUDREN_CHANNELS; c++) {
            e->avg[c] = 0.0f;
            e->gain[c] = 1.0f;
            e->look_pos[c] = 0;
        }
    }
    e->fresh = 0;
    float attack = rdf(p + 0x24), release = rdf(p + 0x28), threshold = rdf(p + 0x2C), in_gain = rdf(p + 0x30),
          out_gain = rdf(p + 0x34);
    int mode1 = p[0x43] == 1;
    for (u32 ch = 0; ch < channels; ch++) {
        float *in = mixbuf(s, m, (i8)p[ch]), *out = mixbuf(s, m, (i8)p[6 + ch]);
        if (!in || !out) continue;
        if (!e->enabled || !e->look) {
            if (in != out) memcpy(out, in, n * sizeof *out);
            continue;
        }
        float* look = e->look + (size_t)ch * e->look_len;
        for (u32 i = 0; i < n; i++) {
            float x = in[i] / 32768.0f * in_gain, ax = fabsf(x);
            e->avg[ch] += (ax - e->avg[ch]) * (ax > e->avg[ch] ? attack : release);
            float att = 1.0f;
            if (e->avg[ch] > threshold)
                att = threshold * (float)(mode1 ? recip_estimate(e->avg[ch]) : 1.0 / e->avg[ch]);
            e->gain[ch] += (att - e->gain[ch]) * (att < e->gain[ch] ? attack : release);
            float delayed = look[e->look_pos[ch]];
            look[e->look_pos[ch]] = x;
            e->look_pos[ch] = (e->look_pos[ch] + 1) % (u32)la_min;
            out[i] = delayed * e->gain[ch] * out_gain * 32768.0f;
        }
    }
}

/* a buffer mixer: inputs 0, outputs 0x18, volumes 0x30, count 0x90 */
static void effect_mix(ARState* s, ARMix* m, AREffect* e) {
    const u8* p = e->p;
    u32 count = rd32(p + 0x90), n = s->frame;
    if (!e->enabled) return;
    for (u32 i = 0; i < count && i < SNX_AUDREN_MIX_OUTS; i++) {
        float vol = rdf(p + 0x30 + i * 4);
        float *in = mixbuf(s, m, (i8)p[i]), *out = mixbuf(s, m, (i8)p[0x18 + i]);
        if (!in || !out || vol == 0.0f) continue;
        for (u32 k = 0; k < n; k++) out[k] += in[k] * vol;
    }
}

/* a mix's effects, by their process order */
static void mix_effects(ARState* s, u32 id) {
    ARMix* m = &s->mix[id];
    u32 done = 0;
    for (;;) {
        AREffect* next = NULL;
        for (u32 i = 0; i < s->neffects; i++) {
            AREffect* e = &s->effect[i];
            if (!e->type || e->mix_id != id || e->order < done) continue;
            if (!next || e->order < next->order) next = e;
        }
        if (!next) return;
        switch (next->type) {
            case SNX_AUDREN_EFFECT_AUX: effect_aux(s, m, next); break;
            case SNX_AUDREN_EFFECT_LIMITER: effect_limiter(s, m, next); break;
            case SNX_AUDREN_EFFECT_MIX: effect_mix(s, m, next); break;
            default:
                warn_once(s, SNX_AUDREN_WARN_EFFECT, "an effect type (delay, reverb, biquad, ...) is not rendered");
        }
        done = next->order + 1;
        if (!done) return; /* the order wrapped */
    }
}

/* ------------------------------------------------------------ the frame */

/* the mixes that reach the final mix, farthest first, and their buffers in that order (Eden's MixContext::SortInfo
   and CalcMixBufferOffset): the final mix takes the last ones */
static u32 sort_mixes(ARState* s) {
    u32 n = 0;
    for (u32 i = 0; i < s->nmix; i++) {
        ARMix* m = &s->mix[i];
        m->distance = -1;
        if (!m->in_use) continue;
        i32 d = 0;
        for (u32 id = i; id != SNX_AUDREN_FINAL_MIX; d++) {
            if (d >= (i32)s->nmix || id >= s->nmix || !s->mix[id].in_use) {
                d = -1;
                break;
            }
            id = (u32)s->mix[id].dest;
        }
        if (d < 0) continue;
        m->distance = d;
        s->order[n++] = (i32)i;
    }
    for (u32 a = 1; a < n; a++) /* farthest first; a stable insertion sort keeps equal ones in mix order */
        for (u32 b = a; b > 0 && s->mix[s->order[b - 1]].distance < s->mix[s->order[b]].distance; b--) {
            i32 t = s->order[b];
            s->order[b] = s->order[b - 1];
            s->order[b - 1] = t;
        }
    i32 offset = 0;
    for (u32 k = 0; k < n; k++) {
        ARMix* m = &s->mix[s->order[k]];
        if ((u32)offset + m->buffers > s->buffers) { /* more buffers than the renderer opened: not mixed */
            m->distance = -1;
            continue;
        }
        m->offset = offset;
        offset += (i32)m->buffers;
    }
    return n;
}

static void play_voice(ARState* s, ARVoice* v) {
    u32 n = s->frame;
    ARMix* m = v->mix_id < s->nmix ? &s->mix[v->mix_id] : NULL;
    if (v->mix_id == SNX_AUDREN_UNUSED_MIX && v->splitter_id != 0xFFFFFFFFu)
        warn_once(s, SNX_AUDREN_WARN_SPLITTER, "voices sent through splitters are not rendered");
    for (int ch = 0; ch < v->channels; ch++) {
        float* tmp = s->buf + (size_t)(s->buffers + ch) * n;
        voice_channel(s, v, ch, tmp);
        if (!m || m->distance < 0) continue;
        ARChannel* c = &s->chan[v->res[ch]];
        float vol = v->prev_volume, dv = (v->volume - v->prev_volume) / (float)n;
        for (u32 i = 0; i < n; i++) tmp[i] *= vol + dv * (float)i;
        for (u32 j = 0; j < m->buffers; j++) {
            float g = c->prev_mix[j], dg = (c->mix[j] - c->prev_mix[j]) / (float)n;
            if (g == 0.0f && dg == 0.0f) continue;
            float* dst = s->buf + (size_t)(m->offset + (i32)j) * n;
            for (u32 i = 0; i < n; i++) dst[i] += tmp[i] * (g + dg * (float)i);
        }
        memcpy(c->prev_mix, c->mix, sizeof c->mix);
    }
    v->prev_volume = v->volume;
}

static void sink_frame(ARState* s, ARSink* k, i16* stereo, int* device) {
    ARMix* f = &s->mix[SNX_AUDREN_FINAL_MIX];
    u32 n = s->frame;
    if (k->type ==
        SNX_AUDREN_SINK_DEVICE) { /* name 0, input count 0x100, inputs 0x104, downmix 0x10B, coefficients 0x10C */
        u32 count = rd32(k->p + 0x100);
        if (!count || count > SNX_AUDREN_CHANNELS || !stereo) return;
        const float* in[SNX_AUDREN_CHANNELS];
        for (u32 c = 0; c < count; c++)
            if (!(in[c] = mixbuf(s, f, (i8)k->p[0x104 + c]))) return;
        float coef[4] = {1.0f, 0.707f, 0.251f, 0.707f};
        if (k->p[0x10B]) memcpy(coef, k->p + 0x10C, sizeof coef);
        for (u32 i = 0; i < n; i++) {
            float l, r;
            if (count >= 6) { /* front left, front right, centre, LFE, back left, back right */
                float mid = in[2][i] * coef[1] + in[3][i] * coef[2];
                l = in[0][i] * coef[0] + mid + in[4][i] * coef[3];
                r = in[1][i] * coef[0] + mid + in[5][i] * coef[3];
            } else {
                l = in[0][i];
                r = count >= 2 ? in[1][i] : l;
            }
            stereo[i * 2] = sat16(l);
            stereo[i * 2 + 1] = sat16(r);
        }
        *device = 1;
    } else if (k->type == SNX_AUDREN_SINK_CIRCULAR) {
        /* address 0, size 8, input count 0xC, inputs 0x1C (the format is the byte at 0x18). A frame is each input
           in turn, n 16-bit samples apiece (Eden's CircularBufferSinkCommand) */
        u64 addr = rd64(k->p);
        u32 size = rd32(k->p + 8), count = rd32(k->p + 0xC);
        if (!addr || !size || count > SNX_AUDREN_CHANNELS) return;
        u32 pos = k->pos;
        for (u32 c = 0; c < count; c++) {
            const float* in = mixbuf(s, f, (i8)k->p[0x1C + c]);
            if (pos + n * 2 <= size) {
                i16* dst = (i16*)(void*)GP(addr + pos);
                for (u32 i = 0; i < n; i++) {
                    i16 v = in ? sat16(in[i]) : 0;
                    memcpy(dst + i, &v, 2);
                }
            }
            pos += n * 2;
            if (pos >= size) pos = 0;
        }
        k->last2 = k->last;
        k->last = k->pos;
        k->pos = (k->pos + count * n * 2) % size;
    }
}

int audren_frame(ARState* s, i16* stereo) {
    u32 n = s->frame;
    memset(s->buf, 0, (size_t)(s->buffers + SNX_AUDREN_CHANNELS) * n * sizeof *s->buf);
    u32 nsorted = sort_mixes(s);
    for (u32 i = 0; i < s->nvoices; i++) {
        ARVoice* v = &s->voice[i];
        if (!v->in_use || !v->wave_count) continue;
        if (voice_prepare(s, v)) play_voice(s, v);
    }
    for (u32 k = 0; k < nsorted; k++) { /* the submixes, then the final mix */
        u32 id = (u32)s->order[k];
        ARMix* m = &s->mix[id];
        if (m->distance < 0) continue;
        mix_effects(s, id);
        if (id == SNX_AUDREN_FINAL_MIX) {
            for (u32 b = 0; b < m->buffers; b++) {
                float* x = s->buf + (size_t)(m->offset + (i32)b) * n;
                for (u32 i = 0; i < n; i++) x[i] *= m->volume;
            }
            continue;
        }
        ARMix* d = &s->mix[m->dest];
        for (u32 a = 0; a < m->buffers; a++)
            for (u32 b = 0; b < d->buffers; b++) {
                float g = m->volume * m->vol[a][b];
                if (g == 0.0f) continue;
                const float* src = s->buf + (size_t)(m->offset + (i32)a) * n;
                float* dst = s->buf + (size_t)(d->offset + (i32)b) * n;
                for (u32 i = 0; i < n; i++) dst[i] += src[i] * g;
            }
    }
    int device = 0;
    if (s->mix[SNX_AUDREN_FINAL_MIX].distance >= 0) {
        for (u32 i = 0; i < s->nsinks; i++)
            if (s->sink[i].in_use && s->sink[i].type == SNX_AUDREN_SINK_DEVICE)
                sink_frame(s, &s->sink[i], device ? NULL : stereo, &device);
        for (u32 i = 0; i < s->nsinks; i++)
            if (s->sink[i].in_use && s->sink[i].type == SNX_AUDREN_SINK_CIRCULAR)
                sink_frame(s, &s->sink[i], NULL, &device);
    }
    s->frames++;
    return device;
}
