/*
 * Hardware MPEG-2 decoding for VLDP on Android (MediaCodec).  See
 * vldp_hwdec.h.  Runs on the VLDP thread only.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vldp_hwdec.h"

#ifdef VLDP_HWDEC

#include "vldp_mediacodec.h"

#include <string.h>
#include <string>
#include <vector>

#include <plog/Log.h>

#include "../io/conout.h"

namespace hwdec {

namespace {

// a decoder that takes no input and puts out nothing for this long (in
// 2 ms waits for an input buffer) is stuck
const int STUCK_TRIES = 500;

AMediaCodec *s_codec = nullptr;
std::string s_name;
frame_fn s_draw = nullptr;
abort_fn s_abort = nullptr;
int s_w = 0, s_h = 0;
std::vector<uint8_t> s_seq;            // sequence header, sent again after a reset
mediacodec::layout s_layout;           // output layout, from the output format
unsigned long s_pictures = 0;          // pictures put out, to tell a stuck decoder

// the picture being collected from the stream
std::vector<uint8_t> s_pending;        // stream bytes not yet sent
size_t s_scan = 0;                     // where to continue looking for start codes
size_t s_unit_start = 0;               // start of the current access unit
bool s_have_pic = false, s_unit_gop = false;
int s_pic_type = 0, s_pic_tr = 0;

// display numbering since the last reset: GOP base + temporal reference
long s_gop_base = 0, s_gop_pics = 0;
bool s_gop_started = false;
long s_expected = 0;                   // next display number to put out
int s_refs = 0;                        // reference pictures sent since the reset
bool s_send_seq = true;
bool s_eos = false;

// All pictures in 'head' are frame pictures with progressive_frame set.
// Hardware decoders deinterlace the others, which libmpeg2 does not.
bool progressive(const uint8_t *d, size_t n, long *pictures)
{
    *pictures = 0;
    for (size_t i = 0; i + 9 <= n; ++i)
    {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        if (d[i + 3] == 0x00) ++*pictures;
        if (d[i + 3] == 0xB5 && (d[i + 4] >> 4) == 8)
        {
            const int structure = d[i + 6] & 3;
            const bool progressive_frame = d[i + 8] >> 7;
            if (structure != 3 || !progressive_frame) return false;
        }
    }
    return true;
}

// The decoder cannot go on: drop it.  VLDP sees active() turn false and
// hands the rest of the stream to libmpeg2.
void fail(const char *why, long code)
{
    LOGW << fmt("VLDP: %s %s (%ld), decoding the rest with libmpeg2",
                s_name.empty() ? "the hardware decoder" : s_name.c_str(), why, code);
    hwdec::close();
}

// Hands every picture the decoder has ready to VLDP.  Waits up to
// 'timeout_us' for the first one.  False when decoding has to stop: VLDP
// has a new command, or the decoder failed.
bool pump(int64_t timeout_us)
{
    for (;;)
    {
        AMediaCodecBufferInfo bi;
        const ssize_t o = AMediaCodec_dequeueOutputBuffer(s_codec, &bi, timeout_us);
        timeout_us = 0;
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            mediacodec::read_output_format(s_codec, &s_layout, "VLDP hardware decoder");
            continue;
        }
        if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (o == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true; // nothing ready
        if (o < 0) { fail("failed", (long)o); return false; }

        size_t cap = 0;
        uint8_t *b = bi.size > 0 ? AMediaCodec_getOutputBuffer(s_codec, o, &cap) : NULL;
        vldp_picture pic;
        const bool have = b && mediacodec::describe(s_layout, s_w, s_h, b + bi.offset, bi.size, &pic);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) s_eos = true;
        if (!have)
        {
            AMediaCodec_releaseOutputBuffer(s_codec, o, false);
            if (s_eos) return true;
            continue;
        }

        // pictures the decoder was not given (or dropped) still count
        const long number = (long)(bi.presentationTimeUs / 1000);
        bool stop = false;
        while (s_expected < number && !stop)
        {
            ++s_expected;
            vldp_picture none = vldp_picture();
            none.placeholder = 1;
            s_draw(none);
            stop = s_abort();
        }
        if (!stop)
        {
            if (number >= s_expected) s_expected = number + 1;
            // VLDP copies the picture straight out of the decoder's buffer;
            // the buffer goes back once it is done with it
            s_draw(pic);
            ++s_pictures;
            stop = s_abort();
        }
        AMediaCodec_releaseOutputBuffer(s_codec, o, false);
        if (stop) return false;
    }
}

// Sends one access unit (headers + one picture) to the decoder.  False when
// decoding has to stop (see pump()).
bool submit(const uint8_t *p, size_t n, int type, int tr, bool gop)
{
    // display number from the GOP and temporal reference
    if (gop || !s_gop_started)
    {
        if (s_gop_started) s_gop_base += s_gop_pics;
        s_gop_pics = 0;
        s_gop_started = true;
    }
    const long number = s_gop_base + tr;
    ++s_gop_pics;

    // like libmpeg2: no B-pictures until two reference pictures are decoded
    if (type == 3 && s_refs < 2) return true;
    if (type != 3) ++s_refs;

    int idle = 0;
    for (;;)
    {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(s_codec, 2000);
        if (in >= 0)
        {
            size_t cap = 0;
            uint8_t *b = AMediaCodec_getInputBuffer(s_codec, in, &cap);
            if (!b) { fail("gave no input buffer", (long)in); return false; }
            size_t len = 0;
            if (s_send_seq && s_seq.size() <= cap)
            {
                memcpy(b, s_seq.data(), s_seq.size());
                len = s_seq.size();
                s_send_seq = false;
            }
            // a cut picture would corrupt every picture that refers to it
            if (n > cap - len) { fail("has input buffers too small for a picture", (long)n); return false; }
            memcpy(b + len, p, n);
            const media_status_t st =
                AMediaCodec_queueInputBuffer(s_codec, in, 0, len + n, (uint64_t)number * 1000, 0);
            if (st != AMEDIA_OK) { fail("refused a picture", (long)st); return false; }
            break;
        }
        if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER) { fail("failed", (long)in); return false; }

        // no room: take pictures out to make some
        const unsigned long before = s_pictures;
        if (!pump(0) || s_abort()) return false;
        idle = s_pictures == before ? idle + 1 : 0;
        if (idle >= STUCK_TRIES) { fail("stopped responding", 0); return false; }
    }
    return pump(0);
}

} // namespace

bool active() { return s_codec != nullptr; }

bool open(const uint8_t *head, size_t head_len, size_t seq_len, int width, int height,
          frame_fn draw, abort_fn abort, const char *file)
{
    close();
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    long pictures = 0;
    if (!progressive(head, head_len, &pictures))
    {
        LOGI << fmt("VLDP: %s is interlaced, decoding it in software", base);
        return false;
    }
    if (pictures == 0 || seq_len < 12 || (width & 1) || (height & 1))
    {
        LOGI << fmt("VLDP: %s does not suit the hardware decoder, decoding it in software", base);
        return false;
    }

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/mpeg2");
    if (!c)
    {
        LOGI << fmt("VLDP: no MPEG-2 MediaCodec decoder, decoding %s in software", base);
        return false;
    }
    const std::string name = mediacodec::codec_name(c);
    if (mediacodec::is_software(name))
    {
        LOGI << fmt("VLDP: the only MPEG-2 decoder is %s (software), decoding %s with libmpeg2",
                    name.c_str(), base);
        AMediaCodec_delete(c);
        return false;
    }

    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/mpeg2");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setBuffer(f, "csd-0", (void *)head, seq_len);
    media_status_t st = mediacodec::configure_video(c, f);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(c);
    if (st != AMEDIA_OK)
    {
        LOGW << fmt("VLDP: %s would not start (%d), decoding %s in software", name.c_str(), (int)st, base);
        AMediaCodec_delete(c);
        return false;
    }

    s_codec = c;
    s_name = name;
    s_draw = draw;
    s_abort = abort;
    s_w = width;
    s_h = height;
    s_seq.assign(head, head + seq_len);
    s_layout = mediacodec::layout();
    reset();
    LOGI << fmt("VLDP: decoding %s (%dx%d) with %s (hardware)", base, width, height,
                name.empty() ? "MediaCodec" : name.c_str());
    return true;
}

void close()
{
    if (!s_codec) return;
    AMediaCodec_stop(s_codec);
    AMediaCodec_delete(s_codec);
    s_codec = nullptr;
}

void reset()
{
    if (!s_codec) return;
    AMediaCodec_flush(s_codec);
    s_pending.clear();
    s_scan = s_unit_start = 0;
    s_have_pic = s_unit_gop = false;
    s_gop_base = s_gop_pics = 0;
    s_gop_started = false;
    s_expected = 0;
    s_refs = 0;
    s_send_seq = true;
    s_eos = false;
}

void decode(const uint8_t *buf, const uint8_t *end)
{
    if (!s_codec || s_eos) return;
    s_pending.insert(s_pending.end(), buf, end);

    // Access units: a picture with the headers in front of it.  A sequence
    // header, GOP header or the next picture ends the current one.
    const uint8_t *d = s_pending.data();
    const size_t n = s_pending.size();
    size_t i = s_scan;
    for (; i + 6 <= n; ++i)
    {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        const uint8_t code = d[i + 3];
        if (code != 0xB3 && code != 0xB8 && code != 0x00 && code != 0xB7) continue;

        if (s_have_pic)
        {
            if (!submit(d + s_unit_start, i - s_unit_start, s_pic_type, s_pic_tr, s_unit_gop))
            {
                // a new command (the caller resets us next) or a failed
                // decoder (closed)
                s_scan = n;
                return;
            }
            s_have_pic = s_unit_gop = false;
            s_unit_start = i;
        }
        if (code == 0xB8) s_unit_gop = true;
        if (code == 0x00)
        {
            s_pic_tr = (d[i + 4] << 2) | (d[i + 5] >> 6);
            s_pic_type = (d[i + 5] >> 3) & 7;
            s_have_pic = true;
        }
    }
    s_scan = i;

    // keep only what has not been sent
    if (s_unit_start > 0)
    {
        s_pending.erase(s_pending.begin(), s_pending.begin() + s_unit_start);
        s_scan -= s_unit_start;
        s_unit_start = 0;
    }
}

void drain()
{
    if (!s_codec || s_eos) return;
    if (s_have_pic && !submit(s_pending.data() + s_unit_start, s_pending.size() - s_unit_start, s_pic_type,
                              s_pic_tr, s_unit_gop))
        return;
    s_have_pic = false;
    s_pending.clear();
    s_scan = s_unit_start = 0;

    for (int tries = 0; tries < STUCK_TRIES; ++tries)
    {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(s_codec, 2000);
        if (in >= 0)
        {
            AMediaCodec_queueInputBuffer(s_codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            break;
        }
        if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER) { fail("failed", (long)in); return; }
        if (!pump(0)) return;
    }
    for (int tries = 0; tries < 400 && s_codec && !s_eos; ++tries)
        if (!pump(5000)) return;
}

} // namespace hwdec

#endif
