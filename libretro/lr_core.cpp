/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * libretro front end for the Hypseus/Daphne laserdisc engine.
 *
 * The engine was written as a program with its own main loop.  Here that loop
 * runs inside a libco coroutine:
 *
 *   - lr_ticks() is a virtual millisecond clock that retro_run() advances by
 *     exactly one video frame.
 *   - When the engine waits for time to pass (SDL_Delay -> lr_delay) on the
 *     coroutine, control returns to retro_run().  The CPU throttle in
 *     cpu::execute therefore runs the emulated CPUs for exactly one frame of
 *     emulated time per retro_run().
 *   - The VLDP MPEG decoder keeps its own thread and is slaved to the emulated
 *     clock.  Before presenting, retro_run() waits for it to catch up so each
 *     frame shows the laserdisc picture that is due at that emulated time.
 *   - Audio is mixed on demand: one frame's worth of samples per retro_run().
 */

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <libretro.h>
#include <libco.h>

#include <plog/Log.h>
#include <plog/Appenders/IAppender.h>
#include <plog/Formatters/FuncMessageFormatter.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#endif
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "lr_assets.h"
#include "lr_gl.h"
#include "lr_vk.h"
#include "lr_threads.h"
// the engine's hooks without its SDL renames, which this file implements
#define LR_COMPAT_NO_RENAMES
#include "lr_compat.h"

#include "hypseus.h"
#include "cpu/cpu.h"
#include "game/game.h"
#include "ldp-out/ldp.h"
#include "io/cmdline.h"
#include "io/homedir.h"
#include "io/input.h"
#include "sound/sound.h"
#include "video/video.h"
#include "vldp/vldp.h"

extern game *g_game;
extern ldp *g_ldp;
static bool g_debug_status; // DAPHNE_LR_LOG=debug|verbose
static bool g_profile;      // DAPHNE_LR_PROFILE=1: per-frame timing summary

#define CORE_NAME     "Daphne (HypseusHW)"
// LR_VERSION comes from display_version in hypseushw_libretro.info (CMake);
// a development build can add its build time (-DLR_BUILD_STAMP=ON) to tell
// installed builds apart in RetroArch's Core Information
#ifdef LR_BUILD_STAMP
#define CORE_VERSION  LR_VERSION " (Hypseus Singe " hypseus_VERSION ", built " __DATE__ " " __TIME__ ")"
#else
#define CORE_VERSION  LR_VERSION " (Hypseus Singe " hypseus_VERSION ")"
#endif

static const double   LR_FPS         = 60000.0 / 1001.0;  // NTSC field rate
static const unsigned LR_SAMPLE_RATE = 44100;
static const unsigned LR_MAX_WIDTH   = 1920;
static const unsigned LR_MAX_HEIGHT  = 1080;

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void lr_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (log_cb)
        log_cb(level, "[Daphne] %s\n", buf);
    else
        fprintf(stderr, "[Daphne] %s\n", buf);
}

namespace {
// Routes the engine's plog output to the frontend log.  The decoder and audio
// threads log too, so log_cb is also called off the frontend's thread: the
// one libretro call made there, which frontends' loggers accept.
class RetroAppender : public plog::IAppender
{
  public:
    void write(const plog::Record &record) override
    {
        enum retro_log_level level = RETRO_LOG_INFO;
        switch (record.getSeverity())
        {
        case plog::fatal:
        case plog::error:   level = RETRO_LOG_ERROR; break;
        case plog::warning: level = RETRO_LOG_WARN; break;
        case plog::info:    level = RETRO_LOG_INFO; break;
        default:            level = RETRO_LOG_DEBUG; break;
        }
        plog::util::nstring msg = plog::FuncMessageFormatter::format(record);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
#ifdef _WIN32
        lr_log(level, "%s", plog::util::toNarrow(msg, plog::codePage::kActive).c_str());
#else
        lr_log(level, "%s", msg.c_str());
#endif
    }
};
RetroAppender g_appender;
}

// ---------------------------------------------------------------------------
// Virtual clock and coroutine
// ---------------------------------------------------------------------------

static cothread_t   g_main_co;
static cothread_t   g_emu_co;
static SDL_ThreadID g_main_thread;     // thread that drives the coroutine
static bool         g_in_emu;          // currently executing on g_emu_co
static uint64_t     g_frame_count;     // frames emulated since load
static uint64_t     g_clock_us;        // virtual clock, microseconds
static uint64_t     g_clock_frames;    // frames the virtual clock has advanced
static uint64_t     g_clock_base_us;   // clock when loading finished
static uint64_t     g_frame_start_ns;  // real time retro_run() started (0 outside it)
static bool         g_emu_stalled;     // engine gave up the frame mid-wait

// retro_run() must finish within one 16.7 ms refresh.  The engine normally
// takes well under a millisecond; when it blocks on the decoder thread longer
// than this, it yields and the game's clock holds still until it is done.
static const uint64_t LR_ENGINE_BUDGET_NS = 6 * 1000000ull;
// Latest point in a frame to keep waiting for the decoder.
static const uint64_t LR_DECODER_BUDGET_NS = 10 * 1000000ull;
// Longest retro_load_game() waits for the engine to start (see there).
static const unsigned LR_INIT_TIMEOUT_MS = 300 * 1000;

// libco's active-thread handle may be process-global (libco.c without
// LIBCO_MP), so never ask libco which coroutine is running; track it ourselves.
static void enter_emu(void)
{
    g_main_thread = SDL_GetCurrentThreadID();
    g_main_co = co_active();
    g_in_emu = true;
    co_switch(g_emu_co);
    g_in_emu = false;
}

// Only the frontend's thread and the coroutine on it touch this.
enum emu_stage { STAGE_NONE, STAGE_FAILED, STAGE_INIT_DONE, STAGE_RUNNING, STAGE_FINISHED };
static emu_stage g_stage = STAGE_NONE;

static std::vector<std::string> g_args;
static std::vector<char *>      g_argv;

extern "C" uint64_t lr_ticks(void) { return g_clock_us / 1000; }

// Wall-clock ms, for engine timeouts that wait on the VLDP thread.
extern "C" uint64_t lr_real_ticks(void) { return SDL_GetTicks(); }

// A game that cannot start (missing ROM or video file) still loads: the core
// shows the error on a black screen, then asks the frontend to quit, as when a
// game ends.  RetroArch on Android crashes or hangs on its way out after a
// failed load it was started with from another app, and launchers that start
// RetroArch for one game wait for it to exit either way.
static bool        g_error_mode;
static unsigned    g_error_frames;
static std::string g_error_msg;
static const unsigned LR_ERROR_FRAMES = 300; // 5 s

// Engine errors: on screen for LR_ERROR_FRAMES, and in the log (printerror()
// has already logged it).  The first one of a game is the one shown: what
// follows it is mostly the engine giving up ("Unknown command line
// parameter: -script" after a missing script).
extern "C" void lr_show_error(const char *msg)
{
    if (!msg || !g_error_msg.empty()) return;
    g_error_msg = msg;
    if (!environ_cb) return;
    struct retro_message m = {msg, LR_ERROR_FRAMES};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static bool readable(const char *file)
{
    FILE *f = file ? fopen(file, "rb") : NULL;
    if (f) fclose(f);
    return f != NULL;
}

static std::string absolute(const std::string &p)
{
    const bool abs = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
    char buf[4096];
    if (abs || !getcwd(buf, sizeof(buf))) return p;
    return std::string(buf) + "/" + p;
}

// The engine's file loaders get an absolute path of a file that exists: on
// Android, SDL looks up any path it cannot open as an absolute file (relative
// paths, missing files) through JNI, in the app's storage and APK assets, and
// a libretro core has no JNI environment.  Singe games open their files
// relative to their folder.
// A Singe script loads its images and sounds in one go, from a slow share
// perhaps: once the frame's time is used up, the rest wait for the next one.
static const char *lr_path(const char *file, std::string &buf)
{
    lr_delay(0);
    if (!file || !readable(file))
    {
        SDL_SetError("Could not open %s", file ? file : "(null)");
        return NULL;
    }
    buf = absolute(file);
    return buf.c_str();
}

extern "C" SDL_Surface *lr_LoadBMP(const char *file)
{
    std::string p;
    return lr_path(file, p) ? SDL_LoadBMP(p.c_str()) : NULL;
}

extern "C" SDL_Surface *lr_LoadPNG(const char *file)
{
    std::string p;
    return lr_path(file, p) ? SDL_LoadPNG(p.c_str()) : NULL;
}

extern "C" bool lr_LoadWAV(const char *path, SDL_AudioSpec *spec, Uint8 **buf, Uint32 *len)
{
    std::string p;
    return lr_path(path, p) ? SDL_LoadWAV(p.c_str(), spec, buf, len) : false;
}

extern "C" void *lr_LoadFile(const char *file, size_t *datasize)
{
    std::string p;
    return lr_path(file, p) ? SDL_LoadFile(p.c_str(), datasize) : NULL;
}

extern "C" SDL_IOStream *lr_IOFromFile(const char *file, const char *mode)
{
    // writing (NVRAM, Singe saves): made absolute as fopen() would resolve
    // it, so SDL never sends it through JNI
    std::string p;
    if (mode && mode[0] != 'r')
    {
        if (!file) { SDL_SetError("No file name"); return NULL; }
        return SDL_IOFromFile(absolute(file).c_str(), mode);
    }
    return lr_path(file, p) ? SDL_IOFromFile(p.c_str(), mode) : NULL;
}

extern "C" SDL_Surface *lr_IMG_Load(const char *file)
{
    std::string p;
    SDL_Surface *s = lr_path(file, p) ? IMG_Load(p.c_str()) : NULL;
    if (!s) lr_log(RETRO_LOG_WARN, "Image %s: %s", file ? file : "(null)", SDL_GetError());
    return s;
}

extern "C" IMG_Animation *lr_IMG_LoadAnimation(const char *file)
{
    std::string p;
    return lr_path(file, p) ? IMG_LoadAnimation(p.c_str()) : NULL;
}

extern "C" TTF_Font *lr_TTF_OpenFont(const char *file, float ptsize)
{
    std::string p;
    TTF_Font *f = lr_path(file, p) ? TTF_OpenFont(p.c_str(), ptsize) : NULL;
    if (!f) lr_log(RETRO_LOG_WARN, "Font %s, %.1f points: %s", file ? file : "(null)", ptsize, SDL_GetError());
    return f;
}

extern "C" void lr_delay(uint32_t ms)
{
    const bool on_emu = g_in_emu && SDL_GetCurrentThreadID() == g_main_thread;

    if (ms > 0 && on_emu)
    {
        const uint64_t target = g_clock_us + (uint64_t)ms * 1000;
        do
            co_switch(g_main_co);
        while (g_clock_us < target);
    }
    else if (ms == 0 && on_emu && g_frame_start_ns &&
             SDL_GetTicksNS() - g_frame_start_ns > LR_ENGINE_BUDGET_NS)
    {
        // SDL_Delay(0) is the engine polling the VLDP thread (opening a
        // file, acknowledging a search).  Out of time: give the frame back.
        g_emu_stalled = true;
        co_switch(g_main_co);
    }
    else
        SDL_Delay(ms);
}

// Mirrors main() in hypseus.cpp.
static void emu_entry(void)
{
    int argc = (int)g_argv.size() - 1;

    if (parse_cmd_line(argc, g_argv.data()))
    {
        if (video::load_bmps())
        {
            if (sound::init())
            {
                if (SDL_input_init())
                {
                    if (g_game->load_roms())
                    {
                        if (g_game->init_video())
                        {
                            // retro_load_game() runs us until this point, so
                            // a missing video file or a laserdisc player that
                            // will not start fails the load
                            if (g_ldp->pre_init())
                            {
                                if (g_game->pre_init())
                                {
                                    g_stage = STAGE_INIT_DONE;
                                    co_switch(g_main_co);
                                    g_stage = STAGE_RUNNING;
                                    g_game->start(); // the engine main loop
                                    g_game->pre_shutdown();
                                }
                                else
                                    lr_log(RETRO_LOG_ERROR, "Game initialization failed");
                                g_ldp->pre_shutdown();
                            }
                            else
                            {
                                lr_log(RETRO_LOG_ERROR, "Could not initialize laserdisc player");
                                // it may have got as far as starting VLDP's
                                // thread, which must not outlive the core
                                g_ldp->shutdown_player();
                            }
                            g_game->shutdown_video();
                        }
                        else
                            lr_log(RETRO_LOG_ERROR, "Game video initialization failed");
                    }
                    else
                        lr_log(RETRO_LOG_ERROR, "Could not load ROM images");
                    SDL_input_shutdown();
                }
                sound::shutdown();
            }
            else
                lr_log(RETRO_LOG_ERROR, "Sound initialization failed");
        }
        else
            lr_log(RETRO_LOG_ERROR, "Could not load bitmaps from pics/");
    }
    else
        lr_log(RETRO_LOG_ERROR, "Bad command line");

    if (g_stage < STAGE_INIT_DONE)
        g_stage = STAGE_FAILED;
    else
        g_stage = STAGE_FINISHED;

    delete g_game;
    g_game = NULL;
    delete g_ldp;
    g_ldp = NULL;
    video::free_bmps();
    video::deinit_display();

    for (;;)
        co_switch(g_main_co);
}

// ---------------------------------------------------------------------------
// Content resolution
// ---------------------------------------------------------------------------

static bool file_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}

static bool dir_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

// mkdir -p (plain POSIX/CRT: SDL's filesystem layer may use JNI on Android)
static void make_dirs(const char *path)
{
    std::string p = path;
    for (size_t i = 1; i <= p.size(); ++i)
    {
        if (i != p.size() && p[i] != '/' && p[i] != '\\') continue;
        const std::string part = p.substr(0, i);
        if (part.empty() || (part.size() == 2 && part[1] == ':')) continue;
#ifdef _WIN32
        _mkdir(part.c_str());
#else
        mkdir(part.c_str(), 0775);
#endif
    }
}

static std::string dirname_of(const std::string &p)
{
    size_t n = p.find_last_of("/\\");
    return n == std::string::npos ? std::string(".") : p.substr(0, n);
}

static std::string basename_of(const std::string &p)
{
    size_t n = p.find_last_of("/\\");
    return n == std::string::npos ? p : p.substr(n + 1);
}

static std::string lower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Framefile base names to try for ROM sets that share a disc.
static std::vector<std::string> framefile_candidates(const std::string &game)
{
    struct alias { const char *prefix; const char *names; };
    static const alias table[] = {
        {"ace",          "ace"},           {"sae",       "ace"},
        {"astron",       "astron"},        {"blazer",    "blazer"},
        {"badland",      "badlands"},      {"begar",     "bega"},
        {"bega",         "bega"},          {"cobraconv", "cobraconv"},
        {"cobra",        "cobraab,cobra"}, {"cliff",     "cliff"},
        {"gtg",          "g2g,gtg"},       {"dle",       "lair"},
        {"lair2",        "lair2"},         {"laireuro",  "laireuro,lair"},
        {"lair",         "lair"},          {"esh",       "esh"},
        {"firefox",      "firefox"},       {"galaxy",    "galaxy"},
        {"sdq",          "sdq,superdon"},  {"superdon",  "superdon,sdq"},
        {"tq",           "tq"},            {"roadblaster", "roadblaster,rb"},
        {"rb",           "roadblaster,rb"},
    };

    std::vector<std::string> out{game};
    for (const auto &a : table)
    {
        if (game.compare(0, strlen(a.prefix), a.prefix) != 0) continue;
        std::string names = a.names;
        size_t start = 0;
        while (start < names.size())
        {
            size_t comma = names.find(',', start);
            if (comma == std::string::npos) comma = names.size();
            out.push_back(names.substr(start, comma - start));
            start = comma + 1;
        }
        break;
    }
    return out;
}

static std::string find_framefile(const std::string &root, const std::string &game)
{
    for (const auto &name : framefile_candidates(game))
    {
        const std::string paths[] = {
            root + "/framefile/" + name + ".txt",
            root + "/vldp/" + name + "/" + name + ".txt",
            root + "/" + name + ".daphne/" + name + ".txt",
        };
        for (const auto &p : paths)
            if (file_exists(p)) return p;
    }
    return "";
}

// Writes any embedded asset (lr_assets.h) missing, or of a different size,
// under 'dir'.
static void unpack_assets(const std::string &dir)
{
    make_dirs((dir + "/pics").c_str());
    make_dirs((dir + "/sound").c_str());

    for (const lr_asset *a = lr_assets; a->path; ++a)
    {
        const std::string p = dir + "/" + a->path;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && (size_t)st.st_size == a->size) continue;

        FILE *f = fopen(p.c_str(), "wb");
        if (!f) { lr_log(RETRO_LOG_WARN, "Could not write %s", p.c_str()); continue; }
        const bool ok = fwrite(a->data, 1, a->size, f) == a->size;
        if (fclose(f) != 0 || !ok) lr_log(RETRO_LOG_WARN, "Could not write %s", p.c_str());
    }
}

// Optional "<game>.commands" sidecar with extra engine arguments.
static void append_commands_file(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;

    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    std::string tok;
    bool quoted = false;
    for (char c : text)
    {
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && isspace((unsigned char)c))
        {
            if (!tok.empty()) { g_args.push_back(tok); tok.clear(); }
        }
        else tok += c;
    }
    if (!tok.empty()) g_args.push_back(tok);
    lr_log(RETRO_LOG_INFO, "Applied arguments from %s", path.c_str());
}

// Per-game settings from the stock RocketLauncher Daphne module
// (Modules/Daphne/Daphne.ini), which is how HyperSpin cabinets usually launch
// daphne.exe: DIP banks, laserdisc seek timing and scoreboard style.
// Variants share their parent's entry, as they do in RocketLauncher.
struct game_preset { const char *names; const char *args; };
static const game_preset g_presets[] = {
    {"ace,sae,ace_a,ace_a2",            "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00000010 -bank 1 00000001 -useoverlaysb 1"},
    {"astron,astronp",                  "-seek_frames_per_ms 20 -bank 0 00000000 -bank 1 00000000"},
    {"badlands,badlandp",               "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00000000 -bank 1 10000011"},
    {"bega,begar1",                     "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00000000 -bank 1 00000111"},
    {"cliff,cliffalt",                  "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00000000 -bank 1 01000000 -bank 2 00000000 -bank 3 01111011"},
    {"cobraab,cobram3,cobra,cobraconv", "-seek_frames_per_ms 20 -bank 0 00000000 -bank 1 01001000"},
    {"esh,eshalt,eshalt2",              "-seek_frames_per_ms 20"},
    {"galaxy,galaxyp,blazer",           "-seek_frames_per_ms 20 -bank 0 00000000 -bank 1 00000000"},
    {"gpworld",                         "-seek_frames_per_ms 20"},
    {"interstellar",                    "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00110001 -bank 1 00000000"},
    {"lair",                            "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 11011001 -bank 1 00100111 -useoverlaysb 1"},
    {"lair2",                           "-seek_frames_per_ms 20"},
    {"mach3",                           "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 01000100"},
    {"roadblaster",                     "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00000000 -bank 1 00000001"},
    {"sdq",                             "-min_seek_delay 1000 -seek_frames_per_ms 20 -bank 0 00100001 -bank 1 00000000"},
    {"tq",                              "-seek_frames_per_ms 20"},
    {"uvt",                             "-seek_frames_per_ms 20 -bank 0 00000000"},
};

static void append_preset(const std::string &name)
{
    for (const auto &p : g_presets)
    {
        const std::string list = std::string(",") + p.names + ",";
        if (list.find("," + name + ",") == std::string::npos) continue;

        std::string tok;
        for (const char *c = p.args;; ++c)
        {
            if (*c == ' ' || *c == '\0')
            {
                if (!tok.empty()) { g_args.push_back(tok); tok.clear(); }
                if (!*c) break;
            }
            else tok += *c;
        }
        lr_log(RETRO_LOG_INFO, "RocketLauncher settings for %s: %s", name.c_str(), p.args);
        return;
    }
}

// RocketLauncher's "Daphne Batch" emulator: a game with a <title>.bat in a
// folder beside framefile/ (such as "_New Version" or "Daphne2") runs that
// batch file instead, which is how Singe games are usually set up.
struct batch_game
{
    std::string file, dir;          // the batch file and its folder
    std::vector<std::string> args;  // daphne.exe's arguments: game, player, options
    bool singe2 = false;            // runs Singe.exe (Singe 2), not daphne.exe
};

static std::vector<std::string> split_args(const std::string &line)
{
    std::vector<std::string> out;
    std::string tok;
    bool quoted = false, any = false;
    for (char c : line)
    {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && isspace((unsigned char)c))
        {
            if (any) out.push_back(tok);
            tok.clear();
            any = false;
        }
        else { tok += c; any = true; }
    }
    if (any) out.push_back(tok);
    return out;
}

static bool read_batch(const std::string &file, batch_game *b)
{
    FILE *f = fopen(file.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 65536) text.append(buf, n);
    fclose(f);

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> tok = split_args(text.substr(pos, end - pos));
        pos = end + 1;

        for (size_t i = 0; i < tok.size(); ++i)
        {
            std::string exe = lower(basename_of(tok[i]));
            if (!exe.empty() && exe[0] == '@') exe.erase(0, 1);
            const bool daphne = exe == "daphne.exe" || exe == "hypseus.exe" || exe == "daphne" || exe == "hypseus";
            const bool singe  = exe == "singe.exe" || exe == "singe32.exe" || exe == "singe";
            if (!daphne && !singe) continue;
            b->file   = file;
            b->dir    = dirname_of(file);
            b->singe2 = singe;
            b->args.assign(tok.begin() + i + 1, tok.end());
            return singe || b->args.size() >= 2;
        }
    }
    return false;
}

static bool find_batch(const std::string &root, const std::string &title, batch_game *b)
{
    const std::string want = lower(title) + ".bat";
    DIR *d = opendir(root.c_str());
    if (!d) return false;
    std::vector<std::string> dirs;
    while (struct dirent *e = readdir(d))
    {
        const std::string n = e->d_name;
        const std::string l = lower(n);
        // not the big data folders
        if (n == "." || n == ".." || l == "framefile" || l == "vldp" || l == "roms" || l == "ram")
            continue;
        if (dir_exists(root + "/" + n)) dirs.push_back(n);
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());

    for (const auto &sub : dirs)
    {
        DIR *s = opendir((root + "/" + sub).c_str());
        if (!s) continue;
        std::string found;
        while (struct dirent *e = readdir(s))
            if (lower(e->d_name) == want) { found = e->d_name; break; }
        closedir(s);
        if (!found.empty() && read_batch(root + "/" + sub + "/" + found, b)) return true;
    }
    return false;
}

// Singe.exe's arguments (Singe 2) as the engine's: the video (-v) becomes a
// framefile of its own, written to 'cache', the script stays, and the
// volumes in percent (-l video, -e effects) carry over.  The rest (data
// folder, window) does not matter here.
static bool singe2_args(const batch_game &b, const std::string &title, const std::string &cache,
                        std::vector<std::string> *out, std::string *err)
{
    std::string video, script, vol_video, vol_effects;
    for (size_t i = 0; i < b.args.size(); ++i)
    {
        const std::string a = lower(b.args[i]);
        auto value = [&]() { return i + 1 < b.args.size() ? b.args[++i] : std::string(); };
        if (a == "-v") video = value();
        else if (a == "-l") vol_video = value();
        else if (a == "-e") vol_effects = value();
        else if (a == "-d" || a == "-x" || a == "-y" || a == "-a") value();
        else if (!a.empty() && a[0] == '-') continue; // flags
        else script = b.args[i];
    }
    for (auto &c : video) if (c == '\\') c = '/';
    for (auto &c : script) if (c == '\\') c = '/';
    if (video.empty() || script.empty())
    {
        *err = basename_of(b.file) + " names no video (-v) or no script";
        return false;
    }
    const std::string video_path = absolute(b.dir + "/" + video);
    if (!readable(video_path.c_str()))
    {
        *err = "Could not open file : " + video_path;
        return false;
    }

    std::string name;
    for (char c : title) name += isalnum((unsigned char)c) ? c : '_';
    const std::string framefile = cache + "/singe2-" + name + ".txt";
    FILE *f = fopen(framefile.c_str(), "wb");
    if (!f)
    {
        *err = "Could not write " + framefile;
        return false;
    }
    fprintf(f, "%s\n\n0 %s\n", dirname_of(video_path).c_str(), basename_of(video_path).c_str());
    fclose(f);

    *out = {"singe", "vldp", "-framefile", framefile, "-script", script};
    auto volume = [&](const char *arg, const std::string &pct) {
        if (pct.empty()) return;
        const int v = std::min(std::max(atoi(pct.c_str()), 0), 100) * 64 / 100; // the engine's 0..64
        out->push_back(arg);
        out->push_back(std::to_string(v));
    };
    volume("-volume_vldp", vol_video);
    volume("-volume_nonvldp", vol_effects);
    return true;
}

// Windowing arguments of daphne.exe that mean nothing inside a frontend, and
// the writable folders, which the core sets (in the save directory).
static void filter_batch_args(std::vector<std::string> &args)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string a = lower(args[i]);
        if (a == "-opengl" || a == "-vulkan" || a == "-fullscreen" || a == "-fullscreen_window" ||
            a == "-noserversend")
            continue;
        if (a == "-x" || a == "-y" || a == "-sound_buffer" || a == "-homedir" || a == "-ramdir")
        {
            ++i;
            continue;
        }
        out.push_back(args[i]);
    }
    args.swap(out);
}

// A batch game runs with the batch file's folder as the working directory, as
// daphne.exe and Singe.exe do when the batch file starts them: its arguments
// (-framefile ..\vldp\...) and Singe scripts (dofile, images, sounds, fonts)
// name their files relative to it, too many places to rewrite.  It is the one
// change of the process's working directory the core makes, for the length of
// the game; the frontend works with absolute paths.  The engine's own requests
// to change it are refused (change_dir() in lr_stubs.cpp).
static std::string g_saved_cwd;

static void enter_dir(const std::string &dir)
{
    char buf[4096];
    if (g_saved_cwd.empty() && getcwd(buf, sizeof(buf))) g_saved_cwd = buf;
    if (chdir(dir.c_str()) != 0)
        lr_log(RETRO_LOG_WARN, "Could not change to %s", dir.c_str());
}

static void leave_dir(void)
{
    if (g_saved_cwd.empty()) return;
    if (chdir(g_saved_cwd.c_str()) != 0)
        lr_log(RETRO_LOG_WARN, "Could not change back to %s", g_saved_cwd.c_str());
    g_saved_cwd.clear();
}

// ---------------------------------------------------------------------------
// Core options
// ---------------------------------------------------------------------------

static struct retro_core_option_v2_category g_option_categories[] = {
    {"video",  "Video",  "Rendering and laserdisc video decoding."},
    {"system", "System", "Game startup and sound."},
    {"input",  "Input",  "Controller mapping."},
    {NULL, NULL, NULL},
};

#define LR_ON_OFF {{"enabled", NULL}, {"disabled", NULL}, {NULL, NULL}}

static struct retro_core_option_v2_definition g_option_defs[] = {
    {"daphne_cpu_fallback", "CPU Fallback", NULL,
     "Render on the CPU instead of the GPU, and decode laserdisc video in software (MPEG-2 with "
     "libmpeg2, Singe 2's H.264 with the platform's software decoder). For comparison and "
     "troubleshooting: much slower on most devices. Takes effect when a game is loaded.",
     NULL, "video", LR_ON_OFF, "disabled"},
    {"daphne_blank_searches", "Blank Video During Searches", NULL,
     "Show black while the laserdisc player seeks, as most players did. Takes effect when a game is loaded.",
     NULL, "video", LR_ON_OFF, "enabled"},
    {"daphne_fastboot", "Fast Boot", NULL,
     "Skip ROM and RAM tests at startup where the game supports it. Takes effect when a game is loaded.",
     NULL, "system", LR_ON_OFF, "enabled"},
    {"daphne_prefer_samples", "Prefer Sound Samples", NULL,
     "Use recorded sound samples instead of emulated sound hardware where a game has both. "
     "Takes effect when a game is loaded.",
     NULL, "system", LR_ON_OFF, "enabled"},
    {"daphne_service_buttons", "Service and Test on L3/R3", NULL,
     "Map the operator's Service and Test switches to the stick clicks. Off by default: on many "
     "setups the stick clicks also carry Coin or Start, and Test reboots most games into their "
     "diagnostics. Takes effect when a game is loaded.",
     NULL, "input", LR_ON_OFF, "disabled"},
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

#undef LR_ON_OFF

// Options in the newest format the frontend understands: v2 (categories,
// sublabels), v1, else the legacy "Description; value|value" strings.
static void set_core_options(retro_environment_t cb)
{
    static struct retro_core_options_v2 v2 = {g_option_categories, g_option_defs};
    const size_t n = sizeof(g_option_defs) / sizeof(g_option_defs[0]); // with the terminator

    unsigned version = 0;
    if (!cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) version = 0;

    if (version >= 2)
    {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &v2);
    }
    else if (version == 1)
    {
        static struct retro_core_option_definition v1[sizeof(g_option_defs) / sizeof(g_option_defs[0])];
        for (size_t i = 0; i < n; ++i)
        {
            v1[i].key           = g_option_defs[i].key;
            v1[i].desc          = g_option_defs[i].desc;
            v1[i].info          = g_option_defs[i].info;
            v1[i].default_value = g_option_defs[i].default_value;
            memcpy(v1[i].values, g_option_defs[i].values, sizeof(v1[i].values));
        }
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, v1);
    }
    else
    {
        // "Description; default|other|..."
        static std::string text[sizeof(g_option_defs) / sizeof(g_option_defs[0])];
        static struct retro_variable vars[sizeof(g_option_defs) / sizeof(g_option_defs[0])];
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const retro_core_option_v2_definition &d = g_option_defs[i];
            text[i] = std::string(d.desc) + "; " + d.default_value;
            for (const retro_core_option_value *v = d.values; v->value; ++v)
                if (strcmp(v->value, d.default_value) != 0) text[i] += std::string("|") + v->value;
            vars[i].key   = d.key;
            vars[i].value = text[i].c_str();
        }
        vars[n - 1].key = vars[n - 1].value = NULL;
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
    }
}

static bool option_enabled(const char *key, bool def)
{
    struct retro_variable var = {key, NULL};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return strcmp(var.value, "enabled") == 0;
    return def;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

struct button_map { unsigned id; Uint8 sw; };
static const button_map g_buttons[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP,     SWITCH_UP},
    {RETRO_DEVICE_ID_JOYPAD_DOWN,   SWITCH_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT,   SWITCH_LEFT},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT,  SWITCH_RIGHT},
    {RETRO_DEVICE_ID_JOYPAD_B,      SWITCH_BUTTON1},
    {RETRO_DEVICE_ID_JOYPAD_A,      SWITCH_BUTTON2},
    {RETRO_DEVICE_ID_JOYPAD_Y,      SWITCH_BUTTON3},
    {RETRO_DEVICE_ID_JOYPAD_X,      SWITCH_SKILL1},
    {RETRO_DEVICE_ID_JOYPAD_L,      SWITCH_SKILL2},
    {RETRO_DEVICE_ID_JOYPAD_R,      SWITCH_SKILL3},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, SWITCH_COIN1},
    {RETRO_DEVICE_ID_JOYPAD_START,  SWITCH_START1},
    {RETRO_DEVICE_ID_JOYPAD_L2,     SWITCH_COIN2},
    {RETRO_DEVICE_ID_JOYPAD_R2,     SWITCH_START2},
    {RETRO_DEVICE_ID_JOYPAD_L3,     SWITCH_SERVICE},
    {RETRO_DEVICE_ID_JOYPAD_R3,     SWITCH_TEST},
};
static bool g_button_state[sizeof(g_buttons) / sizeof(g_buttons[0])];
// The operator's Service and Test switches on L3/R3 (core option, off by
// default): on many setups the stick clicks also carry Coin/Start, and Test
// reboots most games into their diagnostics.
static bool g_service_buttons;

// Singe games run Lua for every input, so theirs is queued and handed over
// in the engine's own input poll (lr_input_drain), never in the middle of a
// Lua call the engine is waiting in.
static bool g_singe;
struct queued_switch { uint8_t sw; bool down; };
static std::vector<queued_switch> g_input_queue;
static bool g_pointer_moved;
static int  g_pointer_x, g_pointer_y, g_pointer_dx, g_pointer_dy;
static int16_t  g_last_gun_x, g_last_gun_y;   // lightgun position last frame
static bool     g_pointer_buttons[3];         // trigger/left, aux A/right, aux B/middle
static unsigned g_port_device = RETRO_DEVICE_JOYPAD; // port 0, see retro_set_controller_port_device

static void reset_input(void)
{
    g_input_queue.clear();
    g_pointer_moved = false;
    g_pointer_x = g_pointer_y = -1;
    g_pointer_dx = g_pointer_dy = 0;
    g_last_gun_x = g_last_gun_y = 0;
    memset(g_pointer_buttons, 0, sizeof(g_pointer_buttons));
}

static void switch_changed(uint8_t sw, bool down)
{
    if (g_singe) { g_input_queue.push_back({sw, down}); return; }
    if (down) input_enable(sw, NOMOUSE);
    else      input_disable(sw, NOMOUSE);
}

extern "C" void lr_input_drain(void)
{
    if (!g_game) return;
    if (g_pointer_moved)
    {
        g_pointer_moved = false;
        g_game->OnMouseMotion((Uint16)g_pointer_x, (Uint16)g_pointer_y, (Sint16)g_pointer_dx,
                              (Sint16)g_pointer_dy, NOMOUSE);
        g_pointer_dx = g_pointer_dy = 0;
    }
    // the queue only grows in poll_input(), never while this runs
    for (const queued_switch &q : g_input_queue)
    {
        if (q.down) input_enable(q.sw, NOMOUSE);
        else        input_disable(q.sw, NOMOUSE);
    }
    g_input_queue.clear();
}

// Singe's gun games aim with a lightgun (RetroArch also makes one of a mouse
// or a touchscreen) or a mouse; a gamepad steers the crosshair with the d-pad.
// With port 0 set to RetroPad both pointer devices are read; set to Lightgun
// or Mouse, only that one.
static void poll_pointer(void)
{
    const int w = video::get_video_width(), h = video::get_video_height();
    if (w <= 0 || h <= 0) return;
    if (g_pointer_x < 0) { g_pointer_x = w / 2; g_pointer_y = h / 2; } // as Singe's own

    const bool gun   = g_port_device != RETRO_DEVICE_MOUSE;
    const bool mouse = g_port_device != RETRO_DEVICE_LIGHTGUN;
    auto gun_state   = [&](unsigned id) { return gun ? input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, id) : 0; };
    auto mouse_state = [&](unsigned id) { return mouse ? input_state_cb(0, RETRO_DEVICE_MOUSE, 0, id) : 0; };

    const int16_t gx = gun_state(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
    const int16_t gy = gun_state(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
    const bool off   = gun_state(RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
    const int16_t mx = mouse_state(RETRO_DEVICE_ID_MOUSE_X);
    const int16_t my = mouse_state(RETRO_DEVICE_ID_MOUSE_Y);

    int x = g_pointer_x, y = g_pointer_y;
    if ((gx != g_last_gun_x || gy != g_last_gun_y) && !off)
    {
        // -0x7fff..0x7fff over the whole picture
        x = (int)(((int64_t)gx + 0x7fff) * w / 0xfffe);
        y = (int)(((int64_t)gy + 0x7fff) * h / 0xfffe);
    }
    else if (mx || my)
    {
        x += mx;
        y += my;
    }
    g_last_gun_x = gx;
    g_last_gun_y = gy;
    x = std::min(std::max(x, 0), w - 1);
    y = std::min(std::max(y, 0), h - 1);
    if (x != g_pointer_x || y != g_pointer_y)
    {
        g_pointer_dx += x - g_pointer_x;
        g_pointer_dy += y - g_pointer_y;
        g_pointer_x = x;
        g_pointer_y = y;
        g_pointer_moved = true;
    }

    static const uint8_t sw[3] = {SWITCH_BUTTON1, SWITCH_BUTTON2, SWITCH_BUTTON3};
    const bool now[3] = {
        gun_state(RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) || mouse_state(RETRO_DEVICE_ID_MOUSE_LEFT),
        gun_state(RETRO_DEVICE_ID_LIGHTGUN_AUX_A)   || mouse_state(RETRO_DEVICE_ID_MOUSE_RIGHT),
        gun_state(RETRO_DEVICE_ID_LIGHTGUN_AUX_B)   || mouse_state(RETRO_DEVICE_ID_MOUSE_MIDDLE),
    };
    for (int i = 0; i < 3; ++i)
        if (now[i] != g_pointer_buttons[i]) switch_changed(sw[i], g_pointer_buttons[i] = now[i]);
}

static void poll_input(void)
{
    input_poll_cb();
    if (g_singe) poll_pointer();

    for (size_t i = 0; i < sizeof(g_buttons) / sizeof(g_buttons[0]); ++i)
    {
        const bool operator_switch = g_buttons[i].sw == SWITCH_SERVICE || g_buttons[i].sw == SWITCH_TEST;
        if (operator_switch && !g_service_buttons) continue;
        bool down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, g_buttons[i].id) != 0;
        if (down == g_button_state[i]) continue;
        g_button_state[i] = down;
        if (g_debug_status)
            lr_log(RETRO_LOG_DEBUG, "frame %llu switch %d %s", (unsigned long long)g_frame_count,
                   g_buttons[i].sw, down ? "down" : "up");

        switch_changed(g_buttons[i].sw, down);
    }
}

// ---------------------------------------------------------------------------
// libretro API
// ---------------------------------------------------------------------------

static unsigned g_geom_w, g_geom_h;
static bool g_can_dupe;
static bool g_use_gpu;    // GL renderer requested and granted
static bool g_use_vk;     // Vulkan renderer requested and granted
static uint64_t g_prof_ns[4];     // engine, decoder wait, compose+present, audio
static uint64_t g_prof_cpu_ns[4]; // CPU time of this thread in the same phases
static uint64_t g_prof_sent;      // frames sent in full (the rest were repeats)
static uint64_t g_prof_helper_base[LR_THREAD_KINDS];
static uint64_t g_prof_worst[10]; // slowest frame: number, wall and CPU per phase, CPU queue
static uint64_t g_prof_runq_ns;   // time this thread was ready to run but had no CPU core
static uint64_t g_prof_late;      // frames shown while the decoder was still behind
static uint64_t g_prof_stalls;    // frames the engine gave back (game time held still)
static void log_search_stats(void);
static bool enter_error_mode(void);
static void run_error_mode(void);

// CPU time used by helper threads, so the timing summary can tell work from
// waiting.  Linux and Android only; elsewhere it all reads 0.
#ifndef __linux__
static uint64_t thread_cpu_ns(void) { return 0; }
extern "C" void lr_register_cpu_thread(int kind) { (void)kind; }
static uint64_t helper_cpu_ns(int kind) { (void)kind; return 0; }
static void forget_helpers(int kind) { (void)kind; }
static uint64_t run_queue_ns(void) { return 0; }
static void close_run_queue(void) {}
#else
static uint64_t clock_ns(clockid_t c)
{
    struct timespec ts;
    if (clock_gettime(c, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t thread_cpu_ns(void) { return clock_ns(CLOCK_THREAD_CPUTIME_ID); }

static std::mutex g_helper_mutex;
static std::vector<clockid_t> g_helper_clocks[LR_THREAD_KINDS];

extern "C" void lr_register_cpu_thread(int kind)
{
    clockid_t c;
    if (kind < 0 || kind >= LR_THREAD_KINDS || pthread_getcpuclockid(pthread_self(), &c) != 0) return;
    std::lock_guard<std::mutex> l(g_helper_mutex);
    g_helper_clocks[kind].push_back(c);
}

// Total CPU time of the registered threads of one kind.  Only valid while
// they are alive: forget them before they exit.
static uint64_t helper_cpu_ns(int kind)
{
    std::lock_guard<std::mutex> l(g_helper_mutex);
    uint64_t t = 0;
    for (clockid_t c : g_helper_clocks[kind]) t += clock_ns(c);
    return t;
}

static void forget_helpers(int kind)
{
    std::lock_guard<std::mutex> l(g_helper_mutex);
    g_helper_clocks[kind].clear();
}

// Time the calling thread has spent ready to run but waiting for a CPU core
// (second field of the kernel's schedstat), or 0 where that isn't available.
static int  g_schedstat_fd = -1;
static long g_schedstat_tid;

static uint64_t run_queue_ns(void)
{
    const long tid = syscall(SYS_gettid);
    if (tid != g_schedstat_tid)
    {
        if (g_schedstat_fd >= 0) close(g_schedstat_fd);
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%ld/schedstat", tid);
        g_schedstat_fd  = open(path, O_RDONLY | O_CLOEXEC);
        g_schedstat_tid = tid;
    }

    char buf[96];
    const ssize_t n = g_schedstat_fd >= 0 ? pread(g_schedstat_fd, buf, sizeof(buf) - 1, 0) : -1;
    if (n <= 0) return 0;
    buf[n] = 0;
    char *end;
    strtoull(buf, &end, 10);
    return strtoull(end, NULL, 10);
}

static void close_run_queue(void)
{
    if (g_schedstat_fd >= 0) close(g_schedstat_fd);
    g_schedstat_fd  = -1;
    g_schedstat_tid = 0;
}
#endif

// Per-phase timing of retro_run(), wall and this thread's CPU time, for the
// summary at unload (DAPHNE_LR_PROFILE).
struct frame_timer
{
    uint64_t wall = 0, cpu = 0, runq = 0;
    uint64_t ph[4] = {}, cph[4] = {}; // engine, decoder wait, compose+present, audio

    void start(uint64_t now)
    {
        wall = now;
        if (!g_profile) return;
        cpu  = thread_cpu_ns();
        runq = run_queue_ns();
    }
    void lap(int i)
    {
        if (!g_profile) return;
        const uint64_t w = SDL_GetTicksNS(), c = thread_cpu_ns();
        ph[i]  = w - wall;
        cph[i] = c - cpu;
        wall = w;
        cpu  = c;
    }
};

static float    g_geom_aspect;
static std::vector<int16_t> g_audio;

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    set_core_options(cb);

    // Port 0 plays; Singe's gun games also aim with a lightgun or a mouse.
    static const struct retro_controller_description port0[] = {
        {"RetroPad", RETRO_DEVICE_JOYPAD},
        {"Lightgun", RETRO_DEVICE_LIGHTGUN},
        {"Mouse",    RETRO_DEVICE_MOUSE},
    };
    static const struct retro_controller_info ports[] = {
        {port0, sizeof(port0) / sizeof(port0[0])},
        {NULL, 0},
    };
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);

    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
    // Named up front: otherwise SDL asks the OS for the executable name the
    // first time a thread is created, which on Android goes through JNI.
    SDL_SetAppMetadata(CORE_NAME, CORE_VERSION, "libretro.hypseus");

    static bool plog_ready = false;
    if (!plog_ready)
    {
        // DAPHNE_LR_LOG=debug|verbose raises engine logging (development aid)
        const char *lvl = SDL_getenv("DAPHNE_LR_LOG");
        plog::Severity sev = plog::info;
        if (lvl && !strcmp(lvl, "debug")) sev = plog::debug;
        if (lvl && !strcmp(lvl, "verbose")) sev = plog::verbose;
        g_debug_status = sev >= plog::debug;
        plog::init(sev, &g_appender);
        plog_ready = true;
    }
    // DAPHNE_LR_PROFILE=1 logs where each frame's time went (development aid)
    const char *prof = SDL_getenv("DAPHNE_LR_PROFILE");
    g_profile = prof && *prof && strcmp(prof, "0") != 0;

    // Singe games draw text with TrueType fonts
    if (!TTF_WasInit() && !TTF_Init())
        lr_log(RETRO_LOG_WARN, "SDL_ttf: %s", SDL_GetError());
}

RETRO_API void retro_deinit(void)
{
    if (TTF_WasInit()) TTF_Quit();
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = CORE_NAME;
    info->library_version  = CORE_VERSION;
    info->valid_extensions = "zip|txt|bat";
    info->need_fullpath    = true;
    info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = g_geom_w ? g_geom_w : 640;
    info->geometry.base_height  = g_geom_h ? g_geom_h : 480;
    info->geometry.max_width    = LR_MAX_WIDTH;
    info->geometry.max_height   = LR_MAX_HEIGHT;
    info->geometry.aspect_ratio = g_geom_aspect > 0 ? g_geom_aspect : 4.0f / 3.0f;
    info->timing.fps            = LR_FPS;
    info->timing.sample_rate    = LR_SAMPLE_RATE;
}

// The gamepad always plays; the device chosen for port 0 picks what Singe
// aims with (see poll_pointer).
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port != 0) return;
    switch (device)
    {
    case RETRO_DEVICE_LIGHTGUN:
    case RETRO_DEVICE_MOUSE:
        g_port_device = device;
        break;
    default:
        g_port_device = RETRO_DEVICE_JOYPAD;
        break;
    }
}

RETRO_API void retro_reset(void)
{
    if (g_stage == STAGE_RUNNING && g_game)
        g_game->reset();
}

static const char *env_dir(unsigned cmd)
{
    const char *dir = NULL;
    if (environ_cb && environ_cb(cmd, &dir) && dir && *dir) return dir;
    return NULL;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path)
    {
        lr_log(RETRO_LOG_ERROR, "No content path");
        return false;
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        lr_log(RETRO_LOG_ERROR, "XRGB8888 is not supported");
        return false;
    }

    g_use_gpu = g_use_vk = false;
    g_singe = false;
    reset_input();
    g_error_mode = false;
    g_error_frames = 0;
    g_error_msg.clear();
    // CPU Fallback: render and decode on the CPU, as the original Daphne
    const bool cpu_fallback = option_enabled("daphne_cpu_fallback", false);

    // Hardware video decoding unless CPU Fallback is on: on Android MediaCodec
    // for MPEG-2 and Singe 2's H.264 (its software decoders otherwise); on
    // Windows Media Foundation's hardware transforms for Singe 2's MP4 (MPEG-2
    // is always libmpeg2 there).  VLDP logs which decoder each file gets.
    g_lr_vldp_hwdec = !cpu_fallback;

    g_service_buttons = option_enabled("daphne_service_buttons", false);

    static const struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Button 1 (Action/Sword)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Button 2"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Button 3"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Skill 1"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Skill 2"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Skill 3"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin 1"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start 1"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Coin 2"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Start 2"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Service"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Test"},
        {0, 0, 0, 0, NULL},
    };
    // without the operator switches the list ends before them
    static struct retro_input_descriptor desc_play[sizeof(desc) / sizeof(desc[0])];
    memcpy(desc_play, desc, sizeof(desc));
    desc_play[sizeof(desc) / sizeof(desc[0]) - 3] = desc[sizeof(desc) / sizeof(desc[0]) - 1];
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
               (void *)(g_service_buttons ? desc : desc_play));

    // Content is a ROM set (<root>/roms/<game>.zip), a framefile
    // (<root>/framefile/<game>.txt, which is what front ends such as HyperSpin
    // pass) or a RocketLauncher batch file (<title>.bat).  A ROM set or
    // framefile with a batch file of the same title runs that instead (see
    // find_batch).  Absolute, as a batch game changes the working directory.
    const std::string path = absolute(game->path);
    std::string name = lower(basename_of(path));
    std::string ext;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos)
    {
        ext  = name.substr(dot + 1);
        name = name.substr(0, dot);
    }
    std::string title = basename_of(path);
    if (title.rfind('.') != std::string::npos) title.erase(title.rfind('.'));

    std::string root, romdir, framefile;
    batch_game batch;
    bool use_batch = false;
    if (ext == "bat")
    {
        use_batch = read_batch(path, &batch);
        root = romdir = dirname_of(path);
        if (!use_batch)
        {
            const std::string msg = "No daphne.exe or Singe.exe command in " + path;
            lr_log(RETRO_LOG_ERROR, "%s", msg.c_str());
            lr_show_error(msg.c_str());
            return enter_error_mode();
        }
    }
    else if (ext == "txt")
    {
        framefile = path;
        const std::string dir = dirname_of(path);
        if (lower(basename_of(dir)) == "framefile")
            root = dirname_of(dir);
        else if (lower(basename_of(dirname_of(dir))) == "vldp")
            root = dirname_of(dirname_of(dir));
        else
            root = dir;
        romdir = dir_exists(root + "/roms") ? root + "/roms" : dir;

        // framefile names that differ from the driver name
        if (name == "g2g") name = "gtg";
        else if (name == "rb") name = "roadblaster";
    }
    else
    {
        romdir = dirname_of(path);
        root   = dirname_of(romdir);
        if (!dir_exists(root + "/vldp") && !dir_exists(root + "/framefile") && dir_exists(romdir + "/vldp"))
            root = romdir;
        framefile = find_framefile(root, name);
    }

    if (!use_batch && !root.empty() && find_batch(root, title, &batch))
    {
        use_batch = true;
        lr_log(RETRO_LOG_INFO, "%s runs %s, as RocketLauncher's Daphne Batch does", title.c_str(),
               batch.file.c_str());
    }
    if (!use_batch && framefile.empty())
    {
        const std::string msg = "No framefile for '" + name + "' under " + root + " (looked in framefile/ and vldp/)";
        lr_log(RETRO_LOG_ERROR, "%s", msg.c_str());
        lr_show_error(msg.c_str());
        return enter_error_mode();
    }

    // Writable home in the save directory: NVRAM and what Singe scripts write
    // (the RAM dir), so nothing is ever written beside the game data.  Assets
    // (pics/, sound/) from the system directory, else unpacked into the home.
    const char *save = env_dir(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
    const char *sys  = env_dir(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
    // absolute, as a batch game runs from its own folder (enter_dir)
    const std::string home   = absolute(std::string(save ? save : romdir.c_str()) + "/daphne");
    std::string assets       = sys ? absolute(std::string(sys) + "/daphne") : std::string();
    if (assets.empty() || !dir_exists(assets + "/pics"))
    {
        assets = home + "/assets";
        unpack_assets(assets);
    }

    g_homedir.set_appdir(assets);
    g_homedir.set_altdir(use_batch ? batch.dir : root);
    // Set here rather than with -ramdir, whose value the command line parser
    // would cut at 80 characters.  Kept by the engine for the whole load.
    const std::string ramdir = home + "/ram";
    make_dirs(ramdir.c_str());
    g_homedir.set_ramdir(ramdir);

    // frame indexes the core builds itself go here, never beside the video
    const std::string cache = home + "/cache";
    make_dirs(cache.c_str());
    vldp_lr_set_cache_dir(cache.c_str());

    g_args.clear();
    g_args.push_back("daphne");
    if (use_batch)
    {
        // the batch file's game, player and options, minus the windowing ones
        enter_dir(batch.dir);
        std::string err;
        if (batch.singe2 && !singe2_args(batch, title, cache, &batch.args, &err))
        {
            leave_dir();
            lr_log(RETRO_LOG_ERROR, "%s", err.c_str());
            lr_show_error(err.c_str());
            return enter_error_mode();
        }
        filter_batch_args(batch.args);
        if (batch.args.empty())
        {
            leave_dir();
            const std::string msg = basename_of(batch.file) + " names no game";
            lr_log(RETRO_LOG_ERROR, "%s", msg.c_str());
            lr_show_error(msg.c_str());
            return enter_error_mode();
        }
        g_args.insert(g_args.end(), batch.args.begin(), batch.args.end());
        name = lower(batch.args[0]);
        g_singe = name == "singe";
    }
    else
    {
        g_args.push_back(name);
        g_args.push_back("vldp");
        g_args.push_back("-framefile");
        g_args.push_back(framefile);
        g_args.push_back("-romdir");
        g_args.push_back(romdir);
    }
    g_args.push_back("-homedir");
    g_args.push_back(home);
    g_args.push_back("-noissues");
    if (option_enabled("daphne_fastboot", true))         g_args.push_back("-fastboot");
    if (option_enabled("daphne_prefer_samples", true))   g_args.push_back("-prefer_samples");
    if (option_enabled("daphne_blank_searches", true))   g_args.push_back("-blank_searches");
    if (!use_batch) append_preset(name); // a batch file has its own settings

    // sidecars come last so they override the preset
    if (!use_batch)
    {
        append_commands_file(romdir + "/" + name + ".commands");
        if (root != romdir) append_commands_file(root + "/" + name + ".commands");
    }
    if (sys) append_commands_file(std::string(sys) + "/daphne/" + (use_batch ? title : name) + ".commands");

    std::string cmdline;
    for (const auto &a : g_args) cmdline += a + " ";
    lr_log(RETRO_LOG_INFO, "Engine arguments: %s", cmdline.c_str());

    g_argv.clear();
    for (auto &a : g_args) g_argv.push_back(&a[0]);
    g_argv.push_back(NULL);

    // Run engine initialisation up to the laserdisc player, so ROM and data
    // errors fail the load instead of showing a black screen.
    lr_clear_quitflag();
    g_stage       = STAGE_NONE;
    g_frame_count = 0;
    g_clock_frames = 0;
    g_emu_stalled = false;
    memset(g_prof_ns, 0, sizeof(g_prof_ns));
    memset(g_prof_cpu_ns, 0, sizeof(g_prof_cpu_ns));
    g_prof_sent = 0;
    memset(g_prof_worst, 0, sizeof(g_prof_worst));
    g_prof_runq_ns = 0;
    g_prof_late = g_prof_stalls = 0;
    vldp_lr_reset_search_stats();
    g_can_dupe = false;
    environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe);
    g_geom_w = g_geom_h = 0; // first frame of a game is always sent in full
    g_clock_us    = 0;
    g_emu_co      = co_create(1024 * 1024 * sizeof(void *) / 4, emu_entry);
    if (!g_emu_co)
    {
        leave_dir();
        lr_log(RETRO_LOG_ERROR, "Could not create the emulation coroutine");
        lr_show_error("Out of memory");
        return enter_error_mode();
    }

    // A backstop: an engine that never finishes starting must not hang the
    // frontend for good.  Generous, as indexing a large video on first use
    // from a slow share takes minutes.
    g_frame_start_ns = 0;
    const Uint64 init_deadline = SDL_GetTicks() + LR_INIT_TIMEOUT_MS;
    while (g_stage == STAGE_NONE && SDL_GetTicks() < init_deadline)
    {
        enter_emu();
        if (g_stage == STAGE_NONE) g_clock_us += 1000; // engine waited during init
    }
    if (g_stage == STAGE_NONE)
    {
        lr_log(RETRO_LOG_ERROR, "The engine did not start within %u s", LR_INIT_TIMEOUT_MS / 1000);
        lr_show_error("The game did not start in time (see the log)");
        // abandon it, but its VLDP thread must not outlive the load
        if (g_ldp) g_ldp->shutdown_player();
        g_stage = STAGE_FAILED;
    }
    g_clock_base_us = g_clock_us;
    for (int k = 0; k < LR_THREAD_KINDS; ++k) g_prof_helper_base[k] = helper_cpu_ns(k);

    if (g_stage == STAGE_FAILED)
    {
        // the decoder thread may have started and exited again
        forget_helpers(LR_THREAD_DECODER);
        forget_helpers(LR_THREAD_COMPOSE);
        co_delete(g_emu_co);
        g_emu_co = NULL;
        g_stage  = STAGE_NONE;
        leave_dir();
        if (g_error_msg.empty()) lr_show_error("The game could not start (see the log)");
        return enter_error_mode();
    }

    // The GPU renderer in whichever API the frontend's video driver uses
    // (Vulkan, else OpenGL (ES) 3), else the CPU one.
    if (!cpu_fallback)
    {
        unsigned preferred = RETRO_HW_CONTEXT_NONE;
        if (!environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred))
            preferred = RETRO_HW_CONTEXT_NONE;
        if (preferred == RETRO_HW_CONTEXT_VULKAN)
            g_use_vk = lr_vk_request(environ_cb, lr_log);
        if (!g_use_vk)
            g_use_gpu = lr_gl_request(environ_cb, lr_log);
    }
    lr_log(RETRO_LOG_INFO, "Video rendering: %s",
           g_use_vk ? "GPU (Vulkan)" : g_use_gpu ? "GPU (OpenGL)" : "CPU");

    memset(g_button_state, 0, sizeof(g_button_state));
    return true;
}

static bool enter_error_mode(void)
{
    lr_log(RETRO_LOG_ERROR, "The game cannot start; showing the error, then quitting");
    g_error_mode   = true;
    g_error_frames = 0;
    g_geom_w = g_geom_h = 0;
    g_geom_aspect = 0;
    return true;
}

// Black screen with the error, then quit.
static void run_error_mode(void)
{
    static uint32_t black[640 * 480];
    if (input_poll_cb) input_poll_cb();
    // again once running: frontends may drop messages sent during the load
    if (g_error_frames == 1 && !g_error_msg.empty())
    {
        struct retro_message m = {g_error_msg.c_str(), LR_ERROR_FRAMES};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
    }
    video_cb(black, 640, 480, 640 * sizeof(uint32_t));
    static int16_t silence[2 * 800];
    if (audio_batch_cb) audio_batch_cb(silence, LR_SAMPLE_RATE / 60);
    if (++g_error_frames == LR_ERROR_FRAMES)
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    g_error_mode = false;
    if (!g_emu_co) return;

    if (g_profile && g_frame_count)
    {
        // Wall time is how long retro_run() took; CPU time is what this
        // thread computed in it.  The difference is time spent blocked.
        const double n = (double)g_frame_count;
        lr_log(RETRO_LOG_INFO, "Per-frame time over %llu frames (%llu sent, %llu repeated), wall/CPU ms: "
               "engine %.2f/%.2f, decoder wait %.2f/%.2f, compose %.2f/%.2f, audio %.2f/%.2f",
               (unsigned long long)g_frame_count, (unsigned long long)g_prof_sent,
               (unsigned long long)(g_frame_count - g_prof_sent),
               g_prof_ns[0] / 1e6 / n, g_prof_cpu_ns[0] / 1e6 / n, g_prof_ns[1] / 1e6 / n,
               g_prof_cpu_ns[1] / 1e6 / n, g_prof_ns[2] / 1e6 / n, g_prof_cpu_ns[2] / 1e6 / n,
               g_prof_ns[3] / 1e6 / n, g_prof_cpu_ns[3] / 1e6 / n);
        lr_log(RETRO_LOG_INFO, "Waiting for a CPU core per frame: %.2f ms", g_prof_runq_ns / 1e6 / n);
        lr_log(RETRO_LOG_INFO, "Frames shown with the decoder behind: %llu; frames the engine gave back: %llu",
               (unsigned long long)g_prof_late, (unsigned long long)g_prof_stalls);
        log_search_stats();
        lr_log(RETRO_LOG_INFO, "Helper thread CPU per frame: MPEG decoder %.2f ms, compose workers %.2f ms",
               (helper_cpu_ns(LR_THREAD_DECODER) - g_prof_helper_base[LR_THREAD_DECODER]) / 1e6 / n,
               (helper_cpu_ns(LR_THREAD_COMPOSE) - g_prof_helper_base[LR_THREAD_COMPOSE]) / 1e6 / n);
        const uint64_t *w = g_prof_worst;
        lr_log(RETRO_LOG_INFO, "Slowest frame %llu: %.2f ms, wall/CPU ms: engine %.2f/%.2f, "
               "decoder wait %.2f/%.2f, compose %.2f/%.2f, audio %.2f/%.2f; waiting for a CPU core %.2f ms",
               (unsigned long long)w[0], (w[1] + w[3] + w[5] + w[7]) / 1e6, w[1] / 1e6, w[2] / 1e6,
               w[3] / 1e6, w[4] / 1e6, w[5] / 1e6, w[6] / 1e6, w[7] / 1e6, w[8] / 1e6, w[9] / 1e6);
    }
    forget_helpers(LR_THREAD_DECODER); // it exits during the shutdown below
    lr_gl_unload();
    lr_vk_unload();

    // Let the engine leave its main loop and run its own shutdown.
    set_quitflag();
    for (int i = 0; i < 600 && g_stage != STAGE_FINISHED && g_stage != STAGE_FAILED; ++i)
    {
        g_clock_us += 1000000 / 60;
        enter_emu();
        SDL_Delay(1); // real time for the VLDP thread to answer shutdown commands
    }
    if (g_stage != STAGE_FINISHED && g_stage != STAGE_FAILED)
    {
        // Abandoned with its game objects (a leak), but the VLDP thread
        // must not outlive the core.
        lr_log(RETRO_LOG_ERROR, "The engine did not shut down; abandoning it");
        if (g_ldp) g_ldp->shutdown_player();
    }

    forget_helpers(LR_THREAD_COMPOSE);
    close_run_queue();
    video::lr_compose_shutdown();
    video::lr_frame_reset(); // nothing of this game's pictures reaches the next
    co_delete(g_emu_co);
    g_emu_co = NULL;
    g_stage  = STAGE_NONE;
    leave_dir();
}

// Search and skip response: how long until the target frame was decoded
// (real time) and shown (game time, which includes the laserdisc seek delay
// the game is set up with).
static void log_search_stats(void)
{
    const float *ready, *shown;
    const unsigned n = vldp_lr_search_stats(&ready, &shown);
    if (!n) return;
    std::vector<float> a(ready, ready + n), b(shown, shown + n);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    auto at = [](const std::vector<float> &v, double q) { return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
    lr_log(RETRO_LOG_INFO, "Searches and skips: %u; target decoded after median %.1f, p90 %.1f, max %.1f ms; "
           "shown after median %.0f, p90 %.0f, max %.0f ms of game time", n, at(a, 0.5), at(a, 0.9), a.back(),
           at(b, 0.5), at(b, 0.9), b.back());
}

static void present(void)
{
    video::lr_frame f;
    video::lr_frame_begin(&f);
    if (f.video_changed) g_lr_presented_pic = g_lr_vldp_shown_pic.load();
    bool unchanged = f.unchanged;

    if ((unsigned)f.w != g_geom_w || (unsigned)f.h != g_geom_h || f.aspect != g_geom_aspect)
    {
        g_geom_w = f.w;
        g_geom_h = f.h;
        g_geom_aspect = f.aspect;

        struct retro_system_av_info av;
        retro_get_system_av_info(&av);
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
        unchanged = false;
    }

    // MPEG frames last two refreshes; let the frontend repeat the last one
    // (not with Vulkan, see lr_vk.cpp)
    if (unchanged && g_can_dupe && !g_use_vk)
    {
        if (f.verify_dupe && !g_use_gpu) video::lr_compose_cpu(f);
        video::lr_frame_end();
        video_cb(NULL, f.w, f.h, 0);
        return;
    }

    if ((g_use_vk && lr_vk_render(f)) || (g_use_gpu && lr_gl_render(f)))
    {
        video::lr_frame_end();
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, f.w, f.h, 0);
    }
    else
    {
        // also when the GPU context failed: frontends take software frames
        // with hardware rendering on
        const uint32_t *pixels = video::lr_compose_cpu(f);
        video::lr_frame_end();
        video_cb(pixels, f.w, f.h, (size_t)f.w * sizeof(uint32_t));
    }
    ++g_prof_sent;
}

RETRO_API void retro_run(void)
{
    if (g_error_mode) { run_error_mode(); return; }
    if (!g_emu_co) return;
    const uint64_t frame_start = SDL_GetTicksNS();
    frame_timer t;
    t.start(frame_start);
    g_frame_start_ns = frame_start;

    poll_input();

    // Advance the emulated clock by one frame and let the engine catch up,
    // unless the engine is still blocked from last frame: then no game time
    // passes until it is done.
    const uint64_t prev_frame = g_frame_count++;
    if (!g_emu_stalled) ++g_clock_frames;
    g_clock_us = g_clock_base_us + (uint64_t)((double)g_clock_frames * 1000000.0 / LR_FPS);

    g_emu_stalled = false;
    if (g_stage != STAGE_FINISHED)
        enter_emu();
    g_frame_start_ns = 0;
    g_prof_stalls += g_emu_stalled;
    t.lap(0);

    // Let the decoder show everything due at this emulated time, within the
    // frame's budget; if it is late the previous picture stays up.  The
    // libretro guidelines leave pacing to the frontend and ask cores not to
    // wait in retro_run(); this short, bounded wait is the exception, as the
    // decoder runs on its own thread in real time and the alternative is
    // showing every picture a frame late.
    const uint64_t deadline = frame_start + LR_DECODER_BUDGET_NS;
    while (!g_emu_stalled && !lr_vldp_caught_up() && SDL_GetTicksNS() < deadline)
        SDL_DelayNS(200 * 1000);
    if (!g_emu_stalled && !lr_vldp_caught_up()) ++g_prof_late;
    t.lap(1);

    present();
    t.lap(2);

    if (g_debug_status && g_frame_count % 60 == 0)
    {
        char buf[256];
        lr_vldp_debug(buf, sizeof(buf));
        cpu::def *c = cpu::get_struct(0);
        lr_log(RETRO_LOG_DEBUG, "frame %llu clock=%llums %s cpu0 pc=%04x cycles=%llu irq=%u/%u nmi=%u/%u",
               (unsigned long long)g_frame_count, (unsigned long long)(g_clock_us / 1000), buf,
               c && c->getpc_callback ? (unsigned)c->getpc_callback() : 0,
               (unsigned long long)cpu::get_total_cycles_executed(0),
               c ? c->uIRQTickCount[0] : 0, c ? c->pending_irq_count[0] : 0,
               c ? c->uNMITickCount : 0, c ? c->pending_nmi_count : 0);
    }

    // One frame of audio, sample-exact over time.
    const uint64_t s0 = (uint64_t)((double)prev_frame * LR_SAMPLE_RATE / LR_FPS);
    const uint64_t s1 = (uint64_t)((double)g_frame_count * LR_SAMPLE_RATE / LR_FPS);
    const unsigned frames = (unsigned)(s1 - s0);
    g_audio.resize(frames * 2);
    sound::lr_mix(g_audio.data(), frames);
    if (audio_batch_cb) audio_batch_cb(g_audio.data(), frames);
    t.lap(3);

    if (g_profile)
    {
        const uint64_t runq = run_queue_ns() - t.runq;
        g_prof_runq_ns += runq;

        uint64_t total = 0;
        for (int i = 0; i < 4; ++i) { g_prof_ns[i] += t.ph[i]; g_prof_cpu_ns[i] += t.cph[i]; total += t.ph[i]; }
        // the first frames after loading are one-off setup with a black screen
        if (g_frame_count > 30 && total > g_prof_worst[1] + g_prof_worst[3] + g_prof_worst[5] + g_prof_worst[7])
        {
            g_prof_worst[0] = g_frame_count;
            for (int i = 0; i < 4; ++i) { g_prof_worst[1 + 2 * i] = t.ph[i]; g_prof_worst[2 + 2 * i] = t.cph[i]; }
            g_prof_worst[9] = runq;
        }
        if (total > 16666667ull && g_frame_count > 1)
            lr_log(RETRO_LOG_INFO, "Slow frame %llu: %.0f ms (engine %.0f, decoder wait %.0f, compose %.0f, "
                   "audio %.0f; waiting for a CPU core %.0f)", (unsigned long long)g_frame_count, total / 1e6,
                   t.ph[0] / 1e6, t.ph[1] / 1e6, t.ph[2] / 1e6, t.ph[3] / 1e6, runq / 1e6);
    }

    if (g_stage == STAGE_FINISHED)
    {
        // an engine error (a Singe script failing, say) stays on screen first
        if (!g_error_msg.empty()) enter_error_mode();
        else environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
