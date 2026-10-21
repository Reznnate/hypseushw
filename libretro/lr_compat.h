/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Force-included into every engine (core/src) translation unit.
 *
 * The engine paces itself with SDL_GetTicks()/SDL_Delay().  In the libretro
 * build time is owned by the frontend: lr_ticks() is a virtual millisecond
 * clock advanced by retro_run(), and lr_delay() called from the emulation
 * coroutine yields back to retro_run() instead of sleeping.  On any other
 * thread (the VLDP decoder) lr_delay() is a real sleep.
 *
 * Object-like macros are used so SDL's own prototypes are renamed as well,
 * which keeps the declarations consistent.  lr_core.cpp, which implements
 * the replacements, includes this with LR_COMPAT_NO_RENAMES.
 */
#ifndef LR_COMPAT_H
#define LR_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr_threads.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t lr_ticks(void);
uint64_t lr_real_ticks(void);
void lr_delay(uint32_t ms);

/* File loaders that fail cleanly for missing files.  On Android, SDL looks
 * up any path it cannot open as an absolute file (relative paths, missing
 * files) through JNI, in the app's storage and APK assets, and a libretro
 * core has no JNI environment. */
struct SDL_Surface;
struct SDL_AudioSpec;
struct SDL_Surface *lr_LoadBMP(const char *file);
struct SDL_Surface *lr_LoadPNG(const char *file);
bool lr_LoadWAV(const char *path, struct SDL_AudioSpec *spec, uint8_t **buf, uint32_t *len);
void *lr_LoadFile(const char *file, size_t *datasize);
struct SDL_IOStream;
struct IMG_Animation;
struct TTF_Font;
struct SDL_IOStream *lr_IOFromFile(const char *file, const char *mode);
struct SDL_Surface *lr_IMG_Load(const char *file);
struct IMG_Animation *lr_IMG_LoadAnimation(const char *file);
struct TTF_Font *lr_TTF_OpenFont(const char *file, float ptsize);

/* Shows an engine error to the player as a frontend notification (the caller
 * logs it).  The engine must never open dialogs of its own inside a frontend. */
void lr_show_error(const char *msg);

/* Hands queued frontend input to the game (from SDL_check_input). */
void lr_input_drain(void);

#ifdef __cplusplus
}

/* Engine functions the core calls (C++ linkage, as defined). */
bool lr_vldp_caught_up();                   /* ldp-out/ldp-vldp.cpp */
void lr_vldp_debug(char *buf, size_t len);  /* ldp-out/ldp-vldp.cpp */
void lr_clear_quitflag();                   /* lr_stubs.cpp */
#endif

#ifndef LR_COMPAT_NO_RENAMES
#define SDL_GetTicks lr_ticks
#define SDL_Delay    lr_delay
#define SDL_LoadBMP  lr_LoadBMP
#define SDL_LoadPNG  lr_LoadPNG
#define SDL_LoadWAV  lr_LoadWAV
#define SDL_LoadFile lr_LoadFile
#define SDL_IOFromFile    lr_IOFromFile
#define IMG_Load          lr_IMG_Load
#define IMG_LoadAnimation lr_IMG_LoadAnimation
#define TTF_OpenFont      lr_TTF_OpenFont
#endif

#endif
