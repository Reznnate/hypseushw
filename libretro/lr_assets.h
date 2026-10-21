/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * The engine's pics/ and sound/ compiled into the core: lr_assets.c, made by
 * cmake/embed_assets.cmake, defines the table, which ends with a NULL path.
 */
#ifndef LR_ASSETS_H
#define LR_ASSETS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lr_asset { const char *path; const unsigned char *data; size_t size; };
extern const struct lr_asset lr_assets[];

#ifdef __cplusplus
}
#endif

#endif
