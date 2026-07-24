/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Kernel Hook Handlers
 *
 * 通过 inline hook 机制挂载到内核:
 *   - brk_handler      → wx_on_brk()       BRK 断点捕获
 *   - single_step_handler → wx_on_step()    单步完成
 *   - do_page_fault    → wx_on_page_fault() 读取隐藏
 *   - copy_process     → wx_on_fork()       fork 保护
 *   - exit_mmap        → wx_on_exit_mmap()  进程退出清理
 */

#include "wxshadow_internal.h"
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <asm/debug-monitors.h>

/* GKI exports: these may not be declared in all kernel header versions */
/* ---- 内核函数指针 (运行时解析) ---- */

/* 原始的 BRK handler (被替换后需要调用它处理非 wxshadow BRK) */
static int (*orig_brk_handler)(struct pt_regs *regs) = NULL;

/* 原始的 single_step_handler */
static int (*orig_step_handler)(struct pt_regs *regs) = NULL;

/* Single-step functions resolved via kallsyms (not exported for modules) */
static void (*fn_user_enable_single_step)(struct task_struct *task) = NULL;
static void (*fn_user_disable_single_step)(struct task_struct *task) = NULL;
/* ---- BRK Handler ---- */
/*
 * 当目标进程执行到 shadow 页中的 BRK 指令时:
 *   1. 检查是否是 wxshadow BRK (imm == 0x007)
 *   2. 查找对应的断点配置
 *   3. 应用寄存器修改
 *   4. 切换到 STEPPING 状态 (r-x) 执行原始指令
 *   5. 开启单步模式
 */
int wx_on_brk(struct pt_regs *regs)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long pc, page_addr;
    int i, bp_index;
    unsigned long flags;
    unsigned int imm;
    unsigned int esr;
    esr = read_sysreg(esr_el1);
    if (!atomic_read(&g_wx_init_done))
        goto call_orig;

    /* 检查 BRK 立即数 */
    imm = wx_esr_brk_imm(esr);
    if (imm != WXSHADOW_BRK_IMM)
        goto call_orig;

    mm = current->mm;
    if (!mm)
        goto call_orig;

    pc = regs->pc;
    page_addr = pc & PAGE_MASK;

    spin_lock_irqsave(&g_wx_global_lock, flags);

    pg = wx_find_page(mm, page_addr);
    if (!pg || pg->dead) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        goto call_orig;
    }

    /* 查找匹配的断点 */
    bp_index = -1;
    for (i = 0; i < pg->nr_bps; i++) {
        if (pg->bps[i].active && pg->bps[i].addr == pc) {
            bp_index = i;
            break;
        }
    }

    if (bp_index < 0) {
        /* 没有匹配的断点, 可能是初始化前遗留的 BRK */
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        wx_page_put(pg);
        goto call_orig;
    }

    /* 标记 BRK inflight */
    pg->brk_in_flight++;

    /* 应用寄存器修改 */
    wx_apply_reg_mods(regs, &pg->bps[bp_index]);

    /* 切换到 STEPPING */
    pg->stepping_task = current;
    wx_pte_switch_to_stepping(pg);

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    /* 开启单步模式 */
    fn_user_enable_single_step(current);

    /* 告知内核: BRK 已处理, 不要发送 SIGTRAP */
    wx_page_put(pg);
    return DBG_HOOK_HANDLED;

call_orig:
    /* 不是 wxshadow BRK, 交给原始 handler */
    if (orig_brk_handler)
        return orig_brk_handler(regs);
    return DBG_HOOK_HANDLED;
}

/* ---- Single-Step Handler ---- */
/*
 * 单步执行完原始指令后:
 *   1. 关闭单步模式
 *   2. 清除 stepping 状态
 *   3. 如果 pending_release: 释放 shadow 页
 *   4. 如果 logical_release: 进入 DORMANT
 *   5. 否则: 切回 SHADOW_X
 */
int wx_on_step(struct pt_regs *regs)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long pc, page_addr;
    unsigned long flags;
    bool should_release = false;
    bool should_dormant = false;

    if (!atomic_read(&g_wx_init_done))
        goto call_orig;

    mm = current->mm;
    if (!mm)
        goto call_orig;

    pc = regs->pc;
    page_addr = pc & PAGE_MASK;

    /* 关闭单步 */
    fn_user_disable_single_step(current);

    spin_lock_irqsave(&g_wx_global_lock, flags);

    pg = wx_find_page(mm, page_addr);
    if (!pg || pg->dead) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        if (pg) wx_page_put(pg);
        goto call_orig;
    }

    /* 检查是否是 wxshadow 的单步 */
    if (pg->state != WX_STATE_STEPPING && pg->stepping_task != current) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        wx_page_put(pg);
        goto call_orig;
    }

    pg->brk_in_flight--;
    pg->stepping_task = NULL;

    /* 检查延迟释放标志 */
    if (pg->release_pending && pg->brk_in_flight <= 0) {
        should_release = true;
    } else if (pg->logical_release_pending && pg->brk_in_flight <= 0) {
        should_dormant = true;
        pg->logical_release_pending = false;
    } else {
        /* 正常恢复: 切回 shadow */
        wx_pte_switch_to_shadow(pg);
    }

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    if (should_release) {
        wx_remove_all_for_mm(mm);
    } else if (should_dormant) {
        /* 进入 DORMANT: 恢复原始 PTE, 保留 shadow 页 */
        pg->state = WX_STATE_DORMANT;
        wx_pte_switch_to_original(pg);
    }

    wx_page_put(pg);
    return DBG_HOOK_HANDLED;

call_orig:
    if (orig_step_handler)
        return orig_step_handler(regs);
    return DBG_HOOK_HANDLED;
}

/* ---- Page Fault Handler (读取隐藏) ---- */
/*
 * 当进程尝试读取 shadow 页 (--x → 不可读) 时触发 permission fault:
 *   1. 检查是否是 shadow 页上的读 fault
 *   2. 临时切到 ORIGINAL (r--) 页
 *   3. 等待单步执行完读指令后切回 shadow
 *
 * 注意: 实际实现中, 由于 do_page_fault 不经过 inline hook 的单步路径,
 * 这里采用简单策略: 切到 original 让读取完成, 然后在 BRK handler 中切回.
 */
int wx_on_page_fault(struct pt_regs *regs)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long fault_addr, page_addr;
    unsigned long flags;
    bool is_write;
    unsigned int esr;

    esr = read_sysreg(esr_el1);

    if (!atomic_read(&g_wx_init_done))
        return DBG_HOOK_ERROR; /* 不处理, 交给原始 fault handler */

    mm = current->mm;
    if (!mm)
        return DBG_HOOK_ERROR;

    /* 获取 fault 地址 */
    fault_addr = (unsigned long)current->thread.fault_address;
    page_addr = fault_addr & PAGE_MASK;

    /* 只处理 permission fault (非 translation fault) */
    if (!wx_is_permission_fault(esr))
        return DBG_HOOK_ERROR;

    /* 判断是读还是写 */
    is_write = (esr & (1U << 6)) != 0;

    spin_lock_irqsave(&g_wx_global_lock, flags);

    pg = wx_find_page(mm, page_addr);
    if (!pg || pg->dead) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        if (pg) wx_page_put(pg);
        return DBG_HOOK_ERROR;
    }

    /* 只在 shadow 状态且是读 fault 时处理 */
    if (pg->state == WX_STATE_SHADOW_X && !is_write) {
        /* 进程读代码段 → 切到 ORIGINAL 让它读完 */
        wx_pte_switch_to_original(pg);

        /* 标记需要在下次 BRK 时切回 shadow */
        /* 由于读操作不需要单步, 在这里简单延迟切回:
         * 使用一个简单的 trick: 在 BRK handler 中会切回 shadow,
         * 如果没有 BRK, 下一次执行时也会因为 --x 权限而产生
         * permission fault (exec), 那时再处理 */
    }

    spin_unlock_irqrestore(&g_wx_global_lock, flags);
    wx_page_put(pg);

    /* 返回 ERROR 让原始 handler 继续处理:
     * 如果是读且已切回 original, 原始 handler 会重新执行读
     * 如果是其他情况, 原始 handler 正常处理 */
    return DBG_HOOK_ERROR;
}

/* ---- 注册/注销 hooks ---- */

/* 解析内核函数地址 */
static int wx_resolve_kernel_symbols(void)
{
    /* 核心函数 - 通过 lsdriver 的 export_fun.h 解析 */
    /* 这些函数在 lsdriver 初始化时已经解析完毕 */

    /* 解析 brk_handler (内核符号, 可能被 LTO 改名) */
    {
        unsigned long addr;
        addr = (unsigned long)wx_kallsyms_lookup("brk_handler");
        /* LTO may mangle the symbol name; try with common suffix */
        if (!addr) addr = (unsigned long)wx_kallsyms_lookup("brk_handler.llvm.0");
        if (!addr) {
            ls_log_tag("wxshadow", "无法解析 brk_handler\n");
            return -ENOENT;
        }
        orig_brk_handler = (void *)addr;
    }

    /* 解析 single_step_handler */
    {
        unsigned long addr;
        addr = (unsigned long)wx_kallsyms_lookup("single_step_handler");
        if (!addr) addr = (unsigned long)wx_kallsyms_lookup("single_step_handler.llvm.0");
        if (!addr) {
            ls_log_tag("wxshadow", "无法解析 single_step_handler\n");
            return -ENOENT;
        }
        orig_step_handler = (void *)addr;
    }

    ls_log_tag("wxshadow", "内核符号解析完成: brk=%px step=%px\n",
               orig_brk_handler, orig_step_handler);

    

    /* 解析单步函数 (GKI未导出, 需要kallsyms) */
    {
        unsigned long addr;
        addr = (unsigned long)wx_kallsyms_lookup("user_enable_single_step");
        if (addr) fn_user_enable_single_step = (void *)addr;
        addr = (unsigned long)wx_kallsyms_lookup("user_disable_single_step");
        if (addr) fn_user_disable_single_step = (void *)addr;
        if (!fn_user_enable_single_step || !fn_user_disable_single_step) {
            ls_log_tag("wxshadow", "无法解析单步函数n");
            return -ENOENT;
        }
    }

    return 0;
}

/* ---- 模块初始化 ---- */
int wxshadow_init(void)
{
    ls_log_tag("wxshadow", "W^X Shadow Hook init start\n");

    /* Resolve __sync_icache_dcache via kallsyms (not exported on GKI) */
    wx_init_icache_fn();

    /* Hooks are installed by lsdriver_main.o (which owns the trampoline slots).
     * Handler functions (wx_on_brk, wx_on_step) are called from there. */
    atomic_set(&g_wx_init_done, 1);
    ls_log_tag("wxshadow", "W^X Shadow Hook ready\n");
    return 0;
}

/* ---- 模块退出 ---- */
void wxshadow_exit(void)
{
    /* 标记为未初始化 */
    atomic_set(&g_wx_init_done, 0);

    /* Hooks 由 lsdriver_main.o 管理，此处不卸载 */

    /* 清理所有 shadow 页 */
    {
        struct wxshadow_page *pg, *tmp;
        LIST_HEAD(dead_list);

        spin_lock(&g_wx_global_lock);
        list_for_each_entry_safe(pg, tmp, &g_wx_page_list, list) {
            wx_destroy_page_locked(pg);
            list_move(&pg->list, &dead_list);
        }
        spin_unlock(&g_wx_global_lock);

        list_for_each_entry_safe(pg, tmp, &dead_list, list) {
            list_del(&pg->list);
            wx_page_put(pg);
        }
    }

    ls_log_tag("wxshadow", "W^X Shadow Hook 模块已退出\n");
}

