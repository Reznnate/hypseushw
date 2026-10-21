# Upstream sources

| Component | Repository | Version | In this repository |
|-----------|-----------|---------|--------------------|
| Hypseus Singe (the engine) | https://github.com/DirtBagXon/hypseus-singe | a16e2521ee3233ef20c44e562008c47f4b4293df | vendored in `core/`, local changes under `__LIBRETRO__` |
| SDL3 (static in the core, shared in the app) | https://github.com/libsdl-org/SDL | release-3.4.18 (829a65d) | submodule `src/SDL3` |
| libogg | https://github.com/xiph/ogg | v1.3.6 (be05b13) | submodule `src/libogg` |
| libvorbis | https://github.com/xiph/vorbis | v1.3.7 (0657aee) | submodule `src/libvorbis` |
| SDL3_image (Singe images) | https://github.com/libsdl-org/SDL_image | release-3.4.8 (0891fc3) | submodule `src/SDL3_image` |
| SDL3_ttf (Singe text) | https://github.com/libsdl-org/SDL_ttf | release-3.2.2 (a1ce367) | submodule `src/SDL3_ttf` |
| FreeType (for SDL3_ttf) | https://github.com/freetype/freetype | VER-2-14-3 (0a0221a) | submodule `src/freetype` |
| FFmpeg (Singe 2's MP4 on the CPU) | https://github.com/FFmpeg/FFmpeg | n8.1.3 (1041abd) | submodule `src/ffmpeg` |
| libretro-common (libretro.h, libco) | https://github.com/libretro/libretro-common | a3194c18ba65ef89734e1898a2f61403c255d368 | submodule `src/libretro-common` |
| libretro's older Daphne core (reference only, not built) | https://github.com/libretro/daphne | 6f1695dd1f376060666eec0a416ff56bb6c9cccc | not included |

`tools/fetch-deps.sh` (or `git submodule update --init`) checks out the
submodules.  SDL3_image's and SDL3_ttf's own submodules (`external/`) are not
needed: the core builds them without those optional codecs.

The changes to the vendored engine are this repository's history of `core/`
since the import commit ("Vendor hypseus-singe a16e252 as core/").
