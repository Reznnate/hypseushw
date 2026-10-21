/*
 * MP4 laserdisc video for Singe 2 games.  See vldp_mp4.h.
 *
 * A backend demuxes and decodes: the platform's (MediaCodec on Android,
 * Media Foundation on Windows), with the hardware decoders, or FFmpeg on the
 * CPU (Linux, and the CPU Fallback core option everywhere).  This part
 * numbers the pictures, drops the ones before a search target, and keeps the
 * audio track decoded ahead of the game.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vldp_mp4.h"

#ifdef VLDP_MP4

#include "vldp.h" // g_lr_vldp_hwdec: off with the CPU Fallback core option

#include <math.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <plog/Log.h>

#include "../io/conout.h"

#ifdef __ANDROID__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include "vldp_mediacodec.h"
#define MP4_PLATFORM_BACKEND 1
#elif defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#define MP4_PLATFORM_BACKEND 1
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

namespace {

std::string base_name(const char *file)
{
    const char *b = strrchr(file, '/');
    const char *b2 = strrchr(file, '\\');
    if (b2 > b) b = b2;
    return b ? b + 1 : file;
}

// One picture from the platform decoder.  Its pixels stay valid until
// video_release() (or the next video_next()/video_seek()).
struct decoded
{
    vldp_picture pic;
    int64_t pts_us;
};

// ---------------------------------------------------------------------------
// Platform decoders (hardware)
// ---------------------------------------------------------------------------

namespace plat {

#ifdef __ANDROID__

// Frames per second from the median step between picture times, which are
// whole microseconds: snapped to the usual rates.  0 if unusable.
double rate_from_step(int64_t step)
{
    if (step <= 0) return 0;
    static const double rates[] = {24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60};
    double best = 1e6 / step;
    for (double r : rates)
        if (fabs(1e6 / r - step) < 2) best = r;
    return best;
}

// A track of the file, with its extractor and decoder.
struct track
{
    int fd = -1;
    AMediaExtractor *ex = nullptr;
    AMediaCodec *codec = nullptr;
    AMediaFormat *format = nullptr; // the track's format
    std::string codec_name;
    bool video = false;
    bool in_eos = false, out_eos = false;
    ssize_t out_index = -1;          // output buffer held by the caller

    // video output layout
    mediacodec::layout layout;
    int w = 0, h = 0;
    // audio output
    int32_t rate = 0, channels = 0;

    bool open(const char *file, const char *kind)
    {
        video = strcmp(kind, "video/") == 0;
        fd = ::open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { LOGE << fmt("MP4: could not open %s", file); return false; }
        struct stat st;
        ex = fstat(fd, &st) == 0 ? AMediaExtractor_new() : nullptr;
        if (!ex || AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK)
        {
            LOGE << fmt("MP4: %s is not a file MediaExtractor can read", file);
            return false;
        }
        const size_t n = AMediaExtractor_getTrackCount(ex);
        for (size_t i = 0; i < n; ++i)
        {
            AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
            const char *mime = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
                strncmp(mime, kind, strlen(kind)) == 0)
            {
                AMediaExtractor_selectTrack(ex, i);
                format = f;
                // the device's choice: its hardware decoder
                codec = AMediaCodec_createDecoderByType(mime);
                if (!codec) { LOGE << fmt("MP4: no decoder for %s", mime); return false; }
                codec_name = mediacodec::codec_name(codec);
                media_status_t s;
                if (video)
                {
                    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
                    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
                    s = mediacodec::configure_video(codec, f);
                }
                else
                    s = AMediaCodec_configure(codec, f, nullptr, nullptr, 0);
                if (s == AMEDIA_OK) s = AMediaCodec_start(codec);
                if (s != AMEDIA_OK)
                {
                    LOGE << fmt("MP4: %s would not start (%d)", codec_name.c_str(), (int)s);
                    return false;
                }
                return true;
            }
            AMediaFormat_delete(f);
        }
        LOGE << fmt("MP4: %s has no %s track", file, kind);
        return false;
    }

    void close()
    {
        if (codec)
        {
            if (out_index >= 0) AMediaCodec_releaseOutputBuffer(codec, out_index, false);
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
        if (format) AMediaFormat_delete(format);
        if (ex) AMediaExtractor_delete(ex);
        if (fd >= 0) ::close(fd);
        *this = track();
    }

    void seek(int64_t us)
    {
        if (out_index >= 0) { AMediaCodec_releaseOutputBuffer(codec, out_index, false); out_index = -1; }
        AMediaExtractor_seekTo(ex, us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        AMediaCodec_flush(codec);
        in_eos = out_eos = false;
    }

    // Sends the next sample to the decoder, if it has room.  A sample that
    // cannot be read ends the stream there.
    void feed(int64_t timeout_us)
    {
        if (in_eos) return;
        const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, timeout_us);
        if (in < 0) return;
        size_t cap = 0;
        uint8_t *b = AMediaCodec_getInputBuffer(codec, in, &cap);
        const ssize_t n = b ? AMediaExtractor_readSampleData(ex, b, cap) : -1;
        if (n < 0)
        {
            const int64_t t = AMediaExtractor_getSampleTime(ex);
            if (t >= 0)
            {
                LOGE << fmt("MP4: could not read the %s sample at %.3f s, ending the stream there",
                            video ? "video" : "audio", t / 1e6);
            }
            AMediaCodec_queueInputBuffer(codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            in_eos = true;
            return;
        }
        AMediaCodec_queueInputBuffer(codec, in, 0, n, AMediaExtractor_getSampleTime(ex), 0);
        AMediaExtractor_advance(ex);
    }

    void read_output_format()
    {
        if (video)
        {
            mediacodec::read_output_format(codec, &layout, "MP4");
            return;
        }
        AMediaFormat *f = AMediaCodec_getOutputFormat(codec);
        if (!f) return;
        int32_t v;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) rate = v;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) channels = v;
        AMediaFormat_delete(f);
    }

    // The next output buffer: 1 with it in 'data', 0 at the end of the
    // stream, -1 if none came within 'timeout_us'.
    int next(const uint8_t **data, size_t *size, int64_t *pts_us, int64_t timeout_us)
    {
        if (out_index >= 0) { AMediaCodec_releaseOutputBuffer(codec, out_index, false); out_index = -1; }
        for (;;)
        {
            if (out_eos) return 0;
            feed(0);
            AMediaCodecBufferInfo bi;
            const ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &bi, in_eos ? timeout_us : 0);
            if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_output_format(); continue; }
            if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (o < 0)
            {
                if (in_eos) return -1;
                // let the decoder work on what it has, then look again
                feed(timeout_us);
                const ssize_t o2 = AMediaCodec_dequeueOutputBuffer(codec, &bi, 0);
                if (o2 == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_output_format(); continue; }
                if (o2 < 0) return -1;
                return take(o2, bi, data, size, pts_us);
            }
            return take(o, bi, data, size, pts_us);
        }
    }

    int take(ssize_t o, const AMediaCodecBufferInfo &bi, const uint8_t **data, size_t *size, int64_t *pts_us)
    {
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) out_eos = true;
        size_t cap = 0;
        uint8_t *b = bi.size > 0 ? AMediaCodec_getOutputBuffer(codec, o, &cap) : nullptr;
        if (!b)
        {
            AMediaCodec_releaseOutputBuffer(codec, o, false);
            return out_eos ? 0 : -1;
        }
        out_index = o;
        *data = b + bi.offset;
        *size = bi.size;
        *pts_us = bi.presentationTimeUs;
        return 1;
    }
};

track s_video;

bool video_open(const char *file, mp4::stream_info *info, int64_t *start_us, std::string *decoder)
{
    if (!s_video.open(file, "video/")) { s_video.close(); return false; }
    *decoder = s_video.codec_name;

    int64_t duration = 0;
    if (!AMediaFormat_getInt64(s_video.format, AMEDIAFORMAT_KEY_DURATION, &duration)) duration = 0;
    int32_t fps = 0;
    double rate = 0;
    if (AMediaFormat_getInt32(s_video.format, AMEDIAFORMAT_KEY_FRAME_RATE, &fps) && fps > 0) rate = fps;
    int32_t sar_w = 0, sar_h = 0;
    AMediaFormat_getInt32(s_video.format, "sar-width", &sar_w);
    AMediaFormat_getInt32(s_video.format, "sar-height", &sar_h);

    // The frame rate from the sample times (exact, unlike the format's
    // rounded value), and where the stream starts.
    std::vector<int64_t> times;
    for (int i = 0; i < 120; ++i)
    {
        const int64_t t = AMediaExtractor_getSampleTime(s_video.ex);
        if (t < 0) break;
        times.push_back(t);
        AMediaExtractor_advance(s_video.ex);
    }
    std::sort(times.begin(), times.end());
    *start_us = times.empty() ? 0 : times[0];
    if (times.size() > 10)
    {
        std::vector<int64_t> d;
        for (size_t i = 1; i < times.size(); ++i) d.push_back(times[i] - times[i - 1]);
        std::sort(d.begin(), d.end());
        const double r = rate_from_step(d[d.size() / 2]);
        if (r > 0) rate = r;
    }
    if (rate <= 0) rate = 30000.0 / 1001;
    AMediaExtractor_seekTo(s_video.ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);

    if (s_video.w <= 0 || s_video.h <= 0 || duration <= *start_us)
    {
        LOGE << fmt("MP4: %s has no picture size or no length", file);
        s_video.close();
        return false;
    }
    info->w = s_video.w;
    info->h = s_video.h;
    info->sar_num = sar_w > 0 && sar_h > 0 ? sar_w : 1;
    info->sar_den = sar_w > 0 && sar_h > 0 ? sar_h : 1;
    info->fpks = (uint32_t)(rate * 1000 + 0.5);
    info->frames = (uint32_t)((double)(duration - *start_us) * rate / 1e6 + 0.5);
    return true;
}

void video_close() { s_video.close(); }
void video_seek(int64_t us) { s_video.seek(us); }

int video_next(decoded *out)
{
    const uint8_t *b;
    size_t size;
    int r = s_video.next(&b, &size, &out->pts_us, 10000);
    if (r <= 0) return r;
    return mediacodec::describe(s_video.layout, s_video.w, s_video.h, b, size, &out->pic) ? 1 : -1;
}

// MediaCodec takes the buffer back on the next next() or seek()
void video_release() {}

track s_audio;

bool audio_open(const char *file, int *rate, int *channels)
{
    if (!s_audio.open(file, "audio/")) { s_audio.close(); return false; }
    AMediaFormat_getInt32(s_audio.format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &s_audio.rate);
    AMediaFormat_getInt32(s_audio.format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &s_audio.channels);
    *rate = s_audio.rate;
    *channels = s_audio.channels;
    return true;
}

void audio_close() { s_audio.close(); }
void audio_seek(int64_t us) { s_audio.seek(us); }

// 16-bit PCM: 1 with it, 0 at the end, -1 none yet.  The format may change
// with the first buffer (rate, channels).
int audio_next(const int16_t **pcm, size_t *frames, int64_t *pts_us, int *rate, int *channels)
{
    const uint8_t *b;
    size_t size;
    int r = s_audio.next(&b, &size, pts_us, 10000);
    if (r <= 0) return r;
    *rate = s_audio.rate;
    *channels = s_audio.channels > 0 ? s_audio.channels : 2;
    *pcm = (const int16_t *)b;
    *frames = size / (2 * (size_t)*channels);
    return 1;
}

#elif defined(_WIN32) // Media Foundation

std::wstring widen(const char *s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// Media Foundation for the calling thread (VLDP's or the audio thread).
struct mf_thread
{
    bool com = false, mf = false;
    void start()
    {
        if (!com) com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!mf) mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    void stop()
    {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
        mf = com = false;
    }
};

struct reader
{
    IMFSourceReader *r = nullptr;
    IMFMediaBuffer *locked = nullptr;
    IMF2DBuffer *locked2d = nullptr;
    DWORD stream = 0;
    UINT32 w = 0, h = 0, alloc_h = 0;
    UINT32 par_num = 1, par_den = 1;
    LONG stride = 0;
    UINT32 rate = 0, channels = 0;
    bool video = false;

    bool open(const char *file, bool is_video)
    {
        video = is_video;
        IMFAttributes *a = nullptr;
        HRESULT hr = MFCreateAttributes(&a, 2);
        if (FAILED(hr)) { LOGE << fmt("MP4: Media Foundation is not available (0x%08lx)", hr); return false; }
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, video ? TRUE : FALSE);
        if (video) a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromURL(widen(file).c_str(), a, &r);
        a->Release();
        if (FAILED(hr)) { LOGE << fmt("MP4: Media Foundation could not open %s (0x%08lx)", file, hr); return false; }

        stream = video ? (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM : (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
        r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(r->SetStreamSelection(stream, TRUE)))
        {
            LOGE << fmt("MP4: %s has no %s track", file, video ? "video" : "audio");
            return false;
        }

        IMFMediaType *t = nullptr;
        hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr))
        {
            t->SetGUID(MF_MT_MAJOR_TYPE, video ? MFMediaType_Video : MFMediaType_Audio);
            t->SetGUID(MF_MT_SUBTYPE, video ? MFVideoFormat_NV12 : MFAudioFormat_PCM);
            if (!video) t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            hr = r->SetCurrentMediaType(stream, nullptr, t);
            t->Release();
        }
        if (FAILED(hr))
        {
            LOGE << fmt("MP4: no %s decoder for %s (0x%08lx)", video ? "video" : "audio", file, hr);
            return false;
        }
        read_type();
        return true;
    }

    void read_type()
    {
        IMFMediaType *t = nullptr;
        if (FAILED(r->GetCurrentMediaType(stream, &t))) return;
        if (video)
        {
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &alloc_h);
            h = alloc_h;
            MFVideoArea area;
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)))
                h = (UINT32)area.Area.cy;
            UINT32 s = 0;
            stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)s : (LONG)w;
            UINT32 pn = 0, pd = 0;
            if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) && pn && pd)
            {
                par_num = pn;
                par_den = pd;
            }
        }
        else
        {
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        }
        t->Release();
    }

    void unlock()
    {
        if (locked2d) { locked2d->Unlock2D(); locked2d->Release(); locked2d = nullptr; }
        if (locked) { locked->Unlock(); locked->Release(); locked = nullptr; }
    }

    void close()
    {
        unlock();
        if (r) r->Release();
        *this = reader();
    }

    void seek(int64_t us)
    {
        unlock();
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = us * 10;
        r->SetCurrentPosition(GUID_NULL, v);
    }

    // The next sample's data, locked until the next call: 1 with it, 0 at
    // the end of the stream (or after a read error, which is logged), -1
    // for a gap.
    int next(BYTE **data, DWORD *size, LONG *pitch, int64_t *pts_us)
    {
        unlock();
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = nullptr;
        const HRESULT hr = r->ReadSample(stream, 0, &index, &flags, &ts, &s);
        if (FAILED(hr))
        {
            LOGE << fmt("MP4: reading the %s failed (0x%08lx), ending the stream there",
                        video ? "video" : "audio", hr);
            return 0;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) read_type();
        if (!s) return (flags & MF_SOURCE_READERF_ENDOFSTREAM) ? 0 : -1;
        IMFMediaBuffer *b = nullptr;
        s->ConvertToContiguousBuffer(&b);
        s->Release();
        if (!b) return -1;
        *pts_us = ts / 10;
        if (pitch && SUCCEEDED(b->QueryInterface(IID_IMF2DBuffer, (void **)&locked2d)) &&
            SUCCEEDED(locked2d->Lock2D(data, pitch)))
        {
            b->GetCurrentLength(size);
            b->Release();
            return 1;
        }
        if (locked2d) { locked2d->Release(); locked2d = nullptr; }
        DWORD max = 0;
        if (FAILED(b->Lock(data, &max, size))) { b->Release(); return -1; }
        locked = b;
        if (pitch) *pitch = stride;
        return 1;
    }
};

mf_thread s_video_mf;
reader s_video;

bool video_open(const char *file, mp4::stream_info *info, int64_t *start_us, std::string *decoder)
{
    s_video_mf.start();
    if (!s_video.open(file, true))
    {
        s_video.close();
        s_video_mf.stop();
        return false;
    }
    *decoder = "Media Foundation";

    PROPVARIANT v;
    PropVariantInit(&v);
    int64_t duration = 0;
    if (SUCCEEDED(s_video.r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)))
        duration = (int64_t)v.uhVal.QuadPart / 10;
    PropVariantClear(&v);

    UINT32 num = 0, den = 0;
    IMFMediaType *t = nullptr;
    double rate = 30000.0 / 1001;
    if (SUCCEEDED(s_video.r->GetNativeMediaType(s_video.stream, 0, &t)))
    {
        if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) && num && den)
            rate = (double)num / den;
        t->Release();
    }
    // where the pictures start (the first one's time)
    *start_us = 0;
    BYTE *data;
    DWORD size;
    LONG pitch;
    int64_t pts;
    if (s_video.next(&data, &size, &pitch, &pts) == 1) *start_us = pts;
    s_video.seek(0);

    if (s_video.w == 0 || s_video.h == 0 || duration <= *start_us)
    {
        LOGE << fmt("MP4: %s has no picture size or no length", file);
        s_video.close();
        s_video_mf.stop();
        return false;
    }
    info->w = (int)s_video.w;
    info->h = (int)s_video.h;
    info->sar_num = (int)s_video.par_num;
    info->sar_den = (int)s_video.par_den;
    info->fpks = (uint32_t)(rate * 1000 + 0.5);
    info->frames = (uint32_t)((double)(duration - *start_us) * rate / 1e6 + 0.5);
    return true;
}

void video_close()
{
    s_video.close();
    s_video_mf.stop();
}

void video_seek(int64_t us) { s_video.seek(us); }

int video_next(decoded *out)
{
    BYTE *b = nullptr;
    DWORD size = 0;
    LONG pitch = 0;
    const int r = s_video.next(&b, &size, &pitch, &out->pts_us);
    if (r <= 0) return r;
    if (pitch <= 0) pitch = (LONG)s_video.w;
    out->pic = vldp_picture();
    out->pic.y = b;
    out->pic.ypitch = pitch;
    out->pic.uv = b + (size_t)pitch * s_video.alloc_h;
    out->pic.uvpitch = pitch;
    return 1;
}

void video_release() { s_video.unlock(); }

mf_thread s_audio_mf;
reader s_audio;

bool audio_open(const char *file, int *rate, int *channels)
{
    s_audio_mf.start();
    if (!s_audio.open(file, false)) { s_audio.close(); s_audio_mf.stop(); return false; }
    *rate = (int)s_audio.rate;
    *channels = (int)s_audio.channels;
    return true;
}

void audio_close()
{
    s_audio.close();
    s_audio_mf.stop();
}

void audio_seek(int64_t us) { s_audio.seek(us); }

int audio_next(const int16_t **pcm, size_t *frames, int64_t *pts_us, int *rate, int *channels)
{
    BYTE *b = nullptr;
    DWORD size = 0;
    const int r = s_audio.next(&b, &size, nullptr, pts_us);
    if (r <= 0) return r;
    *rate = (int)s_audio.rate;
    *channels = s_audio.channels > 0 ? (int)s_audio.channels : 2;
    *pcm = (const int16_t *)b;
    *frames = size / (2 * (size_t)*channels);
    return 1;
}

#endif

} // namespace plat

// ---------------------------------------------------------------------------
// FFmpeg (CPU)
// ---------------------------------------------------------------------------

namespace ff {

std::string error_text(int e)
{
    char s[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, s, sizeof(s));
    return s;
}

// FFmpeg's own errors into the log
void log_callback(void *avcl, int level, const char *f, va_list vl)
{
    if (level > AV_LOG_ERROR) return;
    char line[512];
    int prefix = 1;
    av_log_format_line2(avcl, level, f, vl, line, sizeof(line), &prefix);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) { LOGW << "FFmpeg: " << line; }
}

void init_logging()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(log_callback);
    });
}

// One track of the file, with its demuxer and decoder.
struct track
{
    AVFormatContext *ctx = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    AVStream *st = nullptr;
    bool video = false;
    bool eof = false;          // the decoder has been sent the end of the stream
    int64_t next_pts_us = 0;   // for a frame without a time of its own

    bool open(const char *file, bool is_video)
    {
        init_logging();
        video = is_video;
        int e = avformat_open_input(&ctx, file, nullptr, nullptr);
        if (e < 0)
        {
            LOGE << fmt("MP4: could not open %s (%s)", file, error_text(e).c_str());
            return false;
        }
        if ((e = avformat_find_stream_info(ctx, nullptr)) < 0)
        {
            LOGE << fmt("MP4: could not read %s (%s)", file, error_text(e).c_str());
            return false;
        }
        const AVCodec *codec = nullptr;
        const AVMediaType type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        const int index = av_find_best_stream(ctx, type, -1, -1, &codec, 0);
        if (index < 0 || !codec)
        {
            LOGE << fmt("MP4: %s has no %s track FFmpeg can decode", file, video ? "video" : "audio");
            return false;
        }
        for (unsigned i = 0; i < ctx->nb_streams; ++i)
            ctx->streams[i]->discard = (int)i == index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        st = ctx->streams[index];

        dec = avcodec_alloc_context3(codec);
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        if (!dec || !pkt || !frame || avcodec_parameters_to_context(dec, st->codecpar) < 0)
        {
            LOGE << "MP4: out of memory";
            return false;
        }
        if (video)
        {
            dec->thread_count = 0; // as many as there are cores
            dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if ((e = avcodec_open2(dec, codec, nullptr)) < 0)
        {
            LOGE << fmt("MP4: FFmpeg's %s decoder would not start (%s)", codec->name, error_text(e).c_str());
            return false;
        }
        return true;
    }

    void close()
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&ctx);
        *this = track();
    }

    int64_t to_us(int64_t ts) const { return av_rescale_q(ts, st->time_base, AVRational{1, 1000000}); }

    void seek(int64_t us)
    {
        av_seek_frame(ctx, st->index, av_rescale_q(us, AVRational{1, 1000000}, st->time_base),
                      AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        eof = false;
        next_pts_us = us;
    }

    // The next decoded frame in 'frame' (valid until the next call): 1 with
    // it, 0 at the end of the stream, -1 if none yet.
    int next(int64_t *pts_us)
    {
        for (int i = 0; i < 64; ++i)
        {
            int e = avcodec_receive_frame(dec, frame);
            if (e == 0)
            {
                const int64_t ts = frame->best_effort_timestamp;
                *pts_us = ts != AV_NOPTS_VALUE ? to_us(ts) : next_pts_us;
                const int64_t d = frame->duration > 0 ? to_us(frame->duration) : 0;
                next_pts_us = *pts_us + d;
                return 1;
            }
            if (e == AVERROR_EOF) return 0;
            if (e != AVERROR(EAGAIN))
            {
                LOGE << fmt("MP4: decoding the %s failed (%s), ending the stream there",
                                video ? "video" : "audio", error_text(e).c_str());
                return 0;
            }
            if (eof) return 0;
            e = av_read_frame(ctx, pkt);
            if (e < 0)
            {
                if (e != AVERROR_EOF)
                {
                    LOGE << fmt("MP4: could not read the %s (%s), ending the stream there",
                                    video ? "video" : "audio", error_text(e).c_str());
                }
                avcodec_send_packet(dec, nullptr); // drain what the decoder holds
                eof = true;
                continue;
            }
            if (pkt->stream_index == st->index)
            {
                e = avcodec_send_packet(dec, pkt);
                // a damaged packet: FFmpeg has logged it, the next ones may decode
                (void)e;
            }
            av_packet_unref(pkt);
        }
        return -1;
    }
};

track s_video;

bool video_open(const char *file, mp4::stream_info *info, int64_t *start_us, std::string *decoder)
{
    if (!s_video.open(file, true)) { s_video.close(); return false; }
    *decoder = fmt("FFmpeg %s (CPU)", s_video.dec->codec->name);

    const AVRational fr = av_guess_frame_rate(s_video.ctx, s_video.st, nullptr);
    double rate = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 30000.0 / 1001;

    // where the pictures start, and that they are a layout VLDP can draw
    *start_us = 0;
    int64_t pts = 0;
    if (s_video.next(&pts) == 1)
    {
        *start_us = pts;
        const int f = s_video.frame->format;
        if (f != AV_PIX_FMT_YUV420P && f != AV_PIX_FMT_YUVJ420P)
        {
            LOGE << fmt("MP4: %s has %s pictures; only 8-bit 4:2:0 video is supported", file,
                            av_get_pix_fmt_name((AVPixelFormat)f) ? av_get_pix_fmt_name((AVPixelFormat)f) : "unknown");
            s_video.close();
            return false;
        }
    }
    s_video.seek(*start_us);

    int64_t end_us = 0;
    if (s_video.ctx->duration != AV_NOPTS_VALUE)
    {
        end_us = s_video.ctx->duration;
        if (s_video.ctx->start_time != AV_NOPTS_VALUE) end_us += s_video.ctx->start_time;
    }
    else if (s_video.st->duration != AV_NOPTS_VALUE)
        end_us = s_video.to_us(s_video.st->start_time != AV_NOPTS_VALUE ? s_video.st->start_time : 0) +
                 s_video.to_us(s_video.st->duration);

    const int w = s_video.dec->width, h = s_video.dec->height;
    if (w <= 0 || h <= 0 || end_us <= *start_us)
    {
        LOGE << fmt("MP4: %s has no picture size or no length", file);
        s_video.close();
        return false;
    }
    AVRational sar = av_guess_sample_aspect_ratio(s_video.ctx, s_video.st, nullptr);
    if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
    info->w = w;
    info->h = h;
    info->sar_num = sar.num;
    info->sar_den = sar.den;
    info->fpks = (uint32_t)(rate * 1000 + 0.5);
    info->frames = (uint32_t)((double)(end_us - *start_us) * rate / 1e6 + 0.5);
    return true;
}

void video_close() { s_video.close(); }
void video_seek(int64_t us) { s_video.seek(us); }

int video_next(decoded *out)
{
    const int r = s_video.next(&out->pts_us);
    if (r <= 0) return r;
    const AVFrame *f = s_video.frame;
    out->pic = vldp_picture();
    out->pic.y = f->data[0];
    out->pic.u = f->data[1];
    out->pic.v = f->data[2];
    out->pic.ypitch = f->linesize[0];
    out->pic.uvpitch = f->linesize[1];
    return 1;
}

// the frame stays valid until the next next() or seek()
void video_release() {}

track s_audio;
std::vector<int16_t> s_pcm; // the last audio frame, 16-bit interleaved

bool audio_open(const char *file, int *rate, int *channels)
{
    if (!s_audio.open(file, false)) { s_audio.close(); return false; }
    *rate = s_audio.dec->sample_rate;
    *channels = s_audio.dec->ch_layout.nb_channels;
    return true;
}

void audio_close()
{
    s_audio.close();
    s_pcm.clear();
}

void audio_seek(int64_t us) { s_audio.seek(us); }

int16_t to_s16(float v)
{
    const int s = (int)lrintf(v * 32767.0f);
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// 16-bit PCM: 1 with it, 0 at the end, -1 none yet.
int audio_next(const int16_t **pcm, size_t *frames, int64_t *pts_us, int *rate, int *channels)
{
    const int r = s_audio.next(pts_us);
    if (r <= 0) return r;
    const AVFrame *f = s_audio.frame;
    const int ch = f->ch_layout.nb_channels;
    const int n = f->nb_samples;
    if (ch <= 0 || n <= 0) return -1;
    s_pcm.resize((size_t)n * ch);
    int16_t *o = s_pcm.data();
    const bool planar = av_sample_fmt_is_planar((AVSampleFormat)f->format);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c)
        {
            const int plane = planar ? c : 0;
            const int at = planar ? i : i * ch + c;
            int16_t v;
            switch (av_get_packed_sample_fmt((AVSampleFormat)f->format))
            {
            case AV_SAMPLE_FMT_S16: v = ((const int16_t *)f->extended_data[plane])[at]; break;
            case AV_SAMPLE_FMT_S32: v = (int16_t)(((const int32_t *)f->extended_data[plane])[at] >> 16); break;
            case AV_SAMPLE_FMT_FLT: v = to_s16(((const float *)f->extended_data[plane])[at]); break;
            case AV_SAMPLE_FMT_DBL: v = to_s16((float)((const double *)f->extended_data[plane])[at]); break;
            default: return -1; // not a format the AAC decoder puts out
            }
            o[(size_t)i * ch + c] = v;
        }
    *pcm = o;
    *frames = (size_t)n;
    *rate = f->sample_rate;
    *channels = ch;
    return 1;
}

} // namespace ff

// The decoders a file is played with, chosen when it is opened: the
// platform's (hardware) unless the CPU Fallback core option is on or there
// is none.
struct backend
{
    bool (*video_open)(const char *, mp4::stream_info *, int64_t *, std::string *);
    void (*video_close)();
    void (*video_seek)(int64_t);
    int (*video_next)(decoded *);
    void (*video_release)();
    bool (*audio_open)(const char *, int *, int *);
    void (*audio_close)();
    void (*audio_seek)(int64_t);
    int (*audio_next)(const int16_t **, size_t *, int64_t *, int *, int *);
};

const backend k_ffmpeg = {ff::video_open,  ff::video_close, ff::video_seek, ff::video_next, ff::video_release,
                          ff::audio_open,  ff::audio_close, ff::audio_seek, ff::audio_next};
#ifdef MP4_PLATFORM_BACKEND
const backend k_platform = {plat::video_open, plat::video_close, plat::video_seek, plat::video_next,
                            plat::video_release, plat::audio_open, plat::audio_close, plat::audio_seek,
                            plat::audio_next};
#endif

const backend &choose_backend()
{
#ifdef MP4_PLATFORM_BACKEND
    if (g_lr_vldp_hwdec) return k_platform;
#endif
    return k_ffmpeg;
}

const backend *s_vb = &k_ffmpeg; // the video's

// ---------------------------------------------------------------------------
// Video: picture numbering
// ---------------------------------------------------------------------------

mp4::frame_fn s_draw = nullptr;
mp4::abort_fn s_abort = nullptr;
bool s_open = false;
double s_fps = 30;
int64_t s_start_us = 0;
long s_target = 0;     // first picture to put out after a seek
long s_expected = 0;   // next picture number to put out

long frame_of(int64_t pts_us) { return (long)((double)(pts_us - s_start_us) * s_fps / 1e6 + 0.5); }
int64_t time_of(long frame) { return s_start_us + (int64_t)((double)frame * 1e6 / s_fps); }

} // namespace

namespace mp4 {

bool handles(const char *file)
{
    const char *dot = file ? strrchr(file, '.') : nullptr;
    if (!dot) return false;
    std::string e = dot + 1;
    for (auto &c : e) c = (char)tolower((unsigned char)c);
    return e == "mp4" || e == "m4v" || e == "mkv";
}

bool open(const char *file, frame_fn draw, abort_fn abort, stream_info *info)
{
    close();
    std::string decoder;
    s_vb = &choose_backend();
    if (!s_vb->video_open(file, info, &s_start_us, &decoder)) return false;
    s_draw = draw;
    s_abort = abort;
    s_fps = info->fpks / 1000.0;
    s_open = true;
    s_target = s_expected = 0;
    LOGI << fmt("VLDP: playing %s (%dx%d, %.3f fps, %u frames) with %s", base_name(file).c_str(), info->w,
                info->h, s_fps, info->frames, decoder.c_str());
    return true;
}

void close()
{
    if (!s_open) return;
    s_vb->video_close();
    s_open = false;
}

bool active() { return s_open; }

void seek(uint32_t frame)
{
    if (!s_open) return;
    s_target = s_expected = (long)frame;
    s_vb->video_seek(time_of((long)frame));
}

bool decode_some()
{
    for (int i = 0; i < 4; ++i)
    {
        decoded d;
        const int r = s_vb->video_next(&d);
        if (r == 0) return false;
        if (r < 0) return true; // nothing yet: let VLDP look at its commands
        const long n = frame_of(d.pts_us);
        if (n < s_target) { s_vb->video_release(); continue; } // before the frame searched to

        // pictures missing from the stream still count, as on a laserdisc
        while (s_expected < n)
        {
            ++s_expected;
            vldp_picture none = vldp_picture();
            none.placeholder = 1;
            s_draw(none);
            if (s_abort()) { s_vb->video_release(); return true; }
        }
        if (n >= s_expected)
        {
            s_expected = n + 1;
            s_draw(d.pic);
        }
        s_vb->video_release();
        if (s_abort()) return true;
    }
    return true;
}

} // namespace mp4

// ---------------------------------------------------------------------------
// Audio: decoded ahead into a ring of 44100 Hz stereo
// ---------------------------------------------------------------------------

namespace {

const int OUT_RATE = 44100;
const size_t RING_FRAMES = OUT_RATE; // a second

std::thread s_audio_thread;
std::mutex s_amutex;
std::condition_variable s_acv;
bool s_aquit = false, s_aeos = false;
bool s_afailed = false;          // the track could not be opened
bool s_seek_pending = false;
uint64_t s_seek_sample = 0;      // where the ring's data starts after a seek
uint64_t s_generation = 0;
std::vector<int16_t> s_ring;     // stereo frames
size_t s_rhead = 0, s_rcount = 0;
size_t s_deficit = 0;            // frames of silence handed out that the ring still owes

void ring_put(const int16_t *lr, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        const size_t at = (s_rhead + s_rcount) % RING_FRAMES;
        s_ring[2 * at] = lr[2 * i];
        s_ring[2 * at + 1] = lr[2 * i + 1];
        ++s_rcount;
    }
}

void audio_worker(std::string file)
{
    int rate = 0, channels = 0;
    const backend &b = choose_backend();
    if (!b.audio_open(file.c_str(), &rate, &channels))
    {
        // logged by audio_open(); read() hands out the end of the track
        std::lock_guard<std::mutex> l(s_amutex);
        s_afailed = s_aeos = true;
        return;
    }
    LOGI << fmt("VLDP: audio from %s", base_name(file.c_str()).c_str());

    std::vector<int16_t> out;
    uint64_t want = 0;              // index of the next output sample to write
    uint64_t gen = (uint64_t)-1;
    bool eos = false;

    for (;;)
    {
        uint64_t seek_to = 0;
        bool do_seek = false;
        {
            std::unique_lock<std::mutex> l(s_amutex);
            s_acv.wait(l, [&] { return s_aquit || s_seek_pending || (!eos && s_rcount + 4096 < RING_FRAMES); });
            if (s_aquit) break;
            if (s_seek_pending)
            {
                s_seek_pending = false;
                do_seek = true;
                seek_to = s_seek_sample;
                gen = s_generation;
            }
        }
        if (do_seek)
        {
            b.audio_seek((int64_t)(seek_to * 1000000 / OUT_RATE));
            want = seek_to;
            eos = false;
            continue;
        }

        const int16_t *pcm;
        size_t frames;
        int64_t pts;
        const int r = b.audio_next(&pcm, &frames, &pts, &rate, &channels);
        if (r == 0)
        {
            eos = true;
            std::lock_guard<std::mutex> l(s_amutex);
            if (gen == s_generation) s_aeos = true;
            continue;
        }
        if (r < 0 || rate <= 0 || frames == 0) continue;

        // which output sample this buffer starts at, and how much of it is
        // before the one wanted
        const double first = (double)pts * OUT_RATE / 1e6;
        if (first + (double)frames * OUT_RATE / rate <= (double)want) continue;
        const double step = (double)rate / OUT_RATE;
        double ipos = ((double)want - first) * rate / OUT_RATE; // input frame of 'want'
        if (ipos < 0)
        {
            // a gap before this buffer: silence up to it
            const size_t gap = (size_t)(-ipos / step);
            out.assign(gap * 2, 0);
            want += gap;
            ipos = 0;
        }
        else
            out.clear();

        auto sample = [&](size_t i, int c) -> int {
            if (channels == 1) return pcm[i];
            return pcm[i * channels + c];
        };
        for (; ipos + 1 < (double)frames; ipos += step)
        {
            const size_t i = (size_t)ipos;
            const double f = ipos - i;
            for (int c = 0; c < 2; ++c)
                out.push_back((int16_t)(sample(i, c) * (1 - f) + sample(i + 1, c) * f));
            ++want;
        }

        std::unique_lock<std::mutex> l(s_amutex);
        if (gen != s_generation) continue; // a seek came in meanwhile
        size_t n = out.size() / 2;
        const int16_t *p = out.data();
        // silence already handed out in place of these
        const size_t owed = std::min(n, s_deficit);
        s_deficit -= owed;
        p += 2 * owed;
        n -= owed;
        while (n)
        {
            s_acv.wait(l, [&] { return s_aquit || s_seek_pending || s_rcount < RING_FRAMES; });
            if (s_aquit || s_seek_pending) break;
            const size_t k = std::min(n, RING_FRAMES - s_rcount);
            ring_put(p, k);
            p += 2 * k;
            n -= k;
        }
    }
    b.audio_close();
}

} // namespace

namespace mp4audio {

void open(const char *file)
{
    close();
    s_ring.assign(RING_FRAMES * 2, 0);
    s_rhead = s_rcount = s_deficit = 0;
    s_aquit = s_aeos = s_afailed = false;
    s_seek_pending = true;
    s_seek_sample = 0;
    ++s_generation;
    s_audio_thread = std::thread(audio_worker, std::string(file));
}

void close()
{
    if (!s_audio_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> l(s_amutex);
        s_aquit = true;
    }
    s_acv.notify_all();
    s_audio_thread.join();
}

void seek(uint64_t sample)
{
    std::lock_guard<std::mutex> l(s_amutex);
    s_seek_pending = true;
    s_seek_sample = sample;
    ++s_generation;
    s_rhead = s_rcount = s_deficit = 0;
    s_aeos = s_afailed;
    s_acv.notify_all();
}

long read(uint8_t *buf, size_t bytes)
{
    const size_t frames = bytes / 4;
    int16_t *out = (int16_t *)buf;
    std::lock_guard<std::mutex> l(s_amutex);
    if (s_rcount == 0 && s_aeos) return 0;
    const size_t have = std::min(frames, s_rcount);
    for (size_t i = 0; i < have; ++i)
    {
        const size_t at = (s_rhead + i) % RING_FRAMES;
        out[2 * i] = s_ring[2 * at];
        out[2 * i + 1] = s_ring[2 * at + 1];
    }
    s_rhead = (s_rhead + have) % RING_FRAMES;
    s_rcount -= have;
    s_acv.notify_all();
    if (have < frames && !s_aeos)
    {
        // the decoder is behind (it is still opening the file, or getting
        // to a seek's target): silence now, and those samples are skipped
        // when they arrive so the sound stays in step with the picture
        memset(out + 2 * have, 0, (frames - have) * 4);
        s_deficit += frames - have;
        return (long)(frames * 4);
    }
    return (long)(have * 4);
}

} // namespace mp4audio

#endif
