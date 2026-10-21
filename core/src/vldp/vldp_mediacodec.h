/*
 * Android MediaCodec helpers shared by the MPEG-2 hardware decoder
 * (vldp_hwdec) and the MP4 backend (vldp_mp4): decoder names, the output
 * colour formats VLDP reads, and where a picture lies in an output buffer.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VLDP_MEDIACODEC_H
#define VLDP_MEDIACODEC_H

#if defined(__LIBRETRO__) && defined(__ANDROID__)
#define VLDP_MEDIACODEC 1

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "vldp_internal.h"

namespace mediacodec {

// MediaCodecInfo.CodecCapabilities colour formats
const int32_t COLOR_YUV420_PLANAR     = 19;
const int32_t COLOR_YUV420_SEMIPLANAR = 21;         // NV12
const int32_t COLOR_YUV420_FLEXIBLE   = 0x7F420888; // the decoder's choice of 4:2:0

// How the decoder lays out a picture in its output buffers.
struct layout
{
    int32_t color = COLOR_YUV420_SEMIPLANAR;
    int32_t stride = 0, slice = 0; // 0: the picture's width / height
};

// The decoder's name, or "" before Android 9 (AMediaCodec_getName is looked
// up at run time so the core still loads on older Android).
std::string codec_name(AMediaCodec *c);

// True for Android's own software decoders, which are no faster than
// libmpeg2 (Samsung names its software decoders "*.sw.*").
bool is_software(const std::string &name);

// Configures 'c' as a video decoder, asking for NV12 output.  Decoders that
// refuse a colour format request are configured again with the flexible
// format, which they then report in their output format.
media_status_t configure_video(AMediaCodec *c, AMediaFormat *f);

// Updates 'l' from the decoder's output format.  'who' prefixes the warning
// for a colour format VLDP does not know (it is then read as NV12).
void read_output_format(AMediaCodec *c, layout *l, const char *who);

// Describes the w x h picture in output buffer 'b' of 'size' bytes in place.
// False if the buffer is too small for the layout.
bool describe(const layout &l, int w, int h, const uint8_t *b, size_t size, vldp_picture *pic);

} // namespace mediacodec

#endif
#endif
