/*
 * Hardware MPEG-2 decoding for VLDP on Android (MediaCodec), used in place of
 * libmpeg2 when the device has a hardware decoder and the stream suits it.
 *
 * It puts out exactly the pictures libmpeg2 would, in the same order, so
 * VLDP's frame counting (searches, skips) is unchanged: after a reset the
 * B-pictures that also depend on a picture before the reset point are not
 * decoded, and a placeholder takes their place, as libmpeg2 shows them too
 * (VLDP never lets them reach the screen).
 *
 * If the decoder fails or stops responding it is closed (active() turns
 * false) and VLDP decodes the rest of the stream with libmpeg2.
 *
 * Copyright (C) 2026 HypseusHW contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VLDP_HWDEC_H
#define VLDP_HWDEC_H

#include <stddef.h>
#include <stdint.h>

#if defined(__LIBRETRO__) && defined(__ANDROID__)
#define VLDP_HWDEC 1

#include "vldp_internal.h"

namespace hwdec {

// Receives each decoded picture, in display order.  The pixels point into
// the decoder's output buffer and are valid only during the call.
typedef void (*frame_fn)(const vldp_picture &pic);
// Returns non-zero when decoding should stop (a command that ends the
// playback is waiting).  Also asked while the decoder has no room for input.
typedef int (*abort_fn)(void);

// Prepares a hardware decoder for the stream that starts with 'head' (the
// first part of the file; the sequence header is its first 'seq_len'
// bytes).  False, with the reason logged, when the stream should be
// decoded in software: no hardware decoder, interlaced pictures, ...
bool open(const uint8_t *head, size_t head_len, size_t seq_len, int width, int height,
          frame_fn draw, abort_fn abort, const char *file);
void close();
bool active();

// Forgets everything queued: decoding restarts at the next I-picture.
void reset();
// Decodes this part of the stream, calling 'draw' for each picture.  The
// decoder may be closed on return (see above).
void decode(const uint8_t *buf, const uint8_t *end);
// End of the stream: puts out the pictures still inside the decoder.
void drain();

} // namespace hwdec

#endif
#endif
