/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Dependency Wrappers
 *
 * Contains local implementations of functions from export_fun.h (page table
 * walk, task lookup) and wrapper around inline_hook_frame.h hook functions.
 * This file does NOT include export_fun.h or inline_hook_frame.h to avoid
 * duplicate non-static symbols at link time.
 */

#include "wxshadow_internal.h"
#include "inline_hook_frame.h"
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <asm/pgtable.h>



/* ---- Local page table walk (same logic as export_fun.h get_user_pte) ---- */
static pte_t *wx_local_get_pte(struct mm_struct *mm, uint64_t vaddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;

    if (!mm) return NULL;
    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return NULL;
    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return NULL;
    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud)) return NULL;
    if (pud_leaf(*pud)) return NULL;
    if (pud_bad(*pud)) return NULL;
    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd)) return NULL;
    if (pmd_leaf(*pmd)) return NULL;
    if (pmd_bad(*pmd)) return NULL;
    return pte_offset_kernel(pmd, vaddr);
}

static struct task_struct *wx_local_get_task(pid_t pid)
{
    struct pid *pid_struct;
    struct task_struct *task;
    pid_struct = find_get_pid(pid);
    if (!pid_struct) return NULL;
    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    return task;
}

/* ---- Public wrappers ---- */
pte_t *wx_get_user_pte_impl(struct mm_struct *mm, uint64_t vaddr)
{
    return wx_local_get_pte(mm, vaddr);
}

struct task_struct *wx_get_task_by_pid_impl(pid_t pid)
{
    return wx_local_get_task(pid);
}

unsigned long wx_kallsyms_lookup(const char *name)
{
    return generic_kallsyms_lookup_name(name);
}

/* ---- Managed hooks (opaque state, only deps.c owns hook_entry instances) ---- */
static struct hook_entry g_brk_hook;
static struct hook_entry g_step_hook;

int wx_hook_install(const char *sym, uint64_t addr, void *work_fn)
{
    struct hook_entry *h;
    if (!g_brk_hook.installed) {
        h = &g_brk_hook;
    } else if (!g_step_hook.installed) {
        h = &g_step_hook;
    } else {
        return -ENOSPC;
    }
    memset(h, 0, sizeof(*h));
    h->target_sym = sym;
    h->target_addr = addr;
    h->work_fn = work_fn;
    return hook_entry_install(h);
}

void wx_hook_remove(const char *sym)
{
    if (g_brk_hook.installed && g_brk_hook.target_sym &&
        strcmp(g_brk_hook.target_sym, sym) == 0) {
        hook_entry_remove(&g_brk_hook);
    }
    if (g_step_hook.installed && g_step_hook.target_sym &&
        strcmp(g_step_hook.target_sym, sym) == 0) {
        hook_entry_remove(&g_step_hook);
    }
}

/* Resolve fn_aarch64_insn_patch_text for THIS translation unit.
   Each .o that includes inline_hook_frame.h has its own static copy;
   deps.o's copy must be initialized before hook_entry_install() calls
   trampoline_patch() which dereferences it. */
static int wx_init_patch_text_fn(void)
{
    unsigned long addr;
    if (!fn_aarch64_insn_patch_text) {
        addr = wx_kallsyms_lookup("aarch64_insn_patch_text");
        if (addr)
            fn_aarch64_insn_patch_text = (void *)addr;
    }
    return fn_aarch64_insn_patch_text ? 0 : -ENOENT;
}

int wxshadow_init_hooks(int (*brk_fn)(struct pt_regs *), uint64_t brk_addr,
                         int (*step_fn)(struct pt_regs *), uint64_t step_addr)
{
    int ret;

    /* Must init fn_aarch64_insn_patch_text before hook ops use trampoline_patch */
    ret = wx_init_patch_text_fn();
    if (ret != 0) {
        ls_log_tag("wxshadow", "无法解析 aarch64_insn_patch_text\n");
        return ret;
    }

    ret = wx_hook_install("brk_handler", brk_addr, (void *)brk_fn);
    if (ret != 0) return ret;
    ret = wx_hook_install("single_step_handler", step_addr, (void *)step_fn);
    if (ret != 0) {
        wx_hook_remove("brk_handler");
        return ret;
    }
    return 0;
}

void wxshadow_exit_hooks(void)
{
    wx_hook_remove("single_step_handler");
    wx_hook_remove("brk_handler");
}

