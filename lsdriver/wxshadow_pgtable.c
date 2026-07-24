/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Page Table Operations
 *
 * PTE 读取/写入/切换, TLB invalidate
 *
 * ARM64 PTE 权限组合:
 *   r-- : PTE_VALID | PTE_USER | PTE_RDONLY | PTE_UXN | PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL)
 *   --x : PTE_VALID | PTE_USER | PTE_PXN | PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL)
 *   r-x : PTE_VALID | PTE_USER | PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL)
 *   rw- : PTE_VALID | PTE_USER | PTE_UXN | PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL)
 */

#include "wxshadow_internal.h"
#include <asm/tlbflush.h>
#include <asm/barrier.h>
#include <linux/highmem.h>

/* PTE 权限位掩码 (ARM64) */
#define PTE_USER               (_AT(pteval_t, 1) << 6)
#define PTE_RDONLY             (_AT(pteval_t, 1) << 7)
#define PTE_SHARED             (_AT(pteval_t, 3) << 8)
#define PTE_AF                 (_AT(pteval_t, 1) << 10)
#define PTE_NG                 (_AT(pteval_t, 1) << 11)
#define PTE_PXN                (_AT(pteval_t, 1) << 53)
#define PTE_UXN                (_AT(pteval_t, 1) << 54)
#define PTE_ATTRINDX(t)        (_AT(pteval_t, (t)) << 2)
#define PTE_TYPE_PAGE          (_AT(pteval_t, 3) << 0)

/* Memory attribute index for normal memory */
#define MT_NORMAL_IDX    4

/* 访问 ASID (Address Space ID) */
#ifndef mm_context_id
static inline u64 wx_mm_asid(struct mm_struct *mm)
{
#ifdef CONFIG_ARM64_ASID_BITS
    return atomic64_read(&mm->context.id);
#else
    return 0;
#endif
}
#else
static inline u64 wx_mm_asid(struct mm_struct *mm)
{
    return (u64)mm_context_id(mm);
}
#endif

/* ---- PTE 遍历 ---- */

/* ---- PTE 加锁 ---- */
void wx_pte_read_lock(struct wxshadow_page *pg)
{
    /* 简单的自旋等待 + 原子获取 */
    while (atomic_cmpxchg(&pg->pte_lock, 0, 1) != 0) {
        cpu_relax();
    }
}

void wx_pte_read_unlock(struct wxshadow_page *pg)
{
    atomic_set(&pg->pte_lock, 0);
}

/* ---- 构建不同状态的 PTE ---- */
static pte_t wx_build_pte_readonly(unsigned long pfn, pte_t template)
{
    pteval_t base = (pfn << PAGE_SHIFT) | PTE_TYPE_PAGE | PTE_USER | PTE_RDONLY |
                    PTE_UXN | PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL_IDX);
    return __pte(base);
}

static pte_t wx_build_pte_execonly(unsigned long pfn)
{
    pteval_t base = (pfn << PAGE_SHIFT) | PTE_TYPE_PAGE | PTE_USER | PTE_PXN |
                    PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL_IDX);
    return __pte(base);
}

static pte_t wx_build_pte_rx(unsigned long pfn)
{
    pteval_t base = (pfn << PAGE_SHIFT) | PTE_TYPE_PAGE | PTE_USER |
                    PTE_AF | PTE_NG | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL_IDX);
    /* 不设置 RDONLY 和 PXN = r-x */
    return __pte(base);
}

/* ---- PTE 写入 ---- */
/* Resolved via kallsyms at init time. __sync_icache_dcache is not exported
 * on Android GKI; if resolution fails, fall back to flush_dcache_page(). */
static void (*fn_sync_icache_dcache)(pte_t pte);

void wx_init_icache_fn(void)
{
    unsigned long addr = wx_kallsyms_lookup("__sync_icache_dcache");
    if (addr) {
        fn_sync_icache_dcache = (void *)addr;
    }
}

static int wx_write_pte(struct mm_struct *mm, unsigned long vaddr, pte_t new_pte)
{
    pte_t *ptep = wx_get_user_pte_impl(mm, vaddr);
    if (!ptep) return -EFAULT;

    set_pte(ptep, new_pte);

    if (fn_sync_icache_dcache) {
        fn_sync_icache_dcache(new_pte);
    } else {
        /* Fallback: clean D-cache for the shadow page via kernel linear map.
         * The userspace agent's DC CIVAC + IC IVAU will handle I-cache. */
        struct page *pg = pfn_to_page(pte_pfn(new_pte));
        if (pg) {
            flush_dcache_page(pg);
        }
    }

    return 0;
}

/* ---- TLB Flush ---- */
void wx_tlb_flush(struct mm_struct *mm, unsigned long vaddr)
{
    /* 使用 inner-shareable TLB invalidate by VA */
    unsigned long addr = vaddr & PAGE_MASK;
    u64 asid;

    switch (g_wx_tlb_mode) {
    case WX_TLB_MODE_FULL:
        /* 刷全部 TLB */
        flush_tlb_all();
        return;

    case WX_TLB_MODE_BROADCAST:
        /* 广播 VA flush 到所有 ASID */
        __tlbi(vaale1is, (addr >> 12));
        break;

    case WX_TLB_MODE_PRECISE:
    case WX_TLB_MODE_AUTO:
    default:
        /* 按 ASID 精确 flush */
        asid = wx_mm_asid(mm);
        if (asid != 0) {
            unsigned long tlbi_arg = (addr >> 12) | (asid << 48);
            __tlbi(vae1is, tlbi_arg);
        } else {
            /* 没有 ASID, 回退到广播 flush */
            __tlbi(vaale1is, (addr >> 12));
        }
        break;
    }

    /* 确保 TLB invalidate 操作完成 */
    dsb(ish);
    isb();
}

/* ---- PTE 状态切换 ---- */

/* 切换到 shadow (--x) */
int wx_pte_switch_to_shadow(struct wxshadow_page *pg)
{
    struct mm_struct *mm = (struct mm_struct *)pg->mm;
    pte_t shadow_pte;
    int ret;

    if (pg->dead) return -EINVAL;
    if (pg->state == WX_STATE_SHADOW_X) {
        printk(KERN_EMERG "WXSHADOW: pte_switch already SHADOW_X\n");
        return 0;
    }

    shadow_pte = wx_build_pte_execonly(pg->pfn_shadow);
    printk(KERN_EMERG "WXSHADOW: pte_switch writing shadow pfn=0x%lx to vaddr=0x%llx\n",
           pg->pfn_shadow, (unsigned long long)pg->page_addr);
    ret = wx_write_pte(mm, pg->page_addr, shadow_pte);
    printk(KERN_EMERG "WXSHADOW: pte_switch write_pte ret=%d state_was=%d\n", ret, (int)pg->state);
    if (ret == 0) {
        pg->state = WX_STATE_SHADOW_X;
        wx_tlb_flush(mm, pg->page_addr);
    }
    return ret;
}

/* 切换到 original (r--) */
int wx_pte_switch_to_original(struct wxshadow_page *pg)
{
    struct mm_struct *mm = (struct mm_struct *)pg->mm;
    pte_t orig_pte;
    int ret;

    if (pg->dead) return -EINVAL;
    if (pg->state == WX_STATE_ORIGINAL) return 0;

    orig_pte = wx_build_pte_readonly(pg->pfn_original, __pte(pg->pte_original));
    ret = wx_write_pte(mm, pg->page_addr, orig_pte);
    if (ret == 0) {
        pg->state = WX_STATE_ORIGINAL;
        wx_tlb_flush(mm, pg->page_addr);
    }
    return ret;
}

/* 切换到 stepping (r-x, 指向原始页, 用于单步执行) */
int wx_pte_switch_to_stepping(struct wxshadow_page *pg)
{
    struct mm_struct *mm = (struct mm_struct *)pg->mm;
    pte_t step_pte;
    int ret;

    if (pg->dead) return -EINVAL;
    if (pg->state == WX_STATE_STEPPING) return 0;

    step_pte = wx_build_pte_rx(pg->pfn_original);
    ret = wx_write_pte(mm, pg->page_addr, step_pte);
    if (ret == 0) {
        pg->state = WX_STATE_STEPPING;
        pg->stepping_task = current;
        wx_tlb_flush(mm, pg->page_addr);
    }
    return ret;
}
