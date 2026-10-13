/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compile the production implementation with either the isolated FFmpeg double (default CTest target),
   or real FFmpeg headers/libraries and -DSNX_TEST_REAL_FFMPEG for synthetic I_PCM decode coverage. */
#include "../src/nx/gpu/video/nvdec.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static u8 memory[0x6000];
static u32 regs[0x1000 / 4];
static u8* const setup = memory + 0x1000;
static u8* const stream = memory + 0x2000;
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
u8* iova_ptr(u64 addr, u64 size) {
    return addr && addr <= sizeof memory && size <= sizeof memory - addr ? memory + addr : NULL;
}
/* Surface addressing is deliberately linear in this test; the production NV12 interleave/write path runs. */
u64 bl_offset(u32 x, u32 y, u32 z, u32 pitch, u32 height, u32 bh, u32 bd) {
    (void)z;
    (void)height;
    (void)bh;
    (void)bd;
    return (u64)y * pitch + x;
}
static void put32(u32 off, u32 value) { memcpy(setup + off, &value, 4); }
static void fixture(void) {
    memset(memory, 0, sizeof memory);
    memset(regs, 0, sizeof regs);
    regs[0x200 / 4] = 3;
    regs[0x404 / 4] = 0x10;
    regs[0x408 / 4] = 0x20;
    regs[0x424 / 4] = 0x30;
    regs[0x430 / 4] = 0x40;
    regs[0x474 / 4] = 0x50;
    put32(0x48, 3);
    memcpy(stream, "\x65\x88\x84", 3);
    put32(0x60, 1); /* frame_mbs_only_flag */
    put32(0x64, 1);
    put32(0x68, 1);
    put32(0x8c, 64);
    put32(0x90, 64);
    put32(0xb0, 2 | (1 << 12) | (2 << 14)); /* direct_8x8, 4:2:0, POC type 2 */
    memset(setup + 0x1c0, 16, 6 * 16 + 2 * 64);
    memset(memory + 0x4000, 0xcc, 0x2000);
}
static void status_is(int failed) {
    CHECK(rd32(memory, 0x3000) == (failed ? 0u : 1u));
    CHECK(rd32(memory, 0x3004) == (failed ? 1u : 0u));
    CHECK(rd32(memory, 0x300c) == (u32)failed);
    if (failed)
        for (unsigned i = 0x4000; i < sizeof memory; ++i) CHECK(memory[i] == 0xcc);
}
static void pixels_are(u8 y, u8 u, u8 v) {
    status_is(0);
    for (unsigned row = 0; row < 16; ++row)
        for (unsigned col = 0; col < 16; ++col) CHECK(memory[0x4000 + row * 64 + col] == y);
    for (unsigned row = 0; row < 8; ++row)
        for (unsigned col = 0; col < 8; ++col) {
            CHECK(memory[0x5000 + row * 64 + col * 2] == u);
            CHECK(memory[0x5000 + row * 64 + col * 2 + 1] == v);
        }
}
static void padding_is_zero(const AVPacket* p) {
    CHECK(p->buf != NULL);
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; ++i) CHECK(p->data[p->size + i] == 0);
}
static void packet_tests(void) {
    AVPacket* p = av_packet_alloc();
    CHECK(p != NULL);
    const u8 hdr[] = {0, 0, 0, 1, 0x67, 0x80};
    const u8 annex[] = {0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x61, 0x99};
    const u8 avcc[] = {0, 0, 0, 2, 0x65, 0x88, 0, 0, 0, 2, 0x61, 0x99};
    const u8 converted[] = {0, 0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x61, 0x99};
    CHECK(!make_packet(p, hdr, sizeof hdr, annex, sizeof annex));
    CHECK(p->size == sizeof hdr + sizeof annex);
    CHECK(!memcmp(p->data, hdr, sizeof hdr));
    CHECK(!memcmp(p->data + sizeof hdr, annex, sizeof annex));
    padding_is_zero(p);
    CHECK(!make_packet(p, hdr, 0, avcc, sizeof avcc));
    CHECK(p->size == sizeof converted && !memcmp(p->data, converted, sizeof converted));
    padding_is_zero(p);
    /* A complete AVCC NAL with a 00 00 01 xx length must not be mistaken for an Annex B start code. */
    u8 large[4 + 357];
    memset(large, 0x88, sizeof large);
    large[0] = large[1] = 0;
    large[2] = 1;
    large[3] = 0x65;
    large[4] = 0x65;
    CHECK(!make_packet(p, hdr, 0, large, sizeof large));
    CHECK(p->size == sizeof large && !memcmp(p->data, "\0\0\0\1", 4));
    CHECK(!memcmp(p->data + 4, large + 4, sizeof large - 4));
    padding_is_zero(p);
    /* A much shorter packet must have fresh zero padding, not the previous payload's tail. */
    CHECK(!make_packet(p, hdr, 0, avcc + 4, 2));
    CHECK(p->size == 5 && !memcmp(p->data, "\0\0\1\x65\x88", 5));
    padding_is_zero(p);
    const u8 leading[] = {0, 0, 0, 0, 0, 1, 0x65, 0x80};
    CHECK(!make_packet(p, hdr, 0, leading, sizeof leading));
    CHECK(p->size == sizeof leading && !memcmp(p->data, leading, sizeof leading));
    padding_is_zero(p);
    const u8 bad[][6] = {{0, 0, 0, 8, 0x65, 0x88},
                         {0, 0, 0, 0, 0x65, 0x88},
                         {0, 0, 0, 2, 0xe5, 0x88},
                         {0, 0, 1, 0, 0, 0},
                         {0xff, 0, 0, 0, 0, 0}};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i)
        CHECK(make_packet(p, hdr, 0, bad[i], sizeof bad[i]) == AVERROR_INVALIDDATA);
    for (unsigned i = 0; i < 4; ++i) CHECK(make_packet(p, hdr, 0, avcc, i) == AVERROR_INVALIDDATA);
    av_packet_free(&p);
}

#ifndef SNX_TEST_REAL_FFMPEG
/* Script both API calls: capture=1 invokes the real get_buf callback, including on errors/older output.
   This double tests submission, NOT H.264 decoding or the real FFmpeg ABI. */
typedef struct {
    char call;
    int result, capture;
} Event;
static const Event* events;
static unsigned event_count, event_at, sends;
static int expected_headers, alloc_fail, ref_fail;
static const u8* retry_data;
static int retry_size;
static u8 planes[384];
static Event next_event(char call, AVCodecContext* c) {
    CHECK(event_at < event_count);
    Event e = events[event_at++];
    CHECK(e.call == call);
    if (e.capture) {
        AVFrame f = {0};
        CHECK(!c->get_buffer2(c, &f, 0));
        av_frame_unref(&f);
    }
    return e;
}
static void run(NvdecCtx* d, const Event* script, unsigned n, int headers, int failed) {
    events = script;
    event_count = n;
    event_at = sends = 0;
    expected_headers = headers;
    retry_data = NULL;
    memset(memory + 0x4000, 0xcc, 0x2000);
    nvdec_execute(d, regs);
    CHECK(event_at == event_count);
    status_is(failed);
    if (failed) CHECK(!d->cur_new && !d->cur->buf[0] && !d->last_hdr_n);
}
#define RUN(d, script, headers, failed) run(d, script, sizeof(script) / sizeof *(script), headers, failed)
const AVCodec* avcodec_find_decoder(int id) {
    static AVCodec codec;
    CHECK(id == AV_CODEC_ID_H264);
    return &codec;
}
AVCodecContext* avcodec_alloc_context3(const AVCodec* codec) {
    (void)codec;
    return calloc(1, sizeof(AVCodecContext));
}
int avcodec_open2(AVCodecContext* c, const AVCodec* codec, void* options) {
    (void)codec;
    (void)options;
    CHECK(c->thread_count == 1);
    return 0;
}
void avcodec_free_context(AVCodecContext** c) {
    free(*c);
    *c = NULL;
}
int avcodec_default_get_buffer2(AVCodecContext* c, AVFrame* f, int flags) {
    (void)c;
    (void)flags;
    memset(planes, 81, 256);
    memset(planes + 256, 90, 64);
    memset(planes + 320, 240, 64);
    f->width = f->height = 16;
    f->format = AV_PIX_FMT_YUV420P;
    f->data[0] = planes;
    f->data[1] = planes + 256;
    f->data[2] = planes + 320;
    f->linesize[0] = 16;
    f->linesize[1] = f->linesize[2] = 8;
    f->buf[0] = planes;
    return 0;
}
int avcodec_send_packet(AVCodecContext* c, const AVPacket* p) {
    CHECK(p != NULL);
    padding_is_zero(p);
    ++sends;
    CHECK((p->size > 4 && p->data[4] == 0x67) == expected_headers);
    if (retry_data) CHECK(p->data == retry_data && p->size == retry_size);
    Event e = next_event('S', c);
    if (e.result == AVERROR(EAGAIN)) {
        retry_data = p->data;
        retry_size = p->size;
    }
    return e.result;
}
int avcodec_receive_frame(AVCodecContext* c, AVFrame* f) {
    (void)f;
    return next_event('R', c).result;
}
AVPacket* av_packet_alloc(void) { return calloc(1, sizeof(AVPacket)); }
void av_packet_unref(AVPacket* p) {
    free(p->buf);
    memset(p, 0, sizeof *p);
}
void av_packet_free(AVPacket** p) {
    if (*p) {
        av_packet_unref(*p);
        free(*p);
        *p = NULL;
    }
}
int av_new_packet(AVPacket* p, int size) {
    if (alloc_fail) return AVERROR(ENOMEM);
    p->data = malloc((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
    CHECK(p->data != NULL);
    p->buf = p->data;
    p->size = size;
    memset(p->data, 0xa5, (size_t)size);
    memset(p->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return 0;
}
void av_shrink_packet(AVPacket* p, int size) {
    CHECK(size <= p->size);
    p->size = size;
    memset(p->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
}
AVFrame* av_frame_alloc(void) { return calloc(1, sizeof(AVFrame)); }
void av_frame_unref(AVFrame* f) { memset(f, 0, sizeof *f); }
void av_frame_free(AVFrame** f) {
    free(*f);
    *f = NULL;
}
int av_frame_ref(AVFrame* dst, const AVFrame* src) {
    if (ref_fail) return AVERROR(ENOMEM);
    *dst = *src;
    return 0;
}
const char* av_version_info(void) { return "test double"; }
int av_strerror(int err, char* dst, size_t size) {
    snprintf(dst, size, "%d", err);
    return 0;
}
int av_log_get_level(void) { return AV_LOG_WARNING; }
void av_log_set_level(int level) { (void)level; }
void av_log_set_callback(void (*cb)(void*, int, const char*, va_list)) { (void)cb; }
int av_log_format_line2(void* c, int level, const char* fmt, va_list args, char* line, int size, int* prefix) {
    (void)c;
    (void)level;
    (void)prefix;
    return vsnprintf(line, (size_t)size, fmt, args);
}
static void submission_tests(void) {
    const Event good[] = {{'S', 0, 1}, {'R', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    const Event retry[] = {
        {'S', AVERROR(EAGAIN), 0}, {'R', 0, 1}, {'R', AVERROR(EAGAIN), 0}, {'S', 0, 1}, {'R', AVERROR(EAGAIN), 0}};
    const Event send_bad[] = {{'S', AVERROR_INVALIDDATA, 1}};
    const Event recv_bad[] = {{'S', 0, 1}, {'R', AVERROR_INVALIDDATA, 0}};
    const Event eof[] = {{'S', 0, 1}, {'R', AVERROR_EOF, 0}};
    const Event stalled[] = {{'S', AVERROR(EAGAIN), 0}, {'R', AVERROR(EAGAIN), 0}, {'S', AVERROR(EAGAIN), 0}};
    const Event drain_bad[] = {{'S', AVERROR(EAGAIN), 0}, {'R', AVERROR_INVALIDDATA, 1}};
    const Event old_only[] = {
        {'S', AVERROR(EAGAIN), 0}, {'R', 0, 1}, {'R', AVERROR(EAGAIN), 0}, {'S', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    const Event no_new[] = {{'S', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    fixture();
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    CHECK(dec_open(d));
    AVFrame f = {0};
    ref_fail = 1;
    CHECK(get_buf(d->dec, &f, 0) == AVERROR(ENOMEM));
    CHECK(!d->cur_new);
    av_frame_unref(&f);
    ref_fail = 0;
    RUN(d, good, 1, 0);
    pixels_are(81, 90, 240);
    RUN(d, good, 0, 0); /* unchanged parameter sets are now cached */
    RUN(d, retry, 0, 0);
    CHECK(sends == 2);
    pixels_are(81, 90, 240);
    put32(0x70, 1); /* changed PPS must be resent if submission fails */
    RUN(d, send_bad, 1, 1);
    RUN(d, good, 1, 0);
    RUN(d, recv_bad, 0, 1);
    RUN(d, eof, 1, 1);
    RUN(d, stalled, 1, 1);
    CHECK(sends == 2);
    RUN(d, drain_bad, 1, 1);
    RUN(d, old_only, 1, 1); /* older callback cannot authorize writing this job */
    RUN(d, good, 1, 0);
    put32(0xb0, rd32(setup, 0xb0) | 0xa0); /* second field reuses the first field's buffer */
    RUN(d, no_new, 0, 0);
    RUN(d, send_bad, 0, 1);
    RUN(d, no_new, 1, 1);   /* rejected field cannot publish stale pixels */
    RUN(d, old_only, 1, 1); /* neither can a buffer captured while draining an older job */
    put32(0xb0, rd32(setup, 0xb0) & ~0xa0u);
    RUN(d, good, 1, 0);
    alloc_fail = 1;
    run(d, NULL, 0, 0, 1);
    CHECK(!sends);
    alloc_fail = 0;
    RUN(d, good, 1, 0);
    stream[0] = 0xff;
    run(d, NULL, 0, 0, 1);
    CHECK(!sends);
    stream[0] = 0x65;
    RUN(d, good, 1, 0);
    regs[0x430 / 4] = 0; /* no surface written must not report success */
    RUN(d, good, 0, 1);
    nvdec_free(d);
}
#else
static void pcm_slice(u8 y, u8 u, u8 v) {
    u8 rb[512] = {0};
    BW bw = {.p = rb, .cap = sizeof rb};
    bw_ue(&bw, 0);   /* first_mb_in_slice */
    bw_ue(&bw, 2);   /* I slice */
    bw_ue(&bw, 0);   /* PPS id */
    bw_u(&bw, 0, 4); /* frame_num */
    bw_ue(&bw, 0);   /* idr_pic_id; POC type 2 has no extra fields */
    bw_bit(&bw, 0);
    bw_bit(&bw, 0); /* IDR reference marking */
    bw_se(&bw, 0);  /* slice_qp_delta */
    bw_ue(&bw, 25); /* I_PCM macroblock */
    while (bw.nb) bw_bit(&bw, 0);
    for (unsigned i = 0; i < 384; ++i) bw_u(&bw, i < 256 ? y : i < 320 ? u : v, 8);
    bw_trailing(&bw);
    u32 n = nal_out(stream, 0x1000, 0x65, rb, bw.bytes);
    CHECK(n != 0);
    put32(0x48, n);
}
static void skip_slice(u32 frame_num) {
    u8 rb[32] = {0};
    BW bw = {.p = rb, .cap = sizeof rb};
    bw_ue(&bw, 0);
    bw_ue(&bw, 0);
    bw_ue(&bw, 0); /* first MB, P slice, PPS */
    bw_u(&bw, frame_num, 4);
    bw_bit(&bw, 0); /* num_ref_idx_active_override_flag */
    bw_bit(&bw, 0); /* ref_pic_list_modification_flag_l0 */
    bw_bit(&bw, 0); /* adaptive_ref_pic_marking_mode_flag */
    bw_se(&bw, 0);  /* slice_qp_delta */
    bw_ue(&bw, 1);  /* one skipped MB, predicted from the previous reference */
    bw_trailing(&bw);
    u32 n = nal_out(stream, 0x1000, 0x41, rb, bw.bytes);
    CHECK(n != 0);
    put32(0x48, n);
}
static void submission_tests(void) {
    fixture();
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    pcm_slice(81, 90, 240);
    nvdec_execute(d, regs);
    pixels_are(81, 90, 240);
    CHECK(d->last_hdr_n != 0);
    for (u32 frame_num = 1; frame_num <= 2; ++frame_num) {
        skip_slice(frame_num);
        memset(memory + 0x4000, 0xcc, 0x2000);
        nvdec_execute(d, regs);
        pixels_are(81, 90, 240);
    }
    /* Reuse the same SPS/PPS but decode distinct pixels; both NAL payloads require emulation prevention. */
    pcm_slice(0, 0, 0);
    u32 n = rd32(setup, 0x48) - 4; /* the same NAL in four-byte-length-prefixed form */
    stream[0] = (u8)(n >> 24);
    stream[1] = (u8)(n >> 16);
    stream[2] = (u8)(n >> 8);
    stream[3] = (u8)n;
    nvdec_execute(d, regs);
    pixels_are(0, 0, 0);
    pcm_slice(145, 54, 34);
    n = rd32(setup, 0x48) - 4; /* bare NAL */
    memmove(stream, stream + 4, n);
    put32(0x48, n);
    nvdec_execute(d, regs);
    pixels_are(145, 54, 34);
    nvdec_free(d);
}
#endif
int main(void) {
    packet_tests();
    submission_tests();
    printf("NVDEC packets: %u checks passed (%s)\n", checks, av_version_info());
    return 0;
}
