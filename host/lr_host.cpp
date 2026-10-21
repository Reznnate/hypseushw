/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * lr-host: minimal libretro frontend used to develop and test the core.
 *
 *   lr-host <core> <content> [options]
 *   e.g. lr-host hypseushw_libretro.dll lair.txt --headless --frames 600
 *
 *   --system DIR        system directory (default: ./system)
 *   --save DIR          save directory (default: ./saves)
 *   --opt key=value     core option override
 *   --headless          no window/audio device, run as fast as possible
 *   --realtime          (with --headless) pace frames at the core's frame
 *                       rate instead, as a frontend would
 *   --frames N          stop after N frames (default: run until closed)
 *   --dump DIR          write PNG frames and audio.wav into DIR
 *   --dump-every K      PNG every K frames (default 60)
 *   --press F:BTN[:N]   hold BTN for N frames (default 6) starting at frame F
 *                       BTN: up down left right a b x y l r l2 r2 l3 r3 select start
 *   --vulkan            (with --headless) offer the core Vulkan instead of OpenGL,
 *                       like a frontend running a Vulkan video driver
 *   --no-gl             refuse the core's request for an OpenGL context, like a
 *                       frontend running a Direct3D or Vulkan driver
 *
 * Cores that render with OpenGL get an OpenGL ES 3 context on Android (EGL
 * pbuffer in the adb-shell build) and OpenGL 3.2 core elsewhere.
 *
 * Windowed keys: arrows, Z or Space=B(button1) X=A(button2) C=Y A=X S=L D=R,
 *                5=Select(coin) 1=Start, Esc quits.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libretro.h>

#include "../libretro/lr_glapi.h"

#ifdef LR_HOST_CLI
#include <EGL/egl.h>
#include <dlfcn.h>
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <filesystem>
#include <string>
#include <vector>

// stderr on desktop and the adb-shell build, logcat in the app (via SDL)
static void hlog(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(__ANDROID__) && !defined(LR_HOST_CLI)
    SDL_Log("%s", buf);
#else
    fputs(buf, stderr);
#endif
}

struct Core {
    SDL_SharedObject *lib = nullptr;
#define SYM(n) decltype(&::n) n = nullptr
    SYM(retro_init); SYM(retro_deinit); SYM(retro_api_version);
    SYM(retro_get_system_info); SYM(retro_get_system_av_info);
    SYM(retro_set_environment); SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_load_game); SYM(retro_unload_game); SYM(retro_run);
#undef SYM
};

static Core core;
static std::string g_system_dir = "system", g_save_dir = "saves", g_dump_dir;
static std::map<std::string, std::string> g_opts;
static bool g_headless = false, g_shutdown = false;
static long g_dupes = 0;
static bool g_realtime = false; // pace headless runs at the core's frame rate
static long g_max_frames = -1, g_dump_every = 60, g_frame = 0;

static std::vector<uint32_t> g_fb;
static int g_fb_w = 0, g_fb_h = 0;
static float g_aspect = 4.0f / 3.0f;
static double g_fps = 60.0, g_rate = 44100.0;

// OpenGL for cores that render with it
static bool g_no_gl = false;
static retro_hw_render_callback *g_hw = nullptr;
static lr_glapi hgl;
static GLuint g_fbo = 0, g_fbo_tex = 0;
static bool g_hw_frame = false;         // the last frame is in g_fbo
static SDL_GLContext g_sdl_gl = nullptr;
static bool g_vulkan = false;           // --vulkan
static bool g_hw_vk = false;            // the core renders with Vulkan

#include "lr_host_vk.inc"

static std::vector<int16_t> g_wav;          // headless capture
static SDL_AudioStream *g_audio = nullptr;

struct Press { long frame, len; unsigned id; };
static std::vector<Press> g_presses;
static bool g_keys[16];
static bool g_axis[16];   // analog inputs mapped to buttons

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list ap;
    char msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    hlog("[%s] %s", names[level & 3], msg);
}

static retro_proc_address_t host_gl_proc(const char *name)
{
#ifdef LR_HOST_CLI
    if (auto p = (retro_proc_address_t)eglGetProcAddress(name)) return p;
    static void *gles = dlopen("libGLESv3.so", RTLD_NOW);
    return gles ? (retro_proc_address_t)dlsym(gles, name) : nullptr;
#else
    return (retro_proc_address_t)SDL_GL_GetProcAddress(name);
#endif
}

#ifndef LR_HOST_CLI
// The context the core asks for.  Must be set before the window is created:
// with EGL the window's config is chosen then.
static void gl_attributes(void)
{
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}
#endif

// Creates the GL context ('win' is the window to draw in, or null for a
// headless run) and the framebuffer the core renders into.
static bool gl_setup(SDL_Window **win, int max_w, int max_h)
{
#ifdef LR_HOST_CLI
    (void)win;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min, n = 0;
    EGLConfig cfg;
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */,
                               EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    if (!eglInitialize(d, &maj, &min) || !eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1)
    {
        hlog("EGL: no OpenGL ES 3 config (0x%x)\n", eglGetError());
        return false;
    }
    EGLSurface surf = eglCreatePbufferSurface(d, cfg, pb_attr);
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, surf, surf, ctx))
    {
        hlog("EGL: no context (0x%x)\n", eglGetError());
        return false;
    }
#else
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { hlog("SDL video: %s\n", SDL_GetError()); return false; }
    gl_attributes();
    if (!*win)
        *win = SDL_CreateWindow("lr-host", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    g_sdl_gl = *win ? SDL_GL_CreateContext(*win) : nullptr;
    if (!g_sdl_gl) { hlog("GL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(*win, g_sdl_gl);
    SDL_GL_SetSwapInterval(0);
#endif

    if (const char *missing = lr_glapi_load(&hgl, (lr_gl_proc (*)(const char *))host_gl_proc))
    {
        hlog("GL: no %s\n", missing);
        return false;
    }
    hgl.GenTextures(1, &g_fbo_tex);
    hgl.BindTexture(GL_TEXTURE_2D, g_fbo_tex);
    hgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    hgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    hgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    hgl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, max_w, max_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    hgl.BindTexture(GL_TEXTURE_2D, 0);
    hgl.GenFramebuffers(1, &g_fbo);
    hgl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    hgl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex, 0);
    const GLenum st = hgl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    hgl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { hlog("GL: framebuffer incomplete (0x%x)\n", st); return false; }
    hlog("GL: %s, %s\n", (const char *)hgl.GetString(GL_RENDERER), (const char *)hgl.GetString(GL_VERSION));
    return true;
}

// Copies the core's last hardware-rendered frame (GL or Vulkan) into g_fb
// (for --dump).
static void gl_readback(void)
{
    if (!g_hw || !g_hw_frame || g_fb_w <= 0) return;
    if (g_hw_vk)
    {
        if (!hostvk::readback(g_fb, g_fb_w, g_fb_h)) hlog("Vulkan: frame %ld could not be read back\n", g_frame);
        return;
    }
    std::vector<uint8_t> px((size_t)g_fb_w * g_fb_h * 4);
    hgl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    hgl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    hgl.ReadPixels(0, 0, g_fb_w, g_fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    hgl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g_fb.resize((size_t)g_fb_w * g_fb_h);
    for (int y = 0; y < g_fb_h; ++y)
    {
        const uint8_t *s = &px[(size_t)(g_fb_h - 1 - y) * g_fb_w * 4];
        uint32_t *d = &g_fb[(size_t)y * g_fb_w];
        for (int x = 0; x < g_fb_w; ++x, s += 4) d[x] = (uint32_t)s[0] << 16 | s[1] << 8 | s[2];
    }
}

// Shows the last frame in the window with GL.  Software frames (a core that
// fell back to its CPU renderer) go through the framebuffer texture first.
static void gl_present(SDL_Window *win, bool software, const char *shot)
{
    if (software && !g_fb.empty())
    {
        std::vector<uint32_t> rgba((size_t)g_fb_w * g_fb_h);
        for (int y = 0; y < g_fb_h; ++y)
            for (int x = 0; x < g_fb_w; ++x)
            {
                const uint32_t p = g_fb[(size_t)(g_fb_h - 1 - y) * g_fb_w + x];
                rgba[(size_t)y * g_fb_w + x] = (p >> 16 & 0xFF) | (p & 0xFF00) | (p & 0xFF) << 16 | 0xFF000000u;
            }
        hgl.BindTexture(GL_TEXTURE_2D, g_fbo_tex);
        hgl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        hgl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_fb_w, g_fb_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        hgl.BindTexture(GL_TEXTURE_2D, 0);
    }

    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    float dw = (float)ww, dh = dw / g_aspect;
    if (dh > wh) { dh = (float)wh; dw = dh * g_aspect; }
    const int x0 = (int)((ww - dw) / 2), y0 = (int)((wh - dh) / 2);

    hgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    hgl.Viewport(0, 0, ww, wh);
    hgl.ClearColor(0, 0, 0, 1);
    hgl.Clear(GL_COLOR_BUFFER_BIT);
    if (g_fb_w > 0)
    {
        hgl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        hgl.BlitFramebuffer(0, 0, g_fb_w, g_fb_h, x0, y0, x0 + (int)dw, y0 + (int)dh,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);
        hgl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    // --dump in a window: what is about to be shown, as a PNG
    if (shot)
    {
        std::vector<uint8_t> px((size_t)ww * wh * 4);
        hgl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        hgl.PixelStorei(GL_PACK_ALIGNMENT, 4);
        hgl.ReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<uint32_t> img((size_t)ww * wh);
        for (int y = 0; y < wh; ++y)
        {
            const uint8_t *s = &px[(size_t)(wh - 1 - y) * ww * 4];
            for (int x = 0; x < ww; ++x, s += 4) img[(size_t)y * ww + x] = (uint32_t)s[0] << 16 | s[1] << 8 | s[2];
        }
        SDL_Surface *surf = SDL_CreateSurfaceFrom(ww, wh, SDL_PIXELFORMAT_XRGB8888, img.data(), ww * 4);
        SDL_SavePNG(surf, shot);
        SDL_DestroySurface(surf);
    }
    SDL_GL_SwapWindow(win);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        for (auto *v = (const retro_variable *)data; v->key; ++v)
        {
            if (g_opts.count(v->key)) continue;
            const char *semi = strchr(v->value, ';');
            std::string first = semi ? semi + 1 : "";
            while (!first.empty() && first[0] == ' ') first.erase(0, 1);
            g_opts[v->key] = first.substr(0, first.find('|'));
        }
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto *v = (retro_variable *)data;
        auto it = g_opts.find(v->key);
        v->value = it == g_opts.end() ? nullptr : it->second.c_str();
        return v->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g_aspect = ((retro_game_geometry *)data)->aspect_ratio;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        g_shutdown = true;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
#ifdef __ANDROID__
        *(unsigned *)data = g_vulkan ? RETRO_HW_CONTEXT_VULKAN : RETRO_HW_CONTEXT_OPENGLES3;
#else
        *(unsigned *)data = g_vulkan ? RETRO_HW_CONTEXT_VULKAN : RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
        return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        if (!g_hw_vk) return false;
        *(const retro_hw_render_interface **)data = (const retro_hw_render_interface *)&hostvk::s_iface;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        auto *cb = (retro_hw_render_callback *)data;
        if (cb->context_type == RETRO_HW_CONTEXT_VULKAN)
        {
            if (!g_vulkan) return false;
            g_hw = cb;
            g_hw_vk = true;
            return true;
        }
        if (g_vulkan) return false;
#ifdef __ANDROID__
        const bool ok = cb->context_type == RETRO_HW_CONTEXT_OPENGLES3;
#else
        const bool ok = cb->context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
        if (g_no_gl || !ok) return false;
        cb->get_current_framebuffer = []() -> uintptr_t { return g_fbo; };
        cb->get_proc_address = host_gl_proc;
        g_hw = cb;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) { ++g_dupes; return; }
    g_fb_w = w;
    g_fb_h = h;
    g_hw_frame = data == RETRO_HW_FRAME_BUFFER_VALID;
    if (g_hw_frame) return;
    g_fb.resize((size_t)w * h);
    for (unsigned y = 0; y < h; ++y)
        memcpy(&g_fb[(size_t)y * w], (const uint8_t *)data + y * pitch, w * 4);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    if (g_audio) SDL_PutAudioStreamData(g_audio, data, (int)(frames * 4));
    if (!g_dump_dir.empty() || g_headless) g_wav.insert(g_wav.end(), data, data + frames * 2);
    return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16) return 0;
    for (const auto &p : g_presses)
        if (p.id == id && g_frame >= p.frame && g_frame < p.frame + p.len) return 1;
    return g_keys[id] || g_axis[id];
}

static bool button_id(const std::string &n, unsigned *id)
{
    static const std::map<std::string, unsigned> m = {
        {"b", RETRO_DEVICE_ID_JOYPAD_B}, {"y", RETRO_DEVICE_ID_JOYPAD_Y},
        {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"start", RETRO_DEVICE_ID_JOYPAD_START},
        {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {"a", RETRO_DEVICE_ID_JOYPAD_A}, {"x", RETRO_DEVICE_ID_JOYPAD_X},
        {"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R},
        {"l2", RETRO_DEVICE_ID_JOYPAD_L2}, {"r2", RETRO_DEVICE_ID_JOYPAD_R2},
        {"l3", RETRO_DEVICE_ID_JOYPAD_L3}, {"r3", RETRO_DEVICE_ID_JOYPAD_R3},
    };
    auto it = m.find(n);
    if (it == m.end()) return false;
    *id = it->second;
    return true;
}

static void write_wav(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t data_bytes = (uint32_t)(g_wav.size() * 2), rate = (uint32_t)g_rate;
    uint32_t riff = 36 + data_bytes, fmt_len = 16, byte_rate = rate * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(g_wav.data(), 2, g_wav.size(), f);
    fclose(f);
}

static void dump_frame(long frame)
{
    if (g_dump_dir.empty() || g_fb.empty()) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(g_fb_w, g_fb_h, SDL_PIXELFORMAT_XRGB8888,
                                           g_fb.data(), g_fb_w * 4);
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame%06ld.png", g_dump_dir.c_str(), frame);
    SDL_SavePNG(s, path);
    SDL_DestroySurface(s);
}

static void handle_key(SDL_Keycode k, bool down)
{
    static const std::pair<SDL_Keycode, unsigned> map[] = {
        {SDLK_UP, RETRO_DEVICE_ID_JOYPAD_UP}, {SDLK_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
        {SDLK_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT}, {SDLK_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {SDLK_Z, RETRO_DEVICE_ID_JOYPAD_B}, {SDLK_X, RETRO_DEVICE_ID_JOYPAD_A},
        {SDLK_C, RETRO_DEVICE_ID_JOYPAD_Y}, {SDLK_A, RETRO_DEVICE_ID_JOYPAD_X},
        {SDLK_S, RETRO_DEVICE_ID_JOYPAD_L}, {SDLK_D, RETRO_DEVICE_ID_JOYPAD_R},
        {SDLK_5, RETRO_DEVICE_ID_JOYPAD_SELECT}, {SDLK_1, RETRO_DEVICE_ID_JOYPAD_START},
        {SDLK_SPACE, RETRO_DEVICE_ID_JOYPAD_B},
    };
    for (auto &m : map)
        if (m.first == k) g_keys[m.second] = down;
}

// Waits until the next frame is due.  More than 100 ms behind (a stall),
// it starts counting again from now instead of running frames back to back
// to catch up.
static void pace(Uint64 &next, Uint64 frame_ns)
{
    next += frame_ns;
    const Uint64 now = SDL_GetTicksNS();
    if (next > now) SDL_DelayPrecise(next - now);
    else if (now - next > 100000000ull) next = now;
}

// Releases the framebuffer gl_setup() made, while its context is current.
static void gl_teardown(void)
{
    if (g_fbo) hgl.DeleteFramebuffers(1, &g_fbo);
    if (g_fbo_tex) hgl.DeleteTextures(1, &g_fbo_tex);
    g_fbo = g_fbo_tex = 0;
#ifndef LR_HOST_CLI
    if (g_sdl_gl) SDL_GL_DestroyContext(g_sdl_gl);
    g_sdl_gl = nullptr;
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        hlog("usage: lr-host <core> <content> [options]\n");
        return 2;
    }
    const char *core_path = argv[1], *content = argv[2];

    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--system") g_system_dir = next();
        else if (a == "--save") g_save_dir = next();
        else if (a == "--headless") g_headless = true;
        else if (a == "--realtime") g_realtime = true;
        else if (a == "--frames") g_max_frames = atol(next().c_str());
        else if (a == "--dump") g_dump_dir = next();
        else if (a == "--dump-every") g_dump_every = atol(next().c_str());
        else if (a == "--no-gl") g_no_gl = true;
        else if (a == "--vulkan") g_vulkan = true;
        else if (a == "--opt")
        {
            std::string kv = next();
            size_t eq = kv.find('=');
            if (eq != std::string::npos) g_opts[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (a == "--press")
        {
            std::string spec = next();
            Press p{0, 6, 0};
            char btn[32] = {0};
            int n = sscanf(spec.c_str(), "%ld:%31[^:]:%ld", &p.frame, btn, &p.len);
            if (n < 2 || !button_id(btn, &p.id)) { hlog("bad --press %s\n", spec.c_str()); return 2; }
            g_presses.push_back(p);
        }
        else { hlog("unknown option %s\n", a.c_str()); return 2; }
    }
    if (g_vulkan && !g_headless)
    {
        hlog("--vulkan needs --headless\n");
        return 2;
    }

    // absolute, as RetroArch passes them: a core may change directory (Singe
    // games run from their own folder)
    for (std::string *d : {&g_system_dir, &g_save_dir, &g_dump_dir})
    {
        std::error_code ec;
        if (!d->empty()) *d = std::filesystem::absolute(*d, ec).generic_string();
    }

#ifdef LR_HOST_CLI
    // no JVM in an adb-shell process: name the app so SDL never asks JNI
    SDL_SetMainReady();
    SDL_SetAppMetadata("lr-host", "1", "net.daphnelr.host");
#endif
    if (!SDL_Init(g_headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)))
    {
        hlog("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    core.lib = SDL_LoadObject(core_path);
    if (!core.lib) { hlog("load core: %s\n", SDL_GetError()); return 1; }
#define LOAD(n) core.n = (decltype(core.n))SDL_LoadFunction(core.lib, #n); \
    if (!core.n) { hlog("missing %s\n", #n); return 1; }
    LOAD(retro_init) LOAD(retro_deinit) LOAD(retro_api_version)
    LOAD(retro_get_system_info) LOAD(retro_get_system_av_info)
    LOAD(retro_set_environment) LOAD(retro_set_video_refresh)
    LOAD(retro_set_audio_sample) LOAD(retro_set_audio_sample_batch)
    LOAD(retro_set_input_poll) LOAD(retro_set_input_state)
    LOAD(retro_load_game) LOAD(retro_unload_game) LOAD(retro_run)
#undef LOAD

    core.retro_set_environment(environment);
    core.retro_set_video_refresh(video_refresh);
    core.retro_set_audio_sample_batch(audio_batch);
    core.retro_set_input_poll(input_poll);
    core.retro_set_input_state(input_state);
    core.retro_init();

    retro_system_info sys;
    core.retro_get_system_info(&sys);
    hlog("core: %s %s\n", sys.library_name, sys.library_version);

    // the core may ask for it while loading
    if (g_vulkan && !hostvk::setup()) return 1;

    retro_game_info game = {content, nullptr, 0, nullptr};
    Uint64 t_load = SDL_GetTicksNS();
    if (!core.retro_load_game(&game))
    {
        hlog("retro_load_game failed\n");
        return 1;
    }
    hlog("load: %.1f ms\n", (SDL_GetTicksNS() - t_load) / 1e6);

    retro_system_av_info av;
    core.retro_get_system_av_info(&av);
    g_fps = av.timing.fps;
    g_rate = av.timing.sample_rate;
    g_aspect = av.geometry.aspect_ratio;

    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    int tex_w = 0, tex_h = 0;

    if (!g_headless)
    {
        const SDL_WindowFlags gl_flag = g_hw ? SDL_WINDOW_OPENGL : 0;
#ifndef LR_HOST_CLI
        if (g_hw) gl_attributes();
#endif
#ifdef __ANDROID__
        win = SDL_CreateWindow("Daphne", 0, 0, SDL_WINDOW_FULLSCREEN | gl_flag);
#else
        win = SDL_CreateWindow("lr-host", 960, 720, SDL_WINDOW_RESIZABLE | gl_flag);
#endif
        if (!win) { hlog("window: %s\n", SDL_GetError()); return 1; }
        if (!g_hw)
        {
            ren = SDL_CreateRenderer(win, nullptr);
            if (!ren) { hlog("renderer: %s\n", SDL_GetError()); return 1; }
            SDL_SetRenderVSync(ren, 0);
        }
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)g_rate};
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        SDL_ResumeAudioStreamDevice(g_audio);
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; ++i) SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }

    if (g_hw && g_hw_vk)
    {
        // the Vulkan device was made before loading (hostvk::setup above)
        g_hw->context_reset();
    }
    else if (g_hw)
    {
        if (gl_setup(&win, av.geometry.max_width, av.geometry.max_height))
            g_hw->context_reset();
        else
        {
            // Without context_reset the core never gets its GL renderer
            // going and sends software frames instead.
            hlog("no OpenGL context for the core: it falls back to software frames\n");
            g_hw = nullptr;
            if (!g_headless && win)
            {
                ren = SDL_CreateRenderer(win, nullptr);
                if (!ren) { hlog("renderer: %s\n", SDL_GetError()); return 1; }
                SDL_SetRenderVSync(ren, 0);
            }
        }
    }

    // sized up front: growing it inside retro_run's audio callback would
    // show up as a slow frame
    if (g_headless && g_max_frames > 0)
        g_wav.reserve((size_t)((g_max_frames + 1) * (g_rate / g_fps + 1) * 2));

    const Uint64 frame_ns = (Uint64)(1e9 / g_fps);
    Uint64 next = SDL_GetTicksNS(), run_ns = 0, worst_ns = 0;
    long over_budget = 0;
    bool quit = false;

    while (!quit && !g_shutdown && (g_max_frames < 0 || g_frame < g_max_frames))
    {
        if (!g_headless)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT) quit = true;
                else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)
                {
                    if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) quit = true;
                    handle_key(e.key.key, e.type == SDL_EVENT_KEY_DOWN);
                }
                else if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                    SDL_OpenGamepad(e.gdevice.which);
                else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
                {
                    // triggers -> L2/R2, left stick -> d-pad
                    const int v = e.gaxis.value, t = 16000;
                    switch (e.gaxis.axis)
                    {
                    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  g_axis[RETRO_DEVICE_ID_JOYPAD_L2] = v > t; break;
                    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: g_axis[RETRO_DEVICE_ID_JOYPAD_R2] = v > t; break;
                    case SDL_GAMEPAD_AXIS_LEFTX:
                        g_axis[RETRO_DEVICE_ID_JOYPAD_LEFT]  = v < -t;
                        g_axis[RETRO_DEVICE_ID_JOYPAD_RIGHT] = v > t;
                        break;
                    case SDL_GAMEPAD_AXIS_LEFTY:
                        g_axis[RETRO_DEVICE_ID_JOYPAD_UP]   = v < -t;
                        g_axis[RETRO_DEVICE_ID_JOYPAD_DOWN] = v > t;
                        break;
                    default: break;
                    }
                }
                else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
                {
                    // indexed by SDL_GamepadButton; NONE for SDL_GAMEPAD_BUTTON_GUIDE
                    static const unsigned NONE = ~0u;
                    static const unsigned m[] = {
                        RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
                        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_SELECT, NONE,
                        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L3,
                        RETRO_DEVICE_ID_JOYPAD_R3, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
                        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT};
                    if (e.gbutton.button < (int)(sizeof(m) / sizeof(m[0])) && m[e.gbutton.button] < 16)
                        g_keys[m[e.gbutton.button]] = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                }
            }

            // hold Select+Start for one second to exit (back to the launcher)
            static Uint64 combo_since = 0;
            if (g_keys[RETRO_DEVICE_ID_JOYPAD_SELECT] && g_keys[RETRO_DEVICE_ID_JOYPAD_START])
            {
                if (!combo_since) combo_since = SDL_GetTicks();
                else if (SDL_GetTicks() - combo_since > 1000) quit = true;
            }
            else combo_since = 0;
        }

        Uint64 t0 = SDL_GetTicksNS();
        core.retro_run();
        Uint64 dt = SDL_GetTicksNS() - t0;
        run_ns += dt;
        if (g_frame > 10 && dt > worst_ns) worst_ns = dt;
        if (g_frame > 10 && dt > frame_ns) ++over_budget;

        // A frontend's buffer swap waits for the GPU; outside the timing above.
        if (g_hw && g_hw_vk) hostvk::end_frame();
        else if (g_hw && g_headless) hgl.Finish();

        if (g_headless && g_dump_every > 0 && g_frame % g_dump_every == 0)
        {
            gl_readback();
            dump_frame(g_frame);
        }

        if (g_headless && g_realtime) pace(next, frame_ns);

        if (!g_headless && g_hw)
        {
            std::string shot;
            if (!g_dump_dir.empty() && g_dump_every > 0 && g_frame % g_dump_every == 0)
            {
                char name[64];
                snprintf(name, sizeof(name), "/window%06ld.png", g_frame);
                shot = g_dump_dir + name;
            }
            gl_present(win, !g_hw_frame, shot.empty() ? nullptr : shot.c_str());
            pace(next, frame_ns);
        }
        else if (!g_headless && !g_fb.empty())
        {
            if (!tex || tex_w != g_fb_w || tex_h != g_fb_h)
            {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, g_fb_w, g_fb_h);
                tex_w = g_fb_w;
                tex_h = g_fb_h;
            }
            SDL_UpdateTexture(tex, nullptr, g_fb.data(), g_fb_w * 4);
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            float dw = (float)ww, dh = dw / g_aspect;
            if (dh > wh) { dh = (float)wh; dw = dh * g_aspect; }
            SDL_FRect dst = {(ww - dw) / 2, (wh - dh) / 2, dw, dh};
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, nullptr, &dst);
            SDL_RenderPresent(ren);
            pace(next, frame_ns);
        }
        ++g_frame;
    }

    hlog("frames: %ld  avg retro_run: %.2f ms  worst: %.2f ms  over %.1f ms: %ld  repeated: %ld\n",
            g_frame, g_frame ? run_ns / 1e6 / g_frame : 0.0, worst_ns / 1e6, frame_ns / 1e6,
            over_budget, g_dupes);

    // Audio level per second, to spot silence or dropouts.
    if (!g_wav.empty())
    {
        const size_t per_sec = (size_t)g_rate * 2;
        hlog("audio rms/sec:");
        for (size_t s = 0; s < g_wav.size(); s += per_sec)
        {
            double sum = 0;
            size_t e = std::min(g_wav.size(), s + per_sec);
            for (size_t i = s; i < e; ++i) sum += (double)g_wav[i] * g_wav[i];
            hlog(" %d", (int)sqrt(sum / (e - s)));
        }
        hlog("\n");
        if (!g_dump_dir.empty()) write_wav(g_dump_dir + "/audio.wav");
    }
    if (g_headless)
    {
        gl_readback();
        dump_frame(g_frame);
    }

    // RetroArch's order: the context goes before the game is unloaded
    if (g_hw && g_hw->context_destroy) g_hw->context_destroy();
    core.retro_unload_game();
    if (g_hw_vk) hostvk::shutdown();
    else if (g_fbo) gl_teardown();
    core.retro_deinit();
    SDL_UnloadObject(core.lib);
    SDL_Quit();
    return 0;
}
