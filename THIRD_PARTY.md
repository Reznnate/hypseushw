# Third-party software

The core and the Android app are distributed under the GNU General Public
License version 3 (see `LICENSE`).  They are built from the following
third-party components, each under its own licence, all compatible with the
GPL v3.  The licence texts are in each component's source.

The complete source code, including the exact revisions of these
components, is at https://github.com/Reznnate/hypseushw

## Vendored in this repository

| Component | Where | Licence |
|---|---|---|
| Hypseus Singe (Daphne engine), Matt Ownby, DirtBagXon and contributors | `core/` | GPL-3.0 (`core/LICENSE`); many files GPL-2.0-or-later |
| libmpeg2 | `core/src/3rdparty/libmpeg2/` | GPL-2.0-or-later |
| plog, Sergey Podobry | `core/src/3rdparty/plog/` | MPL-2.0 |
| Lua 5.1.4, Lua.org, PUC-Rio | `core/src/game/singe/` | MIT |
| LPeg, Lua.org, PUC-Rio | `core/src/game/singe/lpeg/` | MIT |
| LuaSocket, Diego Nehab | `core/src/game/singe/socket/` | MIT |
| Lua CJSON, Mark Pulford | `core/src/game/singe/cjson/` | MIT |
| LuaFileSystem, Kepler Project | `core/src/game/singe/lfs.c` | MIT |
| librs232, Petr Stetiar | `core/src/game/singe/rs232*` | MIT |
| SDL3_gfx | `core/src/game/singe/sdl3_gfx/` | zlib |
| ManyMouse, Ryan C. Gordon | `core/src/manymouse/` | zlib |
| libogg configuration header | `libretro/deps/config_types.h` | BSD-3-Clause |

## Git submodules (`src/`, pinned in `UPSTREAM.md`)

| Component | Licence |
|---|---|
| SDL3 | zlib |
| SDL3_image | zlib |
| SDL3_ttf | zlib |
| FreeType | FreeType License (FTL), chosen from FTL / GPL-2.0-or-later |
| libogg | BSD-3-Clause |
| libvorbis | BSD-3-Clause |
| FFmpeg (libavformat, libavcodec, libavutil; MP4/Matroska demuxers, H.264 and AAC decoders only, built without its GPL or non-free parts) | LGPL-2.1-or-later |
| libretro-common (`libretro.h`) | MIT |
| libretro-common libco | public domain (`libretro/lr_libco_aarch64.c` is derived from it) |

zlib comes from the system (MSYS2) or the Android NDK and is under the zlib
licence.

## FreeType credit

Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org).  All rights reserved.
