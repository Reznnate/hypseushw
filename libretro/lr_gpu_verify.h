/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * DAPHNE_LR_VERIFY_GPU=1: the GPU renderers read every frame back and
 * compare it with the CPU renderer's output (development aid; reading back
 * stalls the GPU, so timings from such a run mean nothing).  Shared by the
 * GL and Vulkan renderers.
 */
#ifndef LR_GPU_VERIFY_H
#define LR_GPU_VERIFY_H

#include <libretro.h>
#include <stdint.h>
#include <stdlib.h>

#include "video/video.h"

typedef void (*lr_gpu_log_fn)(enum retro_log_level level, const char *fmt, ...);

struct lr_gpu_verify {
    bool enabled = false;
    uint64_t frames = 0, bad = 0;   // frames checked, frames that differed
};

static inline void lr_gpu_verify_start(lr_gpu_verify &v)
{
    v.enabled = getenv("DAPHNE_LR_VERIFY_GPU") != NULL;
    v.frames = v.bad = 0;
}

// Compares 'rgba' (f.w x f.h, 4 bytes a pixel, rows packed) with the CPU
// renderer's output for the same frame.  'bottom_up' when row 0 of 'rgba'
// is the bottom of the picture (a GL framebuffer).  Logs the first
// differing pixel for the first 20 bad frames.
static inline void lr_gpu_verify_frame(lr_gpu_verify &v, const video::lr_frame &f, const uint8_t *rgba,
                                       bool bottom_up, const char *tag, lr_gpu_log_fn log)
{
    const uint32_t *cpu = video::lr_compose_cpu(f);
    long bad = 0, first_x = -1, first_y = -1;
    uint32_t want = 0, got = 0;
    for (int y = 0; y < f.h; ++y)
    {
        const uint8_t *g = rgba + (size_t)(bottom_up ? f.h - 1 - y : y) * f.w * 4;
        const uint32_t *c = cpu + (size_t)y * f.w;
        for (int x = 0; x < f.w; ++x, g += 4)
        {
            const uint32_t px = (uint32_t)g[0] << 16 | (uint32_t)g[1] << 8 | g[2];
            if (px != (c[x] & 0xFFFFFF) && !bad++)
            {
                first_x = x;
                first_y = y;
                want = c[x] & 0xFFFFFF;
                got = px;
            }
        }
    }
    ++v.frames;
    if (bad && v.bad++ < 20 && log)
        log(RETRO_LOG_ERROR, "%s: %ld pixels differ from the CPU renderer, first at %ld,%ld: %06x instead of %06x",
            tag, bad, first_x, first_y, got, want);
}

// The summary, for retro_unload_game().
static inline void lr_gpu_verify_report(const lr_gpu_verify &v, const char *tag, lr_gpu_log_fn log)
{
    if (v.enabled && v.frames && log)
        log(v.bad ? RETRO_LOG_ERROR : RETRO_LOG_INFO, "%s: %llu of %llu frames checked against the CPU renderer differed",
            tag, (unsigned long long)v.bad, (unsigned long long)v.frames);
}

#endif
