/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Core Lifecycle
 *
 * Shadow page 的创建、查找、销毁、引用计数管理
 */

#include "wxshadow_internal.h"
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <asm/page.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <asm/pgalloc.h>

/* ---- 全局状态 ---- */
struct wxshadow_kernel_fns g_wx_fns;
LIST_HEAD(g_wx_page_list);
DEFINE_SPINLOCK(g_wx_global_lock);
atomic_t g_wx_init_done = ATOMIC_INIT(0);
int g_wx_tlb_mode = WX_TLB_MODE_AUTO;

/* ---- 引用计数 ---- */
void wx_page_get(struct wxshadow_page *pg)
{
    if (!pg) return;
    unsigned long flags;
    spin_lock_irqsave(&g_wx_global_lock, flags);
    pg->refcount++;
    spin_unlock_irqrestore(&g_wx_global_lock, flags);
}

void wx_page_put(struct wxshadow_page *pg)
{
    if (!pg) return;
    unsigned long flags;
    bool destroy = false;

    spin_lock_irqsave(&g_wx_global_lock, flags);
    pg->refcount--;
    if (pg->refcount <= 0 && pg->dead) {
        destroy = true;
    }
    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    if (destroy) {
        if (pg->shadow_kva)
            free_page((unsigned long)pg->shadow_kva);
        kfree(pg);
    }
}

/* ---- 查找 ---- */
struct wxshadow_page *wx_find_page(struct mm_struct *mm, unsigned long page_addr)
{
    struct wxshadow_page *pg;
    unsigned long page = page_addr & PAGE_MASK;

    list_for_each_entry(pg, &g_wx_page_list, list) {
        if (pg->mm == mm && pg->page_addr == page) {
            pg->refcount++;
            return pg;
        }
    }
    return NULL;
}

struct wxshadow_page *wx_find_by_addr(struct mm_struct *mm, unsigned long addr)
{
    return wx_find_page(mm, addr & PAGE_MASK);
}

/* ---- 创建 ---- */
struct wxshadow_page *wx_create_page(struct mm_struct *mm, unsigned long page_addr)
{
    struct wxshadow_page *pg;
    pte_t *ptep;
    pte_t pte;
    pte_t *saved_ptep = NULL;
    unsigned long page = page_addr & PAGE_MASK;
    void *shadow_kva;
    unsigned long pfn_original;

    pr_err("WXSHADOW: wx_create_page enter mm=%px page=0x%llx\n",
           mm, (unsigned long long)page);

    /* 分配 shadow 页 */
    shadow_kva = (void *)__get_free_page(GFP_KERNEL);
    if (!shadow_kva) {
        printk(KERN_EMERG "WXSHADOW: alloc shadow page failed\n");
        return NULL;
    }
    printk(KERN_EMERG "WXSHADOW: shadow_kva=%px\n", shadow_kva);

    /* 获取原始页内容: 用 get_user_pages_remote 代替手动 PTE walk。
     * GUP 是内核标准 API，能处理所有页表级别和 huge page 情况。 */
    {
        struct page *user_page = NULL;
        int ret;
        void *orig_kva;

        ret = get_user_pages_remote(mm, page, 1, 0, &user_page, NULL);
        if (ret <= 0 || !user_page) {
            printk(KERN_EMERG "WXSHADOW: GUP failed page=0x%llx ret=%d\n",
                   (unsigned long long)page, ret);
            free_page((unsigned long)shadow_kva);
            return NULL;
        }

        pfn_original = page_to_pfn(user_page);

        /* 保存原始 PTE 快照和 PTE 指针 (用于后续还原) */
        {
            struct mm_struct *saved_mm = current->active_mm;
            mmgrab(mm);
            mmput(saved_mm);
            current->active_mm = mm;
            ptep = wx_get_user_pte_impl(mm, page);
            if (ptep) {
                pte = READ_ONCE(*ptep);
                saved_ptep = ptep;  /* 保存指针供 destroy 直接使用 */
            } else {
                pte = __pte(0);
                saved_ptep = NULL;
            }
            mmgrab(saved_mm);
            mmput(mm);
            current->active_mm = saved_mm;
        }

        /* 复制内容到 shadow 页 */
        orig_kva = kmap_local_page(user_page);
        if (orig_kva) {
            memcpy(shadow_kva, orig_kva, PAGE_SIZE);
            kunmap_local(orig_kva);
        }
        put_page(user_page);
    }

    /* pte and pfn_original were captured inside the active_mm scope above */

    /* 分配 wxshadow_page 结构 */
    pg = kzalloc(sizeof(*pg), GFP_KERNEL);
    if (!pg) {
        printk(KERN_EMERG "WXSHADOW: kzalloc wxshadow_page failed\n");
        free_page((unsigned long)shadow_kva);
        return NULL;
    }
    printk(KERN_EMERG "WXSHADOW: page created pfn_orig=0x%lx shadow_pfn=0x%lx\n",
           pfn_original, pg->pfn_original);

    /* 初始化 */
    pg->mm = mm;
    pg->page_addr = page;
    pg->pfn_original = pfn_original;
    pg->pte_original = pte_val(pte);
    pg->pfn_shadow = virt_to_pfn(shadow_kva);
    pg->shadow_kva = shadow_kva;
    pg->saved_ptep = saved_ptep;
    pg->state = WX_STATE_ORIGINAL; /* PTE not yet switched — done by caller */
    pg->stepping_task = NULL;
    pg->brk_in_flight = 0;
    pg->refcount = 1;   /* 调用者持有 */
    pg->dead = false;
    pg->release_pending = false;
    pg->logical_release_pending = false;
    pg->fork_paused = false;
    atomic_set(&pg->pte_lock, 0);
    pg->nr_bps = 0;
    pg->nr_patches = 0;
    pg->next_mod_serial = 0;
    INIT_LIST_HEAD(&pg->list);

    ls_log_tag("wxshadow", "创建 shadow page=0x%llx orig_pfn=0x%lx shadow_pfn=0x%lx\n",
               (unsigned long long)page, pfn_original, pg->pfn_shadow);

    /* 加入全局列表 (列表持有引用) */
    pg->refcount++;
    spin_lock(&g_wx_global_lock);
    list_add(&pg->list, &g_wx_page_list);
    spin_unlock(&g_wx_global_lock);

    return pg;
}

/* ---- 销毁 ---- */
void wx_destroy_page_locked(struct wxshadow_page *pg)
{
    struct mm_struct *target_mm;
    pte_t *ptep;

    if (!pg) return;
    pg->dead = true;

    /* PTE 恢复: 释放 spinlock 后走路页表获取当前 PTE 指针,
     * 再用正确的指针写回原始值。缓存指针可能因 THP split 等失效。 */
    target_mm = (struct mm_struct *)pg->mm;
    if (!target_mm || !pg->pte_original)
        return;

    spin_unlock(&g_wx_global_lock);

    /* 切换到目标进程地址空间后走路页表 */
    {
        struct mm_struct *saved_mm = current->active_mm;
        mmgrab(target_mm);
        mmput(saved_mm);
        current->active_mm = target_mm;

        ptep = wx_get_user_pte_impl(target_mm, pg->page_addr);
        if (ptep) {
            set_pte(ptep, __pte(pg->pte_original));
            wx_tlb_flush(target_mm, pg->page_addr);
        }

        mmgrab(saved_mm);
        mmput(target_mm);
        current->active_mm = saved_mm;
    }

    spin_lock(&g_wx_global_lock);
}

/* ---- 进程退出时清理所有 shadow 页 ---- */
void wx_remove_all_for_mm(struct mm_struct *mm)
{
    struct wxshadow_page *pg;
    LIST_HEAD(dead_list);
    bool found;

    /* wx_destroy_page_locked 会临时释放锁, 因此每次只处理一个条目 */
    do {
        found = false;
        spin_lock(&g_wx_global_lock);
        list_for_each_entry(pg, &g_wx_page_list, list) {
            if (pg->mm == mm) {
                wx_destroy_page_locked(pg);  /* 内部 unlock/lock */
                list_del(&pg->list);
                pg->refcount--;
                list_add(&pg->list, &dead_list);
                found = true;
                break;  /* 列表可能已变, 重新开始遍历 */
            }
        }
        spin_unlock(&g_wx_global_lock);

        /* 在锁外释放 */
        if (found) {
            pg = list_first_entry(&dead_list, struct wxshadow_page, list);
            list_del(&pg->list);
            wx_page_put(pg);
        }
    } while (found);
}

/* ---- 根据 PID 获取 mm_struct ---- */
struct mm_struct *wx_get_task_mm(int pid)
{
    struct task_struct *task;
    struct mm_struct *mm;

    task = wx_get_task_by_pid_impl(pid);
    if (!task) return NULL;

    mm = get_task_mm(task);
    put_task_struct(task);
    return mm;
}

/* ---- 退出回调 (在 do_exit 的 hook 中使用) ---- */
void wxshadow_on_process_exit(struct mm_struct *mm)
{
    if (!atomic_read(&g_wx_init_done)) return;
    if (!mm) return;
    wx_remove_all_for_mm(mm);
}

/* ---- fork 保护: 子进程不继承 shadow PFN ---- */
void wxshadow_on_fork_pause(struct mm_struct *mm)
{
    struct wxshadow_page *pg;

    if (!atomic_read(&g_wx_init_done)) return;
    if (!mm) return;

    spin_lock(&g_wx_global_lock);
    list_for_each_entry(pg, &g_wx_page_list, list) {
        if (pg->mm == mm && pg->state == WX_STATE_SHADOW_X) {
            /* 暂时切回原始 PTE, 子进程只会看到原始映射 */
            pte_t *ptep = wx_get_user_pte_impl(mm, pg->page_addr);
            if (ptep) {
                pte_t orig = __pte(pg->pte_original);
                /* 保持 writable 属性以确保 copy-on-write 正常工作 */
                set_pte(ptep, orig);
            }
            pg->fork_paused = true;
        }
    }
    spin_unlock(&g_wx_global_lock);

    /* TLB flush 由调用者 (copy_process hook) 负责 */
}

void wxshadow_on_fork_resume(struct mm_struct *mm)
{
    struct wxshadow_page *pg;

    if (!atomic_read(&g_wx_init_done)) return;
    if (!mm) return;

    spin_lock(&g_wx_global_lock);
    list_for_each_entry(pg, &g_wx_page_list, list) {
        if (pg->mm == mm && pg->fork_paused) {
            /* 恢复 shadow 映射 */
            wx_pte_switch_to_shadow(pg);
            pg->fork_paused = false;
        }
    }
    spin_unlock(&g_wx_global_lock);
}

/* ---- TLB flush mode ---- */
void wxshadow_set_tlb_mode(enum wxshadow_tlb_mode mode)
{
    g_wx_tlb_mode = (int)mode;
    ls_log_tag("wxshadow", "TLB mode=%d\n", g_wx_tlb_mode);
}

enum wxshadow_tlb_mode wxshadow_get_tlb_mode(void)
{
    return (enum wxshadow_tlb_mode)g_wx_tlb_mode;
}
