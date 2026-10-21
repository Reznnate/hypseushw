/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * libco for AArch64, from libretro-common's libco/aarch64.c (public domain,
 * webgeek1234), with the floating-point registers d8-d15 saved as well:
 * the AArch64 calling convention has the callee preserve them, and the
 * original switch loses them, so a float held across a yield of the engine
 * coroutine (a Singe font size, say) came back as garbage.
 *
 * ELF targets (Linux, Android) only: the assembly uses ELF directives.
 */
#define LIBCO_C
#include "libco.h"
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>

static __thread uint64_t co_active_buffer[64];
static __thread cothread_t co_active_handle;

/* layout (bytes): x8-x15 0, x19 72, x20-x29 80, sp/pc 160, d8-d15 176 */
__asm__(
    ".globl co_switch_aarch64\n"
    ".type co_switch_aarch64, %function\n"
    "co_switch_aarch64:\n"
    "  stp x8,  x9,  [x1]\n"
    "  stp x10, x11, [x1, #16]\n"
    "  stp x12, x13, [x1, #32]\n"
    "  stp x14, x15, [x1, #48]\n"
    "  str x19, [x1, #72]\n"
    "  stp x20, x21, [x1, #80]\n"
    "  stp x22, x23, [x1, #96]\n"
    "  stp x24, x25, [x1, #112]\n"
    "  stp x26, x27, [x1, #128]\n"
    "  stp x28, x29, [x1, #144]\n"
    "  mov x16, sp\n"
    "  stp x16, x30, [x1, #160]\n"
    "  stp d8,  d9,  [x1, #176]\n"
    "  stp d10, d11, [x1, #192]\n"
    "  stp d12, d13, [x1, #208]\n"
    "  stp d14, d15, [x1, #224]\n"

    "  ldp x8,  x9,  [x0]\n"
    "  ldp x10, x11, [x0, #16]\n"
    "  ldp x12, x13, [x0, #32]\n"
    "  ldp x14, x15, [x0, #48]\n"
    "  ldr x19, [x0, #72]\n"
    "  ldp x20, x21, [x0, #80]\n"
    "  ldp x22, x23, [x0, #96]\n"
    "  ldp x24, x25, [x0, #112]\n"
    "  ldp x26, x27, [x0, #128]\n"
    "  ldp x28, x29, [x0, #144]\n"
    "  ldp d8,  d9,  [x0, #176]\n"
    "  ldp d10, d11, [x0, #192]\n"
    "  ldp d12, d13, [x0, #208]\n"
    "  ldp d14, d15, [x0, #224]\n"
    "  ldp x16, x17, [x0, #160]\n"
    "  mov sp, x16\n"
    "  br x17\n"
    ".size co_switch_aarch64, .-co_switch_aarch64\n");

void co_switch_aarch64(cothread_t handle, cothread_t current);

cothread_t co_create(unsigned int size, void (*entrypoint)(void))
{
    uint64_t *ptr;
    cothread_t handle;
    size = (size + 1023) & ~1023;
    handle = memalign(1024, size + 512);
    if (!handle) return 0;

    ptr = (uint64_t *)handle;
    for (int i = 0; i < 30; ++i) ptr[i] = 0;
    ptr[20] = (uintptr_t)ptr + size + 512 - 16; /* sp */
    ptr[19] = ptr[20];                          /* x29, frame pointer */
    ptr[21] = (uintptr_t)entrypoint;            /* pc */
    return handle;
}

cothread_t co_active(void)
{
    if (!co_active_handle) co_active_handle = co_active_buffer;
    return co_active_handle;
}

void co_delete(cothread_t handle)
{
    free(handle);
}

void co_switch(cothread_t handle)
{
    cothread_t previous = co_active();
    co_switch_aarch64(co_active_handle = handle, previous);
}
