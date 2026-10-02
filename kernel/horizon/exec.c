// SPDX-License-Identifier: GPL-2.0
/*
 * Loader for Horizon programs, in the image format the Horizon Linux loader
 * (mizu) writes: struct horizon_hdr and its code sets, padded to a page,
 * followed by the memory of each code set. It only accepts images executed
 * with horizon_execve(2)/horizon_execveat(2).
 *
 * Code and read-only data are private file mappings. Data (with bss) is
 * copied into shmem and mapped shared, like the heap, so that services can
 * map it. The address space size of the program (32, 36 or 39 bits) is
 * honoured by placing everything, the stack included, below its end.
 */

#include <linux/binfmts.h>
#include <linux/cpumask.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/horizon.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/personality.h>
#include <linux/sched/task_stack.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <asm/horizon/syscall.h>
#include <asm/processor.h>

#include "internal.h"

#define HZN_MAX_CODESETS	64

struct hzn_layout {
	unsigned long space_end;
	unsigned long alias_size, heap_size, alias_code_size, stack_size;
};

static int hzn_layout(u8 type, struct hzn_layout *l)
{
	switch (type) {
	case HZN_IS_39_BIT:
		l->space_end = 1UL << 39;
		l->alias_size = SZ_64G;
		l->heap_size = SZ_4G + SZ_2G;
		l->alias_code_size = SZ_512G - SZ_128M;
		l->stack_size = SZ_2G;
		return 0;
	case HZN_IS_36_BIT:
		l->space_end = 1UL << 36;
		l->alias_size = SZ_4G + SZ_2G;
		l->heap_size = SZ_4G + SZ_2G;
		l->alias_code_size = SZ_64G - SZ_2G;
		l->stack_size = 0;
		return 0;
	case HZN_IS_32_BIT:
	case HZN_IS_32_BIT_NO_MAP:
		l->space_end = 1UL << 32;
		l->alias_size = SZ_1G;
		l->heap_size = SZ_1G;
		l->alias_code_size = SZ_4G - SZ_1G;
		l->stack_size = 0;
		return 0;
	}
	return -EINVAL;
}

/* Copies [off, off + size) of @file into shmem and maps it shared at @addr. */
static int hzn_map_data(struct file *file, loff_t off, unsigned long addr,
			unsigned long size)
{
	struct file *shm;
	void *buf;
	loff_t wpos = 0;
	unsigned long done, m;
	ssize_t n;
	int ret = 0;

	shm = shmem_file_setup("horizon_data", size, EMPTY_VMA_FLAGS);
	if (IS_ERR(shm))
		return PTR_ERR(shm);
	buf = (void *)__get_free_page(GFP_KERNEL);
	if (!buf) {
		fput(shm);
		return -ENOMEM;
	}
	for (done = 0; done < size; done += PAGE_SIZE) {
		n = kernel_read(file, buf, PAGE_SIZE, &off);
		if (n < 0) {
			ret = n;
			break;
		}
		if (n < PAGE_SIZE)
			memset(buf + n, 0, PAGE_SIZE - n);
		if (!memchr_inv(buf, 0, PAGE_SIZE)) {
			wpos += PAGE_SIZE;
			continue;
		}
		if (kernel_write(shm, buf, PAGE_SIZE, &wpos) != PAGE_SIZE) {
			ret = -EIO;
			break;
		}
	}
	free_page((unsigned long)buf);
	if (!ret) {
		m = vm_mmap(shm, addr, size, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED, 0);
		if (m != addr)
			ret = IS_ERR_VALUE(m) ? (int)m : -EINVAL;
	}
	fput(shm);
	return ret;
}

static int load_horizon_binary(struct linux_binprm *bprm);

static struct linux_binfmt horizon_format = {
	.module		= THIS_MODULE,
	.load_binary	= load_horizon_binary,
};

/* Checks the header and code sets; returns the total memory size. */
static long hzn_check_image(struct linux_binprm *bprm,
			    const struct horizon_hdr *hdr, size_t hdr_size,
			    const struct hzn_layout *l)
{
	loff_t isize = i_size_read(file_inode(bprm->file));
	u64 file_pos = PAGE_ALIGN(hdr_size), total = 0;
	u32 i, j;

	/* Horizon core masks have 64 bits. */
	if (!hzn_valid_priority(hdr->main_thread_priority) ||
	    hdr->ideal_core >= min_t(unsigned int, nr_cpu_ids, 64) ||
	    !cpu_online(hdr->ideal_core))
		return -EINVAL;

	for (i = 0; i < hdr->num_codesets; i++) {
		const struct horizon_codeset_hdr *cs = &hdr->codesets[i];

		if (!cs->memory_size || !PAGE_ALIGNED(cs->memory_size) ||
		    cs->memory_size > l->space_end)
			return -EINVAL;
		for (j = 0; j < ARRAY_SIZE(cs->segments); j++) {
			u64 addr = cs->segments[j].addr, size = cs->segments[j].size;

			if (!size)
				continue;
			if (!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(size) ||
			    addr + size > cs->memory_size ||
			    file_pos + addr + size > isize)
				return -EINVAL;
		}
		file_pos += cs->memory_size;
		total += cs->memory_size;
		if (total > l->space_end)
			return -EINVAL;
	}
	return total;
}

static int load_horizon_binary(struct linux_binprm *bprm)
{
	const struct horizon_hdr *peek = (const void *)bprm->buf;
	struct pt_regs *regs = current_pt_regs();
	struct mm_struct *mm;
	struct horizon_hdr *hdr;
	struct hzn_process *proc;
	struct hzn_thread *thread;
	struct hzn_layout l;
	unsigned long vm_pos, tls, m;
	size_t hdr_size;
	loff_t pos = 0;
	u64 file_pos;
	long total;
	u32 i, j, handle;
	int ret, cpu;

	if (!current->hzn_in_execve || peek->magic != HORIZON_MAGIC)
		return -ENOEXEC;
	/* 32-bit Horizon programs are not supported. */
	if (!peek->is_64bit || !peek->num_codesets ||
	    peek->num_codesets > HZN_MAX_CODESETS || hzn_layout(peek->address_space_type, &l))
		return -ENOEXEC;
	if (!can_mmap_file(bprm->file))
		return -ENOEXEC;

	hdr_size = struct_size(peek, codesets, peek->num_codesets);
	hdr = kmalloc(hdr_size, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;
	if (kernel_read(bprm->file, hdr, hdr_size, &pos) != hdr_size) {
		ret = -ENOEXEC;
		goto out;
	}
	/* From here on only the copy is used. */
	ret = -ENOEXEC;
	if (hdr->num_codesets != peek->num_codesets || !hdr->is_64bit ||
	    hzn_layout(hdr->address_space_type, &l))
		goto out;
	total = hzn_check_image(bprm, hdr, hdr_size, &l);
	if (total < 0)
		goto out;

	ret = -ENOMEM;
	proc = hzn_process_alloc();
	if (!proc)
		goto out;
	proc->title_id = hdr->title_id;
	proc->ideal_core = hdr->ideal_core;
	/* The cores the loader lets it run on, and its ideal core. */
	for_each_cpu_and(cpu, current->cpus_ptr, cpu_online_mask)
		if (cpu < 64)
			proc->core_mask |= BIT_ULL(cpu);
	proc->core_mask |= BIT_ULL(proc->ideal_core);
	proc->address_space_type = hdr->address_space_type;
	proc->system_resource_size = hdr->system_resource_size;
	proc->space_end = l.space_end;
	proc->image_end = HZN_IMAGE_BASE + total;
	proc->alias_code_start = HZN_IMAGE_BASE;
	proc->alias_code_size = l.alias_code_size;
	/*
	 * Regions start on 2 MiB boundaries, as on Horizon: nn::os requires the
	 * heap address svcSetHeapSize returns to be aligned to MemoryHeapUnitSize.
	 */
	proc->alias_start = ALIGN(HZN_IMAGE_BASE + total, SZ_2M);
	proc->alias_size = l.alias_size;
	proc->heap_start = proc->alias_start + l.alias_size;
	proc->heap_region_size = l.heap_size;
	proc->stack_region_size = l.stack_size;
	proc->stack_region_start = l.space_end - l.stack_size;
	proc->tls_base = proc->alias_start + HZN_TLS_OFFSET;
	if (proc->heap_start + proc->heap_region_size > proc->stack_region_start ||
	    proc->heap_start + proc->heap_region_size > l.space_end - SZ_256M) {
		ret = -ENOEXEC;
		goto out_proc;
	}
	proc->heap_file = shmem_file_setup("horizon_heap", proc->heap_region_size,
					   mk_vma_flags(VMA_NORESERVE_BIT));
	if (IS_ERR(proc->heap_file)) {
		ret = PTR_ERR(proc->heap_file);
		proc->heap_file = NULL;
		goto out_proc;
	}
	thread = hzn_thread_alloc(proc);
	if (!thread)
		goto out_proc;
	thread->priority = hdr->main_thread_priority;
	thread->base_priority = hdr->main_thread_priority;
	thread->state = HZN_THREAD_STARTED;
	thread->ideal_core = proc->ideal_core;
	thread->affinity = BIT_ULL(proc->ideal_core);

	ret = begin_new_exec(bprm);
	if (ret)
		goto out_thread;

	/* Point of no return: errors from here on kill the process. */
	clear_thread_flag(TIF_32BIT);
	current->personality &= ~READ_IMPLIES_EXEC;
	setup_new_exec(bprm);

	current->hzn_thread = thread;
	thread->task = current;
	set_thread_flag(TIF_HORIZON);
	horizon_update_cntkctl();
	spin_lock(&proc->lock);
	proc->nr_running = 1;
	spin_unlock(&proc->lock);
	hzn_process_put(proc);		/* the thread holds it now */

	/* For /proc: the code and data of the first code set. */
	mm = current->mm;
	mm->start_code = HZN_IMAGE_BASE + hdr->codesets[0].segments[0].addr;
	mm->end_code = mm->start_code + hdr->codesets[0].segments[0].size;
	mm->start_data = HZN_IMAGE_BASE + hdr->codesets[0].segments[2].addr;
	mm->end_data = mm->start_data + hdr->codesets[0].segments[2].size;
	mm->start_brk = proc->heap_start;
	mm->brk = proc->heap_start;
	/* Keep mappings the kernel places itself below the stack region. */
	mm->mmap_base = l.stack_size ? proc->stack_region_start :
				       l.space_end - SZ_256M;

	ret = setup_arg_pages(bprm, l.space_end, EXSTACK_DEFAULT);
	if (ret)
		goto out;

	vm_pos = HZN_IMAGE_BASE;
	file_pos = PAGE_ALIGN(hdr_size);
	for (i = 0; i < hdr->num_codesets; i++) {
		const struct horizon_codeset_hdr *cs = &hdr->codesets[i];

		for (j = 0; j < ARRAY_SIZE(cs->segments); j++) {
			unsigned long addr = vm_pos + cs->segments[j].addr;
			unsigned long size = cs->segments[j].size;
			loff_t off = file_pos + cs->segments[j].addr;

			if (!size)
				continue;
			if (j == 2) {
				ret = hzn_map_data(bprm->file, off, addr, size);
				if (ret)
					goto out;
				continue;
			}
			m = vm_mmap(bprm->file, addr, size,
				    PROT_READ | (j == 0 ? PROT_EXEC : 0),
				    MAP_FIXED | MAP_PRIVATE, off);
			if (m != addr) {
				ret = IS_ERR_VALUE(m) ? (int)m : -EINVAL;
				goto out;
			}
		}
		vm_pos += cs->memory_size;
		file_pos += cs->memory_size;
	}
	set_binfmt(&horizon_format);

	/* The main thread's TLS, read through TPIDRRO_EL0. */
	tls = vm_mmap(NULL, proc->tls_base, PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	if (IS_ERR_VALUE(tls)) {
		ret = tls;
		goto out;
	}
	thread->tls = tls;
	current->thread.uw.tp_value = tls;
	barrier();	/* see tls_thread_flush() */
	write_sysreg(tls, tpidrro_el0);

	/*
	 * The process local region, in the thread-local area as on Horizon:
	 * the user exception context and the dying message region.
	 */
	m = vm_mmap(NULL, proc->tls_base + PAGE_SIZE, PAGE_SIZE,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	if (IS_ERR_VALUE(m)) {
		ret = m;
		goto out;
	}
	proc->plr = m;

	handle = hzn_thread_handle_create(current);
	if (handle == HZN_INVALID_HANDLE) {
		ret = -EMFILE;
		goto out;
	}
	thread->handle = handle;
	set_cpus_allowed_ptr(current, cpumask_of(proc->ideal_core));
	hzn_apply_priority(current, hdr->main_thread_priority);

	/* SP has to be 16-byte aligned; argv and envp strings are above it. */
	mm->start_stack = arch_align_stack(bprm->p);
	finalize_exec(bprm);
	start_thread(regs, HZN_IMAGE_BASE, mm->start_stack);
	regs->regs[1] = handle;
	ret = 0;
	goto out;

out_thread:
	kfree(thread);
	/* thread held a reference */
	hzn_process_put(proc);
out_proc:
	hzn_process_put(proc);
out:
	kfree(hdr);
	return ret;
}

static int __init init_horizon_binfmt(void)
{
	register_binfmt(&horizon_format);
	return 0;
}
core_initcall(init_horizon_binfmt);
