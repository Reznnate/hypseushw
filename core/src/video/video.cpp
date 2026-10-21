/*
 * ____ HYPSEUS COPYRIGHT NOTICE ____
 *
 * Copyright (C) 2001 Matt Ownby, 2026 DirtBagXon
 *
 * This file is part of HYPSEUS, a laserdisc arcade game emulator
 *
 * HYPSEUS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * HYPSEUS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "../hypseus.h"
#include "../game/game.h"
#include "../io/conout.h"
#include "../io/error.h"
#include "../io/homedir.h"
#include "../io/mpo_fileio.h"
#include "../io/mpo_mem.h"
#include "icon.h"
#include "video.h"
#include <SDL3_image/SDL_image.h>
#include <plog/Log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#ifdef __LIBRETRO__
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#endif

namespace video {

uint64_t g_vflags                       = VSYNC | INTRO;

// default video dimensions
int g_video_width                       = 640;
int g_video_height                      = 480;

int g_viewport_width                    = g_video_width;
int g_viewport_height                   = g_video_height;
int g_aspect_width                      = 0;
int g_aspect_height                     = 0;
int g_alloc_screen                      = 0;
int g_displays_total                    = 0;
int g_display                           = 0;
int g_scale_h_shift                     = 100;
int g_scale_v_shift                     = 100;
int g_scoreboard_screen                 = -1;
int aux_bezel_pos_x                     = 0;
int aux_bezel_pos_y                     = 0;
int scoreboard_window_pos_x             = 0;
int scoreboard_window_pos_y             = 0;

const int g_anun_w                      = 220;
const int g_anun_h                      = 128;
const int g_scoreboard_w                = 340;
const int g_scoreboard_h                = 480;

// the current game overlay dimensions
unsigned int g_overlay_width            = 0;
unsigned int g_overlay_height           = 0;
unsigned int g_probe_width              = g_video_width;
unsigned int g_probe_height             = g_video_height;

TTF_Font *g_font                       = nullptr;
TTF_Font *g_ttfont                     = nullptr;
TTF_TextEngine *g_font_engine          = nullptr;

SDL_Window *g_window                   = NULL;
SDL_Window *g_scoreboard_window        = NULL;

SDL_Renderer *g_renderer               = NULL;
SDL_Renderer *g_scoreboard_renderer    = NULL;

SDL_Surface *g_led_bmps[LED_RANGE]     = {0};
SDL_Surface *g_other_bmps[B_EMPTY]     = {0};
SDL_Surface *g_overlay_surface         = NULL;
#ifdef __LIBRETRO__
// Writes to g_overlay_surface set s_ov_dirty.  Once a game's overlay arrives
// through vid_update_overlay_surface (Singe's full-size RGBA overlays),
// lr_frame_begin() trusts the flag instead of comparing every pixel each frame.
static bool s_ov_dirty = false;
static bool s_ov_tracked = false;
// The game overlays g_overlay_surface currently equals (games double-buffer
// them; emptied once anything else draws on it), and the one the game said
// it left unchanged since it was last passed in.
static SDL_Surface *s_ov_mirrors[4] = {NULL};
static SDL_Surface *s_ov_same = NULL;
static int s_ov_mirror_dx = 0, s_ov_mirror_dy = 0;
static void ov_mirrors_clear() { for (auto &m : s_ov_mirrors) m = NULL; }
static bool ov_mirrors_has(SDL_Surface *s)
{
    for (auto m : s_ov_mirrors) if (m == s) return true;
    return false;
}
static void ov_mirrors_add(SDL_Surface *s)
{
    if (ov_mirrors_has(s)) return;
    for (auto &m : s_ov_mirrors) if (!m) { m = s; return; }
    s_ov_mirrors[0] = s;
}
#endif
SDL_Surface *g_scoreboard_surface      = NULL;
SDL_Surface *g_aux_blit_surface        = NULL;
SDL_Surface *g_scoreboard_blit_surface = NULL;

SDL_Texture *g_overlay_texture         = NULL;
SDL_Texture *g_yuv_texture             = NULL;
SDL_Texture *g_scoreboard_texture      = NULL;
SDL_Texture *g_bezel_texture           = NULL;
SDL_Texture *g_aux_texture             = NULL;

SDL_FRect fScaleRect;
SDL_FRect fOverlayRect;
SDL_FRect *g_yuv_frect[2]              = {NULL};

SDL_Rect g_aux_rect;
SDL_Rect g_bezel_rect;
SDL_Rect g_border_rect;
SDL_Rect g_scaling_rect                = {0};
SDL_Rect g_logical_rect                = {0};
SDL_Rect g_scoreboard_bezel_rect       = {0};
SDL_Rect g_local_size_rect             = {0, 0, 320, 240};
SDL_Rect g_limit_rect                  = g_local_size_rect;
SDL_Rect g_annu_rect                   = g_local_size_rect;

int g_aspect_ratio                     = 0;
int g_scalefactor                      = 100;
int g_scoreboard_bezel_scale           = 14;
int g_aux_bezel_scale                  = 12;
int g_bezel_scalewidth                 = 0;
int g_yuv_display                      = YUV_VISIBLE;
int sboverlay_characterset             = 2;

int8_t g_rescale                       = 0;
int8_t g_yuv_luma                      = YUV_FLAG_LUMA;

uint64_t g_bezelhash                   = 0;

uint8_t g_scanline_alpha               = 128;
uint8_t g_scanline_shunt               = 2;
uint8_t g_bSubtitleShown               = 0;
uint8_t g_yuv_flags                    = 0;

char g_window_title[TITLE_LENGTH]      = "";
char *subchar                          = NULL;
char *srtchar                          = NULL;

SDL_TextureAccess g_texture_access     = SDL_TEXTUREACCESS_STREAMING;

std::string g_bezel_path               = "bezels";
std::string g_bezel_file;
std::string g_tqkeys;

// degrees in clockwise rotation
float g_fRotateDegrees                 = 0.0f;
double g_aux_ratio                     = 1.0f;

// YUV stretching
float g_yuv_scale[2]                   = {1.0f, 1.0f};

// YUV structure
typedef struct {
    uint8_t *Yplane;
    uint8_t *Uplane;
    uint8_t *Vplane;
    int width, height;
    int Ysize, Usize, Vsize;           // The size of each plane in bytes.
    int Ypitch, Upitch, Vpitch;        // The pitch of each plane in bytes.
    SDL_Mutex *mutex;
} m_yuv_surface_t;

// texture size caching
typedef struct {
    SDL_Texture *t;
    int w, h;
} m_textureT;

m_yuv_surface_t                        *g_yuv_surface;
std::vector<SDL_Rect>                  displayDimensions;

#ifdef __LIBRETRO__
// g_yuv_surface receives frames as VLDP prepares them; g_yuv_visible holds the
// frame VLDP has declared due (display_frame), which is what gets composited.
static m_yuv_surface_t                 *g_yuv_visible = NULL;
static bool                            g_lr_yuv_dirty = false;
// VLDP re-creates the YUV surfaces from its own thread when it learns the
// MPEG size; this guards (re)allocation against the frontend compositing.
static std::recursive_mutex            g_lr_yuv_alloc;
static SDL_Surface                     *g_console_font = NULL;
#endif

m_textureT g_mix_texture               = {.t = NULL, .w = 0, .h = 0 };

// blitting flags
bool g_scoreboard_needs_update         = false;
bool g_yuv_video_needs_update          = false;
bool g_aux_needs_update                = false;
bool g_yuv_skip                        = true;

//////////////////////////////////////////////////////////////////////////////

static void ConvertSurface(SDL_Surface **surface, SDL_PixelFormat fmt)
{
    SDL_Surface *tmpSurface = SDL_ConvertSurface(*surface, fmt);
    SDL_DestroySurface(*surface);
    *surface = tmpSurface;
}

static void LogicalPosition(SDL_Rect *port, SDL_Rect *dst, int x, int y)
{
    SDL_Rect tmpRect = {0, 0, dst->w, dst->h};
    tmpRect.x = (((port->w - dst->w) * x) / 100);
    tmpRect.y = (((port->h - dst->h) * y) / 100);

    *dst = tmpRect;
}

static inline void copyPlane(uint8_t *dst, const uint8_t *src, int w, int h, int srcPitch)
{
    if (srcPitch == w)
    {
        memcpy(dst, src, (size_t)w * h);
    } else {
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * w, src + (size_t)y * srcPitch, w);
    }
}

static void calcAuxRect()
{
     double scale = 9.0f - double((g_aux_bezel_scale << 1) / 10.0f);
     g_aux_rect.w = (g_bezel_scalewidth / scale);
     g_aux_rect.h = (g_aux_rect.w * g_aux_ratio);
}

static uint64_t calcHash(const void* data, size_t size)
{
    if (size == 0) return 0;

    const auto* bytes = static_cast<const uint8_t*>(data);

    uint64_t hash = 14695981039346656037ull;

    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static void resize_cleanup()
{
    VIDEO_CLEAR(BEZEL_LOAD);

    SDL_DestroySurface(g_overlay_surface);
    SDL_DestroySurface(g_aux_blit_surface);

    g_overlay_surface = NULL;
    g_aux_blit_surface = NULL;

    SDL_DestroyTexture(g_bezel_texture);
    SDL_DestroyTexture(g_aux_texture);
    SDL_DestroyTexture(g_scoreboard_texture);

    g_bezel_texture = NULL;
    g_aux_texture = NULL;
    g_scoreboard_texture = NULL;

    TTF_CloseFont(g_font);
    TTF_CloseFont(g_ttfont);
    TTF_DestroyRendererTextEngine(g_font_engine);

    g_font = nullptr;
    g_ttfont = nullptr;
    g_font_engine = nullptr;

    SDL_DestroyTexture(g_overlay_texture);
    SDL_DestroyTexture(g_mix_texture.t);

    if (g_renderer)
    {
        SDL_RenderClear(g_renderer);
        SDL_RenderPresent(g_renderer);
        SDL_DestroyRenderer(g_renderer);
    }

    g_mix_texture.t = NULL;
    g_overlay_texture = NULL;
    g_renderer = NULL;

    SDL_Delay(100);
}

static void load_fonts()
{
    if (g_font)
    {
       TTF_CloseFont(g_font);
       g_font = nullptr;
    }

    if (g_ttfont)
    {
        TTF_CloseFont(g_ttfont);
        g_ttfont = nullptr;
    }

    if (g_font_engine)
    {
        TTF_DestroyRendererTextEngine(g_font_engine);
        g_font_engine = nullptr;
    }

    g_font_engine = TTF_CreateRendererTextEngine(g_renderer);

    int fs = g_scaling_rect.w / ((g_aspect_ratio == ASPECTPD) ? 24 : 36);
    const char *base = "fonts/default.ttf";

    const char *ttfont = VIDEO_HAS(LEGACY_OVERLAY) ? "fonts/daphne.ttf" :
        (VIDEO_HAS(LUA_GAME) ? "fonts/daphne.ttf" : "fonts/digital.ttf");

    int size = VIDEO_HAS(LEGACY_OVERLAY) ? 12 : (VIDEO_HAS(LUA_GAME) ? 12 : 6);

    g_font = TTF_OpenFont(base, fs);
    g_ttfont = TTF_OpenFont(ttfont, size);

    TTF_SetFontStyle(g_font, TTF_STYLE_NORMAL);

    if (!g_font)
    {
        LOG_ERROR << fmt("Cannot load TTF font: %s", base);
        set_quitflag();
    }

    if (!g_ttfont)
    {
        LOG_ERROR << fmt("Cannot load TTF font: '%s'", (char*)ttfont);
        set_quitflag();
    }
}

static void format_window_render()
{
    g_scaling_rect.w = (g_viewport_width * g_scalefactor) / 100;
    g_scaling_rect.h = (g_viewport_height * g_scalefactor) / 100;

    float diff_w = (g_viewport_width - g_scaling_rect.w) >> 1;
    float diff_h = (g_viewport_height - g_scaling_rect.h) >> 1;

    g_scaling_rect.x = (diff_w * g_scale_h_shift) / 100;
    g_scaling_rect.y = (diff_h * g_scale_v_shift) / 100;

    g_logical_rect.w = g_mix_texture.w = g_scaling_rect.w;
    g_logical_rect.h = g_mix_texture.h = g_scaling_rect.h;

    if (VIDEO_HAS(BEZEL_OVERLAY)) VIDEO_CLEAR(OVERLAY_LAST);

    if (!g_rescale)
    {
        g_border_rect = (SDL_Rect){0, 0, g_viewport_width, g_viewport_height};
        load_fonts();
    }
}

static void format_fullscreen_render()
{
    int w, h;
    const double ratio = static_cast<double>(g_viewport_width) / g_viewport_height;

    if (VIDEO_HAS(BEZEL_OVERLAY)) VIDEO_SET(OVERLAY_LAST);

    g_bezel_rect = SDL_Rect{0, 0, g_logical_rect.w, g_logical_rect.h};

    if (VIDEO_HAS(RESIZED_MEDIA))
    {
        w = g_viewport_width;
        h = g_viewport_height;
    }
    else
    {
        h = VIDEO_HAS(VERTICAL_ORIENTATION) ? g_logical_rect.w : g_logical_rect.h;

        switch (g_aspect_ratio)
        {
            case ASPECTWS:  w = (h * 16) / 9; break;
            case ASPECTSD:  w = (h * 4) / 3; break;
            case ASPECTPD:
                if (VIDEO_HAS(VERTICAL_ORIENTATION)) h = static_cast<int>(h * 1.33);
                w = (h * 3) / 4;
                break;
            default: w = static_cast<int>(h * ratio); break;
        }
    }

    if (VIDEO_HAS(PRESERVE_ASPECT))
    {
        const double video_ratio = static_cast<double>(w) / h;
        const double screen_ratio = static_cast<double>(g_logical_rect.w) / g_logical_rect.h;

        if (video_ratio > screen_ratio)
        {
            w = g_logical_rect.w;
            h = static_cast<int>(w / video_ratio);
        }
        else
        {
            h = g_logical_rect.h;
            w = static_cast<int>(h * video_ratio);
        }
    }
    else
        w = VIDEO_HAS(FORCE_ASPECT) ? (h * 4) / 3 : (VIDEO_HAS(IGNORE_ASPECT) ?
                                 static_cast<int>(h * ratio) : w);

    g_scaling_rect.w = (w * g_scalefactor) / 100;
    g_scaling_rect.h = (h * g_scalefactor) / 100;
    g_scaling_rect.x = (g_logical_rect.w - g_scaling_rect.w) >> 1;
    g_scaling_rect.y = (g_logical_rect.h - g_scaling_rect.h) >> 1;

    if (VIDEO_HAS(KEYBOARD_BEZEL) && !VIDEO_HAS(SCALED))
    {
        g_scaling_rect.y /= 4;
        g_scale_v_shift = 0;
    }
    else
    {
        g_scaling_rect.x = (g_scaling_rect.x * g_scale_h_shift) / 100;
        g_scaling_rect.y = (g_scaling_rect.y * g_scale_v_shift) / 100;
    }

    if (VIDEO_HAS(SCOREBOARD_BEZEL))
    {
        g_bezel_scalewidth = w;
        double bezel_ratio = static_cast<double>(g_scoreboard_h) / g_scoreboard_w;
        double scale = 9.0 - (static_cast<double>(g_scoreboard_bezel_scale) * 2.0 / 10.0);

        g_scoreboard_bezel_rect = {scoreboard_window_pos_x, scoreboard_window_pos_y,
                           static_cast<int>(g_bezel_scalewidth / scale),
                           static_cast<int>((g_bezel_scalewidth / scale) * bezel_ratio)};
        VIDEO_SET(BEZEL_TOGGLE);
    }

    if (VIDEO_HAS(PRESERVE_ASPECT) && g_aspect_ratio == ASPECTWS)
    {
        if (g_display == 0)
            g_bezel_rect = g_scaling_rect;
        else
            VIDEO_CLEAR(BEZEL_TOGGLE);
    }

    if (!g_rescale)
    {
        g_border_rect = {
            (g_logical_rect.w - w) / 2,
            (g_logical_rect.h - h) / 2,
            w, h
        };
        load_fonts();
    }
}

static bool draw_ranks()
{
    if (VIDEO_HAS(ANNUN_LAMPS)) return false;

    SDL_Rect dest;
    dest.x = 10;
    dest.y = dest.x - 1;

    for (int i = B_ACE_SPACE; i < B_MIA; i++)
    {

        g_scoreboard_surface = g_other_bmps[i];

        dest.w = (uint16_t)g_scoreboard_surface->w;
        dest.h = (uint16_t)g_scoreboard_surface->h;
        SDL_BlitSurface(g_scoreboard_surface, NULL, g_aux_blit_surface, &dest);
        dest.y = dest.y + (ANUN_CHAR_HEIGHT << 1) + (ANUN_CHAR_HEIGHT);
    }

    return true;
}

#ifdef __LIBRETRO__
// The frontend owns the window: only set up the surfaces the engine draws into.
static bool lr_init_display()
{
    g_overlay_width = g_game->get_video_overlay_width();
    g_overlay_height = g_game->get_video_overlay_height();

    g_probe_width = std::max((int)g_probe_width, 320);
    g_probe_height = std::max((int)g_probe_height, 240);
    g_viewport_width = g_probe_width;
    g_viewport_height = g_probe_height;
    g_scaling_rect = g_logical_rect = {0, 0, g_viewport_width, g_viewport_height};

    if (g_game->m_software_scoreboard && !g_scoreboard_blit_surface)
        g_scoreboard_blit_surface = SDL_CreateSurface(g_scoreboard_w, g_scoreboard_h,
                                                      SDL_PIXELFORMAT_RGBA32);

    if (!g_overlay_surface)
        g_overlay_surface = SDL_CreateSurface(g_overlay_width, g_overlay_height,
                                              SDL_PIXELFORMAT_RGBA32);
    if (!g_overlay_surface)
    {
        LOGE << fmt("Could not initialize g_overlay_surface: %s", SDL_GetError());
        return false;
    }

    VIDEO_ASSIGN(ENHANCE_OVERLAY, g_game->has_overlay_upgrade(GAME_OVERLAY_UPGRADE));
    VIDEO_ASSIGN(OVERLAY_DYNAMIC, g_game->get_dynamic_overlay());

    ConvertSurface(&g_other_bmps[B_OVERLAY_LEDS], g_overlay_surface->format);
    Uint32 colorkey = SDL_MapSurfaceRGB(g_other_bmps[B_OVERLAY_LEDS], 0, 0, 0);
    SDL_SetSurfaceColorKey(g_other_bmps[B_OVERLAY_LEDS], true, colorkey);

    ConvertSurface(&g_other_bmps[B_OVERLAY_LDP1450], g_overlay_surface->format);
    colorkey = SDL_MapSurfaceRGB(g_other_bmps[B_OVERLAY_LDP1450], 0, 0, 0);
    SDL_SetSurfaceColorKey(g_other_bmps[B_OVERLAY_LDP1450], true, colorkey);

    VIDEO_ASSIGN(INDEX8, (g_game->get_overlay_depth() == GAME_OVERLAY_STD) ? true : false);
    VIDEO_SET(INITPASS);
    return true;
}
#endif

// Initialize the display systems
bool init_display()
{
#ifdef __LIBRETRO__
    return lr_init_display();
#endif
    bool result = false;
    constexpr char title[] = "HYPSEUS Singe: Multiple Arcade Laserdisc Emulator";

    Uint32 sdl_flags = 0;
    Uint32 sdl_scoreboard_flags = SDL_WINDOW_BORDERLESS;
    const char *driver = SDL_GetCurrentVideoDriver();
    const char *sdl_render_flags = NULL;

    if (driver && strcmp(driver, "kmsdrm") == 0)
        VIDEO_SET(KMSDRM);

    if (VIDEO_HAS(FORCE_TOP))
        sdl_flags |= SDL_WINDOW_ALWAYS_ON_TOP;

    if (VIDEO_HAS(OPENGL))
    {
        sdl_flags |= SDL_WINDOW_OPENGL;
        sdl_scoreboard_flags |= SDL_WINDOW_OPENGL;
    }
    else if (VIDEO_HAS(VULKAN))
    {
        sdl_flags |= SDL_WINDOW_VULKAN;
        sdl_scoreboard_flags |= SDL_WINDOW_VULKAN;
    }

    g_overlay_width = g_game->get_video_overlay_width();
    g_overlay_height = g_game->get_video_overlay_height();

    // Enforce a minimum window size
    g_probe_width = std::max((int)g_probe_width, 320);
    g_probe_height = std::max((int)g_probe_height, 240);

    if (VIDEO_HAS(RESIZED_MEDIA))
    {
        g_viewport_width  = g_video_width;
        g_viewport_height = g_video_height;

    } else {

        if (!VIDEO_HAS(IGNORE_ASPECT) && g_aspect_width > 0)
        {
            g_viewport_width  = g_aspect_width;
            g_viewport_height = g_aspect_height;
        }
        else
        {
            g_viewport_width  = g_probe_width;
            g_viewport_height = g_probe_height;
        }
    }

    // Enforce 4:3 aspect ratio
    if (VIDEO_HAS(FORCE_ASPECT))
    {
        double dCurAspect = (double)g_viewport_width / g_viewport_height;
        const double dTARGET_ASPECT_RATIO = 4.0 / 3.0;

        if (dCurAspect < dTARGET_ASPECT_RATIO)
            g_viewport_height = (g_viewport_width * 3) / 4;
        else if (dCurAspect > dTARGET_ASPECT_RATIO)
            g_viewport_width = (g_viewport_height * 4) / 3;
    }

    if (g_window) resize_cleanup();

    if (g_fRotateDegrees != 0 && g_fRotateDegrees != 180)
    {
        switch(g_aspect_ratio)
        {
        case ASPECTWS:
            g_scalefactor = (!VIDEO_HAS(SCALED)) ? 56 : g_scalefactor;
            break;
        case ASPECTPD:
            g_scalefactor = (!VIDEO_HAS(SCALED)) ? 100 : g_scalefactor;
            break;
        default:
            g_scalefactor = (!VIDEO_HAS(SCALED)) ? 75 : g_scalefactor;
            break;
        }
    }

    SDL_DisplayID *id = SDL_GetDisplays(&g_displays_total);
    displayDimensions.resize(g_displays_total);

    for (int i = 0; i < g_displays_total; i++)
        SDL_GetDisplayBounds(id[i], &displayDimensions[i]);

    SDL_free(id);

    if (g_alloc_screen > g_display && g_alloc_screen < g_displays_total)
        g_display = g_alloc_screen;

    if (VIDEO_HAS(INITPASS) && g_window && !VIDEO_HAS(TEARDOWN))
    {
        SDL_SetWindowSize(g_window, g_viewport_width, g_viewport_height);
    }
    else
    {
        if (VIDEO_HAS(TEARDOWN) && g_window)
        {
            SDL_DestroyWindow(g_window);
            SDL_Delay(40);
        }

        g_window = SDL_CreateWindow(title, g_viewport_width, g_viewport_height,
                     sdl_flags | (VIDEO_HAS(KMSDRM) ? 0 : SDL_WINDOW_HIDDEN));
    }

    if (!g_window)
    {
        LOGE << fmt("Could not initialize window: %s", SDL_GetError());
        set_quitflag();
        goto exit;
    }
    else
    {
        // Check for KMSDRM
        if (VIDEO_HAS(KMSDRM) && (!VIDEO_HAS(INITPASS) || VIDEO_HAS(TEARDOWN)))
        {
            int count = 0;
            SDL_DisplayMode *selected = NULL;
            SDL_DisplayID display = SDL_GetPrimaryDisplay();
            SDL_DisplayID winId = SDL_GetDisplayForWindow(g_window);
            SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(winId, &count);

            const SDL_DisplayMode *current = SDL_GetCurrentDisplayMode(display);

            if (!modes || count == 0)
            {
                LOGE << fmt("KMSDRM: SDL_GetFullscreenDisplayModes failed: %s", SDL_GetError());
                set_quitflag();
                goto exit;
            }

            for (int i = 0; i < count; i++)
            {
                SDL_DisplayMode *m = modes[i];

                if (m->w == displayDimensions[g_display].w &&
                    m->h == displayDimensions[g_display].h &&
                    (current ? m->refresh_rate == current->refresh_rate : false))
                {
                    selected = m;
                    break;
                }
            }

            if (!selected)
            {
                for (int i = 0; i < count; i++)
                {
                    SDL_DisplayMode *m = modes[i];

                    if (m->w == displayDimensions[g_display].w &&
                        m->h == displayDimensions[g_display].h)
                    {
                        if (!selected || m->refresh_rate > selected->refresh_rate)
                            selected = m;
                    }
                }
                LOGW << fmt("KMSDRM: %dx%d %.2f Hz not available, using highest mode detected.",
                             displayDimensions[g_display].w, displayDimensions[g_display].h,
                                 (current ? current->refresh_rate : 0.0f));
            }

            if (!SDL_SetWindowFullscreenMode(g_window, selected))
            {
                LOGE << fmt("KMSDRM: Fullscreen mode failed: %s\n", SDL_GetError());
                SDL_free(modes);
                set_quitflag();
                goto exit;
            }

            LOGI << fmt("KMSDRM Mode: %dx%d, %.2f Hz", selected->w, selected->h, selected->refresh_rate);
            SDL_free(modes);
        }

        if (VIDEO_HAS(FULLSCREEN))
        {
            SDL_SetWindowFullscreen(g_window, true);

            if (VIDEO_HAS(KMSDRM) && !VIDEO_HAS(INITPASS))
                SDL_SyncWindow(g_window);
        }

        if (strlen(g_window_title) > 0)
            SDL_SetWindowTitle(g_window, g_window_title);

        SDL_IOStream* ops = SDL_IOFromConstMem(ghci, sizeof(ghci));
        SDL_Surface *rep = IMG_Load_IO(ops, true);

        if (rep != NULL)
        {
            SDL_SetWindowIcon(g_window, rep);
            SDL_DestroySurface(rep);
        }

        if (VIDEO_HAS(SOFT_RENDER)) sdl_render_flags = SDL_SOFTWARE_RENDERER;

        g_renderer = SDL_CreateRenderer(g_window, sdl_render_flags);
        SDL_SetRenderVSync(g_renderer, VIDEO_HAS(VSYNC) ?
            SDL_RENDERER_VSYNC_ADAPTIVE : SDL_RENDERER_VSYNC_DISABLED);

        SDL_SetWindowPosition(g_window, displayDimensions[g_display].x +
                ((displayDimensions[g_display].w - g_viewport_width) >> 1),
                    displayDimensions[g_display].y +
                        ((displayDimensions[g_display].h - g_viewport_height) >> 1));

        SDL_ShowWindow(g_window);
        SDL_RaiseWindow(g_window);

        if (!g_renderer)
        {
            LOGE << fmt("Could not initialize renderer: %s", SDL_GetError());
            set_quitflag();
            goto exit;
        }
        else
        {
            if (VIDEO_HAS(KEYBOARD_BEZEL))
            {
                g_aux_needs_update = true;
                g_tqkeys = g_bezel_path + "/tqkeys.png";

                if (!mpo_file_exists(g_tqkeys.c_str()))
                    g_tqkeys = "pics/tqkeys.png";
            }

            VIDEO_ASSIGN(LUA_GAME, (g_game->get_game_type() == GAME_SINGE));

            if (VIDEO_HAS(FULLSCREEN))
            {
                g_logical_rect = displayDimensions[g_display];

                if (!g_bezel_file.empty())
                    VIDEO_SET(BEZEL_TOGGLE);

                format_fullscreen_render();
            }
            else format_window_render();

            if (g_game->m_software_scoreboard)
            {
                if (!g_scoreboard_blit_surface)
                    g_scoreboard_blit_surface = SDL_CreateSurface( g_scoreboard_w,
                                                   g_scoreboard_h, SDL_PIXELFORMAT_RGBA32);

                if (!g_scoreboard_blit_surface)
                {
                    LOGE << fmt("Could not initialize g_scoreboard_blit_surface: %s", SDL_GetError());
                    set_quitflag();
                    goto exit;
                }

                if (!VIDEO_HAS(SCOREBOARD_BEZEL) && !g_scoreboard_window)
                {
                    double scale = double((g_scoreboard_bezel_scale >> 1) / 7.0f);

                    g_scoreboard_window = SDL_CreateWindow(NULL,
                        (scale * g_scoreboard_w), (scale * g_scoreboard_h),
                            sdl_scoreboard_flags | SDL_WINDOW_HIDDEN);

                    if (!g_scoreboard_window)
                    {
                        LOGE << fmt("Could not initialize scoreboard window: %s", SDL_GetError());
                        set_quitflag();
                        goto exit;
                    }

                    if (g_displays_total > 1)
                    {
                        int screen;

                        if (g_scoreboard_screen != -1)
                        {
                            screen = g_scoreboard_screen;
                        }
                        else if (g_display == 0)
                        {
                            screen = 1;
                        }
                        else
                        {
                            screen = g_display;
                        }

                        SDL_SetWindowPosition(g_scoreboard_window,
                           displayDimensions[screen].x + scoreboard_window_pos_x,
                              displayDimensions[screen].y + scoreboard_window_pos_y);
                    } else
                        SDL_SetWindowPosition(g_scoreboard_window, scoreboard_window_pos_x,
                                                  scoreboard_window_pos_y);

                    SDL_ShowWindow(g_scoreboard_window);
                    SDL_RaiseWindow(g_scoreboard_window);

                    g_scoreboard_renderer = SDL_CreateRenderer(g_scoreboard_window,
                                                sdl_render_flags);
                    SDL_SetRenderVSync(g_scoreboard_renderer, VIDEO_HAS(VSYNC) ?
                                  SDL_RENDERER_VSYNC_ADAPTIVE : SDL_RENDERER_VSYNC_DISABLED);

                    if (!g_scoreboard_renderer)
                    {
                        LOGE << fmt("Could not initialize scoreboard renderer: %s", SDL_GetError());
                        set_quitflag();
                        goto exit;
                    }

                    SDL_SetRenderDrawColor(g_scoreboard_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
                    SDL_RenderClear(g_scoreboard_renderer);
                    SDL_RenderPresent(g_scoreboard_renderer);
                }
            }

            if (VIDEO_HAS(AUX_BEZEL))
            {
                if (VIDEO_HAS(DED_ANNUN_BEZEL)) g_aux_bezel_scale--;

                g_aux_ratio = (float)g_anun_h / (float)g_anun_w;

                g_aux_blit_surface = SDL_CreateSurface(g_anun_w, g_anun_h,
                                           SDL_PIXELFORMAT_RGBA32);

                if (!g_aux_blit_surface)
                {
                    LOGE << fmt("Failed to create auxillary surface: %s", SDL_GetError());
                    set_quitflag();
                    goto exit;
                }

                calcAuxRect();

                if (VIDEO_HAS(DED_ANNUN_BEZEL))
                    LogicalPosition(&g_logical_rect, &g_aux_rect, 99, 10);
                else
                    LogicalPosition(&g_logical_rect, &g_aux_rect, 100, 90);

                // argument override
                if (aux_bezel_pos_x || aux_bezel_pos_y)
                {
                    g_aux_rect.x = aux_bezel_pos_x;
                    g_aux_rect.y = aux_bezel_pos_y;
                }

                draw_annunciator(0);

                if (!VIDEO_HAS(DED_ANNUN_BEZEL))
                    draw_ranks();
            }

            // Always hide the mouse cursor
            SDL_HideCursor();

            if (VIDEO_HAS(GRAB_MOUSE))
                SDL_SetWindowMouseGrab(g_window, true);

            if (VIDEO_HAS(SCANLINES))
                SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

            if (g_game->get_outline_border())
                g_scale_h_shift = g_scale_v_shift = 100;

            g_overlay_surface =
                SDL_CreateSurface(g_overlay_width, g_overlay_height,
                                         SDL_PIXELFORMAT_RGBA32);

            if (!g_overlay_surface)
            {
                LOGE << fmt("Could not initialize g_overlay_surface: %s", SDL_GetError());
                set_quitflag();
                goto exit;
            }

            // Check for game overlay enhancements (depth and size)
            VIDEO_ASSIGN(ENHANCE_OVERLAY, g_game->has_overlay_upgrade(GAME_OVERLAY_UPGRADE));
            VIDEO_ASSIGN(OVERLAY_DYNAMIC,  g_game->get_dynamic_overlay());

            // Set it's color key to NOT copy 0x000000ff pixels.
            ConvertSurface(&g_other_bmps[B_OVERLAY_LEDS], g_overlay_surface->format);

            Uint32 colorkey = SDL_MapSurfaceRGB(g_other_bmps[B_OVERLAY_LEDS], 0, 0, 0);
            SDL_SetSurfaceColorKey(g_other_bmps[B_OVERLAY_LEDS], true, colorkey);

            if (g_aux_blit_surface)
                ConvertSurface(&g_aux_blit_surface, g_overlay_surface->format);

            ConvertSurface(&g_other_bmps[B_OVERLAY_LDP1450], g_overlay_surface->format);
            colorkey = SDL_MapSurfaceRGB(g_other_bmps[B_OVERLAY_LDP1450], 0, 0, 0);
            SDL_SetSurfaceColorKey(g_other_bmps[B_OVERLAY_LDP1450], true, colorkey);

            g_overlay_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
                                    g_texture_access, g_overlay_width, g_overlay_height);

            if (!g_overlay_texture)
            {
                LOGE << fmt("Could not initialize g_overlay_texture: %s", SDL_GetError());
                set_quitflag();
                goto exit;
            }

            SDL_SetTextureBlendMode(g_overlay_texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(g_overlay_texture, 0xff);

            SDL_SetTextureScaleMode(g_overlay_texture, VIDEO_HAS(SCALE_LINEAR) ?
                      SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

            SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);

            SDL_RenderClear(g_renderer);
            SDL_RenderPresent(g_renderer);

            VIDEO_ASSIGN(INDEX8, (g_game->get_overlay_depth() == GAME_OVERLAY_STD) ? true : false);

            if (!VIDEO_HAS(INITPASS) && VIDEO_HAS(INTRO)) splash(VIDEO_HAS(LOGO));
            result = true;
        }
    }

exit:
    VIDEO_SET(INITPASS);
    return (result);
}

void reset_yuv_overlay()
{
    vid_setup_yuv_overlay(g_probe_width, g_probe_height);
    SDL_Delay(2);
}

void vid_free_yuv_overlay()
{
#ifdef __LIBRETRO__
    std::lock_guard<std::recursive_mutex> lock(g_lr_yuv_alloc);
#endif
    // Here we free both the YUV surface and YUV texture.
    SDL_DestroyMutex (g_yuv_surface->mutex);
   
    free(g_yuv_surface->Yplane);
    free(g_yuv_surface->Uplane);
    free(g_yuv_surface->Vplane);
    free(g_yuv_surface);

#ifdef __LIBRETRO__
    if (g_yuv_visible)
    {
        SDL_DestroyMutex(g_yuv_visible->mutex);
        free(g_yuv_visible->Yplane);
        free(g_yuv_visible->Uplane);
        free(g_yuv_visible->Vplane);
        free(g_yuv_visible);
        g_yuv_visible = NULL;
    }
#endif

    if (g_yuv_texture)
        SDL_DestroyTexture(g_yuv_texture);

    g_yuv_surface = NULL;
    g_yuv_texture = NULL;
}

///////////////////////////////////////////////////////

// deinitializes the window and renderer we have used.
// returns true if successful, false if failure
bool deinit_display()
{
    SDL_SetWindowMouseGrab(g_window, false);

    SDL_DestroyTexture(g_scoreboard_texture);

    SDL_DestroyRenderer(g_scoreboard_renderer);
    SDL_DestroyWindow(g_scoreboard_window);

    SDL_DestroySurface(g_scoreboard_blit_surface);
    SDL_DestroySurface(g_aux_blit_surface);

    g_scoreboard_texture = NULL;
    g_scoreboard_renderer = NULL;
    g_scoreboard_window = NULL;
    g_scoreboard_blit_surface = NULL;
    g_aux_blit_surface = NULL;

    SDL_DestroySurface(g_overlay_surface);

    g_overlay_surface = NULL;

    TTF_CloseFont(g_font);
    TTF_CloseFont(g_ttfont);
    TTF_DestroyRendererTextEngine(g_font_engine);

    g_font = nullptr;
    g_ttfont = nullptr;
    g_font_engine = nullptr;
#ifdef __LIBRETRO__
    SDL_DestroySurface(g_console_font);
    g_console_font = NULL;
#endif

    SDL_DestroyTexture(g_bezel_texture);

    SDL_DestroyTexture(g_aux_texture);

    for (int i = 0; i < 2; ++i)
    {
        delete g_yuv_frect[i];
        g_yuv_frect[i] = NULL;
    }

    g_bezel_texture = NULL;
    g_aux_texture = NULL;

    SDL_DestroyTexture(g_overlay_texture);
    SDL_DestroyTexture(g_mix_texture.t);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);

    g_mix_texture.t = NULL;
    g_overlay_texture = NULL;
    g_renderer = NULL;
    g_window = NULL;
    free(subchar);
    free(srtchar);

    return (true);
}

// Clear the renderer. Good for avoiding texture mess (YUV, LEDs, Overlay...)
void vid_blank()
{
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(g_renderer);
}

static void colorLeds(SDL_Surface *src)
{
    if (VIDEO_HAS(OVERLAY_WHITE)) return;

    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(src->format);

    SDL_Surface* tmp = SDL_CreateSurface(src->w, src->h,
                           fmt->format);
    if (!tmp) return;

    uint8_t r, g, b, a;
    SDL_LockSurface(tmp);
    SDL_LockSurface(src);

    for (int y = 0; y < src->h; ++y)
    {
        for (int x = 0; x < src->w; ++x)
        {
            uint8_t* pixel = ((uint8_t*)src->pixels) + y * src->pitch + x * fmt->bytes_per_pixel;

            SDL_GetRGBA(*(uint32_t*)pixel, fmt, nullptr, &r, &g, &b, &a);

            if (r == 0xff && g == 0xff && b == 0xff)
	        { r = 0xc8; g = 0x0d; b = 0x0d; }
            if (r == 0x50 && g == 0x4a && b == 0x4a)
	        { r = 0x38; g = 0x00; b = 0x00; }

            uint8_t* tmpPixel = ((uint8_t*)tmp->pixels) + y * tmp->pitch + x * fmt->bytes_per_pixel;
            *(uint32_t*)tmpPixel = SDL_MapRGBA(fmt, nullptr, r, g, b, a);
        }
    }

    SDL_UnlockSurface(tmp);
    SDL_UnlockSurface(src);

    SDL_BlitSurface(tmp, NULL, src, NULL);
    SDL_DestroySurface(tmp);
}

static SDL_Surface *load_led_strip(const char *filename)
{
    std::string filepath = LR_ASSET(fmt("pics/%s", filename));

    SDL_Surface *result  = SDL_LoadBMP(filepath.c_str());

    if (!result)
        LOGE << fmt("Could not load bitmap: %s", filepath.c_str());
    else
      colorLeds(result);

    return (result);
}

static SDL_Surface *load_one_bmp(const char *filename, bool bezel)
{
    std::string filepath;

    if (bezel)
        filepath = fmt("%s/%s", g_bezel_path.c_str(), filename);

    if (!mpo_file_exists(filepath.c_str()))
        filepath = LR_ASSET(fmt("pics/%s", filename));

    SDL_Surface *result  = SDL_LoadBMP(filepath.c_str());

    if (!result)
    {
        LOGE << fmt("Could not load bitmap: %s", filepath.c_str());
    }

    return (result);
}

static SDL_Surface *load_one_png(const char *filename)
{
    std::string filepath = fmt("%s/%s", g_bezel_path.c_str(), filename);

    if (!mpo_file_exists(filepath.c_str()))
        filepath = LR_ASSET(fmt("pics/%s", filename));

    SDL_Surface *result = IMG_Load(filepath.c_str());

    if (!result)
    {
        LOGE << fmt("Could not load png: %s", filepath.c_str());
    }

    return (result);
}

// loads all the .bmp's
// returns true if they were all successfully loaded, or a false if they weren't
bool load_bmps()
{
    bool result = true; // assume success unless we hear otherwise
    int index   = 0;
    char filename[81];

    for (; index < LED_RANGE; index++)
    {
        snprintf(filename, sizeof(filename), "led%d.bmp", index);

        g_led_bmps[index] = load_one_bmp(filename, false);

        // If the bit map did not successfully load
        if (g_led_bmps[index] == 0)
        {
            result = false;
        }
    }

    g_other_bmps[B_DL_PLAYER1]  = load_one_bmp("player1.bmp", false);
    g_other_bmps[B_DL_PLAYER2]  = load_one_bmp("player2.bmp", false);
    g_other_bmps[B_DL_LIVES]    = load_one_bmp("lives.bmp", false);
    g_other_bmps[B_DL_CREDITS]  = load_one_bmp("credits.bmp", false);
    g_other_bmps[B_TQ_TIME]     = load_one_bmp("time.bmp", false);

    g_other_bmps[B_OVERLAY_LEDS] =
    load_led_strip(sboverlay_characterset == 1 ? "overlayleds1.bmp" : "overlayleds2.bmp");
   
    g_other_bmps[B_OVERLAY_LDP1450] = load_one_bmp("ldp1450font.bmp", false);

    g_other_bmps[B_ACE_CADET]   = load_one_bmp("cadet.bmp", true);
    g_other_bmps[B_ACE_CAPTAIN] = load_one_bmp("captain.bmp", true);
    g_other_bmps[B_ACE_SPACE]   = load_one_bmp("spaceace.bmp", true);

    g_other_bmps[B_CADET_OFF]   = load_one_bmp("offcadet.bmp", true);
    g_other_bmps[B_CAPTAIN_OFF] = load_one_bmp("offcaptain.bmp", true);
    g_other_bmps[B_ACE_OFF]     = load_one_bmp("offspaceace.bmp", true);
    g_other_bmps[B_CADET_ON]    = load_one_bmp("oncadet.bmp", true);
    g_other_bmps[B_CAPTAIN_ON]  = load_one_bmp("oncaptain.bmp", true);
    g_other_bmps[B_ACE_ON]      = load_one_bmp("onspaceace.bmp", true);

    g_other_bmps[B_ANUN_ON]     = load_one_png("annunon.png");
    g_other_bmps[B_ANUN_OFF]    = load_one_png("annunoff.png");

    // check to make sure they all loaded
    for (index = 0; index < B_EMPTY; index++)
    {
        if (g_other_bmps[index] == NULL && index != B_MIA)
        {
            result = false;
        }
    }

    return (result);
}

// Draw's one of our LED's to the screen
// value contains the bitmap to draw (0-9 is valid)
// x and y contain the coordinates on the screen
// This function is called from img-scoreboard.cpp
// 1 is returned on success, 0 on failure
bool draw_led(int value, int x, int y, unsigned char end)
{
    g_scoreboard_surface = g_led_bmps[value];
    static unsigned char led = 0;

    SDL_Rect dest;
    dest.x = (int16_t)x;
    dest.y = (int16_t)y;
    dest.w = (uint16_t)g_scoreboard_surface->w;
    dest.h = (uint16_t)g_scoreboard_surface->h;

    if (!SDL_BlitSurface(g_scoreboard_surface, NULL, g_scoreboard_blit_surface, &dest))
    {
        LOGE << fmt("Could not Blit Scoreboard LED's %s", SDL_GetError());
        set_quitflag();
        return false;
    }

    if (led == end)
    {

        if (g_scoreboard_texture) SDL_DestroyTexture(g_scoreboard_texture);

        SDL_Renderer *renderer = VIDEO_HAS(SCOREBOARD_BEZEL) ? g_renderer : g_scoreboard_renderer;

        g_scoreboard_texture = SDL_CreateTextureFromSurface(renderer, g_scoreboard_blit_surface);
        if (!g_scoreboard_texture) return false;

        if (!VIDEO_HAS(SCOREBOARD_BEZEL))
        {
            SDL_RenderTexture(g_scoreboard_renderer, g_scoreboard_texture, NULL, NULL);
            g_scoreboard_needs_update = true;
        }
        led = -1;
    }
    led++;
    return true;
}

static bool draw_annunciator1(int which)
{
    g_scoreboard_surface = g_other_bmps[B_ANUN_OFF];

    SDL_Rect dest;
    uint8_t spacer = 4;

    dest.x = g_aux_blit_surface->w - (g_scoreboard_surface->w
                   + (g_scoreboard_surface->w >> 1));

    dest.w = (uint16_t)g_scoreboard_surface->w;
    dest.h = (uint16_t)g_scoreboard_surface->h;

    for (int i = 0; i < ANUN_LEVELS; i++)
    {
        dest.y = ((dest.h + ANUN_CHAR_HEIGHT) * i) + spacer;
        if (VIDEO_HAS(ANNUN_LAMPS)) dest.x = 110 - (-i * 15);
        SDL_FillSurfaceRect(g_aux_blit_surface, &dest, 0x00000000);
        SDL_BlitSurface(g_scoreboard_surface, NULL, g_aux_blit_surface, &dest);
    }

    if (which)
    {
        g_scoreboard_surface = g_other_bmps[B_ANUN_ON];
        if (VIDEO_HAS(ANNUN_LAMPS)) dest.x = 110 - ((-which + 1) * 15);
        dest.y = ((dest.h + ANUN_CHAR_HEIGHT) * --which) + spacer;
        SDL_BlitSurface(g_scoreboard_surface, NULL, g_aux_blit_surface, &dest);
    }

    g_aux_needs_update = true;
    return true;
}

static bool draw_annunciator2(int which)
{
    SDL_Rect dest;

    dest.x = 0;
    dest.h = 40;
    dest.w = 220;

    for (int i = B_ACE_OFF; i < B_EMPTY; i++)
    {
        g_scoreboard_surface = g_other_bmps[i];
        dest.y = (ANUN_RANK_HEIGHT * (i - B_ACE_OFF));
        SDL_FillSurfaceRect(g_aux_blit_surface, &dest, 0x00000000);
        SDL_BlitSurface(g_scoreboard_surface, NULL, g_aux_blit_surface, &dest);
    }

    if (which)
    {
        g_scoreboard_surface = g_other_bmps[B_MIA + which];
        dest.y = ANUN_RANK_HEIGHT * --which;
        SDL_BlitSurface(g_scoreboard_surface, NULL, g_aux_blit_surface, &dest);
    }

    g_aux_needs_update = true;
    return true;
}

bool draw_annunciator(int which)
{
    (VIDEO_HAS(DED_ANNUN_BEZEL)) ? draw_annunciator2(which) : draw_annunciator1(which);

    return true;
}

// Update scoreboard surface
void draw_overlay_leds(unsigned int values[], int num_digits,
                       int start_x, int y)
{
    SDL_Rect src, dest;
#ifdef __LIBRETRO__
    s_ov_dirty = true;
    ov_mirrors_clear();
#endif

    dest.x = start_x;
    dest.y = y;
    dest.w = OVERLAY_LED_WIDTH;
    dest.h = OVERLAY_LED_HEIGHT;

    src.y = 0;
    src.w = OVERLAY_LED_WIDTH;
    src.h = OVERLAY_LED_HEIGHT;

    SDL_Rect base = {0, dest.y + 1, dest.w, dest.h};
    static const Uint32 colorkey = SDL_MapSurfaceRGB(g_other_bmps[B_OVERLAY_LEDS], 0xA, 0xA, 0xA);

    // Draw the digit(s) to the overlay surface
    for (int i = 0; i < num_digits; i++)
    {
        src.x = values[i] * OVERLAY_LED_WIDTH;
        base.x = dest.x - 1;

        if (!VIDEO_HAS(LEGACY_OVERLAY))
            SDL_FillSurfaceRect(g_overlay_surface, &base, colorkey);

        if (!SDL_BlitSurface(g_other_bmps[B_OVERLAY_LEDS], &src, g_overlay_surface, &dest))
        {
            LOGE << fmt("Could not Blit Overlay LED's: %s", SDL_GetError());
            set_quitflag();
            return;
        }
        dest.x += OVERLAY_LED_WIDTH;
    }

    VIDEO_SET(BLOCK_DRIVER_OVERLAY);
}

void draw_singleline_LDP1450(char *LDP1450_String, int start_x, int y)
{
    SDL_Rect src, dest;
#ifdef __LIBRETRO__
    s_ov_dirty = true;
    ov_mirrors_clear();
#endif

    int i = 0;
    int value = 0;
    int LDP1450_strlen;
    VIDEO_SET(BLOCK_DRIVER_OVERLAY);

    if (g_aspect_ratio == ASPECTSD &&
             g_probe_width == NOSQUARE)
        start_x = (start_x - (start_x >> 2));

    dest.x = start_x;
    dest.y = y;
    dest.w = OVERLAY_LDP1450_WIDTH;
    dest.h = OVERLAY_LDP1450_HEIGHT;

    src.y = 0;
    src.w = OVERLAY_LDP1450_WIDTH;
    src.h = OVERLAY_LDP1450_HEIGHT;

    LDP1450_strlen = strlen(LDP1450_String);

    if (!LDP1450_strlen)
    {
        strcpy(LDP1450_String,"           ");
        LDP1450_strlen = strlen(LDP1450_String);
    }
    else
    {
        if (LDP1450_strlen <= 11)
        {
            for (i=LDP1450_strlen; i<=11; i++)
                 LDP1450_String[i] = 32;

            LDP1450_strlen = strlen(LDP1450_String);
        }
    }

    for (i=0; i<LDP1450_strlen; i++)
    {
         value = LDP1450_String[i];

         if (value >= 0x26 && value <= 0x39) value -= 0x25;
         else if (value >= 0x41 && value <= 0x5a) value -= 0x2a;
         else if (value == 0x13) value = 0x32;
         else value = 0x31;

         src.x = value * OVERLAY_LDP1450_WIDTH;
         SDL_FillSurfaceRect(g_overlay_surface, &dest, 0x00000000);
         if (!SDL_BlitSurface(g_other_bmps[B_OVERLAY_LDP1450], &src, g_overlay_surface, &dest))
         {
             LOGE << fmt("Could not draw LDP1450 line: %s", SDL_GetError());
             set_quitflag();
             return;
         }
         dest.x += OVERLAY_LDP1450_CHARACTER_SPACING;
    }
}

//  used to draw non LED stuff like scoreboard text
//  'which' corresponds to enumerated values
bool draw_othergfx(int which, int x, int y)
{
    g_scoreboard_surface = g_other_bmps[which];

    if (g_scoreboard_renderer && which == B_DL_PLAYER1)
        SDL_RenderClear(g_scoreboard_renderer);

    SDL_Rect dest;
    dest.x = (int16_t)x;
    dest.y = (int16_t)y;
    dest.w = (uint16_t)g_scoreboard_surface->w;
    dest.h = (uint16_t)g_scoreboard_surface->h;

    SDL_BlitSurface(g_scoreboard_surface, NULL, g_scoreboard_blit_surface, &dest);

    return true;
}

static void free_one_bmp(SDL_Surface *candidate)
{
	SDL_DestroySurface(candidate);
}

// de-allocates all of the .bmps that we have allocated
void free_bmps()
{
    int nuke_index = 0;

    // get rid of all the LED's
    for (; nuke_index < LED_RANGE; nuke_index++)
    {
        free_one_bmp(g_led_bmps[nuke_index]);
    }
    for (nuke_index = 0; nuke_index < B_EMPTY; nuke_index++)
    {
        // check to make sure it exists before we try to free
        if (g_other_bmps[nuke_index])
        {
            free_one_bmp(g_other_bmps[nuke_index]);
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

std::vector<SDL_Rect> get_displays() { return displayDimensions; }

SDL_Renderer *get_renderer() { return g_renderer; }
SDL_Window *get_window() { return g_window; }
SDL_Texture *get_overlay_texture() { return g_overlay_texture; }
#ifdef __LIBRETRO__
// callers draw into it (overlay scoreboard)
SDL_Surface *get_screen_leds()
{
    s_ov_dirty = true;
    ov_mirrors_clear();
    return g_overlay_surface;
}
#else
SDL_Surface *get_screen_leds() { return g_overlay_surface; }
#endif
SDL_Rect get_scoreboard_rect() { return g_scoreboard_bezel_rect; }
SDL_Rect get_aux_rect() { return g_aux_rect; }

TTF_Font *get_font() { return g_font; }
TTF_TextEngine *get_font_engine() { return g_font_engine; }

Uint16 get_video_width() { return g_video_width; }
Uint16 get_video_height() { return g_video_height; }
Uint16 get_viewport_width() { return g_viewport_width; }
Uint16 get_viewport_height() { return g_viewport_height; }

int get_scoreboard_bezel_scale() { return g_scoreboard_bezel_scale; }
int get_aux_bezel_scale() { return g_aux_bezel_scale; }

uint64_t get_bezelstatus() { return g_bezelhash; }

int get_display_no() { return g_display; }
int get_scale_h_shift() { return g_scale_h_shift; };
int get_scale_v_shift() { return g_scale_v_shift; };
SDL_TextureAccess get_textureaccess() { return g_texture_access; }
int get_scalefactor() { return g_scalefactor; }
unsigned int get_logical_width() { return g_logical_rect.w; }
unsigned int get_logical_height() { return g_logical_rect.h; }
float get_fRotateDegrees() { return g_fRotateDegrees; }

bool get_opengl() { return VIDEO_HAS(OPENGL); }
bool get_vulkan() { return VIDEO_HAS(VULKAN); }
bool get_fullscreen() { return VIDEO_HAS(FULLSCREEN); }
bool get_aux_bezel() { return (g_aux_texture != NULL); }
bool get_video_resized() { return VIDEO_HAS(RESIZED_MEDIA); }
bool use_legacy_font() { return VIDEO_HAS(LEGACY_OVERLAY); }

void set_fullscreen(bool value) { VIDEO_ASSIGN(FULLSCREEN, value); }
void set_software_render(bool value) { VIDEO_ASSIGN(SOFT_RENDER, value); }
void set_opengl(bool value) { VIDEO_ASSIGN(OPENGL, value); }
void set_vulkan(bool value) { VIDEO_ASSIGN(VULKAN, value); }
void set_teardown() { VIDEO_SET(TEARDOWN); }
void set_forcetop(bool value) { VIDEO_ASSIGN(FORCE_TOP, value);  }
void set_textureaccess(SDL_TextureAccess value) { g_texture_access = value; }
void set_grabmouse(bool value) { VIDEO_ASSIGN(GRAB_MOUSE, value); }
void set_vsync(bool value) { VIDEO_ASSIGN(VSYNC, value); }
void set_intro(bool value) { VIDEO_ASSIGN(INTRO, value); }
void set_logo(bool value) { VIDEO_ASSIGN(LOGO, value); }
void set_yuv_blue(bool value) { VIDEO_ASSIGN(YUV_BLUE, value); }
void set_scanlines(bool value) { VIDEO_ASSIGN(SCANLINES, value); }
void set_shunt(uint8_t value) { g_scanline_shunt = value; }
void set_alpha(uint8_t value) { g_scanline_alpha = value; }
void set_queue_screenshot(bool value) { VIDEO_ASSIGN(TAKE_SCREENSHOT, value); }
void set_scale_linear(bool value) { VIDEO_ASSIGN(SCALE_LINEAR, value); }
void set_sboverlay_characterset(int value) { sboverlay_characterset = value; }
void set_sboverlay_white(bool value) { VIDEO_ASSIGN(OVERLAY_WHITE, value); }
void set_legacy_overlay(bool value) { VIDEO_ASSIGN(LEGACY_OVERLAY, value); }
void set_force_aspect_ratio(bool value) { VIDEO_ASSIGN(FORCE_ASPECT, value); }
void set_ignore_aspect_ratio(bool value) { VIDEO_ASSIGN(IGNORE_ASPECT, value); }
void set_preserve_aspect_ratio(bool value) { VIDEO_ASSIGN(PRESERVE_ASPECT, value); }
void set_aspect_ratio(int fRatio) { g_aspect_ratio = fRatio; }
void set_detected_height(int pHeight) { g_probe_height = pHeight; }
void set_detected_width(int pWidth) { g_probe_width = pWidth; }
void set_scoreboard_bezel_scale(int value) { g_scoreboard_bezel_scale = value; }
void set_aux_bezel_scale(int value) { g_aux_bezel_scale = value; }
void set_ded_annun_bezel(bool bEnabled) { VIDEO_ASSIGN(DED_ANNUN_BEZEL, bEnabled); }
void set_annun_lamponly(bool bEnabled) { VIDEO_ASSIGN(ANNUN_LAMPS, bEnabled); }
void set_scale_h_shift(int value) { g_scale_h_shift = value; }
void set_scale_v_shift(int value) { g_scale_v_shift = value; }
void set_display_screen(int value) { g_alloc_screen = value; }
void set_scoreboard_screen(int value) { g_scoreboard_screen = value; }

static void set_subtitle_display(const char *s)
{
    free(subchar);
    subchar = strdup(s);
}

static void set_srt_display(const char *s)
{
    free(srtchar);
    srtchar = strdup(s);
}

void set_grayscale(bool value)
{
    if (value) g_yuv_flags |= YUV_FLAG_GRAYSCALE;
    else g_yuv_flags &= ~YUV_FLAG_GRAYSCALE;
}

void set_blendfilter(bool value)
{
    if (value) g_yuv_flags |= YUV_FLAG_BLEND;
    else g_yuv_flags &= ~YUV_FLAG_BLEND;
}

void set_overlayscalemode(bool value)
{
    VIDEO_ASSIGN(SCALE_LINEAR, value);
    SDL_SetTextureScaleMode(g_overlay_texture, value ?
      SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

void set_luma(bool value, uint8_t luma)
{
    uint8_t mask = (uint8_t)-(value && (luma != YUV_FLAG_LUMA)) & YUV_FLAG_LUMA;
    g_yuv_flags = (g_yuv_flags & ~YUV_FLAG_LUMA) | mask;
    g_yuv_luma = luma - YUV_FLAG_LUMA;
}

void set_overlay_alpha(uint8_t alpha)
{
    SDL_SetTextureAlphaMod(g_overlay_texture, alpha);
}

uint64_t set_overlaybezel(bool value, bool cli)
{
    if (cli)
        VIDEO_ASSIGN(BEZEL_OVERLAY, value);
    else if (g_bezelhash > 0)
        VIDEO_ASSIGN(BEZEL_OVERLAY | OVERLAY_LAST, value);

    return g_bezelhash;
}

void toggle_grabmouse()
{
    const bool grabmode = g_game->get_manymouse() ||
          (SDL_GetWindowRelativeMouseMode(g_window) == false);

    VIDEO_TOGGLE(GRAB_MOUSE);
    const bool state = (VIDEO_HAS(GRAB_MOUSE) ^ !grabmode) ? true : false;

    if (grabmode)
        SDL_SetWindowMouseGrab(g_window, state);
    else {
        SDL_SetWindowRelativeMouseMode(g_window, state);
        VIDEO_TOGGLE(video::GRAB_MOUSE);
    }
}

void toggle_bilinearscale()
{
    VIDEO_TOGGLE(SCALE_LINEAR);
    set_overlayscalemode((VIDEO_HAS(SCALE_LINEAR) ? true : false));
}

void set_yuv_flash()
{
    g_yuv_display = YUV_FLASH;
}

void set_yuv_blank(int value)
{
    g_yuv_display = value;

    if (value == YUV_BLANK)
    {
        vid_update_yuv_overlay(g_yuv_surface->Yplane, g_yuv_surface->Uplane,
            g_yuv_surface->Vplane, g_yuv_surface->Ypitch, g_yuv_surface->Upitch,
            g_yuv_surface->Vpitch);
#ifdef __LIBRETRO__
        vid_commit_yuv_overlay();
#endif
    }
}

void set_scalefactor(int value)
{
     g_scalefactor = value;
     VIDEO_SET(SCALED);
}

void set_vertical_orientation(bool value)
{
     VIDEO_ASSIGN(VERTICAL_ORIENTATION, value);
     VIDEO_SET(SCALED);
}

void set_window_title(char* value)
{
     strncpy(g_window_title, value, TITLE_LENGTH - 1);

     g_window_title[TITLE_LENGTH - 1] = '\0';

     if (g_window)
         SDL_SetWindowTitle(g_window, g_window_title);
}

void set_game_window(const char* value)
{
     std::string game(value);
     std::string title = "Hypseus Singe: [" + game + "]";
     strncpy(g_window_title, title.c_str(), TITLE_LENGTH - 1);

     g_window_title[TITLE_LENGTH - 1] = '\0';
     if (g_window)
         SDL_SetWindowTitle(g_window, g_window_title);
}

void set_rotate_degrees(float fDegrees)
{
     VIDEO_SET(FULLSCREEN);

     (fDegrees == 0) ? VIDEO_CLEAR(ROTATED_MEDIA) :
                       VIDEO_SET(ROTATED_MEDIA);

     g_fRotateDegrees = fDegrees;
}

void set_scoreboard_bezel(bool bEnabled)
{
     if (bEnabled)
     {
         if (!VIDEO_HAS(FULLSCREEN) && g_yuv_surface)
             vid_toggle_fullscreen();
         VIDEO_SET(FULLSCREEN);
     }
     VIDEO_ASSIGN(SCOREBOARD_BEZEL, bEnabled);
}

void set_aux_bezel(bool bEnabled)
{
     if (bEnabled)
     {
         VIDEO_SET(FULLSCREEN);
         VIDEO_SET(SCOREBOARD_BEZEL);
         g_game->m_software_scoreboard = true;
         VIDEO_SET(SCALE_LINEAR);
     }
     VIDEO_ASSIGN(AUX_BEZEL,bEnabled);
}

void set_bezel_reverse(bool d)
{
     char s[] = "reverse priority";
     VIDEO_TOGGLE(BEZEL_REVERSE);
     if (d) draw_subtitle(s, 1, true);
}

void set_tq_keyboard(bool bEnabled)
{
     if (bEnabled)
     {
         g_scalefactor = (g_scalefactor == 100) ? 75 : g_scalefactor;
         VIDEO_SET(FULLSCREEN);
         VIDEO_SET(SCOREBOARD_BEZEL);
         g_game->m_software_scoreboard = true;
     }
     VIDEO_ASSIGN(KEYBOARD_BEZEL, bEnabled);
}

void set_bezel_file(const char *bezelFile)
{
     g_bezel_file = bezelFile;

     if (!g_bezel_file.empty())
     {
         VIDEO_SET(FULLSCREEN);
     }
}

void set_bezel_path(const char *bezelPath)
{
     g_bezel_path = bezelPath;
     if (g_bezel_path.back() == '/')
         g_bezel_path.pop_back();
}

void set_fRotateDegrees(float fDegrees, bool verbose)
{
     char d[8]; // In-game

     if (verbose)
     {
         snprintf(d, sizeof(d), "%d\xC2\xB0", int(fDegrees));
         draw_subtitle(d, 1, true);
     }

     (fDegrees == 0) ? VIDEO_CLEAR(ROTATED_MEDIA) :
                       VIDEO_SET(ROTATED_MEDIA);

     g_fRotateDegrees = fDegrees;
}

static SDL_FRect* copy_rect(const SDL_FRect* src)
{
     if (src == NULL) return NULL;

     SDL_FRect* dst = new SDL_FRect(*src);
     return dst;
}

static bool allocate_rect(SDL_FRect** rect)
{
     if (*rect == NULL)
     {
         *rect = new(std::nothrow) SDL_FRect();

         if (*rect == nullptr)
         {
             printerror("Failed in allocate_rect: g_yuv_frect");
             set_quitflag();
             return false;
         }
     }
     return true;
}

void reset_yuv_rect()
{
     delete g_yuv_frect[0];
     g_yuv_frect[0] = copy_rect(g_yuv_frect[1]);
}

void set_yuv_rect(float x, float y, float w, float h)
{
     if (allocate_rect(&g_yuv_frect[0]))
         *g_yuv_frect[0] = {x, y, w, h};
}

void set_yuv_scale(int v, uint8_t axis)
{
     if (allocate_rect(&g_yuv_frect[0]))
         g_yuv_scale[axis] = -0.01043f * v + 1.00043f;
}

void reset_scalefactor(int value, uint8_t bezel, bool verbose)
{
     bool rst = true;

     switch(bezel)
     {
     case 1: // Scoreboard
         if (!g_scoreboard_texture) { rst = false ; break; }
         g_scoreboard_bezel_scale = value;
         break;
     case 2: // Aux
         if (!g_aux_texture) { rst = false ; break; }
         g_aux_bezel_scale = value;
         calcAuxRect();
         break;
     default:
         g_scalefactor = value;
         break;
     }

     char s[32]; // In-game
     if (rst)
     {
         g_rescale = VIDEO_HAS(FULLSCREEN) ? 1 : 2;
         VIDEO_SET(SCALED);

         if (!verbose) return;
         snprintf(s, sizeof(s), "scale: %d", value);
     }
     else snprintf(s, sizeof(s), "invalid layer");

     draw_subtitle(s, 1, true);
}

static void calc_kb_rect()
{
     double scale = (double)((g_aux_bezel_scale / 25.0f) + 0.52f);
     g_aux_rect.w = (g_bezel_scalewidth / 2.25f) * scale;
     g_aux_rect.h = (g_aux_rect.w * g_aux_ratio);
}

void scalekeyboard(int value)
{
     if (!g_aux_texture) return;

     g_aux_bezel_scale = value;

     calc_kb_rect();

     char s[32]; // In-game
     snprintf(s, sizeof(s), "scale: %d", value);
     draw_subtitle(s, 1, true);
}

void reset_shiftvalue(int value, bool vert, uint8_t bezel)
{
     int h, v;
     bool rst = true;

     switch(bezel)
     {
     case 1: // Scoreboard
         if (!g_scoreboard_texture) { rst = false ; break; }
         if (vert) scoreboard_window_pos_y = value;
         else scoreboard_window_pos_x = value;
         h = scoreboard_window_pos_x;
         v = scoreboard_window_pos_y;
         break;
     case 2: // Aux
         if (!g_aux_texture) { rst = false ; break; }
         if (vert) g_aux_rect.y = value;
         else g_aux_rect.x = value;
         h = g_aux_rect.x;
         v = g_aux_rect.y;
         break;
     default:
         if (g_game->get_outline_border()) return;
         if (VIDEO_HAS(KEYBOARD_BEZEL)) VIDEO_SET(SCALED);
         if (vert) g_scale_v_shift = value;
         else g_scale_h_shift = value;
         h = g_scale_h_shift - 0x64;
         v = g_scale_v_shift - 0x64;
         break;
     }

     char s[32]; // In-game
     if (rst)
     {
         g_rescale = VIDEO_HAS(FULLSCREEN) ? 1 : 2;
         snprintf(s, sizeof(s), "x:%d, y:%d", h, v);
     }
     else snprintf(s, sizeof(s), "invalid layer");

     draw_subtitle(s, 1, true);
}

// position annunicator bezel
void set_aux_bezel_position(int xValue, int yValue)
{
    aux_bezel_pos_x = xValue;
    aux_bezel_pos_y = yValue;
}

// position scoreboard_scoreboard
void set_scoreboard_window_position(int xValue, int yValue)
{
    scoreboard_window_pos_x = xValue;
    scoreboard_window_pos_y = yValue;
}

// deal with MPEG headers aspect ratio
void set_aspect_change(int aspectWidth, int aspectHeight)
{
    g_aspect_width  = aspectWidth;
    g_aspect_height = aspectHeight;
}

// sets g_video_width
void set_video_width(Uint16 width)
{
    // Let the user specify whatever width s/he wants (and suffer the
    // consequences)
    // We need to support arbitrary resolution to accomodate stuff like screen
    // rotation
    g_video_width = width;
    VIDEO_SET(RESIZED_MEDIA);
}

// sets g_video_height
void set_video_height(Uint16 height)
{
    // Let the user specify whatever height s/he wants (and suffer the
    // consequences)
    // We need to support arbitrary resolution to accomodate stuff like screen
    // rotation
    g_video_height = height;
    VIDEO_SET(RESIZED_MEDIA);
}

#ifdef __LIBRETRO__
// Daphne 1.0 console font: 256 glyphs of 6x13 in a single strip.
static void lr_draw_console_text(const char *t, int x, int y, SDL_Surface *overlay,
                                 SDL_Color c)
{
    if (!g_console_font)
    {
        g_console_font = SDL_LoadBMP(LR_ASSET("pics/ConsoleFont.bmp").c_str());
        if (!g_console_font)
        {
            LOGE << "Could not load pics/ConsoleFont.bmp";
            return;
        }
        // glyphs are white on a magenta key
        SDL_SetSurfaceColorKey(g_console_font, true,
                               SDL_MapSurfaceRGB(g_console_font, 0xff, 0, 0xff));
    }

    SDL_SetSurfaceColorMod(g_console_font, c.r, c.g, c.b);
    SDL_Rect src = {0, 0, 6, 13};
    SDL_Rect dst = {x, y, 6, 13};

    for (; *t; ++t, dst.x += 6)
    {
        src.x = (unsigned char)*t * 6;
        SDL_BlitSurface(g_console_font, &src, overlay, &dst);
    }
}
#endif

void draw_string(const char *t, int col, int row, SDL_Surface *overlay,
                     SDL_Color rgb, bool outline)
{
#ifdef __LIBRETRO__
    if (!g_ttfont)
    {
        if (outline)
        {
            static const SDL_Color padding = {0x3D, 0x38, 0x36, 0xFF};
            static const int offsets[4][2] = {{-1, 0}, {1, 0}, {0,-1}, {0, 1}};
            for (int i = 0; i < 4; ++i)
                lr_draw_console_text(t, col + offsets[i][0], row + offsets[i][1],
                                     overlay, padding);
        }
        lr_draw_console_text(t, col, row, overlay, rgb);
        return;
    }
#endif
    SDL_Rect dest;
    dest.y = (int16_t)(row);
    dest.x = (int16_t)(col);
    dest.w = (uint16_t)(6 * strlen(t));
    dest.h = 14;

    SDL_Surface *text_surface = TTF_RenderText_Solid(g_ttfont, t, strlen(t), rgb);

    if (!text_surface)
    {
        LOGE << fmt("Could not draw_string %s", SDL_GetError());
        set_quitflag();
        return;
    }

    if (outline)
    {
        static const SDL_Color padding = {0x3D, 0x38, 0x36, 0xFF};
        static const int offsets[4][2] = {{-1, 0}, {1, 0}, {0,-1}, {0, 1}};

        SDL_Surface *outline = TTF_RenderText_Solid(g_ttfont, t, strlen(t), padding);
        SDL_Rect r = dest;

        for (int i = 0; i < 4; ++i)
        {
            r.x = dest.x + offsets[i][0];
            r.y = dest.y + offsets[i][1];
            SDL_BlitSurface(outline, NULL, overlay, &r);
        }

        SDL_DestroySurface(outline);
    }

    SDL_BlitSurface(text_surface, NULL, overlay, &dest);
    SDL_DestroySurface(text_surface);
}

void draw_srt(const char *s, uint8_t func, int pos = -1)
{
    if (!s) return;
#ifdef __LIBRETRO__
    // drawn with SDL's renderer, which a libretro core has not got
    if (!g_font || !g_font_engine) return;
#endif

    static int message_timer = 0;
    static float base_y = 90.0f;

    const int timeout = 30;
    const int pad = 6;

    int x = (int)(g_scaling_rect.x + g_scaling_rect.w * 0.5f);

    if (pos > 0)
        base_y = g_scaling_rect.y + g_scaling_rect.h * (pos / 100.0f);

    if (func & 1)
    {
        message_timer = 0;
        set_srt_display(s);
        g_bSubtitleShown |= (1 << 1);
    }
    else if (func & 2 || message_timer > timeout)
    {
        g_bSubtitleShown = 0;
        return;
    }

    char *copy = SDL_strdup(s);
    char *saveptr = NULL;

    int max_w = 0;
    int line_count = 0;

    for (char *line = strtok_r(copy, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr))
    {
        int w = 0, h = 0;
        TTF_GetStringSize(g_font, line, 0, &w, &h);

        if (w > max_w)
            max_w = w;

        line_count++;
    }

    int line_skip = TTF_GetFontLineSkip(g_font);
    int total_h = line_count * line_skip;

    SDL_free(copy);

    SDL_FRect bg;
    bg.x = x - (max_w / 2.0f) - (pad * 3.0f);
    bg.y = base_y - (pad / 2.0f);
    bg.w = max_w + (pad * 6.0f);
    bg.h = total_h + pad;

    SDL_SetRenderDrawColor(g_renderer, 0x14, 0x14, 0x14, 0xff);
    SDL_RenderFillRect(g_renderer, &bg);

    copy = SDL_strdup(s);
    saveptr = NULL;

    float y = base_y;

    for (char *line = strtok_r(copy, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr))
    {
        TTF_Text *text = TTF_CreateText(g_font_engine, g_font, line, 0);

        int w = 0, h = 0;
        TTF_GetStringSize(g_font, line, 0, &w, &h);

        float lx = x - w / 2.0f;

        TTF_DrawRendererText(text, (int)lx, (int)y);

        TTF_DestroyText(text);

        y += line_skip;
    }

    SDL_free(copy);

    message_timer++;
}

void draw_subtitle(const char *s, uint8_t func, bool center = false)
{
    if (!s) return;
#ifdef __LIBRETRO__
    // drawn with SDL's renderer, which a libretro core has not got
    if (!g_font || !g_font_engine) return;
#endif

    SDL_FPoint p;
    p.y = (g_scaling_rect.h * 0.92f) + g_scaling_rect.y;

    static int m_message_timer;
    static bool align = false;
    const int timeout = 200;

    if (func & 1)
    {
        align = center;
        m_message_timer = 0;
        g_bSubtitleShown |= (1 << 0);
        set_subtitle_display(s);
    }
    else if (m_message_timer > timeout)
    {
        g_bSubtitleShown = 0;
        return;
    }

    const int pad_x = (int)(g_scaling_rect.w * 0.03f);

    int w = 0, h = 0;
    TTF_GetStringSize(g_font, s, 0, &w, &h);

    if (align)
    {
        p.x = (float)((g_scaling_rect.w / 2) + g_scaling_rect.x - (w / 2));
    }
    else
    {
        p.x = (float)(g_scaling_rect.x + pad_x);
    }

    TTF_Text *text = TTF_CreateText(g_font_engine, g_font, s, strlen(s));

    TTF_DrawRendererText(text, (int)p.x, (int)p.y);
    TTF_DestroyText(text);

    m_message_timer++;
}

void vid_toggle_fullscreen()
{
    VIDEO_CLEAR(BEZEL_TOGGLE);

    SDL_SyncWindow(g_window);
    Uint32 current = SDL_GetWindowFlags(g_window);
    bool flip = (current & SDL_WINDOW_FULLSCREEN) != 0;

    if (!SDL_SetWindowFullscreen(g_window, flip ? false : true))
    {
        LOGW << fmt("Toggle fullscreen failed: %s", SDL_GetError());
        return;
    }

    if (!flip)
    {
        g_logical_rect = displayDimensions[g_display];
        VIDEO_SET(FULLSCREEN);
        format_fullscreen_render();
        notify_stats(g_overlay_width, g_overlay_height, "u");

        if (g_aux_texture) g_aux_needs_update = true;

        if (!g_bezel_file.empty()) VIDEO_SET(BEZEL_TOGGLE);
        return;
    }

    VIDEO_CLEAR(FULLSCREEN);
    format_window_render();
    notify_stats(g_overlay_width, g_overlay_height, "u");

    SDL_Rect dimensions;
    SDL_DisplayID id = SDL_GetDisplayForWindow(g_window);
    SDL_GetDisplayBounds(id, &dimensions);

    SDL_SetWindowSize(g_window, g_viewport_width, g_viewport_height);
    SDL_SetWindowPosition(g_window, dimensions.x + ((dimensions.w - g_viewport_width) >> 1),
                dimensions.y + ((dimensions.h - g_viewport_height) >> 1));
}

void vid_toggle_scanlines()
{
    char s[16] = "scanlines off";

    if (VIDEO_HAS(SCANLINES))
    {
        if (g_scanline_shunt < 10)
        {
            g_scanline_shunt++;
        }
        else
        {
            VIDEO_CLEAR(SCANLINES);
            SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
            g_scanline_shunt = 2;
        }
    }
    else
    {
        VIDEO_SET(SCANLINES);
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    }

    if (VIDEO_HAS(SCANLINES)) snprintf(s, sizeof(s), "shunt: %d", g_scanline_shunt);

    draw_subtitle(s, 1, true);
}

void vid_toggle_bezel()
{
    if (VIDEO_HAS(FULLSCREEN))
    {
        if (g_bezel_texture || g_aux_texture || VIDEO_HAS(SCOREBOARD_BEZEL))
            VIDEO_TOGGLE(BEZEL_TOGGLE);

        char s[] = "bezel toggle";
        draw_subtitle(s, 1, true);
    }
}

void vid_scoreboard_switch()
{
    if (!g_scoreboard_window) return;

    g_scoreboard_screen = max(0, min(g_scoreboard_screen, g_displays_total - 1));

    char s[16] = "screen: 0";

    if (g_displays_total > 1)
    {
        snprintf(s, sizeof(s), "screen: %d", (unsigned char)g_scoreboard_screen);

        SDL_SetWindowPosition(g_scoreboard_window,
           displayDimensions[g_scoreboard_screen].x + scoreboard_window_pos_x,
              displayDimensions[g_scoreboard_screen].y + scoreboard_window_pos_y);

        if (++g_scoreboard_screen == g_displays_total) g_scoreboard_screen = 0;

    } else
        SDL_SetWindowPosition(g_scoreboard_window, scoreboard_window_pos_x,
                                 scoreboard_window_pos_y);

    SDL_RaiseWindow(g_scoreboard_window);
    draw_subtitle(s, 1, true);
}

void vid_setup_yuv_overlay (int width, int height)
{
#ifdef __LIBRETRO__
    std::lock_guard<std::recursive_mutex> lock(g_lr_yuv_alloc);
#endif
    // Prepare the YUV overlay, wich means setting up both the YUV surface and YUV texture.

    // If we have already been here, free things first.

    if (g_yuv_surface)
    {
        // Free both the YUV surface and YUV texture.
        vid_free_yuv_overlay();
    }

    g_yuv_surface = (m_yuv_surface_t*) malloc (sizeof(m_yuv_surface_t));

    // 12 bits (1 + 0.5 bytes) per pixel, and each plane has different size. Crazy stuff.
    g_yuv_surface->Ysize = width * height;
    g_yuv_surface->Usize = g_yuv_surface->Ysize / 4;
    g_yuv_surface->Vsize = g_yuv_surface->Ysize / 4;
   
    g_yuv_surface->Yplane = (uint8_t*) malloc (g_yuv_surface->Ysize);
    g_yuv_surface->Uplane = (uint8_t*) malloc (g_yuv_surface->Usize);
    g_yuv_surface->Vplane = (uint8_t*) malloc (g_yuv_surface->Vsize);

    g_yuv_surface->width  = width;
    g_yuv_surface->height = height;

    g_yuv_surface->Ypitch = g_yuv_surface->width;
    g_yuv_surface->Upitch = g_yuv_surface->width / 2;
    g_yuv_surface->Vpitch = g_yuv_surface->width / 2;

    // Setup the threaded access stuff, since this surface is accessed from the vldp thread, too.
    g_yuv_surface->mutex = SDL_CreateMutex();

    if (g_yuv_frect[0])
    {
        g_yuv_frect[0]->w = (float)(width * g_yuv_scale[YUV_H]);
        g_yuv_frect[0]->h = (float)(height * g_yuv_scale[YUV_V]);
        g_yuv_frect[0]->x = (float)((width / 2) - (g_yuv_frect[0]->w / 2));
        g_yuv_frect[0]->y = (float)((height / 2) - (g_yuv_frect[0]->h / 2));
        g_yuv_frect[1] = copy_rect(g_yuv_frect[0]);
    }

#ifdef __LIBRETRO__
    // black, like the visible one: the two are swapped
    memset(g_yuv_surface->Yplane, 0x10, g_yuv_surface->Ysize);
    memset(g_yuv_surface->Uplane, 0x80, g_yuv_surface->Usize);
    memset(g_yuv_surface->Vplane, 0x80, g_yuv_surface->Vsize);

    g_yuv_visible = (m_yuv_surface_t*) malloc (sizeof(m_yuv_surface_t));
    *g_yuv_visible = *g_yuv_surface;
    g_yuv_visible->Yplane = (uint8_t*) malloc (g_yuv_visible->Ysize);
    g_yuv_visible->Uplane = (uint8_t*) malloc (g_yuv_visible->Usize);
    g_yuv_visible->Vplane = (uint8_t*) malloc (g_yuv_visible->Vsize);
    g_yuv_visible->mutex = SDL_CreateMutex();
    memset(g_yuv_visible->Yplane, 0x10, g_yuv_visible->Ysize);
    memset(g_yuv_visible->Uplane, 0x80, g_yuv_visible->Usize);
    memset(g_yuv_visible->Vplane, 0x80, g_yuv_visible->Vsize);
    g_lr_yuv_dirty = true;
#endif
}

#ifdef __LIBRETRO__
// Called by VLDP when the frame it prepared is due to be shown.
void vid_commit_yuv_overlay()
{
    std::lock_guard<std::recursive_mutex> lock(g_lr_yuv_alloc);
    if (!g_yuv_surface || !g_yuv_visible) return;

    m_yuv_surface_t *back = g_yuv_surface, *front = g_yuv_visible;
    SDL_LockMutex(back->mutex);
    SDL_LockMutex(front->mutex);

    // Every vid_update_yuv_overlay() path (blend, luma and grayscale included)
    // rewrites the whole back buffer from the decoder's picture, so the two
    // are exchanged rather than copied.  The frontend side reads the visible
    // one under g_lr_yuv_alloc, which we hold.
    g_yuv_surface = front;
    g_yuv_visible = back;
    g_lr_yuv_dirty = true;

    SDL_UnlockMutex(front->mutex);
    SDL_UnlockMutex(back->mutex);
}

// BT.601 limited range, 16.16 fixed point
#if defined(__aarch64__)
// 16 pixels of one row; same fixed-point math as the scalar loop below, so
// the output is bit-identical.
static inline void yuv_to_xrgb_neon16(const uint8_t *Y, const uint8_t *U, const uint8_t *V,
                                      uint32_t *d)
{
    const uint8x16_t y8 = vld1q_u8(Y);
    const uint8x8_t u8 = vld1_u8(U), v8 = vld1_u8(V);

    // chroma: one sample per two pixels
    const int16x8_t u16 = vreinterpretq_s16_u16(vsubl_u8(u8, vdup_n_u8(128)));
    const int16x8_t v16 = vreinterpretq_s16_u16(vsubl_u8(v8, vdup_n_u8(128)));
    const int16x8x2_t uu = vzipq_s16(u16, u16);
    const int16x8x2_t vv = vzipq_s16(v16, v16);

    const int16x8_t yl = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(y8), vdup_n_u8(16)));
    const int16x8_t yh = vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(y8), vdup_n_u8(16)));
    const int16x4_t ys[4] = {vget_low_s16(yl), vget_high_s16(yl), vget_low_s16(yh), vget_high_s16(yh)};
    const int16x4_t us[4] = {vget_low_s16(uu.val[0]), vget_high_s16(uu.val[0]),
                             vget_low_s16(uu.val[1]), vget_high_s16(uu.val[1])};
    const int16x4_t vs[4] = {vget_low_s16(vv.val[0]), vget_high_s16(vv.val[0]),
                             vget_low_s16(vv.val[1]), vget_high_s16(vv.val[1])};

    uint16x4_t r16[4], g16[4], b16[4];
    for (int i = 0; i < 4; ++i)
    {
        const int32x4_t c = vmulq_n_s32(vmovl_s16(ys[i]), 76309);
        const int32x4_t u = vmovl_s16(us[i]), v = vmovl_s16(vs[i]);
        r16[i] = vqshrun_n_s32(vmlaq_n_s32(c, v, 104597), 16);
        g16[i] = vqshrun_n_s32(vmlaq_n_s32(vmlaq_n_s32(c, u, -25675), v, -53279), 16);
        b16[i] = vqshrun_n_s32(vmlaq_n_s32(c, u, 132201), 16);
    }

    for (int h = 0; h < 2; ++h)
    {
        uint8x8x4_t px;
        px.val[0] = vqmovn_u16(vcombine_u16(b16[2 * h], b16[2 * h + 1]));
        px.val[1] = vqmovn_u16(vcombine_u16(g16[2 * h], g16[2 * h + 1]));
        px.val[2] = vqmovn_u16(vcombine_u16(r16[2 * h], r16[2 * h + 1]));
        px.val[3] = vdup_n_u8(0);
        vst4_u8((uint8_t *)(d + 8 * h), px);
    }
}
#endif

// One row of a YUV420 surface to XRGB8888.  Assumes an even width, as MPEG
// pictures (whole macroblocks) always have: U and V hold width/2 samples.
static void yuv420_row_to_xrgb(const lr_frame &f, int y, uint32_t *d)
{
    const int w = f.w;
    const uint8_t *Y = f.y + (size_t)y * f.ypitch;
    const uint8_t *U = f.u + (size_t)(y >> 1) * f.upitch;
    const uint8_t *V = f.v + (size_t)(y >> 1) * f.vpitch;

    int x = 0;
#if defined(__aarch64__)
    for (; x + 16 <= w; x += 16)
        yuv_to_xrgb_neon16(Y + x, U + (x >> 1), V + (x >> 1), d + x);
#endif
    for (; x < w; ++x)
    {
        const int c = (Y[x] - 16) * 76309;
        const int u = U[x >> 1] - 128;
        const int v = V[x >> 1] - 128;

        int r = (c + 104597 * v) >> 16;
        int g = (c - 25675 * u - 53279 * v) >> 16;
        int b = (c + 132201 * u) >> 16;

        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);

        d[x] = (uint32_t)(r << 16 | g << 8 | b);
    }
}

// What lr_compose_cpu() hands to each band of rows.
struct compose_job {
    const lr_frame *yuv;        // set when the video rows must be converted
    uint32_t *video, *out;
    int w, h;
    const uint8_t *ov;          // RGBA32 overlay; NULL when there is none to blend
    int ov_pitch;
    SDL_Rect lr;
    const int *xmap;
    const uint8_t *orow_used;
};

// Convert (if needed) and composite rows [y0, y1) in one pass, so each row
// is still in cache when it is blended.
static void compose_rows(const compose_job &j, int y0, int y1)
{
    for (int y = y0; y < y1; ++y)
    {
        uint32_t *vrow = j.video + (size_t)y * j.w;
        uint32_t *drow = j.out + (size_t)y * j.w;

        if (j.yuv) yuv420_row_to_xrgb(*j.yuv, y, vrow);

        const int oy = j.ov ? j.lr.y + (int)(((int64_t)y * j.lr.h) / j.h) : 0;
        if (!j.ov || !j.orow_used[oy - j.lr.y])
        {
            memcpy(drow, vrow, (size_t)j.w * sizeof(uint32_t));
            continue;
        }

        // RGBA32 is byte order R,G,B,A
        const uint8_t *orow = j.ov + (size_t)oy * j.ov_pitch;
        for (int x = 0; x < j.w; ++x)
        {
            const uint8_t *p = orow + j.xmap[x];
            const uint32_t a = p[3];

            if (a == 0)
                drow[x] = vrow[x];
            else if (a == 255)
                drow[x] = (uint32_t)(p[0] << 16 | p[1] << 8 | p[2]);
            else
            {
                const uint32_t v = vrow[x];
                const uint32_t ia = 255 - a;
                const uint32_t r = (p[0] * a + ((v >> 16) & 0xff) * ia) / 255;
                const uint32_t g = (p[1] * a + ((v >> 8) & 0xff) * ia) / 255;
                const uint32_t b = (p[2] * a + (v & 0xff) * ia) / 255;
                drow[x] = r << 16 | g << 8 | b;
            }
        }
    }
}

// Two helper threads share the rows with the calling thread, so a 720p
// frame fits in the frontend's frame time next to the game and MPEG decoder
// threads.  Rows go out in small bands to whichever thread is free, so one
// thread being preempted does not hold up a third of the frame.
namespace {
class compose_pool
{
  public:
    static const int WORKERS = 2;
    static const int BANDS   = 16;

    // lr_compose_shutdown() joins the threads; if the frontend exits without
    // unloading, let them go rather than abort in std::thread's destructor
    ~compose_pool()
    {
        for (auto &t : m_threads)
            if (t.joinable()) t.detach();
    }

    void run(const compose_job &job)
    {
        start();
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_job = &job;
            m_next_band = 0;
            m_pending = WORKERS;
            ++m_generation;
        }
        m_wake.notify_all();

        do_bands(job);

        std::unique_lock<std::mutex> l(m_mutex);
        m_done.wait(l, [this] { return m_pending == 0; });
        m_job = NULL;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        for (auto &t : m_threads) t.join();
        m_threads.clear();
        m_quit = false;
    }

  private:
    void do_bands(const compose_job &j)
    {
        for (int b; (b = m_next_band.fetch_add(1)) < BANDS;)
            compose_rows(j, (int)((int64_t)j.h * b / BANDS), (int)((int64_t)j.h * (b + 1) / BANDS));
    }

    void start()
    {
        if (!m_threads.empty()) return;
        for (int i = 0; i < WORKERS; ++i)
            m_threads.emplace_back(&compose_pool::worker, this);
    }

    void worker()
    {
        lr_register_cpu_thread(LR_THREAD_COMPOSE);
        uint64_t seen = 0;
        std::unique_lock<std::mutex> l(m_mutex);
        for (;;)
        {
            m_wake.wait(l, [&] { return m_quit || m_generation != seen; });
            if (m_quit) return;
            seen = m_generation;
            const compose_job *job = m_job;
            l.unlock();

            do_bands(*job);

            l.lock();
            if (--m_pending == 0) m_done.notify_one();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    std::vector<std::thread> m_threads;
    const compose_job *m_job = NULL;
    std::atomic<int> m_next_band{0};
    uint64_t m_generation = 0;
    int m_pending = 0;
    bool m_quit = false;
};

compose_pool g_compose_pool;
}

void lr_compose_shutdown() { g_compose_pool.stop(); }

// What the next frame is made of, shared by the CPU and GPU renderers.
static m_yuv_surface_t *s_frame_locked = NULL;
static std::unique_lock<std::recursive_mutex> s_frame_alloc_lock;
static bool s_frame_force = false;

// What lr_frame_begin() last saw, to tell what changed.
static std::vector<uint8_t> s_ov_copy;   // untracked overlays (see s_ov_tracked)
static const void *s_ov_pixels = NULL;   // tracked overlays: which surface,
static size_t s_ov_bytes = 0;            // and its size
static SDL_Rect s_ov_rect;
static int s_vw = 0, s_vh = 0, s_w = 0, s_h = 0;

// The CPU renderer's buffers: the converted video, the composite, and the
// overlay's column map and used rows.
static std::vector<uint32_t> s_cpu_video, s_cpu_out, s_cpu_prev;
static std::vector<int> s_cpu_xmap;
static std::vector<uint8_t> s_cpu_orow_used;

void lr_frame_invalidate() { s_frame_force = true; }

void lr_overlay_unchanged(SDL_Surface *overlay) { s_ov_same = overlay; }

void lr_frame_reset()
{
    std::vector<uint8_t>().swap(s_ov_copy);
    s_ov_rect = SDL_Rect{0, 0, 0, 0};
    s_ov_pixels = NULL;
    s_ov_bytes = 0;
    s_ov_tracked = false;
    s_ov_dirty = false;
    ov_mirrors_clear();
    s_ov_same = NULL;
    s_vw = s_vh = s_w = s_h = 0;
    std::vector<uint32_t>().swap(s_cpu_video);
    std::vector<uint32_t>().swap(s_cpu_out);
    std::vector<uint32_t>().swap(s_cpu_prev);
    std::vector<int>().swap(s_cpu_xmap);
    std::vector<uint8_t>().swap(s_cpu_orow_used);
    s_frame_force = true;
}

// Collects the visible laserdisc frame and the game overlay, and works out
// what changed since the last call.  Locks the video surface until
// lr_frame_end(), so the VLDP thread cannot swap or reallocate it meanwhile.
void lr_frame_begin(lr_frame *f)
{
    *f = lr_frame();
    const bool force = s_frame_force;
    s_frame_force = false;

    s_frame_alloc_lock = std::unique_lock<std::recursive_mutex>(g_lr_yuv_alloc);
    m_yuv_surface_t *visible = g_yuv_visible;
    if (visible)
    {
        SDL_LockMutex(visible->mutex);
        s_frame_locked = visible;

        if (force || g_lr_yuv_dirty || s_vw != visible->width || s_vh != visible->height)
        {
            s_vw = visible->width;
            s_vh = visible->height;
            g_lr_yuv_dirty = false;
            f->video_changed = true;
        }
        f->y = visible->Yplane;
        f->u = visible->Uplane;
        f->v = visible->Vplane;
        f->ypitch = visible->Ypitch;
        f->upitch = visible->Upitch;
        f->vpitch = visible->Vpitch;
    }

    // The frame is the size of the MPEG video, or the viewport before any
    // video has been seen.
    f->has_video = s_vw != 0;
    f->w = s_vw ? s_vw : g_viewport_width;
    f->h = s_vh ? s_vh : g_viewport_height;
    bool changed = f->video_changed || force;
    if (f->w != s_w || f->h != s_h)
    {
        s_w = f->w;
        s_h = f->h;
        changed = true;
    }

    // Like SDL_RenderTexture: the source rect is clipped to the overlay and
    // what remains is stretched over the whole frame.
    SDL_Surface *ov = g_overlay_surface;
    SDL_Rect lr = {0, 0, 0, 0};
    if (ov)
    {
        const SDL_Rect bounds = {0, 0, ov->w, ov->h};
        if (!SDL_GetRectIntersection(&g_limit_rect, &bounds, &lr))
            lr.w = lr.h = 0;
    }

    // Most games redraw their (small) overlay through several paths, so its
    // pixels are compared with the last composited copy.  Overlays that come
    // through vid_update_overlay_surface report their own changes.
    const size_t ov_bytes = ov ? (size_t)ov->pitch * ov->h : 0;
    bool ov_changed;
    if (s_ov_tracked)
    {
        const void *pixels = ov ? ov->pixels : NULL;
        ov_changed = s_ov_dirty || pixels != s_ov_pixels || ov_bytes != s_ov_bytes;
        s_ov_pixels = pixels;
        s_ov_bytes = ov_bytes;
        if (!s_ov_copy.empty()) std::vector<uint8_t>().swap(s_ov_copy);
    }
    else
        ov_changed = ov_bytes != s_ov_copy.size() ||
                     (ov_bytes && memcmp(s_ov_copy.data(), ov->pixels, ov_bytes) != 0);
    s_ov_dirty = false;
    if (force || ov_changed || !SDL_RectsEqual(&lr, &s_ov_rect))
    {
        if (!s_ov_tracked)
            s_ov_copy.assign((const uint8_t *)(ov ? ov->pixels : NULL),
                             (const uint8_t *)(ov ? ov->pixels : NULL) + ov_bytes);
        s_ov_rect = lr;
        f->ov_changed = true;
        changed = true;
    }

    if (ov && lr.w > 0 && lr.h > 0)
    {
        f->ov = (const uint8_t *)ov->pixels;
        f->ov_pitch = ov->pitch;
        f->ov_w = ov->w;
        f->ov_h = ov->h;
        f->lr = lr;
    }

    f->unchanged = !changed;
    f->compose = changed;

    // DAPHNE_LR_VERIFY_DUPES=1: recomposite frames reported as unchanged and
    // check they really are (development aid)
    static const bool s_verify = SDL_getenv("DAPHNE_LR_VERIFY_DUPES") != NULL;
    if (s_verify && !changed)
    {
        f->verify_dupe = true;
        f->compose = true;
    }

    int aw = f->w, ah = f->h;
    if (!VIDEO_HAS(IGNORE_ASPECT) && g_aspect_width > 0 && g_aspect_height > 0)
    {
        aw = g_aspect_width;
        ah = g_aspect_height;
    }
    f->aspect = VIDEO_HAS(FORCE_ASPECT) ? 4.0f / 3.0f : (float)aw / (float)ah;
}

void lr_frame_end()
{
    if (s_frame_locked) SDL_UnlockMutex(s_frame_locked->mutex);
    s_frame_locked = NULL;
    if (s_frame_alloc_lock.owns_lock()) s_frame_alloc_lock.unlock();
}

// The CPU renderer: composites the frame into XRGB8888.  Only does any work
// when f.compose is set; otherwise returns the previous frame.
const uint32_t *lr_compose_cpu(const lr_frame &f)
{
    std::vector<uint32_t> &s_video = s_cpu_video, &s_out = s_cpu_out, &s_prev = s_cpu_prev;
    std::vector<int> &s_xmap = s_cpu_xmap;
    std::vector<uint8_t> &s_orow_used = s_cpu_orow_used;
    const size_t n = (size_t)f.w * f.h;
    const lr_frame *convert = NULL;

    if (f.video_changed && f.y)
    {
        s_video.resize(n);
        convert = &f;
    }
    else if (s_video.size() != n)
        s_video.assign(n, 0); // no video yet: black
    if (s_out.size() != n) s_out.resize(n);

    if (f.verify_dupe) s_prev = s_out;

    if (f.compose)
    {
        compose_job job = {convert, s_video.data(), s_out.data(), f.w, f.h, NULL, 0, f.lr, NULL, NULL};

        if (f.ov)
        {
            const SDL_Rect &lr = f.lr;
            if ((int)s_xmap.size() != f.w) s_xmap.resize(f.w);
            for (int x = 0; x < f.w; ++x)
                s_xmap[x] = (lr.x + (int)(((int64_t)x * lr.w) / f.w)) * 4;

            // Overlays are mostly transparent: find the rows with any visible
            // pixel once, and copy the video straight through everywhere else.
            s_orow_used.assign(lr.h, 0);
            for (int oy = 0; oy < lr.h; ++oy)
            {
                const uint8_t *o = f.ov + (size_t)(lr.y + oy) * f.ov_pitch + (size_t)lr.x * 4;
                uint8_t any = 0;
                for (int x = 0; x < lr.w; ++x) any |= o[4 * x + 3]; // alpha
                s_orow_used[oy] = any != 0;
            }

            job.ov = f.ov;
            job.ov_pitch = f.ov_pitch;
            job.xmap = s_xmap.data();
            job.orow_used = s_orow_used.data();
        }

        g_compose_pool.run(job);
    }

    if (f.verify_dupe && s_prev != s_out)
    {
        LOGE << "repeated frame differs from a full composite";
    }

    return s_out.data();
}
#endif

static void vid_flash_yuv_surface()
{
    // White: YUV#ED8080 - D4 = 90%
    memset(g_yuv_surface->Yplane, 0xd4, g_yuv_surface->Ysize);
    memset(g_yuv_surface->Uplane, 0x80, g_yuv_surface->Usize);
    memset(g_yuv_surface->Vplane, 0x80, g_yuv_surface->Vsize);
}

static void vid_blank_yuv_surface()
{
    // Blue: YUV#1DEB6B - Black: YUV#108080
    uint8_t Y_value = VIDEO_HAS(YUV_BLUE) ? 0x1d : 0x10;
    uint8_t U_value = VIDEO_HAS(YUV_BLUE) ? 0xeb : 0x80;
    uint8_t V_value = VIDEO_HAS(YUV_BLUE) ? 0x6b : 0x80;

    memset(g_yuv_surface->Yplane, Y_value, g_yuv_surface->Ysize);
    memset(g_yuv_surface->Uplane, U_value, g_yuv_surface->Usize);
    memset(g_yuv_surface->Vplane, V_value, g_yuv_surface->Vsize);
}

static void vid_blank_yuv_texture()
 {
    if (!g_yuv_texture) return;

    float w, h;
    SDL_GetTextureSize(g_yuv_texture, &w, &h);

    uint8_t *y_plane = NULL;
    uint8_t *u_plane = NULL;
    uint8_t *v_plane = NULL;

    uint8_t Y_value = VIDEO_HAS(YUV_BLUE) ? 0x1d : 0x10;
    uint8_t U_value = VIDEO_HAS(YUV_BLUE) ? 0xeb : 0x80;
    uint8_t V_value = VIDEO_HAS(YUV_BLUE) ? 0x6b : 0x80;

    y_plane = (uint8_t *)malloc(w * h);
    u_plane = (uint8_t *)malloc(w * h / 4);
    v_plane = (uint8_t *)malloc(w * h / 4);

    memset(y_plane, Y_value, w * h);
    memset(u_plane, U_value, w * h / 4);
    memset(v_plane, V_value, w * h / 4);

    SDL_UpdateYUVTexture(g_yuv_texture, NULL, y_plane, w,
                             u_plane, w / 2, v_plane, w / 2);
    free(y_plane);
    free(u_plane);
    free(v_plane);
}

static inline void blendPlane(const uint8_t *src, uint8_t *dst, int w, int h,
	int srcPitch, int dstPitch)
{
    for (int y = 0; y < h; ++y)
    {
        const uint8_t *line0 = src + ((y > 0 ? y - 1 : y) * srcPitch);
        const uint8_t *line1 = src + ( y * srcPitch);
        const uint8_t *line2 = src + ((y < h - 1 ? y + 1 : y) * srcPitch);
        uint8_t *row = dst + y * dstPitch;

        for (int x = 0; x < w; ++x)
            row[x] = (uint8_t)(((uint32_t)
		    line0[x] + line1[x] + line2[x] + 1U) * 341U >> 10);
    }
}

static inline void lumaControl(const uint8_t *src, uint8_t *dst, int width, int height,
        int srcPitch, int dstPitch)
{
    for (int y = 0; y < height; y++)
    {
        const uint8_t *s = src + y * srcPitch;
        uint8_t *d = dst + y * dstPitch;

        for (int x = 0; x < width; x++)
        {
            int yv = s[x];
            int val = yv + ((yv * g_yuv_luma) >> 3);

            if (val < 0) val = 0;
            else if (val > 255) val = 255;

            d[x] = (uint8_t)val;
        }
    }
}

static SDL_Texture *vid_create_yuv_texture(int width, int height)
{
    g_yuv_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_YV12,
        g_texture_access, width, height);

    g_yuv_skip = true;

    if (!g_yuv_texture)
    {
        LOGE << fmt("Could not initialize g_yuv_texture %s", SDL_GetError());
        set_quitflag();
    }
    else if (g_yuv_flags & YUV_FLAG_BLEND)
            SDL_SetTextureScaleMode(g_yuv_texture, SDL_SCALEMODE_LINEAR);
    else
            SDL_SetTextureScaleMode(g_yuv_texture, SDL_SCALEMODE_NEAREST);

    return g_yuv_texture;
}

// REMEMBER it updates the YUV surface ONLY: the YUV texture is updated on vid_blit().
int vid_update_yuv_overlay(uint8_t *Yplane, uint8_t *Uplane, uint8_t *Vplane,
	int Ypitch, int Upitch, int Vpitch)
{
    // This function is called from the vldp thread, so access to the
    // yuv surface is protected (mutexed).
    // As a reminder, mutexes are very simple: this fn tries to lock(=get)
    // the mutex, but if it's already taken, LockMutex doesn't return
    // until the mutex is free and we can lock(=get) it here.

#ifdef __LIBRETRO__
    std::lock_guard<std::recursive_mutex> alloc_lock(g_lr_yuv_alloc);
    if (!g_yuv_surface) return 0;
#endif
    SDL_LockMutex(g_yuv_surface->mutex);

    switch(g_yuv_display)
    {
    case YUV_BLANK:
        vid_blank_yuv_surface();
        break;
    case YUV_SHUTTER:
        vid_blank_yuv_surface();
        g_yuv_display = YUV_VISIBLE;
        break;
    case YUV_FLASH:
        vid_flash_yuv_surface();
        g_yuv_display = YUV_VISIBLE;
        break;
    default:
        if (!g_yuv_flags)
        {
            copyPlane(g_yuv_surface->Yplane, Yplane,
                g_yuv_surface->width, g_yuv_surface->height, Ypitch);
            copyPlane(g_yuv_surface->Uplane, Uplane,
                g_yuv_surface->width / 2, g_yuv_surface->height / 2, Upitch);
            copyPlane(g_yuv_surface->Vplane, Vplane,
                g_yuv_surface->width / 2, g_yuv_surface->height / 2, Vpitch);
            break;
        }

        if (g_yuv_flags & YUV_FLAG_BLEND)
        {
            blendPlane(Yplane, g_yuv_surface->Yplane, g_yuv_surface->width,
                g_yuv_surface->height, Ypitch, g_yuv_surface->Ypitch);
        }
        else
        {
            copyPlane(g_yuv_surface->Yplane, Yplane,
                g_yuv_surface->width, g_yuv_surface->height, Ypitch);
        }

        if (g_yuv_flags & YUV_FLAG_LUMA)
        {
            lumaControl(Yplane, g_yuv_surface->Yplane, g_yuv_surface->width,
                g_yuv_surface->height, Ypitch, g_yuv_surface->Ypitch);
        }

        if (g_yuv_flags & YUV_FLAG_GRAYSCALE)
        {
            memset(g_yuv_surface->Uplane, 0x80, g_yuv_surface->Usize);
            memset(g_yuv_surface->Vplane, 0x80, g_yuv_surface->Vsize);
        }
        else
        {
            int w = g_yuv_surface->width  >> 1;
            int h = g_yuv_surface->height >> 1;
            blendPlane(Uplane, g_yuv_surface->Uplane, w, h, Upitch, g_yuv_surface->Upitch);
            blendPlane(Vplane, g_yuv_surface->Vplane, w, h, Vpitch, g_yuv_surface->Vpitch);
        }
        break;
    }

    g_yuv_surface->Ypitch = g_yuv_surface->width;
    g_yuv_surface->Upitch = g_yuv_surface->width / 2;
    g_yuv_surface->Vpitch = g_yuv_surface->width / 2;

    g_yuv_video_needs_update = true;

    SDL_UnlockMutex(g_yuv_surface->mutex);

    return 0;
}

#ifdef __LIBRETRO__
// One row of interleaved U/V samples into separate U and V rows.
void lr_deinterleave_uv(const uint8_t *uv, uint8_t *u, uint8_t *v, int n)
{
    int i = 0;
#if defined(__aarch64__)
    for (; i + 16 <= n; i += 16) {
        uint8x16x2_t p = vld2q_u8(uv + 2 * i);
        vst1q_u8(u + i, p.val[0]);
        vst1q_u8(v + i, p.val[1]);
    }
#endif
    for (; i < n; ++i) {
        u[i] = uv[2 * i];
        v[i] = uv[2 * i + 1];
    }
}

int lr_update_yuv_overlay_nv12(const uint8_t *Yplane, int Ypitch, const uint8_t *UVplane, int UVpitch)
{
    std::unique_lock<std::recursive_mutex> alloc_lock(g_lr_yuv_alloc);
    if (!g_yuv_surface) return 0;
    const int cw = g_yuv_surface->width / 2, ch = g_yuv_surface->height / 2;

    // Blanking, flashing and the picture effects stay with the planar path.
    if (g_yuv_display != YUV_VISIBLE || g_yuv_flags) {
        // scratch planes kept between pictures (VLDP thread only)
        static std::vector<uint8_t> u, v;
        u.resize((size_t)cw * ch);
        v.resize((size_t)cw * ch);
        for (int r = 0; r < ch; ++r)
            lr_deinterleave_uv(UVplane + (size_t)r * UVpitch, &u[(size_t)r * cw], &v[(size_t)r * cw], cw);
        return vid_update_yuv_overlay((uint8_t *)Yplane, u.data(), v.data(), Ypitch, cw, cw);
    }

    SDL_LockMutex(g_yuv_surface->mutex);
    copyPlane(g_yuv_surface->Yplane, Yplane, g_yuv_surface->width, g_yuv_surface->height, Ypitch);
    for (int r = 0; r < ch; ++r)
        lr_deinterleave_uv(UVplane + (size_t)r * UVpitch, g_yuv_surface->Uplane + (size_t)r * cw,
                        g_yuv_surface->Vplane + (size_t)r * cw, cw);
    g_yuv_surface->Ypitch = g_yuv_surface->width;
    g_yuv_surface->Upitch = cw;
    g_yuv_surface->Vpitch = cw;
    g_yuv_video_needs_update = true;
    SDL_UnlockMutex(g_yuv_surface->mutex);
    return 0;
}
#endif

void vid_update_overlay_surface(SDL_Surface *overlay)
{
    if (VIDEO_HAS(BLOCK_DRIVER_OVERLAY))
        return;

    SDL_Rect src = (SDL_Rect){0, 0, overlay->w, overlay->h};
    SDL_Rect* dst = NULL;

    if (VIDEO_HAS(ENHANCE_OVERLAY))
    {
        g_limit_rect.w = overlay->w - (g_limit_rect.x << 1);
        g_limit_rect.h = overlay->h - (g_limit_rect.y << 1);
        dst = &g_limit_rect;
    }

    if (VIDEO_HAS(OVERLAY_DYNAMIC))
        g_limit_rect = src;

    // else texture was updated in driver
    if (VIDEO_HAS(INDEX8))
    {
        SDL_BlitSurface(overlay, NULL, g_overlay_surface, dst);
#ifdef __LIBRETRO__
        if (g_overlay_texture) // none in the core: it composites the surface
#endif
        SDL_UpdateTexture(g_overlay_texture, &src,
             (void *)g_overlay_surface->pixels, g_overlay_surface->pitch);
    }
#ifdef __LIBRETRO__
    else
    {
        // Singe sets its overlay's size from the script (overlaySetResolution);
        // the copy follows, as the overlay texture does in a desktop build
        if (g_overlay_surface &&
            (g_overlay_surface->w != overlay->w || g_overlay_surface->h != overlay->h))
        {
            SDL_Surface *s = SDL_CreateSurface(overlay->w, overlay->h, SDL_PIXELFORMAT_RGBA32);
            if (s)
            {
                SDL_DestroySurface(g_overlay_surface);
                g_overlay_surface = s;
                ov_mirrors_clear();
                g_overlay_width = overlay->w;
                g_overlay_height = overlay->h;
                if (!VIDEO_HAS(ENHANCE_OVERLAY)) g_limit_rect = src;
            }
        }
        // No texture to upload to: copy the RGBA overlay, only the rows that
        // changed, and note whether any did.  Singe overlays can be full HD,
        // so this replaces a full copy here plus a full compare every frame
        // in lr_frame_begin().
        if (!g_overlay_surface)
            return;
        // (a blit to dst only uses its position)
        const int dx = dst ? dst->x : 0, dy = dst ? dst->y : 0;
        const bool same = s_ov_same == overlay;
        s_ov_same = NULL;
        if (same && ov_mirrors_has(overlay) && dx == s_ov_mirror_dx && dy == s_ov_mirror_dy)
        {
            s_ov_tracked = true; // the game drew nothing new since the last copy
            return;
        }
        if (overlay->format == g_overlay_surface->format &&
            SDL_BYTESPERPIXEL(overlay->format) == 4 && dx >= 0 && dy >= 0 &&
            dx + overlay->w <= g_overlay_surface->w && dy + overlay->h <= g_overlay_surface->h)
        {
            const size_t row = (size_t)overlay->w * 4;
            bool copied = false;
            for (int y = 0; y < overlay->h; ++y)
            {
                const uint8_t *s = (const uint8_t *)overlay->pixels + (size_t)y * overlay->pitch;
                uint8_t *d = (uint8_t *)g_overlay_surface->pixels +
                             (size_t)(y + dy) * g_overlay_surface->pitch + (size_t)dx * 4;
                if (memcmp(d, s, row) != 0)
                {
                    memcpy(d, s, row);
                    copied = true;
                }
            }
            // unchanged: it also still equals the overlays it equalled before
            if (copied || dx != s_ov_mirror_dx || dy != s_ov_mirror_dy)
            {
                s_ov_dirty = true;
                ov_mirrors_clear();
            }
            ov_mirrors_add(overlay);
            s_ov_mirror_dx = dx;
            s_ov_mirror_dy = dy;
        }
        else
        {
            SDL_SetSurfaceBlendMode(overlay, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(overlay, NULL, g_overlay_surface, dst);
            s_ov_dirty = true;
            ov_mirrors_clear();
        }
        s_ov_tracked = true;
    }
#endif
}

static void take_screenshot()
{
    struct       stat info;
    char         filename[64];
    int32_t      screenshot_num = 0;
    const char   dir[12] = "screenshots";

    if (stat(dir, &info ) != 0 )
    {
         LOGW << fmt("'%s' directory does not exist.", dir);
         return;
    }
    else if (!(info.st_mode & S_IFDIR))
    {
        LOGW << fmt("'%s' is not a directory.", dir);
        return;
    }
    else if (g_display != 0)
    {
         LOGE << "Screenshots require the primary display.";
         return;
    }

    SDL_Surface *screenshot = NULL;

    if (VIDEO_HAS(FULLSCREEN)) SDL_GetDisplayBounds(0, &g_logical_rect);
    else SDL_GetCurrentRenderOutputSize(g_renderer, &g_logical_rect.w, &g_logical_rect.h);

    screenshot = SDL_RenderReadPixels(g_renderer, &g_logical_rect);

    if (!screenshot)
    {
        LOGE << fmt("Cannot ReadPixels - Something bad happened: %s", SDL_GetError());
        set_quitflag();
    }

    for (;;)
    {
        screenshot_num++;
        snprintf(filename, sizeof(filename), "%s%shypseus-%d.png",
          dir, PATH_SEPARATOR, screenshot_num);

        if (!mpo_file_exists(filename))
            break;
    }

    if (IMG_SavePNG(screenshot, filename))
        LOGI << fmt("Wrote screenshot: %s", filename);
    else
        LOGE <<  fmt("Could not write screenshot: %s !! - %s", filename, SDL_GetError());

    SDL_DestroySurface(screenshot);
}

static void draw_scanlines(int l)
{
    int xe = g_scaling_rect.x + g_scaling_rect.w;
    int ye = g_scaling_rect.y + g_scaling_rect.h;

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, g_scanline_alpha);

    if (g_aspect_ratio == ASPECTPD)
    {
        for (int i = 0; i < g_scaling_rect.w; i += l)
        {
            int x = (int)(g_scaling_rect.x + i);
            SDL_RenderLine(g_renderer, x, g_scaling_rect.y, x, ye);
        }
    }
    else
    {
        for (int i = 0; i < g_scaling_rect.h; i += l)
        {
            int y = (int)(g_scaling_rect.y + i);
            SDL_RenderLine(g_renderer, g_scaling_rect.x, y, xe, y);
        }
    }

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
}

static void draw_border(int s, int c)
{
    SDL_FRect tb, lb, rb, bb;
    unsigned char r = 0xff, g = 0xff, b = 0xff;

    switch(c)
    {
       case 0x62:
          r = 0x00; g = 0x66; b = 0xff;
          break;
       case 0x67:
          r = 0x66; g = 0xff; b = 0x40;
          break;
       case 0x72:
          r = 0xff; g = 0x1a; b = 0x1a;
          break;
       case 0x78:
          r = 0x00; g = 0x00; b = 0x00;
          break;
    }

    SDL_SetRenderDrawColor(g_renderer, r, g, b, SDL_ALPHA_OPAQUE);

    tb.x = lb.x = bb.x = g_border_rect.x;
    tb.y = lb.y = rb.y = g_border_rect.y;
    rb.x = (g_border_rect.w + g_border_rect.x) - s;
    bb.y = (g_border_rect.h - s) + g_border_rect.y;
    tb.w = bb.w = g_border_rect.w;
    tb.h = bb.h = lb.w = rb.w = s;
    lb.h = rb.h = g_border_rect.h;

    SDL_RenderFillRect(g_renderer, &tb);
    SDL_RenderFillRect(g_renderer, &lb);
    SDL_RenderFillRect(g_renderer, &rb);
    SDL_RenderFillRect(g_renderer, &bb);

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
}

static bool mixTexture(m_textureT *mix)
{
    if (mix->t && mix->w == g_logical_rect.w && mix->h == g_logical_rect.h)
        return true;

    SDL_Texture *temp = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET, g_logical_rect.w, g_logical_rect.h);

    if (!temp)
    {
        LOGE << fmt("Mixing texture creation failed: %s", SDL_GetError());
        set_quitflag();
        return false;
    }

    SDL_SetTextureBlendMode(temp, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(temp, 0xff);
    SDL_SetTextureScaleMode(temp, VIDEO_HAS(SCALE_LINEAR) ?
          SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

    if (mix->t) SDL_DestroyTexture(mix->t);

    mix->t = temp;
    mix->w = g_logical_rect.w;
    mix->h = g_logical_rect.h;

    return true;
}

static void vid_render_aux()
{
    if (g_aux_texture)
    {
        SDL_UpdateTexture(g_aux_texture, &g_annu_rect,
            (void *)g_aux_blit_surface->pixels, g_aux_blit_surface->pitch);

        g_aux_needs_update = false;
    }
    else
    {
        if (VIDEO_HAS(KEYBOARD_BEZEL))
        {
            g_aux_needs_update = false;
            g_aux_texture = IMG_LoadTexture(g_renderer, g_tqkeys.c_str());

            if (!g_aux_texture)
            {
                LOGE << fmt("Failed on keyboard texture: %s, %s",
                             g_tqkeys.c_str(), SDL_GetError());
                set_quitflag();
                return;
            }

            SDL_FPoint size;
            SDL_GetTextureSize(g_aux_texture, &size.x, &size.y);

            g_aux_ratio = size.y / size.x;

            calc_kb_rect();
            LogicalPosition(&g_logical_rect, &g_aux_rect, 50, 100);

            // argument override
            if (aux_bezel_pos_x || aux_bezel_pos_y)
            {
                g_aux_rect.x = aux_bezel_pos_x;
                g_aux_rect.y = aux_bezel_pos_y;
            }
        }
        else
        {
            g_aux_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
                                g_texture_access, g_anun_w, g_anun_h);

            if (!g_aux_texture)
            {
                LOGE << fmt("Failed on annunciator texture: %s", SDL_GetError());
                set_quitflag();
            }
            else SDL_SetTextureBlendMode(g_aux_texture, SDL_BLENDMODE_BLEND);
        }
    }
}

static void vid_render_texture(SDL_Texture *texture, SDL_Rect rect)
{
    if (texture)
    {
        SDL_FRect frect;
        SDL_RectToFRect(&rect, &frect);
        SDL_RenderTexture(g_renderer, texture, NULL, &frect);
    }
}

static void vid_render_bezels()
{
    if (!VIDEO_HAS(BEZEL_LOAD))
    {
        if (!g_bezel_file.empty() && !g_bezel_texture)
        {
            size_t fileSize = 0;
            std::string bezelpath =  g_bezel_path + std::string(PATH_SEPARATOR) + g_bezel_file;

            void* fileData = SDL_LoadFile(bezelpath.c_str(), &fileSize);

            g_bezelhash = calcHash(fileData, fileSize);

            if (g_bezelhash > 0)
            {
                SDL_IOStream* stream = SDL_IOFromConstMem(fileData, fileSize);

                g_bezel_texture = stream ? IMG_LoadTexture_IO(g_renderer, stream, true) : nullptr;
            }

            SDL_free(fileData);

            if (g_bezel_texture)
            {
                LOGI << fmt("Loaded bezel file: %s", bezelpath.c_str());
                SDL_SetTextureScaleMode(g_bezel_texture, VIDEO_HAS(SCALE_LINEAR) ?
                              SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            }
            else
            {
                LOGW << fmt("Failed to load bezel: %s", bezelpath.c_str());
                VIDEO_CLEAR(BEZEL_OVERLAY | OVERLAY_LAST);
            }
        }
        VIDEO_SET(BEZEL_LOAD);
    }

    if (VIDEO_HAS(BEZEL_REVERSE))
    {
        vid_render_texture(g_scoreboard_texture, g_scoreboard_bezel_rect);
        vid_render_texture(g_aux_texture, g_aux_rect);
        vid_render_texture(g_bezel_texture, g_bezel_rect);
    }
    else
    {
        vid_render_texture(g_bezel_texture, g_bezel_rect);
        vid_render_texture(g_aux_texture, g_aux_rect);
        vid_render_texture(g_scoreboard_texture, g_scoreboard_bezel_rect);
    }

    if (VIDEO_HAS(OVERLAY_LAST))
        SDL_RenderTexture(g_renderer, g_overlay_texture, &fOverlayRect, &fScaleRect);
}

void vid_blit()
{
#ifdef __LIBRETRO__
    // the frontend composites once per retro_run() (lr_frame_begin() and
    // lr_compose_cpu() or a GPU renderer)
    return;
#endif
    // *IF* we get to SDL_VIDEO_BLIT from game::blit(), then the access to the
    // overlay and scoreboard textures is done from the "hypseus" thread, that blocks
    // until all blitting operations are completed and only then loops again, so NO
    // need to protect the access to these surfaces or their needs_update booleans.
    // However, since we get here from game::blit(), the yuv "surface" is accessed
    // simultaneously from the vldp thread and from here, the main thread (to update
    // the YUV texture from the YUV surface), so access to that surface and it's
    // boolean DO need to be protected with a mutex.

    // Handle any rescaling that occurred.
    switch(g_rescale)
    {
    case 1:
       format_fullscreen_render();
       break;
    case 2:
       format_window_render();
       break;
    default:
       break;
    }

    // Clear the renderer before the SDL_RenderTexture() calls for this frame.
    // Prevents stroboscopic effects on the background in fullscreen mode,
    // and is recommended by SDL_Rendercopy() documentation.

    if (!mixTexture(&g_mix_texture)) return;

    SDL_SetRenderTarget(g_renderer, g_mix_texture.t);
    SDL_RenderClear(g_renderer);

    // Does YUV texture need update from the YUV surface
    if (g_yuv_surface)
    {
        SDL_LockMutex(g_yuv_surface->mutex);

        if (g_yuv_video_needs_update)
        {
            if (!g_yuv_texture)
            {
                g_yuv_texture = vid_create_yuv_texture(
                    g_yuv_surface->width, g_yuv_surface->height);
            }

            g_yuv_video_needs_update = false;

            if (g_yuv_skip)
            {
                if (g_yuv_display == YUV_VISIBLE)
                    g_yuv_skip = false;

                vid_blank_yuv_texture();
            }
            else SDL_UpdateYUVTexture(g_yuv_texture, NULL,
                g_yuv_surface->Yplane, g_yuv_surface->Ypitch,
                g_yuv_surface->Uplane, g_yuv_surface->Upitch,
                g_yuv_surface->Vplane, g_yuv_surface->Vpitch);
        }

        SDL_UnlockMutex(g_yuv_surface->mutex);
    }

    // Does OVERLAY texture need update from the local surfaces
    if (VIDEO_HAS(BLOCK_DRIVER_OVERLAY))
    {
        SDL_UpdateTexture(g_overlay_texture, &g_local_size_rect,
	    (void *)g_overlay_surface->pixels, g_overlay_surface->pitch);
    }

    SDL_RectToFRect(&g_scaling_rect, &fScaleRect);

    if (g_yuv_texture)
        SDL_RenderTexture(g_renderer, g_yuv_texture, g_yuv_frect[0], &fScaleRect);

    SDL_RectToFRect(&g_limit_rect, &fOverlayRect);

    if (!VIDEO_HAS(OVERLAY_LAST))
        SDL_RenderTexture(g_renderer, g_overlay_texture, &fOverlayRect, &fScaleRect);

    if (g_aux_needs_update) vid_render_aux();

    if (VIDEO_HAS(SCANLINES))
        draw_scanlines(g_scanline_shunt);

    if (g_game->get_outline_border())
        draw_border(g_game->get_outline_border(),
            g_game->get_outline_border_color());

    SDL_SetRenderTarget(g_renderer, NULL);
    SDL_RenderClear(g_renderer);

    if (VIDEO_HAS(ROTATED_MEDIA))
        SDL_RenderTextureRotated(g_renderer, g_mix_texture.t,  &fScaleRect,
             &fScaleRect, g_fRotateDegrees, NULL, SDL_FLIP_NONE);
    else
        SDL_RenderTexture(g_renderer, g_mix_texture.t, &fScaleRect,  &fScaleRect);

    // If there's a subtitle overlay
    if (g_bSubtitleShown)
    {
        if (g_bSubtitleShown & (1 << 0 ))
            draw_subtitle(subchar, 0);
        else draw_srt(srtchar, 0);
    }

    if (VIDEO_HAS(BEZEL_TOGGLE)) vid_render_bezels();

    if (VIDEO_HAS(TAKE_SCREENSHOT))
    {
        VIDEO_CLEAR(TAKE_SCREENSHOT);
        take_screenshot();
    }

    SDL_RenderPresent(g_renderer);

    if (g_scoreboard_needs_update)
    {
        SDL_RenderPresent(g_scoreboard_renderer);
        g_scoreboard_needs_update = false;
    }

    g_rescale = 0;
}

int get_yuv_overlay_width()
{
    if (g_yuv_surface) return g_yuv_surface->width;
    return 0;
}

int get_yuv_overlay_height()
{
    if (g_yuv_surface) return g_yuv_surface->height;
    return 0;
}

bool get_yuv_overlay_ready()
{
#ifdef __LIBRETRO__
    return g_yuv_surface != NULL;
#endif
    if (g_yuv_surface && g_yuv_texture) return true;
    return false;
}

void set_overlay_offset(int offsetx, int offsety)
{
    g_limit_rect.x = offsetx;
    g_limit_rect.y = offsety;
}

void notify_stats(int overlaywidth, int overlayheight, const char* input)
{
    char s[4] = {0};
    if (!VIDEO_HAS(OVERLAY_DYNAMIC)) snprintf(s, sizeof(s), "[s]");

    LOGI << fmt("Viewport Stats:|w:%dx%d|v:%dx%d|o:%dx%d%s|l:%dx%d|%s",
         g_viewport_width, g_viewport_height, g_probe_width,
           g_probe_height, overlaywidth, overlayheight, s,
             g_logical_rect.w, g_logical_rect.h, input);
}

void notify_positions()
{
    char s[] = "positions logged";

    LOGI << fmt("Scale video (0): %d", g_scalefactor);
    LOGI << fmt("Shift video (0): x:%d, y:%d", (g_scale_h_shift - 0x64),
                         (g_scale_v_shift - 0x64));

    if (g_scoreboard_texture)
    {
        LOGI << fmt("Scale score (1): %d", g_scoreboard_bezel_scale);
        LOGI << fmt("Shift score (1): x:%d, y:%d", scoreboard_window_pos_x, scoreboard_window_pos_y);
    }

    if (g_aux_texture)
    {
        LOGI << fmt("Scale aux (2): %d", g_aux_bezel_scale);
        LOGI << fmt("Shift aux (2): x:%d, y:%d", g_aux_rect.x, g_aux_rect.y);
    }

    LOGI << fmt("Rotation: %d\xC2\xB0", (int)g_fRotateDegrees);

    draw_subtitle(s, 1, true);
}

}
