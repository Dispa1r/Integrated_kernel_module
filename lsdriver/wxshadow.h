/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook Module for lsdriver
 *
 * 基于 Shadow Page Table 的无痕断点/Patch 模块 (ARM64)。
 *
 * 核心原理:
 *   - 创建原始代码页的 shadow 副本, 在断点偏移处写入 BRK 指令
 *   - shadow 页映射为 --x (仅执行), 原始页切换为 r-- (仅读)
 *   - 进程读取代码段 → page fault → 临时切回原始页(r--) → 返回原始数据
 *   - 进程执行代码段 → 触发 BRK → 捕获寄存器 → 单步原始指令 → 切回 shadow
 *   - 对进程自身的内存完整性检测完全透明
 *
 * 页面状态机: NONE → SHADOW_X(--x) ↔ ORIGINAL(r--) ↔ STEPPING(r-x) ↘ DORMANT
 */

#ifndef WXSHADOW_H
#define WXSHADOW_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/list.h>
#include <asm/pgtable.h>
#include <asm/memory.h>

/* Forward-declare shared-memory types */
struct wxshadow_bp_cfg;
struct wxshadow_patch_cfg;
struct wxshadow_state_info;

/* BRK 立即数 (不可与内核调试器冲突) */
#define WXSHADOW_BRK_IMM    0x007
#define AARCH64_BREAK_MON   0xd4200000
#define WXSHADOW_BRK_INSN   (AARCH64_BREAK_MON | (WXSHADOW_BRK_IMM << 5))

/* AArch64 指令固定 4 字节 */
#define WXSHADOW_INSN_SIZE  4

/* 每页限制 */
#define WXSHADOW_MAX_BPS_PER_PAGE      128
#define WXSHADOW_MAX_PATCHES_PER_PAGE  128
#define WXSHADOW_MAX_REG_MODS          4
#define WXSHADOW_MAX_PATCH_BYTES       256

/* 页面状态 */
enum wxshadow_state {
    WX_STATE_NONE = 0,      /* 未分配 shadow */
    WX_STATE_ORIGINAL,      /* VA → 原始页 (r--) */
    WX_STATE_SHADOW_X,      /* VA → shadow 页 (--x) */
    WX_STATE_STEPPING,      /* 单步中 (r-x), 指向原始页 */
    WX_STATE_DORMANT,       /* Hook 已退休, VA 还原为原始, shadow 保留 */
};

/* TLB flush 模式 */
enum wxshadow_tlb_mode {
    WX_TLB_MODE_AUTO = 0,
    WX_TLB_MODE_PRECISE,
    WX_TLB_MODE_BROADCAST,
    WX_TLB_MODE_FULL,
};

/* 脏位图 (PAGE_SIZE / 32 words of 64-bit) */
#define WXSHADOW_DIRTY_BITMAP_WORDS ((PAGE_SIZE) / (sizeof(unsigned long) * 8))

/* ---- 内部数据结构 (kernel-only) ---- */

/* 单个寄存器修改 */
struct wxshadow_reg_mod {
    u8 reg_idx;         /* 0-30=x0-x30, 31=sp */
    bool enabled;
    u64 value;
};

/* 单个断点 */
struct wxshadow_bp {
    unsigned long addr;         /* 断点地址 */
    bool active;
    u64 serial;                 /* 写入顺序号 */
    struct wxshadow_reg_mod reg_mods[WXSHADOW_MAX_REG_MODS];
    int nr_reg_mods;
};

/* 单个 Patch */
struct wxshadow_patch {
    u16 offset;                 /* 页内偏移 */
    u16 len;                    /* 长度 (bytes) */
    bool active;
    u64 serial;
    u8 data[WXSHADOW_MAX_PATCH_BYTES];
};

/* 每个目标页面一个实例 */
struct wxshadow_page {
    struct list_head list;

    /* 所有者 */
    void *mm;                   /* struct mm_struct * */
    unsigned long page_addr;    /* 页起始地址 (PAGE_SIZE 对齐) */

    /* 物理页信息 */
    unsigned long pfn_original;
    u64 pte_original;           /* 原始 PTE 快照 */
    unsigned long pfn_shadow;
    void *shadow_kva;           /* shadow 页内核虚拟地址 */
    pte_t *saved_ptep;          /* 原始 PTE 指针 (创建时保存, 销毁时直接写入) */

    /* 状态 */
    enum wxshadow_state state;
    void *stepping_task;        /* 当前单步线程的 task_struct */
    int brk_in_flight;          /* BRK 处理器中尚未进入 STEPPING 的计数 */

    /* 生命周期 */
    int refcount;
    bool dead;
    bool release_pending;       /* 单步完成后彻底释放 */
    bool logical_release_pending;/* 单步完成后进入 DORMANT */
    bool fork_paused;

    /* 每页 PTE 锁 (原子操作) */
    atomic_t pte_lock;

    /* 断点和 Patch */
    struct wxshadow_bp bps[WXSHADOW_MAX_BPS_PER_PAGE];
    int nr_bps;
    struct wxshadow_patch patches[WXSHADOW_MAX_PATCHES_PER_PAGE];
    int nr_patches;
    u64 next_mod_serial;

    /* 脏位图: 标记 shadow 页中哪些 word 已被修改 */
    unsigned long bp_dirty[WXSHADOW_DIRTY_BITMAP_WORDS];
    unsigned long patch_dirty[WXSHADOW_DIRTY_BITMAP_WORDS];
};

/* ---- Hook 方法 ---- */
enum wx_hook_method {
    WX_HOOK_METHOD_NONE = 0,
    WX_HOOK_METHOD_DIRECT,      /* 直接替换函数指针 */
    WX_HOOK_METHOD_REGISTER,    /* register_user_*_hook API */
};

/* ---- 内核函数指针 (运行时解析) ---- */
struct wxshadow_kernel_fns {
    /* 页表操作 */
    void (*flush_tlb_range)(struct vm_area_struct *vma,
                            unsigned long start, unsigned long end);
    void (*flush_tlb_page)(struct vm_area_struct *vma, unsigned long uaddr);

    /* 内核内存分配 */
    void *(*kmalloc)(size_t size, gfp_t flags);
    void (*kfree)(const void *ptr);

    /* Page allocator */
    struct page *(*alloc_page)(gfp_t flags);
    void (*__free_page)(struct page *page);

    /* VA/PA translation */
    unsigned long (*page_to_phys)(struct page *page);
    void *(*phys_to_virt)(unsigned long phys);
    void *(*page_address)(struct page *page);
};

/* ---- 公共 API ---- */

/* 模块初始化/退出 */
int wxshadow_init(void);
void wxshadow_exit(void);

/* 通过共享内存请求处理断点/Patch (参数通过 vmemrw_info.user_buffer 传递) */
int wxshadow_handle_set_bp(int pid, uint64_t addr, const struct wxshadow_bp_cfg *cfg);
int wxshadow_handle_del_bp(int pid, uint64_t addr);
int wxshadow_handle_patch(int pid, uint64_t page_addr, const struct wxshadow_patch_cfg *cfg);
int wxshadow_handle_release(int pid, uint64_t addr);
int wxshadow_handle_get_state(int pid, struct wxshadow_state_info *info);

/* 进程退出时清理该进程的所有 shadow 页 */
void wxshadow_on_process_exit(struct mm_struct *mm);

/* fork 时保护: 让子进程不继承 shadow PFN */
void wxshadow_on_fork_pause(struct mm_struct *mm);
void wxshadow_on_fork_resume(struct mm_struct *mm);

/* 设置 TLB flush 模式 */
void wxshadow_set_tlb_mode(enum wxshadow_tlb_mode mode);
enum wxshadow_tlb_mode wxshadow_get_tlb_mode(void);

#endif /* WXSHADOW_H */
