// SPDX-License-Identifier: GPL-2.0
/* Typed Horizon memory handles. The pages stay in shmem, including IPC mappings. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/pagemap.h>
#include <asm/cacheflush.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <asm/tlb.h>
#include "internal.h"

enum hzn_mem_kind { HZN_MEM_SHARED, HZN_MEM_TRANSFER, HZN_MEM_CODE };
struct hzn_mem_object {
	struct list_head source_node;
	struct mutex lock;
	struct file *backing;
	struct mm_struct *owner;
	unsigned long source, size;
	pgoff_t offset;
	u32 owner_perm, remote_perm;
	enum hzn_mem_kind kind;
	bool mapped, owner_mapped;
};
struct hzn_object_mapping {
	struct list_head node;
	struct file *object;
	unsigned long addr, size;
	u32 state;
	bool owner;
};
static DEFINE_MUTEX(hzn_sources_lock);
static LIST_HEAD(hzn_sources);

/* Caller holds mmap_lock. Check every VMA and every page offset. */
static bool hzn_backed_range(struct mm_struct *mm, unsigned long start,
			     unsigned long size, struct file *file, pgoff_t off)
{
	unsigned long a = start;
	struct vm_area_struct *vma;

	while (a < start + size) {
		vma = vma_lookup(mm, a);
		if (!vma || vma->vm_file != file || !(vma->vm_flags & VM_SHARED) ||
		    vma->vm_pgoff + ((a - vma->vm_start) >> PAGE_SHIFT) !=
		    off + ((a - start) >> PAGE_SHIFT))
			return false;
		a = min(vma->vm_end, start + size);
	}
	return true;
}

/* Works for a remote owner too; closing a transferred handle restores its access. */
static int hzn_mem_protect(struct hzn_mem_object *obj, u32 perm)
{
	struct mm_struct *mm = obj->owner;
	struct vm_area_struct *vma, *prev = NULL;
	struct mmu_gather tlb;
	unsigned long a = obj->source, end = a + obj->size, next;
	VMA_ITERATOR(vmi, mm, a);
	int ret = -EFAULT;

	if (!mmget_not_zero(mm))
		return -ESRCH;
	mmap_write_lock(mm);
	if (!hzn_backed_range(mm, a, obj->size, obj->backing, obj->offset))
		goto unlock;
	tlb_gather_mmu(&tlb, mm);
	while (a < end) {
		vma_iter_set(&vmi, a);
		vma = vma_find(&vmi, end);
		prev = vma_prev(&vmi);
		vma = vma_find(&vmi, end);
		if (a > vma->vm_start)
			prev = vma;
		next = min(vma->vm_end, end);
		ret = mprotect_fixup(&vmi, &tlb, vma, &prev, a, next,
			(vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) | perm);
		if (ret)
			break;
		a = next;
	}
	tlb_finish_mmu(&tlb);
unlock:
	mmap_write_unlock(mm);
	mmput(mm);
	return ret;
}

static int hzn_mem_release(struct inode *inode, struct file *file)
{
	struct hzn_mem_object *obj = file->private_data;

	if (obj->kind != HZN_MEM_SHARED) {
		mutex_lock(&hzn_sources_lock);
		list_del(&obj->source_node);
		mutex_unlock(&hzn_sources_lock);
		/* A Linux munmap/exec may already have removed the original mapping. */
		hzn_mem_protect(obj, PROT_READ | PROT_WRITE);
	}
	mmdrop(obj->owner);
	fput(obj->backing);
	kfree(obj);
	return 0;
}

static int hzn_mem_mmap(struct vm_area_desc *desc)
{
	struct hzn_mem_object *obj = desc->file->private_data;
	struct vm_area_desc inner = *desc;
	u32 perm = vma_flags_to_legacy(desc->vma_flags) & (VM_READ | VM_WRITE | VM_EXEC);
	u32 allowed = desc->mm == obj->owner ? obj->owner_perm : obj->remote_perm;
	int ret;

	if (desc->pgoff || desc->end - desc->start != obj->size ||
	    !(vma_flags_to_legacy(desc->vma_flags) & VM_SHARED))
		return -EINVAL;
	/* Transfer mappings grant the receiver RW; owner_perm describes the source. */
	if (obj->kind == HZN_MEM_TRANSFER)
		allowed = PROT_READ | PROT_WRITE;
	if (obj->kind == HZN_MEM_CODE ||
	    (allowed != HZN_MEMORY_PERMISSION_DONT_CARE && (perm & ~allowed)))
		return -EACCES;
	if (allowed != HZN_MEMORY_PERMISSION_DONT_CARE) {
		if (!(allowed & PROT_WRITE))
			vma_flags_clear(&desc->vma_flags, VMA_MAYWRITE_BIT);
		if (!(allowed & PROT_EXEC))
			vma_flags_clear(&desc->vma_flags, VMA_MAYEXEC_BIT);
	}
	inner.vma_flags = desc->vma_flags;
	inner.file = obj->backing;
	inner.vm_file = obj->backing;
	inner.pgoff = obj->offset;
	ret = vfs_mmap_prepare(obj->backing, &inner);
	if (ret)
		return ret;
	*desc = inner;
	desc->vm_file = get_file(obj->backing);
	return 0;
}

static const struct file_operations hzn_mem_fops = {
	.release = hzn_mem_release,
	.mmap_prepare = hzn_mem_mmap,
};

static long hzn_mem_range(unsigned long addr, u64 size)
{
	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr || addr + size > hzn_current()->proc->space_end)
		return HZN_RESULT_INVALID_MEMORY_REGION;
	return HZN_RESULT_SUCCESS;
}

bool hzn_memory_source_overlaps(struct mm_struct *mm, unsigned long addr, u64 size)
{
	struct hzn_mem_object *obj;
	bool found = false;

	mutex_lock(&hzn_sources_lock);
	list_for_each_entry(obj, &hzn_sources, source_node)
		if (obj->owner == mm && obj->source < addr + size &&
		    obj->source + obj->size > addr) {
			found = true;
			break;
		}
	mutex_unlock(&hzn_sources_lock);
	return found;
}

void hzn_query_memory_object(struct hzn_process *proc, struct memory_info *mi,
			     unsigned long addr)
{
	struct hzn_object_mapping *map;
	struct hzn_mem_object *obj;
	unsigned long start = mi->addr, end = start + mi->size;

	list_for_each_entry(map, &proc->object_maps, node) {
		if (addr >= map->addr && addr < map->addr + map->size) {
			mi->state = map->state;
			start = max(start, map->addr);
			end = min(end, map->addr + map->size);
		} else if (map->addr + map->size <= addr) {
			start = max(start, map->addr + map->size);
		} else if (map->addr > addr) {
			end = min(end, map->addr);
		}
	}
	mutex_lock(&hzn_sources_lock);
	list_for_each_entry(obj, &hzn_sources, source_node) {
		if (obj->owner != current->mm)
			continue;
		if (addr >= obj->source && addr < obj->source + obj->size) {
			mi->attr |= HZN_MEMORY_ATTRIBUTE_LOCKED;
			start = max(start, obj->source);
			end = min(end, obj->source + obj->size);
		} else if (obj->source + obj->size <= addr) {
			start = max(start, obj->source + obj->size);
		} else if (obj->source > addr) {
			end = min(end, obj->source);
		}
	}
	mutex_unlock(&hzn_sources_lock);
	mi->addr = start;
	mi->size = end - start;
}

static long hzn_new_memory(enum hzn_mem_kind kind, unsigned long addr, u64 size,
			   u32 owner_perm, u32 remote_perm, u32 *handle)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_mem_object *obj;
	struct file *file, *backing;
	pgoff_t offset = 0;
	long ret = HZN_RESULT_OUT_OF_MEMORY;
	u32 h;

	obj = kzalloc_obj(*obj);
	if (!obj)
		return ret;
	mutex_init(&obj->lock);
	obj->kind = kind;
	obj->source = addr;
	obj->size = size;
	obj->owner_perm = owner_perm;
	obj->remote_perm = remote_perm;
	obj->owner = current->mm;
	mmgrab(obj->owner);
	mutex_lock(&proc->mem_lock);
	if (kind == HZN_MEM_SHARED) {
		backing = shmem_file_setup("horizon_shared", size, EMPTY_VMA_FLAGS);
		if (IS_ERR(backing))
			goto fail;
	} else {
		struct vm_area_struct *vma;
		unsigned long a = addr;

		ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
		if (hzn_memory_source_overlaps(current->mm, addr, size))
			goto fail;
		/* Only an ordinary, writable heap range may be loaned. */
		if (addr < proc->heap_start || addr + size > proc->heap_start + proc->heap_size)
			goto fail;
		mmap_read_lock(current->mm);
		while (a < addr + size) {
			vma = vma_lookup(current->mm, a);
			if (!vma || (vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) != 3)
				break;
			a = min(vma->vm_end, addr + size);
		}
		mmap_read_unlock(current->mm);
		if (a != addr + size)
			goto fail;
		backing = hzn_find_shared(current->mm, addr, size, &offset);
		if (!backing)
			goto fail;
	}
	obj->backing = backing;
	obj->offset = offset;
	file = anon_inode_getfile("horizon_memory", &hzn_mem_fops, obj, O_RDWR);
	if (IS_ERR(file)) {
		fput(backing);
		ret = HZN_RESULT_OUT_OF_MEMORY;
		goto fail;
	}
	if (kind != HZN_MEM_SHARED) {
		if (kind == HZN_MEM_CODE) {
			void *buf = (void *)__get_free_page(GFP_KERNEL);
			loff_t pos = (loff_t)offset << PAGE_SHIFT;
			unsigned long i;

			if (!buf) {
				ret = HZN_RESULT_OUT_OF_MEMORY;
				goto fail_file;
			}
			memset(buf, 0xff, PAGE_SIZE);
			for (i = 0; i < size; i += PAGE_SIZE)
				if (kernel_write(backing, buf, PAGE_SIZE, &pos) != PAGE_SIZE)
					break;
			free_page((unsigned long)buf);
			if (i != size) {
				ret = HZN_RESULT_OUT_OF_MEMORY;
				goto fail_file;
			}
		}
		if (hzn_mem_protect(obj, owner_perm)) {
			ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
			goto fail_file;
		}
		mutex_lock(&hzn_sources_lock);
		list_add(&obj->source_node, &hzn_sources);
		mutex_unlock(&hzn_sources_lock);
	}
	mutex_unlock(&proc->mem_lock);
	h = hzn_handle_add(file);
	if (!h)
		return HZN_RESULT_OUT_OF_HANDLES;
	*handle = h;
	return HZN_RESULT_SUCCESS;
fail_file:
	/* release also handles a failed creation before insertion. */
	INIT_LIST_HEAD(&obj->source_node);
	mutex_unlock(&proc->mem_lock);
	fput(file);
	return ret;
fail:
	mutex_unlock(&proc->mem_lock);
	mmdrop(obj->owner);
	kfree(obj);
	return ret;
}

long hzn_create_shared_memory(u64 size, u32 owner, u32 remote, u32 *handle)
{
	if (!size || !PAGE_ALIGNED(size) || size > TASK_SIZE_MAX)
		return HZN_RESULT_INVALID_SIZE;
	if ((owner != PROT_READ && owner != (PROT_READ | PROT_WRITE)) ||
	    (remote != PROT_READ && remote != (PROT_READ | PROT_WRITE) &&
	     remote != HZN_MEMORY_PERMISSION_DONT_CARE))
		return HZN_RESULT_INVALID_NEW_MEMORY_PERMISSION;
	return hzn_new_memory(HZN_MEM_SHARED, 0, size, owner, remote, handle);
}

long hzn_create_transfer_memory(unsigned long addr, u64 size, u32 perm, u32 *handle)
{
	long ret = hzn_mem_range(addr, size);

	if (ret)
		return ret;
	if (perm != 0 && perm != PROT_READ && perm != (PROT_READ | PROT_WRITE))
		return HZN_RESULT_INVALID_NEW_MEMORY_PERMISSION;
	return hzn_new_memory(HZN_MEM_TRANSFER, addr, size, perm, 3, handle);
}

long hzn_create_code_memory(unsigned long addr, u64 size, u32 *handle)
{
	long ret = hzn_mem_range(addr, size);

	return ret ? ret : hzn_new_memory(HZN_MEM_CODE, addr, size, 0, 0, handle);
}

long hzn_transfer_memory_hint(u32 handle, u64 *addr)
{
	struct file *file = hzn_handle_get(handle);
	long ret = HZN_RESULT_INVALID_HANDLE;

	if (file && file->f_op == &hzn_mem_fops) {
		struct hzn_mem_object *obj = file->private_data;

		if (obj->kind == HZN_MEM_TRANSFER) {
			*addr = obj->source;
			ret = HZN_RESULT_SUCCESS;
		}
	}
	if (file)
		fput(file);
	return ret;
}

static long hzn_map_object(u32 handle, unsigned long addr, u64 size, u32 perm,
			   bool transfer, int code_op)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_object_mapping *map;
	struct hzn_mem_object *obj;
	struct file *file;
	long ret = hzn_mem_range(addr, size);
	unsigned long m;
	u32 allowed;
	bool owner = code_op == 2;

	if (ret)
		return ret;
	file = hzn_handle_get(handle);
	if (!file || file->f_op != &hzn_mem_fops) {
		if (file)
			fput(file);
		return HZN_RESULT_INVALID_HANDLE;
	}
	obj = file->private_data;
	ret = HZN_RESULT_INVALID_HANDLE;
	if (obj->kind != (code_op >= 0 ? HZN_MEM_CODE :
			 transfer ? HZN_MEM_TRANSFER : HZN_MEM_SHARED))
		goto put;
	ret = HZN_RESULT_INVALID_SIZE;
	if (size != obj->size)
		goto put;
	allowed = current->mm == obj->owner ? obj->owner_perm : obj->remote_perm;
	if (transfer)
		allowed = obj->owner_perm;
	ret = HZN_RESULT_INVALID_NEW_MEMORY_PERMISSION;
	if (code_op >= 0) {
		if ((!owner && perm != 3) || (owner && perm != 1 && perm != 5))
			goto put;
		if (owner && current->mm != obj->owner) {
			ret = HZN_RESULT_INVALID_STATE;
			goto put;
		}
	} else if (transfer) {
		ret = HZN_RESULT_INVALID_STATE;
		if ((perm != 0 && perm != 1 && perm != 3) || perm != allowed)
			goto put;
	} else if ((perm != 1 && perm != 3) ||
		   (allowed != HZN_MEMORY_PERMISSION_DONT_CARE && perm != allowed)) {
		goto put;
	}
	map = kzalloc_obj(*map);
	ret = HZN_RESULT_OUT_OF_MEMORY;
	if (!map)
		goto put;
	mutex_lock(&proc->mem_lock);
	mutex_lock(&obj->lock);
	ret = HZN_RESULT_INVALID_STATE;
	if ((transfer || code_op >= 0) && (owner ? obj->owner_mapped : obj->mapped))
		goto unlock;
	if (owner && (perm & PROT_EXEC)) {
		pgoff_t p;
		for (p = obj->offset; p < obj->offset + (size >> PAGE_SHIFT); p++) {
			struct folio *folio = filemap_get_folio(obj->backing->f_mapping, p);
			if (IS_ERR(folio)) {
				ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
				goto unlock;
			}
			sync_icache_aliases((unsigned long)folio_address(folio),
				(unsigned long)folio_address(folio) + folio_size(folio));
			folio_put(folio);
		}
	}
	m = vm_mmap(code_op >= 0 ? obj->backing : file, addr, size, transfer ? 3 : perm,
		    MAP_SHARED | MAP_FIXED_NOREPLACE,
		    code_op >= 0 ? (unsigned long)obj->offset << PAGE_SHIFT : 0);
	ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
	if (m != addr)
		goto unlock;
	if (owner)
		obj->owner_mapped = true;
	else if (transfer || code_op >= 0)
		obj->mapped = true;
	map->addr = addr;
	map->size = size;
	map->object = file; /* retain the type until the last mapping is gone */
	map->owner = owner;
	map->state = code_op >= 0 ? (owner ? HZN_MEMORY_STATE_GENERATED_CODE :
				   HZN_MEMORY_STATE_CODE_OUT) : transfer ?
		(obj->owner_perm ? HZN_MEMORY_STATE_SHARED_TRANSFERRED :
		 HZN_MEMORY_STATE_TRANSFERRED) : HZN_MEMORY_STATE_SHARED;
	list_add(&map->node, &proc->object_maps);
	mutex_unlock(&obj->lock);
	mutex_unlock(&proc->mem_lock);
	return HZN_RESULT_SUCCESS;
unlock:
	mutex_unlock(&obj->lock);
	mutex_unlock(&proc->mem_lock);
	kfree(map);
put:
	fput(file);
	return ret;
}

static long hzn_unmap_object(u32 handle, unsigned long addr, u64 size,
			     bool transfer, int code_op)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_object_mapping *map;
	struct hzn_mem_object *obj;
	struct file *file;
	long ret = hzn_mem_range(addr, size);

	if (ret)
		return ret;
	file = hzn_handle_get(handle);
	if (!file || file->f_op != &hzn_mem_fops) {
		if (file)
			fput(file);
		return HZN_RESULT_INVALID_HANDLE;
	}
	obj = file->private_data;
	ret = HZN_RESULT_INVALID_HANDLE;
	/* nnSdk uses UnmapSharedMemory for transfer-memory guard-page rollback. */
	if (code_op >= 0 ? obj->kind != HZN_MEM_CODE :
	    transfer ? obj->kind != HZN_MEM_TRANSFER : obj->kind == HZN_MEM_CODE)
		goto put;
	ret = HZN_RESULT_INVALID_SIZE;
	if (size != obj->size)
		goto put;
	mutex_lock(&proc->mem_lock);
	mutex_lock(&obj->lock);
	ret = HZN_RESULT_INVALID_MEMORY_REGION;
	list_for_each_entry(map, &proc->object_maps, node) {
		if (map->object != file || map->addr != addr || map->size != size ||
		    (code_op >= 0 && map->owner != (code_op == 3)))
			continue;
		mmap_write_lock(current->mm);
		if (hzn_backed_range(current->mm, addr, size, obj->backing, obj->offset))
			ret = do_munmap(current->mm, addr, size, NULL) ?
				HZN_RESULT_OUT_OF_MEMORY : HZN_RESULT_SUCCESS;
		mmap_write_unlock(current->mm);
		if (ret)
			break;
		list_del(&map->node);
		if (map->owner)
			obj->owner_mapped = false;
		else
			obj->mapped = false;
		fput(map->object);
		kfree(map);
		break;
	}
	mutex_unlock(&obj->lock);
	mutex_unlock(&proc->mem_lock);
put:
	fput(file);
	return ret;
}

long hzn_map_transfer_memory(u32 h, unsigned long a, u64 s, u32 p)
{
	return hzn_map_object(h, a, s, p, true, -1);
}
long hzn_unmap_transfer_memory(u32 h, unsigned long a, u64 s)
{
	return hzn_unmap_object(h, a, s, true, -1);
}
long hzn_map_typed_shared_memory(u32 h, unsigned long a, u64 s, u32 p)
{
	return hzn_map_object(h, a, s, p, false, -1);
}
long hzn_unmap_typed_shared_memory(u32 h, unsigned long a, u64 s)
{
	return hzn_unmap_object(h, a, s, false, -1);
}
bool hzn_is_memory_object(struct file *file)
{
	return file->f_op == &hzn_mem_fops;
}
long hzn_control_code_memory(u32 h, u32 op, unsigned long a, u64 s, u32 p)
{
	if (op > 3)
		return HZN_RESULT_INVALID_ENUM_VALUE;
	if (op & 1)
		return p ? HZN_RESULT_INVALID_NEW_MEMORY_PERMISSION :
			hzn_unmap_object(h, a, s, false, op);
	return hzn_map_object(h, a, s, p, false, op);
}
void hzn_process_free_object_maps(struct hzn_process *proc)
{
	struct hzn_object_mapping *map, *tmp;

	list_for_each_entry_safe(map, tmp, &proc->object_maps, node) {
		struct hzn_mem_object *obj = map->object->private_data;

		mutex_lock(&obj->lock);
		if (map->owner)
			obj->owner_mapped = false;
		else
			obj->mapped = false;
		mutex_unlock(&obj->lock);
		list_del(&map->node);
		fput(map->object);
		kfree(map);
	}
}
