/*
 * $Id$
 * Retro path library
 *
 * ____ HYPSEUS COPYRIGHT NOTICE ____
 *
 * Copyright (C) 2026 DirtBagXon
 *
 * This file is part of HYPSEUS SINGE, a laserdisc arcade game emulator
 *
 * HYPSEUS SINGE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * HYPSEUS SINGE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __LIBRETRO__
#include <errno.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

#include "luretro.h"

enum {
    PATH_DAPHNE,
    PATH_SINGE,
    PATH_FRAMEWORK,
    PATH_END
};

static char g_abpath[REWRITE_MAXPATH] = "\0";
static unsigned char g_espath = 0;
static unsigned char g_zipath = 0;

unsigned char get_espath() { return g_espath; }
unsigned char get_zipath() { return g_zipath; }

void lua_set_espath(unsigned char value) { g_espath = value; }
void lua_set_zipath(unsigned char value) { g_zipath = value; }

void lua_set_abpath(const char *value)
{
    strncpy(g_abpath, value, sizeof(g_abpath) - 1);
    g_abpath[sizeof(g_abpath) - 1] = '\0';
}

unsigned char bPath(const char* src, const char* path)
{
    return (strncmp(src, path, strlen(path)) == 0);
}

// We block in lfs, isolate to zip rampath via io
int lua_mkdir(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) == -1) {
        /* EEXIST: a drive root ("C:") fails stat but exists */
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    return 0;
}

void lua_espath(const char *src, char *dst, int dstsize)
{
    char *out = dst;
    int rem = dstsize - 1;

    unsigned char bSet = 0, end = 0, folder = 0, path = PATH_DAPHNE;

    if (bPath(src, "singe/Framework"))
        path = PATH_FRAMEWORK;

    if (bPath(src, "singe/")) {
        folder = PATH_SINGE;
        src += 6;
    }

    #define SAFE_APPEND_STR(str)                                   \
        do {                                                       \
            size_t slen = strlen(str);                             \
            if ((int)slen > rem) slen = rem;                       \
            memcpy(out, str, slen);                                \
            out += slen;                                           \
            rem -= slen;                                           \
        } while (0)

    #define SAFE_APPEND_CH(ch)         \
        do {                           \
            if (rem > 0) {             \
                *out++ = (ch);         \
                rem--;                 \
            }                          \
        } while (0)

    for (int i = 0; rem > 0; src++, i++) {
        if (end != PATH_END) {
            if (*src == '\0') {
                end = PATH_END;
            }

            if (i == 0 && folder == PATH_SINGE) {
                if (g_abpath[0] != 0) {
                    SAFE_APPEND_STR(g_abpath);
                } else {
                    const char* traverse = "/../";
                    const char* romdir = get_romdir_path();

                    SAFE_APPEND_STR(romdir);
                    SAFE_APPEND_STR(traverse);
                }
            }

            if (*src == '/') {
                if (bSet < 0xf) {
                    bSet++;
                    continue;
                }
            }

            if (bSet == 1) {
                switch(path) {
                case PATH_FRAMEWORK:
                    SAFE_APPEND_CH('/');
                    break;
                default:
#ifdef ABSTRACT_SINGE
                    SAFE_APPEND_STR(".hypseus/");
#else
                    SAFE_APPEND_STR(".daphne/");
#endif
                    break;
                }
                bSet = 0xf; //bool
            }

            if (*src != '\0') {
	        char ch = *src;
#ifdef _WIN32
                if (ch == '\\') ch = '/';
#endif
                SAFE_APPEND_CH(ch);
            }
            else
                break;
        }
    }

    *out = '\0';

    #undef SAFE_APPEND_STR
    #undef SAFE_APPEND_CH
}

void lua_setmeta(void* d, size_t n, long long s)
{
    uint8_t k[32] = {0};
    uint8_t p[12] = {0};

    lua_settab((uint64_t)s, k, p);
    lua_push(d, n, k, p, 0);
}

void lua_rampath(const char *src, char *dst, int dstsize)
{
    const char *ramdir = get_ramdir_path();
    size_t ramdir_len = strlen(ramdir);

    if (ramdir_len >= (size_t)dstsize) {
        dst[0] = '\0';
        return;
    }

    memcpy(dst, ramdir, ramdir_len);

    char *out = dst + ramdir_len;
    int rem = dstsize - ramdir_len - 1;

    while (*src && rem > 0) {
        char ch = *src++;
#ifdef _WIN32
        if (ch == '\\') ch = '/';
#endif
        *out++ = ch;
        rem--;
    }

#ifdef __LIBRETRO__
    /* a shortened name would be a different file: fail instead */
    if (*src) {
        dst[0] = '\0';
        return;
    }
#endif
    *out = '\0';
}

#ifdef __LIBRETRO__
static int lr_has_parent_ref(const char *p)
{
    for (;;) {
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - p == 2 && p[0] == '.' && p[1] == '.') return 1;
        if (!*e) return 0;
        p = e + 1;
    }
}

/* The game data stays read-only in the libretro core: files a game writes
 * live under the RAM dir (the frontend's save folder), and reads look there
 * first.  Fills dst and returns 1 for a relative path that stays inside the
 * RAM dir; returns 0 with errno set for an absolute path, a ".." segment or
 * a name too long for dst. */
int lua_lr_ramfile(const char *src, char *dst, int dstsize)
{
    dst[0] = '\0';
    if (!src || src[0] == '/' || src[0] == '\\' || (src[0] && src[1] == ':') ||
        lr_has_parent_ref(src)) {
        errno = EACCES;
        return 0;
    }
    lua_rampath(src, dst, dstsize);
    if (dst[0] == '\0') {
        errno = ENAMETOOLONG;
        return 0;
    }
    return 1;
}

static int lr_copy_file(const char *from, const char *to)
{
    char buf[16384];
    size_t n;
    int ok = 1;
    FILE *in = fopen(from, "rb"), *out;

    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) remove(to);
    return ok;
}

/* Opens a file the way io.open, io.input/io.output and io.lines do here.
 * Writes ("w", "a" or any "+" mode) only go to the RAM dir; anything else is
 * refused with EACCES.  "r+" on a file the game ships but has not saved yet
 * starts from a copy of the shipped file, so updating it in place works
 * without touching the game's folder.  Returns 1 when the open is decided
 * (*pf, or NULL with errno set; used names the path for the error), or 0
 * for a read the RAM dir has no copy of: the caller then reads the game's
 * own file as upstream does. */
int lua_lr_fopen(const char *filename, const char *mode, FILE **pf,
                 char *used, int usedsize)
{
    int ram = lua_lr_ramfile(filename, used, usedsize);

    *pf = NULL;
    if (!strpbrk(mode, "wa+")) {
        if (ram) *pf = fopen(used, mode);
        if (*pf) return 1;
        snprintf(used, usedsize, "%s", filename);
        return 0;
    }

    if (!ram) {
        int err = errno;
        snprintf(used, usedsize, "%s", filename);
        errno = err;
        return 1;
    }
    if (lua_chkdir(used) != 0) return 1;
    if (mode[0] == 'r') {
        struct stat st;
        if (stat(used, &st) != 0) lr_copy_file(filename, used);
    }
    *pf = fopen(used, mode);
    return 1;
}
#endif

int lua_chkdir(const char *path)
{
    char tmp[REWRITE_MAXPATH];

    int out = snprintf(tmp, sizeof(tmp), "%s", path);
    if (out < 0 || out >= (int)sizeof(tmp))
        return -1;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';

            if (lua_mkdir(tmp) != 0)
                return -1;

            *p = '/';
        }
    }

    return 0;
}
