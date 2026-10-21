/*
 * Android MediaCodec helpers for VLDP.  See vldp_mediacodec.h.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vldp_mediacodec.h"

#ifdef VLDP_MEDIACODEC

#include <dlfcn.h>

#include <plog/Log.h>

#include "../io/conout.h"

namespace mediacodec {

typedef media_status_t (*get_name_fn)(AMediaCodec *, char **);
typedef void (*release_name_fn)(AMediaCodec *, char *);

std::string codec_name(AMediaCodec *c)
{
    static get_name_fn get = (get_name_fn)dlsym(RTLD_DEFAULT, "AMediaCodec_getName");
    static release_name_fn rel = (release_name_fn)dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName");
    char *n = nullptr;
    std::string r;
    if (get && get(c, &n) == AMEDIA_OK && n)
    {
        r = n;
        if (rel) rel(c, n);
    }
    return r;
}

bool is_software(const std::string &n)
{
    if (n.rfind("OMX.google.", 0) == 0 || n.rfind("c2.android.", 0) == 0) return true;
    return n.find(".sw.") != std::string::npos ||
           (n.size() > 3 && n.compare(n.size() - 3, 3, ".sw") == 0);
}

media_status_t configure_video(AMediaCodec *c, AMediaFormat *f)
{
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_YUV420_SEMIPLANAR);
    media_status_t st = AMediaCodec_configure(c, f, nullptr, nullptr, 0);
    if (st != AMEDIA_OK)
    {
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_YUV420_FLEXIBLE);
        st = AMediaCodec_configure(c, f, nullptr, nullptr, 0);
    }
    return st;
}

void read_output_format(AMediaCodec *c, layout *l, const char *who)
{
    AMediaFormat *f = AMediaCodec_getOutputFormat(c);
    if (!f) return;
    int32_t v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) l->color = v;
    if (AMediaFormat_getInt32(f, "stride", &v) && v > 0) l->stride = v;
    if (AMediaFormat_getInt32(f, "slice-height", &v) && v > 0) l->slice = v;
    if (l->color != COLOR_YUV420_PLANAR && l->color != COLOR_YUV420_SEMIPLANAR) {
        LOGW << fmt("%s: unexpected colour format 0x%x, reading it as NV12", who, l->color);
    }
    AMediaFormat_delete(f);
}

bool describe(const layout &l, int w, int h, const uint8_t *b, size_t size, vldp_picture *pic)
{
    const int stride = l.stride > 0 ? l.stride : w;
    const int slice = l.slice > 0 ? l.slice : h;
    const size_t y_bytes = (size_t)stride * slice;
    *pic = vldp_picture();
    pic->y = b;
    pic->ypitch = stride;
    if (l.color == COLOR_YUV420_PLANAR)
    {
        // the V plane follows the U plane; its last row need not be padded
        const size_t c_bytes = (size_t)(stride / 2) * (slice / 2);
        if (size < y_bytes + c_bytes + (size_t)(stride / 2) * (h / 2 - 1) + w / 2) return false;
        pic->u = b + y_bytes;
        pic->v = b + y_bytes + c_bytes;
        pic->uvpitch = stride / 2;
    }
    else
    {
        if (size < y_bytes + (size_t)stride * (h / 2 - 1) + w) return false;
        pic->uv = b + y_bytes;
        pic->uvpitch = stride;
    }
    return true;
}

} // namespace mediacodec

#endif
