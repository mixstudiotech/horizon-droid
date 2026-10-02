// SPDX-License-Identifier: GPL-2.0
/*
 * Write watching for services (HZN_SCTL_MEMWATCH_GET[_CLEAR]), e.g. so that
 * GPU emulation sees CPU writes to memory it mirrors.
 *
 * A page counts as written when its PTE is writable. Clearing write-protects
 * those PTEs again, so that the next write faults and makes the PTE
 * writable. This is the write-protect half of soft-dirty tracking without a
 * soft-dirty bit: the PTE dirty state, which reclaim and writeback rely on,
 * is never touched. Like userfaultfd write-protection without PTE markers,
 * a written page that is swapped out (or a shmem page that is reclaimed)
 * before the next query is not reported.
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/pagewalk.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/tlbflush.h>

#include "internal.h"

struct hzn_memwatch {
	unsigned long start;
	bool clear;
	loff_t *vec;
	size_t len, max;
};

/* Write-protecting a pinned COW page could make it diverge from the pin. */
static bool hzn_pte_is_pinned(struct vm_area_struct *vma, unsigned long addr,
			      pte_t pte)
{
	struct folio *folio;

	if (!is_cow_mapping(vma->vm_flags))
		return false;
	if (likely(!mm_flags_test(MMF_HAS_PINNED, vma->vm_mm)))
		return false;
	folio = vm_normal_folio(vma, addr, pte);
	return folio && folio_maybe_dma_pinned(folio);
}

static int hzn_memwatch_pmd(pmd_t *pmd, unsigned long addr, unsigned long end,
			    struct mm_walk *walk)
{
	struct hzn_memwatch *mw = walk->private;
	struct vm_area_struct *vma = walk->vma;
	pte_t *start_pte, *pte, ptent, old;
	bool flush = false;
	spinlock_t *ptl;
	unsigned long a;

	if (pmd_trans_huge(pmdp_get_lockless(pmd)))
		split_huge_pmd(vma, pmd, addr);

	start_pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	pte = start_pte;
	if (!pte) {
		walk->action = ACTION_AGAIN;
		return 0;
	}
	for (a = addr; a < end && mw->len < mw->max; a += PAGE_SIZE, pte++) {
		ptent = ptep_get(pte);
		if (!pte_present(ptent) || !pte_write(ptent))
			continue;
		mw->vec[mw->len++] = a - mw->start;
		if (!mw->clear || hzn_pte_is_pinned(vma, a, ptent))
			continue;
		old = ptep_modify_prot_start(vma, a, pte);
		ptep_modify_prot_commit(vma, a, pte, old, pte_wrprotect(old));
		flush = true;
	}
	pte_unmap_unlock(start_pte, ptl);
	if (flush)
		flush_tlb_range(vma, addr, end);
	cond_resched();
	/* Stop once the vector is full; the rest is reported next time. */
	return mw->len < mw->max ? 0 : 1;
}

static int hzn_memwatch_test(unsigned long start, unsigned long end,
			     struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;

	if ((vma->vm_flags & (VM_PFNMAP | VM_IO)) || is_vm_hugetlb_page(vma))
		return 1;
	return 0;
}

static const struct mm_walk_ops hzn_memwatch_ops = {
	.test_walk	= hzn_memwatch_test,
	.pmd_entry	= hzn_memwatch_pmd,
	.walk_lock	= PGWALK_RDLOCK,
};

/*
 * Stores the offsets (from @start) of the written pages of [start, start +
 * len) of @mm in @uvec, at most @vec_len of them, and returns their number.
 */
long hzn_memwatch(struct mm_struct *mm, unsigned long start, size_t len,
		  bool clear, loff_t __user *uvec, size_t vec_len)
{
	struct hzn_memwatch mw = { .start = start, .clear = clear };
	struct mmu_notifier_range range;
	unsigned long end;
	long ret;

	if (!PAGE_ALIGNED(start) || !len || !uvec || !vec_len ||
	    check_add_overflow(start, (unsigned long)len, &end) ||
	    end > TASK_SIZE_MAX)
		return -EINVAL;
	end = PAGE_ALIGN(end);

	mw.max = min_t(size_t, vec_len, (end - start) >> PAGE_SHIFT);
	mw.vec = kvmalloc_array(mw.max, sizeof(*mw.vec), GFP_KERNEL);
	if (!mw.vec)
		return -ENOMEM;

	mmap_read_lock(mm);
	if (clear) {
		inc_tlb_flush_pending(mm);
		mmu_notifier_range_init(&range, MMU_NOTIFY_SOFT_DIRTY, 0, mm,
					start, end);
		mmu_notifier_invalidate_range_start(&range);
	}
	ret = walk_page_range(mm, start, end, &hzn_memwatch_ops, &mw);
	if (clear) {
		mmu_notifier_invalidate_range_end(&range);
		dec_tlb_flush_pending(mm);
	}
	mmap_read_unlock(mm);

	if (ret >= 0) {
		ret = mw.len;
		if (mw.len && copy_to_user(uvec, mw.vec, mw.len * sizeof(*mw.vec)))
			ret = -EFAULT;
	}
	kvfree(mw.vec);
	return ret;
}
