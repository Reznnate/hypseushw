/*
 * MP4 laserdisc video for Singe 2 games (H.264 or HEVC, with AAC sound, in
 * .mp4/.m4v/.mkv), decoded by the platform: MediaCodec on Android, Media
 * Foundation on Windows.  The hardware decoder is used by default; with the
 * CPU Fallback core option (g_lr_vldp_hwdec off) Android's software decoder
 * is chosen and Media Foundation's hardware transforms are turned off.
 *
 * Frame N is the Nth picture of the file in display order (the numbering
 * Singe 2 uses).  A search puts out pictures from the frame searched to:
 * the ones between the key frame and it are decoded and dropped here, so to
 * VLDP the file behaves like an MPEG-2 stream that starts at that frame.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VLDP_MP4_H
#define VLDP_MP4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __LIBRETRO__
#define VLDP_MP4 1

#include "vldp_internal.h"

namespace mp4 {

// Receives each picture, in display order (valid only during the call).
typedef void (*frame_fn)(const vldp_picture &pic);
// Returns non-zero when decoding should stop (a command that ends the
// playback is waiting).
typedef int (*abort_fn)(void);

struct stream_info
{
    int w, h;              // picture size
    int sar_num, sar_den;  // shape of a pixel (1:1 for square pixels)
    uint32_t fpks;         // frames per kilosecond
    uint32_t frames;       // number of pictures
};

// True for the file names this backend plays (.mp4, .m4v, .mkv).
bool handles(const char *file);

// Opens the video track.  False, with the reason logged, on failure.
bool open(const char *file, frame_fn draw, abort_fn abort, stream_info *info);
void close();
bool active();

// The next picture put out will be frame 'frame'.
void seek(uint32_t frame);
// Decodes some of the stream, putting out pictures.  False once the last
// picture is out (or the decoder failed, which is logged).
bool decode_some();

} // namespace mp4

// The audio track, as 44100 Hz 16-bit stereo, decoded ahead on a thread of
// its own.
namespace mp4audio {

// Starts opening the file's audio track on the audio thread and returns at
// once: a file on a network share can take a while.  Until it is open, and
// if it cannot be (logged there), read() hands out silence.
void open(const char *file);
void close();
// The next sample read will be 'sample' (44100 Hz).
void seek(uint64_t sample);
// Fills 'buf' with up to 'bytes' of PCM.  Returns the bytes filled, 0 at
// the end of the track.  Never waits: what the decoder has not caught up
// with is silence, and the samples it stood for are dropped when they
// arrive, so the sound stays in step with the picture.
long read(uint8_t *buf, size_t bytes);

} // namespace mp4audio

#endif
#endif
