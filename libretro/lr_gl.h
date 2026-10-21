/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * GPU renderer (libretro hardware rendering with OpenGL ES 3 / OpenGL 3.2).
 */
#ifndef LR_GL_H
#define LR_GL_H

#include <libretro.h>

namespace video { struct lr_frame; }

typedef void (*lr_gl_log_fn)(enum retro_log_level level, const char *fmt, ...);

// Asks the frontend for a GL context; call from retro_load_game().  False
// when the frontend cannot provide one (use the CPU renderer then).
bool lr_gl_request(retro_environment_t environ_cb, lr_gl_log_fn log);
// The context is up and the renderer works.
bool lr_gl_ready(void);
// Draws the frame into the frontend's framebuffer.  False if not ready.
bool lr_gl_render(const video::lr_frame &f);
// Logs the DAPHNE_LR_VERIFY_GPU result; call from retro_unload_game().
void lr_gl_unload(void);

#endif
