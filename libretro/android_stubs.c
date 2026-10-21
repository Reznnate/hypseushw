/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * SDL3's Android platform layer (src/core/android) is always linked on
 * Android and refers to the Android video/event backend, which the core
 * builds without (SDL_VIDEO=OFF).  These entry points are only reached from
 * SDLActivity's Java callbacks, which never run inside a libretro core, so
 * empty definitions are enough to satisfy the linker.
 */
#include <stdbool.h>
#include <stddef.h>

void *Android_Window = NULL;

void Android_SetWindowSafeAreaInsets(int l, int r, int t, int b) { (void)l; (void)r; (void)t; (void)b; }
void Android_SetScreenResolution(int sw, int sh, int dw, int dh, float density, float rate)
{ (void)sw; (void)sh; (void)dw; (void)dh; (void)density; (void)rate; }
void Android_SetOrientation(int o) { (void)o; }
void Android_SetDarkMode(bool d) { (void)d; }
void Android_SendResize(void *w) { (void)w; }
void Android_RestoreScreenKeyboard(void *d, void *w) { (void)d; (void)w; }
void Android_QuitEvents(void) {}
void Android_PumpEvents(long long timeout) { (void)timeout; }
void Android_InitEvents(void) {}
void Android_OnTouch(void *w, int dev, int id, int a, float x, float y, float p)
{ (void)w; (void)dev; (void)id; (void)a; (void)x; (void)y; (void)p; }
void Android_OnPen(void *w, int id, int dt, int b, int a, float x, float y, float p)
{ (void)w; (void)id; (void)dt; (void)b; (void)a; (void)x; (void)y; (void)p; }
void Android_OnMouse(void *w, int b, int a, float x, float y, bool r)
{ (void)w; (void)b; (void)a; (void)x; (void)y; (void)r; }
bool Android_OnKeyDown(int k) { (void)k; return false; }
bool Android_OnKeyUp(int k) { (void)k; return false; }
long long Android_ConvertJavaTouchID(int id) { return id; }
