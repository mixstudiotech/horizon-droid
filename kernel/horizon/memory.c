// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon memory SVCs.
 *
 * All writable memory a Horizon process gets from the kernel (heap, data
 * segments, physical and transfer memory) is backed by shmem and mapped
 * MAP_SHARED, so that a service can map any of it without copying (see
 * HZN_SCTL_MAP_MEMORY), svcMapMemory can alias it with plain mmap() and
 * svcSetMemoryPermission can map it again with another protection.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "internal.h"

#define HZN_PERM_MASK	(HZN_MEMORY_PERMISSION_READ | \
			 HZN_MEMORY_PERMISSION_WRITE | \
			 HZN_MEMORY_PERMISSION_EXECUTE)

/* Horizon memory permissions have the PROT_* (and VM_*) values. */
static_assert(HZN_MEMORY_PERMISSION_READ == PROT_READ);
static_assert(HZN_MEMORY_PERMISSION_WRITE == PROT_WRITE);
static_assert(HZN_MEMORY_PERMISSION_EXECUTE == PROT_EXEC);
static_assert(VM_READ == PROT_READ && VM_WRITE == PROT_WRITE &&
	      VM_EXEC == PROT_EXEC);

static bool hzn_range_ok(unsigned long addr, u64 size)
{
	return size && PAGE_ALIGNED(addr) && PAGE_ALIGNED(size) &&
	       addr + size > addr;
}

static bool hzn_in(unsigned long addr, u64 size, unsigned long start,
		   unsigned long len)
{
	return len && addr >= start && addr + size <= start + len;
}

/*
 * Ranges with MemoryAttribute_PermissionLocked: their permission can no
 * longer change. Sorted, disjoint and not adjacent; under mem_lock.
 */
struct hzn_locked {
	struct list_head node;
	unsigned long start, end;
};

static bool hzn_locked_overlaps(struct hzn_process *proc, unsigned long addr,
				u64 size)
{
	struct hzn_locked *l;

	list_for_each_entry(l, &proc->locked, node)
		if (l->start < addr + size && l->end > addr)
			return true;
	return false;
}

static int hzn_lock_range(struct hzn_process *proc, unsigned long start,
			  unsigned long end)
{
	struct hzn_locked *l, *tmp, *n;
	struct list_head *pos = &proc->locked;

	list_for_each_entry_safe(l, tmp, &proc->locked, node) {
		if (l->end < start) {
			pos = &l->node;
			continue;
		}
		if (l->start > end)
			break;
		/* Overlapping or adjacent: absorb it. */
		start = min(start, l->start);
		end = max(end, l->end);
		list_del(&l->node);
		kfree(l);
	}
	n = kmalloc_obj(*n);
	if (!n)
		return -ENOMEM;
	n->start = start;
	n->end = end;
	list_add(&n->node, pos);
	return 0;
}

void hzn_process_free_locked(struct hzn_process *proc)
{
	struct hzn_locked *l, *tmp;

	list_for_each_entry_safe(l, tmp, &proc->locked, node) {
		list_del(&l->node);
		kfree(l);
	}
}

/*
 * The memory state of the mapping @vma at @addr. Data segments of the image
 * are shared shmem, its code and read-only data private file mappings (of a
 * file that may be on tmpfs), so the state of module data stays CodeData
 * when its permission changes (RELRO).
 */
static u32 hzn_vma_state(struct hzn_process *proc, struct vm_area_struct *vma,
			 unsigned long addr, bool alias_dst)
{
	if (alias_dst)
		return HZN_MEMORY_STATE_STACK;
	if (addr >= HZN_IMAGE_BASE && addr < proc->image_end)
		return (vma->vm_flags & VM_SHARED) && vma_is_shmem(vma) ?
		       HZN_MEMORY_STATE_CODE_DATA : HZN_MEMORY_STATE_CODE;
	if (addr >= proc->heap_start && addr < proc->heap_start + proc->heap_size)
		return HZN_MEMORY_STATE_NORMAL;
	if (vma_is_anonymous(vma) && addr >= proc->tls_base &&
	    addr < proc->tls_base + HZN_TLS_AREA_SIZE)
		return HZN_MEMORY_STATE_THREAD_LOCAL;
	if (addr >= proc->alias_start && addr < proc->alias_start + proc->alias_size)
		return HZN_MEMORY_STATE_ALIAS;
	if (addr >= proc->alias_code_start &&
	    addr < proc->alias_code_start + proc->alias_code_size)
		/*
		 * Shared memory a service hands out as a plain file (svcMapSharedMemory
		 * maps it without a memory object) is Shared, as on Horizon. nn::diag
		 * takes AliasCode for code that nn::ro loaded and fails to find it.
		 */
		return vma->vm_flags & VM_SHARED ? HZN_MEMORY_STATE_SHARED :
		       HZN_MEMORY_STATE_ALIAS_CODE;
	return HZN_MEMORY_STATE_STACK;
}

static bool hzn_is_alias_dst(struct hzn_process *proc, unsigned long addr)
{
	struct hzn_alias *a;

	list_for_each_entry(a, &proc->aliases, node)
		if (addr >= a->dst && addr < a->dst + a->size)
			return true;
	return false;
}

/*
 * Whether every page of [addr, addr + size) is mapped with a state in
 * @states (a mask of 1 << state). Called with mem_lock held.
 */
static bool hzn_range_state(struct hzn_process *proc, unsigned long addr,
			    u64 size, u32 states)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long a = addr;
	u32 state;

	mmap_read_lock(mm);
	while (a < addr + size) {
		vma = vma_lookup(mm, a);
		if (!vma)
			break;
		state = hzn_vma_state(proc, vma, a, hzn_is_alias_dst(proc, a));
		if (!(states & BIT(state)))
			break;
		a = vma->vm_end;
	}
	mmap_read_unlock(mm);
	return a >= addr + size;
}

/*
 * The states transfer and code memory may be made of: the heap and the data
 * of modules (Horizon's Normal and CodeData, which can transfer).
 */
#define HZN_TRANSFER_STATES	(BIT(HZN_MEMORY_STATE_NORMAL) | \
				 BIT(HZN_MEMORY_STATE_CODE_DATA) | \
				 BIT(HZN_MEMORY_STATE_ALIAS_CODE_DATA))

/*
 * Whether [addr, addr + size) is all memory that may be loaned: in one of
 * those states, and not permission-locked (Horizon loans memory without
 * attributes only). Called with mem_lock held.
 */
bool hzn_transferable(struct hzn_process *proc, unsigned long addr, u64 size)
{
	return !hzn_locked_overlaps(proc, addr, size) &&
	       hzn_range_state(proc, addr, size, HZN_TRANSFER_STATES);
}

/* The states whose permission svcSetMemoryPermission may change. */
#define HZN_REPROTECT_STATES	(BIT(HZN_MEMORY_STATE_NORMAL) | \
				 BIT(HZN_MEMORY_STATE_CODE_DATA) | \
				 BIT(HZN_MEMORY_STATE_ALIAS_CODE_DATA))
/* And those svcSetMemoryAttribute may lock the permission of. */
#define HZN_PERMISSION_LOCK_STATES	(BIT(HZN_MEMORY_STATE_CODE_DATA) | \
					 BIT(HZN_MEMORY_STATE_ALIAS_CODE_DATA))

/*
 * svcSetMemoryPermission: the program's heap and the data of its modules,
 * for nn::os::SetMemoryPermission and for the RELRO protection of rtld and
 * nn::ro. Both are shmem, so mapping the same pages again with the new
 * protection changes it without touching the contents.
 */
long hzn_set_memory_permission(unsigned long addr, u64 size, u32 perm)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct file *file;
	unsigned long m;
	pgoff_t pgoff;
	long ret = HZN_RESULT_INVALID_CURRENT_MEMORY;

	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	switch (perm) {
	case HZN_MEMORY_PERMISSION_NONE:
	case HZN_MEMORY_PERMISSION_READ:
	case HZN_MEMORY_PERMISSION_READ_WRITE:
		break;
	default:
		return HZN_RESULT_INVALID_NEW_MEMORY_PERMISSION;
	}
	if (addr + size > proc->space_end)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;

	mutex_lock(&proc->mem_lock);
	if (hzn_memory_source_overlaps(current->mm, addr, size) ||
	    hzn_locked_overlaps(proc, addr, size) ||
	    !hzn_range_state(proc, addr, size, HZN_REPROTECT_STATES))
		goto out;
	file = hzn_find_shared(current->mm, addr, size, &pgoff);
	if (!file)
		goto out;
	m = vm_mmap(file, addr, size, perm, MAP_SHARED | MAP_FIXED,
		    (unsigned long)pgoff << PAGE_SHIFT);
	fput(file);
	ret = m == addr ? HZN_RESULT_SUCCESS : HZN_RESULT_OUT_OF_MEMORY;
out:
	mutex_unlock(&proc->mem_lock);
	return ret;
}

/*
 * svcSetMemoryAttribute. Uncached CPU mappings make no difference to
 * anything here; PermissionLocked can only be set, on module data that is
 * not loaned (Horizon refuses Locked memory).
 */
long hzn_set_memory_attribute(unsigned long addr, u64 size, u32 mask, u32 value)
{
	const u32 supported = HZN_MEMORY_ATTRIBUTE_UNCACHED |
			      HZN_MEMORY_ATTRIBUTE_PERMISSION_LOCKED;
	struct hzn_process *proc = hzn_current()->proc;
	long ret = HZN_RESULT_INVALID_CURRENT_MEMORY;

	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	if ((mask | value) != mask || (mask | supported) != supported ||
	    (mask & HZN_MEMORY_ATTRIBUTE_PERMISSION_LOCKED) !=
	    (value & HZN_MEMORY_ATTRIBUTE_PERMISSION_LOCKED))
		return HZN_RESULT_INVALID_COMBINATION;
	if (addr + size > proc->space_end)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	if (!(mask & HZN_MEMORY_ATTRIBUTE_PERMISSION_LOCKED))
		return HZN_RESULT_SUCCESS;

	mutex_lock(&proc->mem_lock);
	if (!hzn_memory_source_overlaps(current->mm, addr, size) &&
	    hzn_range_state(proc, addr, size, HZN_PERMISSION_LOCK_STATES))
		ret = hzn_lock_range(proc, addr, addr + size) ?
		      HZN_RESULT_OUT_OF_MEMORY : HZN_RESULT_SUCCESS;
	mutex_unlock(&proc->mem_lock);
	return ret;
}

/*
 * If [start, start + size) of @mm is one contiguous MAP_SHARED mapping of a
 * single shmem object, returns a reference to that object and the page
 * offset of @start in it.
 */
struct file *hzn_find_shared(struct mm_struct *mm, unsigned long start,
			     unsigned long size, pgoff_t *pgoff)
{
	unsigned long addr = start, end = start + size;
	struct vm_area_struct *vma;
	struct file *file = NULL, *f = NULL;
	pgoff_t first = 0, off;

	if (!size || end <= start || end > TASK_SIZE_MAX)
		return NULL;

	mmap_read_lock(mm);
	while (addr < end) {
		vma = vma_lookup(mm, addr);
		if (!vma || !(vma->vm_flags & VM_SHARED) || !vma_is_shmem(vma))
			goto out;
		off = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);
		if (addr == start) {
			f = vma->vm_file;
			first = off;
		} else if (vma->vm_file != f ||
			   off != first + ((addr - start) >> PAGE_SHIFT)) {
			goto out;
		}
		addr = vma->vm_end;
	}
	file = get_file(f);
	*pgoff = first;
out:
	mmap_read_unlock(mm);
	return file;
}

/* Copies [addr, addr + size) of the current process into @file. */
static int hzn_copy_to_file(struct file *file, unsigned long addr,
			    unsigned long size)
{
	void *buf = (void *)__get_free_page(GFP_KERNEL);
	unsigned long off;
	loff_t pos = 0;
	int ret = 0;

	if (!buf)
		return -ENOMEM;
	for (off = 0; off < size; off += PAGE_SIZE) {
		if (copy_from_user(buf, (const void __user *)(addr + off),
				   PAGE_SIZE)) {
			ret = -EFAULT;
			break;
		}
		if (!memchr_inv(buf, 0, PAGE_SIZE)) {	/* keep it sparse */
			pos += PAGE_SIZE;
			continue;
		}
		if (kernel_write(file, buf, PAGE_SIZE, &pos) != PAGE_SIZE) {
			ret = -EIO;
			break;
		}
	}
	free_page((unsigned long)buf);
	return ret;
}

/*
 * Replaces [start, start + size) of the current process, keeping its
 * contents and protection, with a new shmem object. Returns a reference to
 * the object in *filep.
 */
long hzn_make_shared(unsigned long start, unsigned long size,
		     struct file **filep)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct vm_area_struct *vma;
	unsigned long prot, addr;
	struct file *file;
	int ret;

	mutex_lock(&proc->mem_lock);
	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, start);
	prot = vma ? vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC) : 0;
	mmap_read_unlock(current->mm);
	ret = -EFAULT;
	if (!vma)
		goto out;

	file = shmem_file_setup("horizon_shared", size, EMPTY_VMA_FLAGS);
	ret = PTR_ERR(file);
	if (IS_ERR(file))
		goto out;
	ret = hzn_copy_to_file(file, start, size);
	if (!ret) {
		addr = vm_mmap(file, start, size, prot, MAP_SHARED | MAP_FIXED, 0);
		if (addr != start)
			ret = IS_ERR_VALUE(addr) ? (int)addr : -EINVAL;
	}
	if (ret)
		fput(file);
	else
		*filep = file;
out:
	mutex_unlock(&proc->mem_lock);
	return ret;
}

long hzn_set_heap_size(u64 size, unsigned long *addr)
{
	struct hzn_process *proc = hzn_current()->proc;
	unsigned long base = proc->heap_start, cur, m;
	struct hzn_alias *a;
	long ret = HZN_RESULT_SUCCESS;

	if (size % SZ_2M || size > proc->heap_region_size)
		return HZN_RESULT_INVALID_SIZE;

	mutex_lock(&proc->mem_lock);
	cur = proc->heap_size;
	if (size > cur) {
		m = vm_mmap(proc->heap_file, base + cur, size - cur,
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED_NOREPLACE, cur);
		if (m != base + cur) {
			ret = HZN_RESULT_OUT_OF_MEMORY;
			goto out;
		}
	} else if (size < cur) {
		if (hzn_memory_source_overlaps(current->mm, base + size, cur - size)) {
			ret = HZN_RESULT_INVALID_STATE;
			goto out;
		}
		/* Memory aliased by svcMapMemory has to be unmapped first. */
		list_for_each_entry(a, &proc->aliases, node) {
			if (a->src < base + cur && a->src + a->size > base + size) {
				ret = HZN_RESULT_INVALID_STATE;
				goto out;
			}
		}
		vm_munmap(base + size, cur - size);
		vfs_fallocate(proc->heap_file,
			      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			      size, cur - size);
	}
	proc->heap_size = size;
	*addr = base;
out:
	mutex_unlock(&proc->mem_lock);
	return ret;
}

/* svcMapMemory: dst aliases src, and src becomes inaccessible. */
long hzn_map_memory(unsigned long dst, unsigned long src, u64 size)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_alias *a;
	struct file *file;
	pgoff_t pgoff;
	unsigned long m;
	long ret;

	if (!hzn_range_ok(dst, size) || !hzn_range_ok(src, size))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!hzn_in(dst, size, proc->stack_region_start, proc->stack_region_size) &&
	    !hzn_in(dst, size, proc->alias_start, proc->alias_size))
		return HZN_RESULT_INVALID_MEMORY_REGION;

	a = kzalloc_obj(*a);
	if (!a)
		return HZN_RESULT_OUT_OF_MEMORY;

	mutex_lock(&proc->mem_lock);
	file = (hzn_memory_source_overlaps(current->mm, src, size) ||
		hzn_locked_overlaps(proc, src, size)) ? NULL :
	       hzn_find_shared(current->mm, src, size, &pgoff);
	if (!file) {
		ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
		goto err;
	}
	m = vm_mmap(file, dst, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED_NOREPLACE, (unsigned long)pgoff << PAGE_SHIFT);
	if (m != dst) {
		fput(file);
		ret = HZN_RESULT_INVALID_MEMORY_REGION;
		goto err;
	}
	vm_munmap(src, size);

	a->dst = dst;
	a->src = src;
	a->size = size;
	a->file = file;
	a->pgoff = pgoff;
	list_add(&a->node, &proc->aliases);
	mutex_unlock(&proc->mem_lock);
	return HZN_RESULT_SUCCESS;

err:
	mutex_unlock(&proc->mem_lock);
	kfree(a);
	return ret;
}

long hzn_unmap_memory(unsigned long dst, unsigned long src, u64 size)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_alias *a;
	unsigned long m;

	mutex_lock(&proc->mem_lock);
	list_for_each_entry(a, &proc->aliases, node)
		if (a->dst == dst && a->src == src && a->size == size)
			goto found;
	mutex_unlock(&proc->mem_lock);
	return HZN_RESULT_INVALID_MEMORY_REGION;

found:
	vm_munmap(dst, size);
	m = vm_mmap(a->file, src, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, (unsigned long)a->pgoff << PAGE_SHIFT);
	list_del(&a->node);
	fput(a->file);
	kfree(a);
	mutex_unlock(&proc->mem_lock);
	return m == src ? HZN_RESULT_SUCCESS : HZN_RESULT_OUT_OF_MEMORY;
}

void hzn_process_free_aliases(struct hzn_process *proc)
{
	struct hzn_alias *a, *tmp;

	list_for_each_entry_safe(a, tmp, &proc->aliases, node) {
		list_del(&a->node);
		fput(a->file);
		kfree(a);
	}
}

/*
 * Narrows the block [mi->addr, mi->addr + mi->size) around @addr to where
 * PermissionLocked is the same as at @addr, and sets it if it is locked.
 */
static void hzn_clip_locked(struct hzn_process *proc, struct memory_info *mi,
			    unsigned long addr)
{
	unsigned long start = mi->addr, end = mi->addr + mi->size;
	struct hzn_locked *l;

	list_for_each_entry(l, &proc->locked, node) {
		if (l->end <= start || l->start >= end)
			continue;
		if (addr >= l->start && addr < l->end) {
			start = max(start, l->start);
			end = min(end, l->end);
			mi->attr |= HZN_MEMORY_ATTRIBUTE_PERMISSION_LOCKED;
		} else if (l->end <= addr) {
			start = max(start, l->end);
		} else {
			end = min(end, l->start);
		}
	}
	mi->addr = start;
	mi->size = end - start;
}

long hzn_query_memory(struct memory_info *mi, unsigned long addr)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	struct hzn_alias *a;

	memset(mi, 0, sizeof(*mi));
	if (addr >= proc->space_end) {
		mi->addr = proc->space_end;
		mi->size = 0 - proc->space_end;
		mi->state = HZN_MEMORY_STATE_INACCESSIBLE;
		return HZN_RESULT_SUCCESS;
	}

	mutex_lock(&proc->mem_lock);
	list_for_each_entry(a, &proc->aliases, node) {
		if (addr >= a->src && addr < a->src + a->size) {
			/*
			 * The source of an svcMapMemory alias is locked and keeps its
			 * state: module data (a static thread stack) or the heap.
			 */
			mi->addr = a->src;
			mi->size = a->size;
			mi->state = a->src >= HZN_IMAGE_BASE && a->src < proc->image_end ?
				    HZN_MEMORY_STATE_CODE_DATA : HZN_MEMORY_STATE_NORMAL;
			mi->attr = HZN_MEMORY_ATTRIBUTE_LOCKED;
			goto out_unlock;
		}
	}

	mmap_read_lock(mm);
	vma = find_vma_prev(mm, addr, &prev);
	if (!vma || vma->vm_start > addr) {
		mi->addr = prev ? prev->vm_end : 0;
		mi->size = (vma ? min(vma->vm_start, proc->space_end) :
			    proc->space_end) - mi->addr;
		mi->state = HZN_MEMORY_STATE_FREE;
		goto out;
	}

	mi->addr = vma->vm_start;
	mi->size = vma->vm_end - vma->vm_start;
	mi->perm = vma->vm_flags & HZN_PERM_MASK;
	mi->state = hzn_vma_state(proc, vma, addr, hzn_is_alias_dst(proc, addr));
	hzn_clip_locked(proc, mi, addr);
	hzn_query_memory_object(proc, mi, addr);
out:
	mmap_read_unlock(mm);
out_unlock:
	mutex_unlock(&proc->mem_lock);
	return HZN_RESULT_SUCCESS;
}

long hzn_map_shared_memory(u32 handle, unsigned long addr, u64 size, u32 perm)
{
	struct file *file;
	unsigned long m;

	if (!hzn_range_ok(addr, size))
		return HZN_RESULT_INVALID_ADDRESS;
	file = hzn_handle_get(handle);
	if (!file)
		return HZN_RESULT_INVALID_HANDLE;
	if (hzn_is_memory_object(file)) {
		fput(file);
		return hzn_map_typed_shared_memory(handle, addr, size, perm);
	}
	m = vm_mmap(file, addr, size, perm & HZN_PERM_MASK,
		    MAP_SHARED | MAP_FIXED_NOREPLACE, 0);
	fput(file);
	if (m == (unsigned long)-EEXIST)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	return m == addr ? HZN_RESULT_SUCCESS : HZN_RESULT_INVALID_ADDRESS;
}

long hzn_unmap_shared_memory(u32 handle, unsigned long addr, u64 size)
{
	struct vm_area_struct *vma;
	struct file *file;
	long ret = HZN_RESULT_SUCCESS;

	if (!hzn_range_ok(addr, size))
		return HZN_RESULT_INVALID_ADDRESS;
	file = hzn_handle_get(handle);
	if (!file)
		return HZN_RESULT_INVALID_HANDLE;

	if (hzn_is_memory_object(file)) {
		fput(file);
		return hzn_unmap_typed_shared_memory(handle, addr, size);
	}
	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, addr);
	if (!vma || vma->vm_end < addr + size || vma->vm_file != file)
		ret = HZN_RESULT_INVALID_MEMORY_REGION;
	mmap_read_unlock(current->mm);
	fput(file);

	if (ret == HZN_RESULT_SUCCESS)
		vm_munmap(addr, size);
	return ret;
}

long hzn_map_physical_memory(unsigned long addr, u64 size)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct file *file;
	unsigned long m;

	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr ||
	    !hzn_in(addr, size, proc->alias_start, proc->alias_size))
		return HZN_RESULT_INVALID_MEMORY_REGION;
	if (!proc->system_resource_size)
		return HZN_RESULT_INVALID_STATE;

	file = shmem_file_setup("horizon_phys", size, EMPTY_VMA_FLAGS);
	if (IS_ERR(file))
		return HZN_RESULT_OUT_OF_MEMORY;
	m = vm_mmap(file, addr, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED_NOREPLACE, 0);
	fput(file);
	return m == addr ? HZN_RESULT_SUCCESS : HZN_RESULT_OUT_OF_MEMORY;
}

long hzn_unmap_physical_memory(unsigned long addr, u64 size)
{
	struct hzn_process *proc = hzn_current()->proc;

	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr ||
	    !hzn_in(addr, size, proc->alias_start, proc->alias_size))
		return HZN_RESULT_INVALID_MEMORY_REGION;
	if (!proc->system_resource_size)
		return HZN_RESULT_INVALID_STATE;

	vm_munmap(addr, size);
	return HZN_RESULT_SUCCESS;
}
