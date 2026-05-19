#include <kernel/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/spinlock.h>
#include <kernel/stacktrace.h>
#include <kernel/macros.h>
#include <kernel/cpufeature.h>
#include <kernel/mmu.h>
#include <kernel/apic.h>
#include <kernel/panic.h>

/* Store CPU Features as a uint64_t and access them based on bits in the variable */
uint64_t cpu_features = 0;

/* Get available features of CPU and store in uint64_t variable */
void __init cpu_feature_init(void) {
	uint32_t unused, ecx = 0, edx = 0;
	__get_cpuid(1, &unused, &unused, &ecx, &edx);

	cpu_features = ((uint64_t)edx << 32) | ecx;
	kprintf("cpu: CPU Has features edx:eax of cpuid 1 = %p\n", cpu_features);
}
