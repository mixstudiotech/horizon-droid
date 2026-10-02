// SPDX-License-Identifier: GPL-2.0
/* Whole-cache maintenance is rare, and must exclude other CPUs' writers. */
#include <linux/cpu.h>
#include <linux/stop_machine.h>
#include <asm/barrier.h>
#include <asm/sysreg.h>
#include "internal.h"

static int hzn_flush_cpu_cache(void *unused)
{
	u64 clidr = read_sysreg(clidr_el1), saved = read_sysreg(csselr_el1);
	bool ccidx = ((read_sysreg(id_aa64mmfr2_el1) >> 20) & 15) == 1;
	unsigned int level;

	dsb(sy);
	for (level = 0; level < 7; level++) {
		u64 ccsidr;
		unsigned int type = (clidr >> (level * 3)) & 7;
		unsigned int sets, ways, line, way_shift, set, way;

		if (!type)
			break;
		if (type < 2)
			continue;
		write_sysreg(level << 1, csselr_el1);
		isb();
		ccsidr = read_sysreg(ccsidr_el1);
		line = (ccsidr & 7) + 4;
		ways = ccidx ? (ccsidr >> 3) & 0x1fffff : (ccsidr >> 3) & 0x3ff;
		sets = ccidx ? (ccsidr >> 32) & 0xffffff : (ccsidr >> 13) & 0x7fff;
		way_shift = ways ? __builtin_clz(ways) : 0;
		for (way = 0; way <= ways; way++)
			for (set = 0; set <= sets; set++) {
				u64 operand = (level << 1) | ((u64)set << line) |
					      ((u64)way << way_shift);
				asm volatile("dc cisw, %0" : : "r" (operand) : "memory");
			}
		dsb(sy);
	}
	write_sysreg(saved, csselr_el1);
	isb();
	return 0;
}

long hzn_flush_entire_data_cache(void)
{
	int ret = stop_machine(hzn_flush_cpu_cache, NULL, cpu_online_mask);

	return ret ? HZN_RESULT_OUT_OF_RESOURCE : HZN_RESULT_SUCCESS;
}
