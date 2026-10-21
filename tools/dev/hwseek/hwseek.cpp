// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HypseusHW contributors
/*
 * hwseek: how fast can a decoder show an exact laserdisc frame after a search?
 *
 *   hwseek <video.m2v> <index.dat> [--seeks N] [--seed S] [--codec NAME] [--no-surface]
 *          [--play N] [--compare]
 *
 * Seeks the way VLDP does: from the frame index (Daphne v2 or VLDP v3 .dat),
 * back to the nearest I-frame (one more if the target is within two frames of
 * it), then decodes forward until the target frame comes out.  The same
 * random targets go through:
 *
 *   - libmpeg2, the core's software decoder;
 *   - the Android MediaCodec decoder for video/mpeg2 (the hardware one when
 *     the device has it, or the one named with --codec), decoding into an
 *     AImageReader surface (the zero-copy path a GPU renderer would use), or
 *     into memory with --no-surface.
 *
 * The data for each seek is read into memory first, so the times are decode
 * times only.
 *
 * Then both decode N frames (default 600) of continuous playback, flat out,
 * reporting time and CPU per frame.  MediaCodec decodes in Android's media
 * codec process, so that process's CPU time is reported too.
 *
 * --compare checks instead that both decoders put out the same sequence of
 * pictures after a search (what VLDP's frame counting relies on): it compares
 * a coarse luma signature of each output picture.
 *
 * Build: tools/dev/hwseek/build.sh
 */
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <dirent.h>

#include <algorithm>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <mpeg2.h>
}

static double now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const uint64_t NO_I = ~0ull;

static double cpu_ms()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// CPU time (user + system) of Android's media codec processes, in ms, or -1
// where /proc does not tell.
static double codec_process_cpu_ms()
{
    double total = -1;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    while (dirent *e = readdir(d))
    {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], buf[512] = "";
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        if (!strstr(buf, "media.codec") && !strstr(buf, "omx") && !strstr(buf, "media.swcodec")) continue;
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        f = fopen(path, "rb");
        if (!f) continue;
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        // fields 14 and 15 (utime, stime) follow the ")" that ends the name
        const char *p = strrchr(buf, ')');
        unsigned long ut = 0, st = 0;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2)
            total = (total < 0 ? 0 : total) + (ut + st) * 1000.0 / sysconf(_SC_CLK_TCK);
    }
    closedir(d);
    return total;
}

// ---------------------------------------------------------------------------
// Frame index and stream
// ---------------------------------------------------------------------------

static std::vector<uint64_t> read_index(const char *path, bool *uses_fields)
{
    std::vector<uint64_t> pos;
    FILE *f = fopen(path, "rb");
    if (!f) return pos;
    uint8_t hdr[16];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    int width = got >= 8 && hdr[0] == 2 ? 4 : got == 16 && hdr[0] == 3 ? 8 : 0;
    *uses_fields = hdr[2] != 0;
    if (width)
    {
        fseek(f, width == 4 ? 8 : 16, SEEK_SET);
        uint8_t b[8];
        while (fread(b, width, 1, f) == 1)
        {
            uint64_t v = 0;
            for (int i = width - 1; i >= 0; --i) v = (v << 8) | b[i];
            if (width == 4 && v == 0xFFFFFFFFu) v = NO_I;
            pos.push_back(v);
        }
    }
    fclose(f);
    return pos;
}

static std::vector<uint8_t> read_range(FILE *f, uint64_t start, uint64_t len)
{
    std::vector<uint8_t> d(len);
    fseeko(f, (off_t)start, SEEK_SET);
    d.resize(fread(d.data(), 1, len, f));
    return d;
}

// Sequence header (and extensions) at the start of the file, up to the first
// GOP: what VLDP feeds libmpeg2 before decoding from an arbitrary position.
static std::vector<uint8_t> read_seq_header(FILE *f)
{
    std::vector<uint8_t> d = read_range(f, 0, 64 * 1024);
    for (size_t i = 0; i + 4 <= d.size(); ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && d[i + 3] == 0xB8)
        {
            d.resize(i);
            return d;
        }
    d.clear();
    return d;
}

// One coded picture with the headers in front of it.
struct Picture {
    size_t start, end;   // in the seek buffer
    int type;            // 1 I, 2 P, 3 B
    int temporal_ref;
    bool gop_start;      // a GOP header precedes it
    int structure;       // picture coding extension: 3 frame, 1/2 field
    bool rff, tff, progressive;
};

static std::vector<Picture> split_pictures(const std::vector<uint8_t> &d)
{
    std::vector<Picture> pics;
    size_t unit_start = 0;
    bool have_pic = false, gop = false;
    Picture cur = {};
    for (size_t i = 0; i + 6 <= d.size(); ++i)
    {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const uint8_t code = d[i + 3];
        // headers that start a new access unit: sequence, GOP, picture
        // picture coding extension of the current picture
        if (code == 0xB5 && have_pic && (d[i + 4] >> 4) == 8 && i + 9 <= d.size())
        {
            cur.structure   = d[i + 6] & 3;
            cur.tff         = d[i + 7] >> 7;
            cur.rff         = (d[i + 7] >> 1) & 1;
            cur.progressive = d[i + 8] >> 7;
        }
        if (code == 0xB3 || code == 0xB8 || code == 0x00)
        {
            if (have_pic)
            {
                cur.end = i;
                pics.push_back(cur);
                have_pic = false;
                unit_start = i;
                gop = false;
            }
            if (code == 0xB8) gop = true;
            if (code == 0x00)
            {
                cur.start = unit_start;
                cur.temporal_ref = (d[i + 4] << 2) | (d[i + 5] >> 6);
                cur.type = (d[i + 5] >> 3) & 7;
                cur.gop_start = gop;
                cur.structure = 3;
                cur.rff = cur.tff = false;
                cur.progressive = true;
                have_pic = true;
            }
        }
    }
    return pics; // the last, possibly cut-off, picture is left out
}

// Display index of each picture, from its GOP's first frame and temporal
// reference.  'first_frame' is the frame number of the I-frame we start at.
static std::vector<long> display_numbers(const std::vector<Picture> &pics, long first_frame)
{
    std::vector<long> n(pics.size());
    long base = 0, gop_pics = 0;
    for (size_t i = 0; i < pics.size(); ++i)
    {
        if (i == 0 || pics[i].gop_start)
        {
            base = i == 0 ? first_frame - pics[0].temporal_ref : base + gop_pics;
            gop_pics = 0;
        }
        n[i] = base + pics[i].temporal_ref;
        ++gop_pics;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Seek plan (as VLDP's idle_handler_search)
// ---------------------------------------------------------------------------

struct Seek {
    long target, start_frame, skip;
    uint64_t start_pos, end_pos;
};

static bool plan(const std::vector<uint64_t> &pos, long target, uint64_t file_size, Seek *s)
{
    long actual = target, skip = 0;
    uint64_t p = pos[actual];
    for (int skipped_I = 1;; ++skipped_I)
    {
        while (p == NO_I && actual > 0) { ++skip; p = pos[--actual]; }
        if (skipped_I < 2 && skip < 3 && actual > 0) { p = NO_I; continue; }
        break;
    }
    if (p == NO_I) return false;
    // enough data to get past the target: up to the second I-frame after it
    uint64_t end = file_size;
    int found = 0;
    for (long i = target + 1; i < (long)pos.size(); ++i)
        if (pos[i] != NO_I && ++found == 2) { end = pos[i]; break; }
    *s = {target, actual, skip, p, end};
    return true;
}

// ---------------------------------------------------------------------------
// libmpeg2
// ---------------------------------------------------------------------------

// Returns ms until the target frame is decoded, or -1.
static double seek_libmpeg2(const std::vector<uint8_t> &seq, const std::vector<uint8_t> &data, long skip)
{
    mpeg2dec_t *dec = mpeg2_init();
    const mpeg2_info_t *info = mpeg2_info(dec);
    const double t0 = now_ms();

    // as vldp: reset, feed the cached sequence header, then the stream
    mpeg2_reset(dec, 1);
    long shown = 0;
    double t = -1;
    for (int pass = 0; pass < 2 && t < 0; ++pass)
    {
        const std::vector<uint8_t> &b = pass ? data : seq;
        mpeg2_buffer(dec, (uint8_t *)b.data(), (uint8_t *)b.data() + b.size());
        for (;;)
        {
            mpeg2_state_t st = mpeg2_parse(dec);
            if (st == STATE_BUFFER) break;
            if ((st == STATE_SLICE || st == STATE_END || st == STATE_INVALID_END) && info->display_fbuf)
            {
                if (pass && shown++ == skip) { t = now_ms() - t0; break; }
            }
        }
    }
    mpeg2_close(dec);
    return t;
}

// ---------------------------------------------------------------------------
// MediaCodec
// ---------------------------------------------------------------------------

struct HwDecoder {
    AMediaCodec *codec = nullptr;
    AImageReader *reader = nullptr;
    std::string name;
};

static bool hw_open(HwDecoder *h, const char *codec_name, bool surface, int w, int ht,
                    const std::vector<uint8_t> &seq)
{
    h->codec = codec_name ? AMediaCodec_createCodecByName(codec_name)
                          : AMediaCodec_createDecoderByType("video/mpeg2");
    if (!h->codec) { fprintf(stderr, "no MediaCodec decoder\n"); return false; }
    char *nm = nullptr;
    if (AMediaCodec_getName(h->codec, &nm) == AMEDIA_OK && nm)
    {
        h->name = nm;
        AMediaCodec_releaseName(h->codec, nm);
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/mpeg2");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, ht);
    AMediaFormat_setBuffer(fmt, "csd-0", (void *)seq.data(), seq.size());

    ANativeWindow *win = nullptr;
    if (surface)
    {
        if (AImageReader_newWithUsage(w, ht, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4,
                                      &h->reader) != AMEDIA_OK ||
            AImageReader_getWindow(h->reader, &win) != AMEDIA_OK)
        {
            fprintf(stderr, "no AImageReader surface\n");
            return false;
        }
    }
    media_status_t st = AMediaCodec_configure(h->codec, fmt, win, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(h->codec) != AMEDIA_OK)
    {
        fprintf(stderr, "configure/start failed (%d)\n", (int)st);
        return false;
    }
    return true;
}

struct HwResult {
    double flush_ms, out_ms, image_ms;
    int fed;
};

// Flush, then feed pictures from the seek buffer until the output with the
// target's timestamp arrives.  Pictures carry their display number as the
// timestamp.
static bool seek_hw(HwDecoder *h, const std::vector<uint8_t> &data, const std::vector<Picture> &pics,
                    const std::vector<long> &disp, long target, HwResult *r)
{
    const double t0 = now_ms();
    AMediaCodec_flush(h->codec);
    r->flush_ms = now_ms() - t0;
    r->out_ms = r->image_ms = -1;
    r->fed = 0;

    // drop whatever the image reader still holds
    if (h->reader)
    {
        AImage *img;
        while (AImageReader_acquireNextImage(h->reader, &img) == AMEDIA_OK) AImage_delete(img);
    }

    size_t next = 0;
    bool eos_sent = false;
    while (now_ms() - t0 < 3000)
    {
        ssize_t in = AMediaCodec_dequeueInputBuffer(h->codec, 0);
        if (in >= 0)
        {
            size_t cap;
            uint8_t *buf = AMediaCodec_getInputBuffer(h->codec, in, &cap);
            if (next < pics.size())
            {
                const Picture &p = pics[next];
                size_t n = std::min(cap, p.end - p.start);
                memcpy(buf, &data[p.start], n);
                AMediaCodec_queueInputBuffer(h->codec, in, 0, n, (uint64_t)disp[next] * 1000, 0);
                ++next;
                ++r->fed;
            }
            else if (!eos_sent)
            {
                AMediaCodec_queueInputBuffer(h->codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                eos_sent = true;
            }
        }

        AMediaCodecBufferInfo bi;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(h->codec, &bi, 500);
        if (out >= 0)
        {
            const long frame = (long)(bi.presentationTimeUs / 1000);
            const bool hit = frame == target;
            AMediaCodec_releaseOutputBuffer(h->codec, out, hit && h->reader);
            if (hit)
            {
                r->out_ms = now_ms() - t0;
                if (!h->reader) { r->image_ms = r->out_ms; return true; }
                for (;;)
                {
                    AImage *img;
                    if (AImageReader_acquireLatestImage(h->reader, &img) == AMEDIA_OK)
                    {
                        r->image_ms = now_ms() - t0;
                        AImage_delete(img);
                        return true;
                    }
                    if (now_ms() - t0 > 3000) return false;
                    struct timespec ts = {0, 100000};
                    nanosleep(&ts, nullptr);
                }
            }
            if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return false;
        }
    }
    return false;
}

// Continuous playback through MediaCodec: feeds every picture, releases every
// output (to the surface when there is one).  Returns the number of frames
// that came out.
static long play_hw(HwDecoder *h, const std::vector<uint8_t> &data, const std::vector<Picture> &pics,
                    long nframes)
{
    AMediaCodec_flush(h->codec);
    size_t next = 0;
    long out_frames = 0;
    bool eos_sent = false;
    const double t0 = now_ms();
    while (out_frames < nframes && now_ms() - t0 < 60000)
    {
        ssize_t in = AMediaCodec_dequeueInputBuffer(h->codec, 0);
        if (in >= 0)
        {
            size_t cap;
            uint8_t *buf = AMediaCodec_getInputBuffer(h->codec, in, &cap);
            if (next < pics.size())
            {
                const Picture &p = pics[next];
                size_t n = std::min(cap, p.end - p.start);
                memcpy(buf, &data[p.start], n);
                AMediaCodec_queueInputBuffer(h->codec, in, 0, n, (uint64_t)next * 1000, 0);
                ++next;
            }
            else if (!eos_sent)
            {
                AMediaCodec_queueInputBuffer(h->codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                eos_sent = true;
            }
        }
        AMediaCodecBufferInfo bi;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(h->codec, &bi, 1000);
        if (out >= 0)
        {
            AMediaCodec_releaseOutputBuffer(h->codec, out, h->reader != nullptr);
            if (bi.size > 0 || h->reader) ++out_frames;
            if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
        if (h->reader)
        {
            AImage *img;
            while (AImageReader_acquireNextImage(h->reader, &img) == AMEDIA_OK) AImage_delete(img);
        }
    }
    return out_frames;
}

static long play_libmpeg2(const std::vector<uint8_t> &seq, const std::vector<uint8_t> &data, long nframes)
{
    mpeg2dec_t *dec = mpeg2_init();
    const mpeg2_info_t *info = mpeg2_info(dec);
    long shown = 0;
    for (int pass = 0; pass < 2 && shown < nframes; ++pass)
    {
        const std::vector<uint8_t> &b = pass ? data : seq;
        mpeg2_buffer(dec, (uint8_t *)b.data(), (uint8_t *)b.data() + b.size());
        for (;;)
        {
            mpeg2_state_t st = mpeg2_parse(dec);
            if (st == STATE_BUFFER) break;
            if ((st == STATE_SLICE || st == STATE_END || st == STATE_INVALID_END) && info->display_fbuf &&
                pass && ++shown >= nframes)
                break;
        }
    }
    mpeg2_close(dec);
    return shown;
}

// ---------------------------------------------------------------------------

static void stats(const char *what, std::vector<double> v)
{
    if (v.empty()) { printf("%-34s no results\n", what); return; }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    printf("%-34s n=%-3zu min %6.1f  median %6.1f  p90 %6.1f  max %6.1f  mean %6.1f ms\n", what, v.size(),
           v.front(), pct(0.5), pct(0.9), v.back(), sum / v.size());
}

// HWSEEK_PGM=<frame>: also save the luma of output number <frame> of each
// decoder as a PGM (development aid).
static long g_pgm_index = -1;
static long g_flag_pics, g_flag_field, g_flag_rff, g_flag_interlaced;
static void save_pgm(const char *name, const uint8_t *y, int pitch, int w, int h)
{
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int r = 0; r < h; ++r) fwrite(y + (size_t)r * pitch, 1, w, f);
    fclose(f);
}

// Coarse signature of a picture: mean luma of a 16x9 grid of blocks.
typedef std::vector<int> Sig;

static Sig signature(const uint8_t *y, int pitch, int w, int h)
{
    Sig s(16 * 9);
    for (int by = 0; by < 9; ++by)
        for (int bx = 0; bx < 16; ++bx)
        {
            long sum = 0, n = 0;
            for (int yy = by * h / 9; yy < (by + 1) * h / 9; yy += 2)
                for (int xx = bx * w / 16; xx < (bx + 1) * w / 16; xx += 2, ++n) sum += y[(size_t)yy * pitch + xx];
            s[by * 16 + bx] = n ? (int)(sum / n) : 0;
        }
    return s;
}

static double sig_diff(const Sig &a, const Sig &b)
{
    if (a.empty() || b.empty()) return -1;
    long d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += abs(a[i] - b[i]);
    return (double)d / a.size();
}

static std::vector<Sig> outputs_libmpeg2(const std::vector<uint8_t> &seq, const std::vector<uint8_t> &data,
                                         size_t max_out)
{
    std::vector<Sig> out;
    mpeg2dec_t *dec = mpeg2_init();
    const mpeg2_info_t *info = mpeg2_info(dec);
    mpeg2_reset(dec, 0);
    for (int pass = 0; pass < 2 && out.size() < max_out; ++pass)
    {
        const std::vector<uint8_t> &b = pass ? data : seq;
        mpeg2_buffer(dec, (uint8_t *)b.data(), (uint8_t *)b.data() + b.size());
        for (;;)
        {
            mpeg2_state_t st = mpeg2_parse(dec);
            if (st == STATE_BUFFER) break;
            if ((st == STATE_SLICE || st == STATE_END || st == STATE_INVALID_END) && info->display_fbuf && pass)
            {
                if ((long)out.size() == g_pgm_index)
                    save_pgm("sw.pgm", info->display_fbuf->buf[0], info->sequence->width,
                             info->sequence->picture_width, info->sequence->picture_height);
                out.push_back(signature(info->display_fbuf->buf[0], info->sequence->width,
                                        info->sequence->picture_width, info->sequence->picture_height));
                if (out.size() >= max_out) break;
            }
        }
    }
    mpeg2_close(dec);
    return out;
}

// All pictures through MediaCodec (output to memory), then end of stream.
static std::vector<Sig> outputs_hw(HwDecoder *h, const std::vector<uint8_t> &seq, const std::vector<uint8_t> &data,
                                   const std::vector<Picture> &pics, int w, int ht, size_t max_out)
{
    std::vector<Sig> out;
    AMediaCodec_flush(h->codec);
    size_t next = 0;
    bool eos_sent = false;
    int stride = w, slice = ht, refs = 0;
    const double t0 = now_ms();
    while (out.size() < max_out && now_ms() - t0 < 5000)
    {
        ssize_t in = AMediaCodec_dequeueInputBuffer(h->codec, 0);
        if (in >= 0)
        {
            size_t cap;
            uint8_t *buf = AMediaCodec_getInputBuffer(h->codec, in, &cap);
            // like libmpeg2 after a reset: no B-pictures until two reference
            // pictures have been decoded (their other reference is missing)
            // (libmpeg2 still puts them out, and VLDP counts them: keep a
            // placeholder in their place)
            while (next < pics.size() && pics[next].type == 3 && refs < 2)
            {
                out.push_back(Sig());
                ++next;
            }
            if (next < pics.size())
            {
                if (pics[next].type != 3) ++refs;
                // the sequence header again in front of the first picture
                size_t n = 0;
                if (next == 0) { memcpy(buf, seq.data(), seq.size()); n = seq.size(); }
                const Picture &p = pics[next];
                const size_t m = std::min(cap - n, p.end - p.start);
                memcpy(buf + n, &data[p.start], m);
                AMediaCodec_queueInputBuffer(h->codec, in, 0, n + m, (uint64_t)next * 1000, 0);
                ++next;
            }
            else if (!eos_sent)
            {
                AMediaCodec_queueInputBuffer(h->codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                eos_sent = true;
            }
        }
        AMediaCodecBufferInfo bi;
        ssize_t o = AMediaCodec_dequeueOutputBuffer(h->codec, &bi, 1000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaFormat *f = AMediaCodec_getOutputFormat(h->codec);
            static bool printed = false;
            if (!printed) { printf("output format: %s\n", AMediaFormat_toString(f)); printed = true; }
            int32_t v;
            if (AMediaFormat_getInt32(f, "stride", &v) && v > 0) stride = v;
            if (AMediaFormat_getInt32(f, "slice-height", &v) && v > 0) slice = v;
            AMediaFormat_delete(f);
        }
        else if (o >= 0)
        {
            if (bi.size > 0)
            {
                size_t sz;
                uint8_t *ob = AMediaCodec_getOutputBuffer(h->codec, o, &sz);
                if (ob && (long)out.size() == g_pgm_index) save_pgm("hw.pgm", ob + bi.offset, stride, w, ht);
                if (ob) out.push_back(signature(ob + bi.offset, stride, w, ht));
            }
            AMediaCodec_releaseOutputBuffer(h->codec, o, false);
            if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }
    (void)slice;
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: hwseek <video.m2v> <index.dat> [--seeks N] [--seed S] [--codec NAME] [--no-surface]\n");
        return 2;
    }
    int nseeks = 40;
    unsigned seed = 1;
    const char *codec_name = nullptr;
    bool surface = true;
    long play_frames = 600;
    bool compare = false;
    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--seeks" && i + 1 < argc) nseeks = atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (a == "--codec" && i + 1 < argc) codec_name = argv[++i];
        else if (a == "--no-surface") surface = false;
        else if (a == "--play" && i + 1 < argc) play_frames = atol(argv[++i]);
        else if (a == "--compare") { compare = true; surface = false; }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    bool uses_fields = false;
    std::vector<uint64_t> pos = read_index(argv[2], &uses_fields);
    FILE *f = fopen(argv[1], "rb");
    if (pos.empty() || !f) { fprintf(stderr, "cannot read %s or %s\n", argv[1], argv[2]); return 1; }
    fseeko(f, 0, SEEK_END);
    const uint64_t file_size = (uint64_t)ftello(f);
    if (uses_fields) { fprintf(stderr, "field-based streams are not handled by this test\n"); return 1; }

    std::vector<uint8_t> seq = read_seq_header(f);
    if (seq.size() < 12) { fprintf(stderr, "no sequence header\n"); return 1; }
    const int w = (seq[4] << 4) | (seq[5] >> 4), h = ((seq[5] & 15) << 8) | seq[6];
    long iframes = 0;
    for (uint64_t p : pos) iframes += p != NO_I;
    printf("%s: %dx%d, %zu frames, %ld I-frames (one every %.1f)\n", argv[1], w, h, pos.size(), iframes,
           (double)pos.size() / iframes);

    HwDecoder hw;
    if (!hw_open(&hw, codec_name, surface, w, h, seq)) return 1;
    printf("MediaCodec decoder: %s, output to %s\n", hw.name.c_str(), surface ? "an AImageReader surface" : "memory");

    srand(seed);

    if (compare)
    {
        if (const char *e = getenv("HWSEEK_PGM")) g_pgm_index = atol(e);
        int same = 0, shifted = 0, bad = 0, total = 0;
        for (int k = 0; k < nseeks * 3 && total < nseeks; ++k)
        {
            const long target = 100 + (long)(((uint64_t)rand() * RAND_MAX + rand()) % (pos.size() - 200));
            Seek sk;
            if (!plan(pos, target, file_size, &sk)) continue;
            std::vector<uint8_t> data = read_range(f, sk.start_pos, sk.end_pos - sk.start_pos);
            std::vector<Picture> pics = split_pictures(data);
            if (pics.empty()) continue;
            for (const Picture &pc : pics)
            {
                ++g_flag_pics;
                g_flag_field += pc.structure != 3;
                g_flag_rff += pc.rff;
                g_flag_interlaced += !pc.progressive;
            }
            if (getenv("HWSEEK_DUMP") && total < 3)
            {
                printf("pictures from I-frame %ld:", sk.start_frame);
                for (size_t i = 0; i < pics.size() && i < 16; ++i)
                    printf(" %s%c%d", pics[i].gop_start ? "|" : "", "?IPBD"[pics[i].type & 3], pics[i].temporal_ref);
                printf("\n");
            }
            const size_t want = (size_t)sk.skip + 8;
            std::vector<Sig> a = outputs_libmpeg2(seq, data, want);
            std::vector<Sig> b = outputs_hw(&hw, seq, data, pics, w, h, want + 4);
            ++total;

            // best alignment of the hardware sequence against libmpeg2's
            int best_shift = 0;
            double best = 1e9;
            for (int sh = -3; sh <= 3; ++sh)
            {
                double d = 0;
                int n = 0;
                for (size_t i = 0; i < a.size(); ++i)
                {
                    long j = (long)i + sh;
                    if (j < 0 || j >= (long)b.size() || b[j].empty()) continue;
                    d += sig_diff(a[i], b[j]);
                    ++n;
                }
                if (n >= (int)a.size() - 3 && d / n < best) { best = d / n; best_shift = sh; }
            }
            const double at_target = (size_t)sk.skip < a.size() && (size_t)sk.skip < b.size()
                                         ? sig_diff(a[sk.skip], b[sk.skip]) : -1;
            if (best_shift == 0 && best < 3) ++same;
            else if (best < 3) ++shifted;
            else ++bad;
            if (total <= 12 || best_shift != 0 || best >= 3)
                printf("frame %6ld: from I-frame %6ld (skip %2ld), outputs libmpeg2 %2zu / hw %2zu, "
                       "best shift %+d (diff %.2f), target diff %.2f\n",
                       target, sk.start_frame, sk.skip, a.size(), b.size(), best_shift, best, at_target);
        }
        printf("%d searches: same sequence %d, shifted %d, different %d\n", total, same, shifted, bad);
        printf("pictures: %ld, field pictures %ld, repeat_first_field %ld, not progressive_frame %ld\n",
               g_flag_pics, g_flag_field, g_flag_rff, g_flag_interlaced);
        return 0;
    }
    std::vector<double> sw_ms, hw_flush, hw_out, hw_img, hw_fed, skips, read_ms;
    long gop_types[4] = {0};
    bool b_frames = false;
    int failed_hw = 0, failed_sw = 0, done = 0;

    for (int k = 0; k < nseeks * 3 && done < nseeks; ++k)
    {
        const long target = 100 + (long)(((uint64_t)rand() * RAND_MAX + rand()) % (pos.size() - 200));
        Seek s;
        if (!plan(pos, target, file_size, &s)) continue;
        if (s.end_pos - s.start_pos > 64ull << 20) continue;

        double t = now_ms();
        std::vector<uint8_t> data = read_range(f, s.start_pos, s.end_pos - s.start_pos);
        read_ms.push_back(now_ms() - t);
        std::vector<Picture> pics = split_pictures(data);
        if (pics.empty()) continue;
        std::vector<long> disp = display_numbers(pics, s.start_frame);
        for (const Picture &p : pics)
        {
            if (p.type >= 1 && p.type <= 3) ++gop_types[p.type];
            b_frames |= p.type == 3;
        }
        if (std::find(disp.begin(), disp.end(), target) == disp.end()) continue;

        const double sw = seek_libmpeg2(seq, data, s.skip);
        if (sw >= 0) sw_ms.push_back(sw); else ++failed_sw;

        HwResult r;
        if (seek_hw(&hw, data, pics, disp, target, &r))
        {
            hw_flush.push_back(r.flush_ms);
            hw_out.push_back(r.out_ms);
            hw_img.push_back(r.image_ms);
            hw_fed.push_back(r.fed);
        }
        else
            ++failed_hw;
        skips.push_back((double)s.skip);
        ++done;
    }

    printf("stream: %ld I, %ld P, %ld B pictures in the seek ranges%s\n", gop_types[1], gop_types[2],
           gop_types[3], b_frames ? "" : " (no B-frames)");
    std::sort(skips.begin(), skips.end());
    printf("%d seeks; frames decoded past the I-frame: median %.0f, max %.0f\n", done,
           skips.empty() ? 0.0 : skips[skips.size() / 2], skips.empty() ? 0.0 : skips.back());
    stats("reading the data (not counted)", read_ms);
    stats("libmpeg2: target decoded", sw_ms);
    stats("MediaCodec: flush", hw_flush);
    stats("MediaCodec: target output", hw_out);
    if (surface) stats("MediaCodec: target in the surface", hw_img);
    printf("MediaCodec: pictures fed before the target came out: median %.0f, max %.0f\n",
           hw_fed.empty() ? 0.0 : (std::sort(hw_fed.begin(), hw_fed.end()), hw_fed[hw_fed.size() / 2]),
           hw_fed.empty() ? 0.0 : hw_fed.back());

    // Continuous playback from an I-frame a third of the way in.
    if (play_frames > 0)
    {
        long start = (long)pos.size() / 3;
        while (start < (long)pos.size() && pos[start] == NO_I) ++start;
        uint64_t end = file_size;
        for (long i = start + play_frames + 30; i < (long)pos.size(); ++i)
            if (pos[i] != NO_I) { end = pos[i]; break; }
        std::vector<uint8_t> data = read_range(f, pos[start], end - pos[start]);
        std::vector<Picture> pics = split_pictures(data);

        double t = now_ms(), c = cpu_ms();
        long n = play_libmpeg2(seq, data, play_frames);
        double wall = now_ms() - t, cpu = cpu_ms() - c;
        printf("libmpeg2 playback:   %ld frames, %.2f ms per frame (%.0f fps); CPU %.2f ms per frame\n", n,
               wall / n, 1000.0 * n / wall, cpu / n);

        const double codec0 = codec_process_cpu_ms();
        t = now_ms();
        c = cpu_ms();
        n = play_hw(&hw, data, pics, play_frames);
        wall = now_ms() - t;
        cpu = cpu_ms() - c;
        const double codec1 = codec_process_cpu_ms();
        if (n > 0)
        {
            printf("MediaCodec playback: %ld frames, %.2f ms per frame (%.0f fps); CPU %.2f ms per frame here", n,
                   wall / n, 1000.0 * n / wall, cpu / n);
            if (codec0 >= 0 && codec1 >= 0)
                printf(" + %.2f ms in the media codec process\n", (codec1 - codec0) / n);
            else
                printf(" (media codec process not readable)\n");
        }
        else
            printf("MediaCodec playback: no frames came out\n");
    }
    if (failed_hw || failed_sw) printf("failed: MediaCodec %d, libmpeg2 %d\n", failed_hw, failed_sw);

    AMediaCodec_stop(hw.codec);
    AMediaCodec_delete(hw.codec);
    if (hw.reader) AImageReader_delete(hw.reader);
    fclose(f);
    return 0;
}
