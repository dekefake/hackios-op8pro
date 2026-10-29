/*
 * Starts the pthread kernel extension when it is linked statically into the
 * kernel image.
 *
 * com.apple.kec.pthread (open source, libpthread/kern) normally arrives as a
 * prelinked kext whose start routine registers its function table with
 * pthread_shims.c. This configuration has no kext loading at all, so the kext
 * is built as libpthread_kext.a, linked in, and started from here before
 * bsd_init() needs it.
 */
#include <mach/kmod.h>
#include <kern/startup.h>
#include <kern/debug.h>
#include <kern/thread.h>
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/user.h>

extern kern_return_t pthread_start(kmod_info_t *ki, void *d);

__startup_func
static void
pthread_kext_static_start(void)
{
	kern_return_t kr = pthread_start(NULL, NULL);
	if (kr != KERN_SUCCESS) {
		panic("static pthread kext failed to start: %d", kr);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pthread_kext_static_start);

/*
 * For osfmk/arm64/user_exception_trace.c: what the kernel's workqueue says about a thread (its stack, flags, priority, thread port), to tell a
 * worker whose stack pointer is wrong from one that is not a worker at all. Zeros for a thread that is not one.
 */
void
hk_workq_info(thread_t thread, uint64_t *stack, uint32_t *flags, uint32_t *pri, uint32_t *thport)
{
	struct uthread *uth = get_bsdthread_info(thread);

	if (uth != NULL) {
		*stack = (uint64_t)uth->uu_workq_stackaddr;
		*flags = (uint32_t)uth->uu_workq_flags;
		*pri = (uint32_t)uth->uu_workq_pri.qos_req;
		*thport = (uint32_t)uth->uu_workq_thport;
	}
}
