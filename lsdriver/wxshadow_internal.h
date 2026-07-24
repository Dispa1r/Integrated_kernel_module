/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Internal Header
 * 内部声明、内联辅助函数、内核函数指针
 */

#ifndef WXSHADOW_INTERNAL_H
#define WXSHADOW_INTERNAL_H

#include "wxshadow.h"
#include "lsdriver_log.h"
#include "io_struct.h"
#include <linux/sched.h>

/* Forward declarations to avoid pulling in export_fun.h/inline_hook_frame.h
   which define non-static symbols that cause linker duplicate errors. */

/* from export_fun.h: page table walker (implemented in deps.c) */
pte_t *wx_get_user_pte_impl(struct mm_struct *mm, uint64_t vaddr);
struct task_struct *wx_get_task_by_pid_impl(pid_t pid);

/* from inline_hook_frame.h: hook management (implemented in deps.c) */
int wx_hook_install(const char *sym, uint64_t addr, void *work_fn);
void wx_hook_remove(const char *sym);
unsigned long wx_kallsyms_lookup(const char *name);
int wxshadow_init_hooks(int (*brk_fn)(struct pt_regs *), uint64_t brk_addr,
                         int (*step_fn)(struct pt_regs *), uint64_t step_addr);
void wxshadow_exit_hooks(void);

/* ---- 全局状态 ---- */
extern struct wxshadow_kernel_fns g_wx_fns;
extern struct list_head g_wx_page_list;
extern spinlock_t g_wx_global_lock;       /* 全局 page_list 锁 */
extern atomic_t g_wx_init_done;           /* 模块是否完成初始化 */
extern int g_wx_tlb_mode;                 /* TLB flush 模式 */

/* ---- BRK handler 返回值 ---- */
#define DBG_HOOK_HANDLED    0
#define DBG_HOOK_ERROR      1

/* ---- 断点/Patch 位图操作 ---- */
static inline int wx_bitmap_word_index(unsigned long page_offset)
{
    return page_offset / (WXSHADOW_DIRTY_BITMAP_WORDS ? sizeof(unsigned long) : 8);
}

static inline int wx_bitmap_bit_offset(unsigned long page_offset)
{
    return (page_offset / WXSHADOW_INSN_SIZE) %
           (sizeof(unsigned long) * 8);
}

static inline void wx_set_dirty(unsigned long *bitmap, unsigned long page_offset)
{
    __set_bit(wx_bitmap_bit_offset(page_offset),
              &bitmap[wx_bitmap_word_index(page_offset)]);
}

static inline bool wx_is_dirty(const unsigned long *bitmap, unsigned long page_offset)
{
    return test_bit(wx_bitmap_bit_offset(page_offset),
                    &bitmap[wx_bitmap_word_index(page_offset)]);
}

/* ---- 页表操作 (wxshadow_pgtable.c) ---- */
pte_t *wx_get_user_pte(struct mm_struct *mm, unsigned long vaddr);
int wx_pte_switch_to_shadow(struct wxshadow_page *pg);
int wx_pte_switch_to_original(struct wxshadow_page *pg);
int wx_pte_switch_to_stepping(struct wxshadow_page *pg);
void wx_tlb_flush(struct mm_struct *mm, unsigned long vaddr);
void wx_pte_read_lock(struct wxshadow_page *pg);
void wx_pte_read_unlock(struct wxshadow_page *pg);
void wx_init_icache_fn(void);

/* ---- Shadow 页核心 (wxshadow_core.c) ---- */
struct wxshadow_page *wx_find_page(struct mm_struct *mm, unsigned long page_addr);
struct wxshadow_page *wx_find_by_addr(struct mm_struct *mm, unsigned long addr);
struct wxshadow_page *wx_create_page(struct mm_struct *mm, unsigned long page_addr);
void wx_destroy_page_locked(struct wxshadow_page *pg);
void wx_page_get(struct wxshadow_page *pg);
void wx_page_put(struct wxshadow_page *pg);
void wx_remove_all_for_mm(struct mm_struct *mm);

/* ---- 断点/Patch shadow 页写入 (wxshadow_bp.c) ---- */
int wx_write_bp_to_shadow(struct wxshadow_page *pg, int bp_index);
int wx_write_patch_to_shadow(struct wxshadow_page *pg, int patch_index);
int wx_rebuild_shadow_page(struct wxshadow_page *pg);

/* ---- ESR 解析 ---- */
static inline bool wx_is_brk_insn(unsigned int esr)
{
    /* ESR_EL1.EC == 0x3C 是 BRK 指令异常 */
    return ((esr >> 26) & 0x3F) == 0x3C;
}

static inline unsigned int wx_esr_brk_imm(unsigned int esr)
{
    return (esr >> 0) & 0xFFFF;
}

static inline bool wx_is_single_step(unsigned int esr)
{
    /* ESR_EL1.EC == 0x32 是单步异常 (SS) */
    return ((esr >> 26) & 0x3F) == 0x32;
}

static inline bool wx_is_data_abort(unsigned int esr)
{
    unsigned int ec = (esr >> 26) & 0x3F;
    /* 0x24 = 低地址级 data abort, 0x25 = 同级别 data abort */
    return ec == 0x24 || ec == 0x25;
}

static inline bool wx_is_insn_abort(unsigned int esr)
{
    unsigned int ec = (esr >> 26) & 0x3F;
    return ec == 0x20 || ec == 0x21;
}

static inline bool wx_is_translation_fault(unsigned int esr)
{
    /* DFSC[5:0] = 0b0001xx 是 translation fault */
    unsigned int dfsc = esr & 0x3F;
    return (dfsc & 0x3C) == 0x04;
}

static inline bool wx_is_permission_fault(unsigned int esr)
{
    /* DFSC[5:0] = 0b0011xx 是 permission fault */
    unsigned int dfsc = esr & 0x3F;
    return (dfsc & 0x3C) == 0x0C;
}

/* ---- 寄存器修改 ---- */
static inline void wx_apply_reg_mods(struct pt_regs *regs, struct wxshadow_bp *bp)
{
    int i;
    for (i = 0; i < bp->nr_reg_mods; i++) {
        struct wxshadow_reg_mod *mod = &bp->reg_mods[i];
        if (!mod->enabled) continue;
        if (mod->reg_idx <= 30) {
            /* x0-x30 */
            *(&regs->regs[0] + mod->reg_idx) = mod->value;
        } else if (mod->reg_idx == 31) {
            /* sp */
            regs->sp = mod->value;
        }
    }
}

/* ---- 连接线程中指定的进程 ---- */
struct mm_struct *wx_get_task_mm(int pid);

#endif /* WXSHADOW_INTERNAL_H */
