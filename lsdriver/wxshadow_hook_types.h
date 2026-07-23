/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal struct definition shared between deps.c and inline_hook_frame.h
   to avoid duplicate symbol issues. */

#ifndef WXSHADOW_HOOK_TYPES_H
#define WXSHADOW_HOOK_TYPES_H

#include <linux/types.h>

#define HOOK_STUB_WORDS 4

struct hook_entry {
    const char *target_sym;
    uint64_t target_addr;
    void *work_fn;
    uint32_t *trampoline;
    uint32_t saved_insn[HOOK_STUB_WORDS];
    bool installed;
    int slot_index;
};

#endif
