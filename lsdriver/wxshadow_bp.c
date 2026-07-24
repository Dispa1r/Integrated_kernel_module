/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * W^X Shadow Memory Hook - Breakpoint & Patch Operations
 *
 * 通过共享内存接口处理:
 *   - request_op_wxshadow_set_bp:  设置无痕断点
 *   - request_op_wxshadow_del_bp:  删除无痕断点
 *   - request_op_wxshadow_patch:   设置无痕 Patch
 *   - request_op_wxshadow_release: 释放 shadow 页
 *   - request_op_wxshadow_get_state: 查询状态
 */

#include "wxshadow_internal.h"
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>

/* ---- Shadow 页脏位图管理 ---- */
static void wx_mark_bp_dirty(struct wxshadow_page *pg, int bp_index)
{
    if (!pg || bp_index < 0 || bp_index >= WXSHADOW_MAX_BPS_PER_PAGE) return;
    if (!pg->bps[bp_index].active) return;

    unsigned long offset = pg->bps[bp_index].addr & (PAGE_SIZE - 1);
    __set_bit(wx_bitmap_bit_offset(offset), &pg->bp_dirty[wx_bitmap_word_index(offset)]);
}

/* ---- 将断点写入 shadow 页 ---- */
int wx_write_bp_to_shadow(struct wxshadow_page *pg, int bp_index)
{
    unsigned long page_offset;
    u32 *shadow_ptr;

    if (!pg || !pg->shadow_kva) return -EINVAL;
    if (bp_index < 0 || bp_index >= pg->nr_bps) return -EINVAL;
    if (!pg->bps[bp_index].active) return -ENOENT;

    page_offset = pg->bps[bp_index].addr & (PAGE_SIZE - 1);
    if (page_offset + WXSHADOW_INSN_SIZE > PAGE_SIZE) return -EINVAL;

    shadow_ptr = (u32 *)((unsigned long)pg->shadow_kva + page_offset);
    *shadow_ptr = WXSHADOW_BRK_INSN;

    pg->bps[bp_index].serial = pg->next_mod_serial++;
    wx_set_dirty(pg->bp_dirty, page_offset);

    return 0;
}

/* ---- 将 Patch 写入 shadow 页 ---- */
int wx_write_patch_to_shadow(struct wxshadow_page *pg, int patch_index)
{
    unsigned long offset;
    u8 *shadow_ptr;

    if (!pg || !pg->shadow_kva) return -EINVAL;
    if (patch_index < 0 || patch_index >= pg->nr_patches) return -EINVAL;
    if (!pg->patches[patch_index].active) return -ENOENT;

    offset = pg->patches[patch_index].offset;
    if (offset + pg->patches[patch_index].len > PAGE_SIZE) return -EINVAL;

    shadow_ptr = (u8 *)pg->shadow_kva + offset;
    memcpy(shadow_ptr, pg->patches[patch_index].data, pg->patches[patch_index].len);
    pg->patches[patch_index].serial = pg->next_mod_serial++;

    /* 标记脏区域 */
    {
        int i;
        for (i = 0; i < pg->patches[patch_index].len; i += WXSHADOW_INSN_SIZE) {
            wx_set_dirty(pg->patch_dirty, offset + i);
        }
    }

    return 0;
}

/* ---- 重建整个 shadow 页 (按 serial 顺序写入所有 BP 和 Patch) ---- */
int wx_rebuild_shadow_page(struct wxshadow_page *pg)
{
    int i;

    if (!pg || !pg->shadow_kva) return -EINVAL;

    /* 复制原始页 (kmap 安全方式) */
    {
        struct page *orig_page = pfn_to_page(pg->pfn_original);
        void *orig_kva = kmap_local_page(orig_page);
        if (!orig_kva) return -EFAULT;
        memcpy(pg->shadow_kva, orig_kva, PAGE_SIZE);
        kunmap_local(orig_kva);
    }

    /* 按 serial 顺序重建所有修改 (简单的冒泡排序) */
    /* 由于 bp/patch 数量有限, 直接遍历写入即可 */
    pg->next_mod_serial = 0;

    /* 写入所有 patch */
    for (i = 0; i < pg->nr_patches; i++) {
        if (pg->patches[i].active) {
            wx_write_patch_to_shadow(pg, i);
        }
    }

    /* 写入所有断点 (BRK 指令在 patch 之上) */
    for (i = 0; i < pg->nr_bps; i++) {
        if (pg->bps[i].active) {
            wx_write_bp_to_shadow(pg, i);
        }
    }

    return 0;
}

/* ========================================================
 *   共享内存请求处理
 * ======================================================== */

/* ---- 设置断点 ---- */
int wxshadow_handle_set_bp(int pid, uint64_t addr, const struct wxshadow_bp_cfg *cfg)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long page_addr;
    int i, bp_index;
    unsigned long flags;

    if (!cfg || addr == 0) return -EINVAL;
    if (cfg->nr_reg_mods < 0 || cfg->nr_reg_mods > WXSHADOW_MAX_REG_MODS) return -EINVAL;

    printk(KERN_EMERG "WXSHADOW_SET_BP: pid=%d addr=0x%llx\n", pid, (unsigned long long)addr);

    mm = wx_get_task_mm(pid);
    if (!mm) {
        printk(KERN_EMERG "WXSHADOW_SET_BP: no mm for pid=%d\n", pid);
        return -ESRCH;
    }
    pr_err("WXSHADOW: got mm pid=%d addr=0x%llx\n", pid, (unsigned long long)addr);

    page_addr = addr & PAGE_MASK;

    /* Restore: full set_bp implementation */
    spin_lock_irqsave(&g_wx_global_lock, flags);

    pg = wx_find_page(mm, page_addr);
    if (!pg) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        pg = wx_create_page(mm, page_addr);
        if (!pg) {
            pr_err("WXSHADOW: wx_create_page failed\n");
            mmput(mm);
            return -ENOMEM;
        }
        spin_lock_irqsave(&g_wx_global_lock, flags);
    }

    /* 检查是否超过限制 */
    if (pg->nr_bps >= WXSHADOW_MAX_BPS_PER_PAGE) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        wx_page_put(pg);
        mmput(mm);
        return -ENOSPC;
    }

    /* 查找是否已存在相同地址的断点, 有则覆盖 */
    bp_index = -1;
    for (i = 0; i < pg->nr_bps; i++) {
        if (pg->bps[i].active && pg->bps[i].addr == addr) {
            bp_index = i;
            break;
        }
    }

    if (bp_index < 0) {
        bp_index = pg->nr_bps++;
    }

    /* 设置断点 */
    pg->bps[bp_index].addr = addr;
    pg->bps[bp_index].active = true;
    pg->bps[bp_index].nr_reg_mods = cfg->nr_reg_mods;
    for (i = 0; i < cfg->nr_reg_mods; i++) {
        pg->bps[bp_index].reg_mods[i].reg_idx = cfg->reg_mods[i].reg_idx;
        pg->bps[bp_index].reg_mods[i].enabled = cfg->reg_mods[i].enabled;
        pg->bps[bp_index].reg_mods[i].value = cfg->reg_mods[i].value;
    }

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    /* 重建 shadow 页并写入 BRK */
    {
        int ret = wx_rebuild_shadow_page(pg);
        if (ret != 0) {
            /* 回滚 */
            spin_lock_irqsave(&g_wx_global_lock, flags);
            pg->bps[bp_index].active = false;
            spin_unlock_irqrestore(&g_wx_global_lock, flags);
            wx_page_put(pg);
            mmput(mm);
            return ret;
        }
    }

    /* 激活 shadow 映射 */
    {
        int switch_ret = wx_pte_switch_to_shadow(pg);
        if (switch_ret != 0) {
            ls_log_tag("wxshadow", "SET_BP pte_switch_to_shadow failed: ret=%d addr=0x%llx\n",
                       switch_ret, (unsigned long long)addr);
            wx_page_put(pg);
            mmput(mm);
            return switch_ret;
        }
    }

    ls_log_tag("wxshadow", "SET_BP pid=%d addr=0x%llx bp_index=%d reg_mods=%d\n",
               pid, (unsigned long long)addr, bp_index, cfg->nr_reg_mods);

    wx_page_put(pg);
    mmput(mm);
    return 0;
}

/* ---- 删除断点 ---- */
int wxshadow_handle_del_bp(int pid, uint64_t addr)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long flags;
    int i, removed = 0;

    mm = wx_get_task_mm(pid);
    if (!mm) return -ESRCH;

    spin_lock_irqsave(&g_wx_global_lock, flags);

    if (addr == 0) {
        /* 删除该进程的所有断点 */
        list_for_each_entry(pg, &g_wx_page_list, list) {
            if (pg->mm == mm) {
                for (i = 0; i < pg->nr_bps; i++) {
                    if (pg->bps[i].active) {
                        pg->bps[i].active = false;
                        removed++;
                    }
                }
                wx_rebuild_shadow_page(pg);
            }
        }
    } else {
        /* 删除指定地址的断点 */
        pg = wx_find_by_addr(mm, addr);
        if (pg) {
            for (i = 0; i < pg->nr_bps; i++) {
                if (pg->bps[i].active && pg->bps[i].addr == addr) {
                    pg->bps[i].active = false;
                    removed++;
                    break;
                }
            }
            wx_rebuild_shadow_page(pg);
        }
    }

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    if (pg) wx_page_put(pg);
    mmput(mm);

    ls_log_tag("wxshadow", "DEL_BP pid=%d addr=0x%llx removed=%d\n",
               pid, (unsigned long long)addr, removed);

    return removed > 0 ? 0 : -ENOENT;
}

/* ---- 设置 Patch ---- */
int wxshadow_handle_patch(int pid, uint64_t page_addr, const struct wxshadow_patch_cfg *cfg)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    int i, patch_index;
    unsigned long flags;

    if (!cfg || cfg->len == 0 || cfg->len > WXSHADOW_MAX_PATCH_BYTES)
        return -EINVAL;
    if (cfg->offset + cfg->len > PAGE_SIZE)
        return -EINVAL;

    mm = wx_get_task_mm(pid);
    if (!mm) return -ESRCH;

    

    spin_lock_irqsave(&g_wx_global_lock, flags);

    pg = wx_find_page(mm, page_addr);
    if (!pg) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        pg = wx_create_page(mm, page_addr);
        if (!pg) {
            mmput(mm);
            return -ENOMEM;
        }
        spin_lock_irqsave(&g_wx_global_lock, flags);
    }

    if (pg->nr_patches >= WXSHADOW_MAX_PATCHES_PER_PAGE) {
        spin_unlock_irqrestore(&g_wx_global_lock, flags);
        wx_page_put(pg);
        mmput(mm);
        return -ENOSPC;
    }

    /* 查找是否已存在重叠的 patch */
    patch_index = -1;
    for (i = 0; i < pg->nr_patches; i++) {
        if (pg->patches[i].active && pg->patches[i].offset == cfg->offset) {
            patch_index = i;
            break;
        }
    }
    if (patch_index < 0) {
        patch_index = pg->nr_patches++;
    }

    pg->patches[patch_index].offset = cfg->offset;
    pg->patches[patch_index].len = cfg->len;
    pg->patches[patch_index].active = true;
    memcpy(pg->patches[patch_index].data, cfg->data, cfg->len);

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    /* 重建 shadow 页并写入 Patch */
    {
        int ret = wx_rebuild_shadow_page(pg);
        if (ret != 0) {
            spin_lock_irqsave(&g_wx_global_lock, flags);
            pg->patches[patch_index].active = false;
            spin_unlock_irqrestore(&g_wx_global_lock, flags);
            wx_page_put(pg);
            mmput(mm);
            return ret;
        }
    }

    /* 激活 shadow */
    {
        int switch_ret = wx_pte_switch_to_shadow(pg);
        if (switch_ret != 0) {
            ls_log_tag("wxshadow", "PATCH pte_switch_to_shadow failed: ret=%d page=0x%llx\n",
                       switch_ret, (unsigned long long)page_addr);
            wx_page_put(pg);
            mmput(mm);
            return switch_ret;
        }
    }

    ls_log_tag("wxshadow", "PATCH pid=%d page=0x%llx offset=%u len=%u\n",
               pid, (unsigned long long)page_addr, cfg->offset, cfg->len);

    wx_page_put(pg);
    mmput(mm);
    return 0;
}

/* ---- 释放 shadow 页 ---- */
int wxshadow_handle_release(int pid, uint64_t addr)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg, *tmp;
    unsigned long flags;
    int released = 0;

    mm = wx_get_task_mm(pid);
    if (!mm) return -ESRCH;

    spin_lock_irqsave(&g_wx_global_lock, flags);

    if (addr == 0) {
        /* 释放该进程的所有 shadow 页 */
        LIST_HEAD(dead_list);

        list_for_each_entry_safe(pg, tmp, &g_wx_page_list, list) {
            if (pg->mm == mm) {
                if (pg->state == WX_STATE_STEPPING && pg->stepping_task) {
                    /* 有线程正在单步, 标记延迟释放 */
                    pg->release_pending = true;
                    pg->logical_release_pending = true;
                } else {
                    wx_destroy_page_locked(pg);
                    list_move(&pg->list, &dead_list);
                }
                released++;
            }
        }
        spin_unlock_irqrestore(&g_wx_global_lock, flags);

        list_for_each_entry_safe(pg, tmp, &dead_list, list) {
            list_del(&pg->list);
            wx_page_put(pg);
        }
    } else {
        /* 释放指定地址的 shadow 页 */
        pg = wx_find_by_addr(mm, addr);
        spin_unlock_irqrestore(&g_wx_global_lock, flags);

        if (pg) {
            if (pg->state == WX_STATE_STEPPING && pg->stepping_task) {
                spin_lock_irqsave(&g_wx_global_lock, flags);
                pg->release_pending = true;
                pg->logical_release_pending = true;
                spin_unlock_irqrestore(&g_wx_global_lock, flags);
            } else {
                spin_lock_irqsave(&g_wx_global_lock, flags);
                wx_destroy_page_locked(pg);
                spin_unlock_irqrestore(&g_wx_global_lock, flags);
            }
            wx_page_put(pg);
            released = 1;
        }
    }

    mmput(mm);

    ls_log_tag("wxshadow", "RELEASE pid=%d addr=0x%llx released=%d\n",
               pid, (unsigned long long)addr, released);

    return released > 0 ? 0 : -ENOENT;
}

/* ---- 查询状态 ---- */
int wxshadow_handle_get_state(int pid, struct wxshadow_state_info *info)
{
    struct mm_struct *mm;
    struct wxshadow_page *pg;
    unsigned long flags;
    int count = 0;

    if (!info) return -EINVAL;
    memset(info, 0, sizeof(*info));

    mm = wx_get_task_mm(pid);
    if (!mm) return -ESRCH;

    spin_lock_irqsave(&g_wx_global_lock, flags);

    list_for_each_entry(pg, &g_wx_page_list, list) {
        if (pg->mm != mm) continue;
        if (pg->dead) continue;

        info->total_pages++;
        info->total_bps += pg->nr_bps;
        info->total_patches += pg->nr_patches;

        if (count < WXSHADOW_MAX_BP_RESULTS) {
            info->entries[count].page_addr = pg->page_addr;
            info->entries[count].nr_bps = pg->nr_bps;
            info->entries[count].nr_patches = pg->nr_patches;
            info->entries[count].state = (int32_t)pg->state;
            info->entries[count].refcount = pg->refcount;
            count++;
        }
    }

    spin_unlock_irqrestore(&g_wx_global_lock, flags);

    info->entry_count = count;
    mmput(mm);

    return 0;
}
