/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Engine globals and helpers normally defined in core/src/hypseus.cpp and
 * globals.h (which also hold the standalone main()).
 */

#include <SDL3/SDL.h>
#include <plog/Log.h>
#include <string.h>

#include "config.h"
#include "hypseus.h"
#include "game/game.h"
#include "ldp-out/ldp.h"

game *g_game = NULL;
ldp *g_ldp   = NULL;

static Uint8 quitflag              = 0;
static int search_offset           = 0;
static Uint8 frame_modifier        = 0;
static Uint8 realscoreboard        = 0;
static unsigned int rsb_port       = 0;
static unsigned int idleexit       = 0;
static unsigned char startsilent   = 0;
static unsigned char scoreboard_usb_port = 0;
static unsigned char scoreboard_usb_impl = 0;
static unsigned int scoreboard_usb_baud  = 300;
static bool scoreboard_usb_rts     = false;

const char *get_hypseus_version() { return hypseus_VERSION; }

unsigned char get_filename(char *s, unsigned char n)
{
    unsigned char result = 0;
    for (size_t i = 0; i < strlen(s); i++) {
        if (s[i] == '+') {
            result = 1;
            s[i]   = (char)(n + 0x30);
        }
    }
    return result;
}

void set_quitflag() { quitflag = 1; }
unsigned char get_quitflag() { return quitflag; }
void lr_clear_quitflag() { quitflag = 0; }

// The working directory belongs to the process, frontend included: the core
// alone changes it, to a batch game's folder for the length of that game, and
// restores it (enter_dir/leave_dir in lr_core.cpp).  The engine's own
// requests are refused.
bool change_dir(const char *dir)
{
    LOGW << "ignoring request to change directory to " << (dir ? dir : "(null)");
    return false;
}
void set_cur_dir(const char *) {}

void set_search_offset(int i) { search_offset = i; }
int get_search_offset() { return search_offset; }
unsigned char get_frame_modifier() { return frame_modifier; }
void set_frame_modifier(unsigned char value) { frame_modifier = value; }

void set_scoreboard(unsigned char value) { realscoreboard = value; }
unsigned char get_scoreboard() { return realscoreboard; }
void set_scoreboard_port(unsigned int value) { rsb_port = value; }
unsigned int get_scoreboard_port() { return rsb_port; }
void set_scoreboard_usb_port(unsigned char value) { scoreboard_usb_port = value; }
void set_scoreboard_usb_impl(unsigned char value) { scoreboard_usb_impl = value; }
void set_scoreboard_usb_baud(unsigned int value) { scoreboard_usb_baud = value; }
void set_scoreboard_usb_rts(bool value) { scoreboard_usb_rts = value; }
unsigned char get_usb_port() { return scoreboard_usb_port; }
unsigned char get_usb_impl() { return scoreboard_usb_impl; }
unsigned int get_usb_baud() { return scoreboard_usb_baud; }
bool get_usb_rts() { return scoreboard_usb_rts; }

// logging goes to the frontend (see lr_core.cpp)
void reset_logfile(int, char **) {}
void set_log_was_disabled(bool) {}

// io/network.cpp (usage statistics) is not built
void net_server_send(bool) {}
bool net_send_enabled() { return false; }
unsigned int get_sys_mem() { return 0; }
char *get_sdl_compile() { static char s[] = "SDL3 (static)"; return s; }
char *get_sdl_linked() { return get_sdl_compile(); }
// no build time: builds are reproducible (the core's version says which)
char *get_build_time() { static char s[] = "libretro core " LR_VERSION; return s; }
const char *get_id() { return "libretro"; }
char *get_os_description() { static char s[] = "libretro"; return s; }
char *get_cpu_name() { static char s[] = "unknown"; return s; }
char *get_video_description() { static char s[] = "libretro"; return s; }

void set_idleexit(unsigned int value) { idleexit = value; }
unsigned int get_idleexit() { return idleexit; }
void set_startsilent(unsigned char value) { startsilent = value; }
unsigned char get_startsilent() { return startsilent; }
