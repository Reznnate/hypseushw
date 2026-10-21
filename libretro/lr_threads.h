/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * Helper threads report themselves to the core, so the per-frame timing
 * summary (DAPHNE_LR_PROFILE) can show how much CPU they used.
 */
#ifndef LR_THREADS_H
#define LR_THREADS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { LR_THREAD_DECODER, LR_THREAD_COMPOSE, LR_THREAD_KINDS };

/* Called by a helper thread when it starts.  'kind' is one of LR_THREAD_*. */
void lr_register_cpu_thread(int kind);

#ifdef __cplusplus
}
#endif

#endif
