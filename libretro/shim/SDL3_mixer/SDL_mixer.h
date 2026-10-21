/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Stand-in for SDL3_mixer in the libretro build.  Singe's music functions
 * (musicLoad and friends) would open their own audio device; none of the
 * supported games use them, so the mixer never initializes and musicLoad
 * reports the error.
 */
#ifndef LR_SHIM_SDL_MIXER_H
#define LR_SHIM_SDL_MIXER_H

#include <SDL3/SDL.h>

typedef struct MIX_Mixer MIX_Mixer;
typedef struct MIX_Audio MIX_Audio;
typedef struct MIX_Track MIX_Track;

#define MIX_PROP_PLAY_LOOPS_NUMBER "SDL_mixer.play.loops"

static inline bool MIX_Init(void) { return SDL_SetError("Singe music is not supported by this core"); }
static inline void MIX_Quit(void) {}
static inline MIX_Mixer *MIX_CreateMixerDevice(SDL_AudioDeviceID d, const SDL_AudioSpec *s)
{ (void)d; (void)s; return NULL; }
static inline void MIX_DestroyMixer(MIX_Mixer *m) { (void)m; }
static inline bool MIX_SetMixerGain(MIX_Mixer *m, float g) { (void)m; (void)g; return false; }
static inline MIX_Audio *MIX_LoadAudio(MIX_Mixer *m, const char *p, bool d)
{ (void)m; (void)p; (void)d; return NULL; }
static inline MIX_Audio *MIX_LoadAudio_IO(MIX_Mixer *m, SDL_IOStream *io, bool d, bool c)
{ (void)m; (void)d; if (c && io) SDL_CloseIO(io); return NULL; }
static inline void MIX_DestroyAudio(MIX_Audio *a) { (void)a; }
static inline MIX_Track *MIX_CreateTrack(MIX_Mixer *m) { (void)m; return NULL; }
static inline void MIX_DestroyTrack(MIX_Track *t) { (void)t; }
static inline bool MIX_SetTrackAudio(MIX_Track *t, MIX_Audio *a) { (void)t; (void)a; return false; }
static inline bool MIX_PlayTrack(MIX_Track *t, SDL_PropertiesID p) { (void)t; (void)p; return false; }
static inline bool MIX_TrackPlaying(MIX_Track *t) { (void)t; return false; }
static inline bool MIX_PauseTrack(MIX_Track *t) { (void)t; return false; }
static inline bool MIX_ResumeTrack(MIX_Track *t) { (void)t; return false; }
static inline bool MIX_StopTrack(MIX_Track *t, Sint64 f) { (void)t; (void)f; return false; }

#endif
