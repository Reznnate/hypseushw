/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Vulkan renderer (libretro hardware rendering), for frontends running a
 * Vulkan video driver.  Same job and same output as the GL renderer.
 */
#ifndef LR_VK_H
#define LR_VK_H

#include <libretro.h>

namespace video { struct lr_frame; }

typedef void (*lr_vk_log_fn)(enum retro_log_level level, const char *fmt, ...);

// Asks the frontend for a Vulkan device; call from retro_load_game().
// False when it cannot provide one.
bool lr_vk_request(retro_environment_t environ_cb, lr_vk_log_fn log);
// Renders the frame and hands the image to the frontend.  False if not ready.
bool lr_vk_render(const video::lr_frame &f);
// Logs the DAPHNE_LR_VERIFY_GPU result; call from retro_unload_game().
void lr_vk_unload(void);

#endif
