/*
 * hk_fastrpc.c -- FastRPC with the OnePlus 8 Pro's sensor DSP (SLPI), the part of Android's adsprpc driver and of its
 * sscrpcd daemon that the sensors need: the DSP's root process ("attachguestos") given a LISTENER, so that what runs
 * there can open, read and write files of ours. Qualcomm's SEE does that for its registry (/mnt/vendor/persist/sensors/
 * registry) and its configuration (/vendor/etc/sensors, /odm/etc/sensor); without it SEE answers every SUID lookup with
 * no sensor at all (observed 2026-11-07: "accel", "gyro", "mag", "proximity" ... each an empty list).
 *
 * Sources: Linux drivers/misc/fastrpc.c (the messages, the argument page), Qualcomm's own user library
 * (github.com/quic/fastrpc: fastrpc_apps_user.c's attach, listener_android.c and listener_buf.h for the reverse calls'
 * packing, the QAIC stubs and skeletons of remotectl, adsp_listener, adsp_default_listener, apps_remotectl, apps_std).
 *
 *   transport   GLINK channel "fastrpcglink-apps-dsp" (hk_slpi.c opens it when the DSP does); a call is a fastrpc_msg
 *               { pid, tid, ctx, handle, sc, addr, size } pointing at an argument page in memory the DSP reaches through
 *               the application SMMU as stream 0x541 (dt: qcom,msm_fastrpc_compute_cb13, "sdsprpc-smd"); its answer is
 *               { ctx, retval }. Addresses carry the stream's low bits above bit 32, as Android's do.
 *   argument page  rpra[n] {pv, len}, list[n] {num, pgidx}, pages[n] {addr, size}, 16 fd slots, 64 crc slots, then the
 *               buffers, each aligned to 128
 *   start       attach (handle 1, method 0, our process id; pid 0 in the message, as a kernel call), the listener's
 *               init2 (handle 3, method 3), remotectl_open("adsp_default_listener") (handle 0) and its register
 *   then        adsp_listener_next2 (handle 3, method 4) for ever: it gives back the last reverse call's result and
 *               out-buffers and brings the next one -- handle, scalars, its in-buffers packed (len, align 8, bytes; then
 *               the out-buffers' lengths)
 * Reverse handles served: 0 apps_remotectl (open/close), FRPC_APPS_STD apps_std (files and directories, see apps_std()),
 * FRPC_APPS_MSG adspmsgd_apps (the DSP's log lines, said). Files: what the DSP names under /mnt/vendor/persist, /persist,
 * /vendor, /odm is looked for in /private/var/hackios/sensors (writable; init makes it, etc/var.skel) and then in
 * /hackios/sensors (the image's copy of the phone's own, made from its persist, vendor and odm partitions); writes go
 * to the first.
 *
 * Boot-arg slpi_frpc=0: not started. slpi_dsplog=1: the DSP's own log lines too (session_up()). Started by hk_slpi.c once the sensor DSP is up and linked.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vnode_if.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t hk_smmu_lock;
extern int unlink1(vfs_context_t ctx, vnode_t start_dvp, user_addr_t path_arg, enum uio_seg segflg, int unlink_flags);

/* hk_slpi.c's */
int hk_slpi_fastrpc_send(const void *data, uint32_t length);
bool hk_slpi_fastrpc_ready(void);
void hk_slpi_log(const char *line);

void hk_fastrpc_input(const uint8_t *data, uint32_t length);
void hk_fastrpc_start(void);
bool hk_fastrpc_listening(void);

static LCK_GRP_DECLARE(frpc_lock_group, "hk-fastrpc");
static LCK_MTX_DECLARE(frpc_lock, &frpc_lock_group);

static unsigned said;
static void say(const char *format, ...) __printflike(1, 2);
static void
say(const char *format, ...)
{
	char line[320];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	printf("%s", line);
	if (said++ < 400) {
		hk_slpi_log(line);
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- the DSP's view of our memory: stream 0x541 of the application SMMU (as hk_sound.c's 0x1801) ------------ */

#define SMMU_PHYS               0x15000000ul
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define FRPC_PTE                (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

#define FRPC_STREAM             0x541                   /* dt: qcom,msm_fastrpc_compute_cb13 */
#define FRPC_IOVA               0x80000000ull           /* dt: its qcom,iommu-dma-addr-pool */
#define FRPC_CB_BITS            ((uint64_t)(FRPC_STREAM & 0xf) << 32)
#define FRPC_POOL               (2u << 20)
/*
 * Eight slots of 256 KB (2026-11-13; four of 512 KB before): 0-3 each session's listener and its other calls (slot, slot + 1),
 * 4-5 a call made while one of the session's reverse calls is served (FRPC_NESTED: the remote map apps_mem asks for), 6-7 the
 * memory the DSP's log goes into (FRPC_LOGBUF: adspmsgd's init3 asks apps_mem for it). FRPC_HALF is a slot's size.
 */
#define FRPC_HALF               (FRPC_POOL / 8)
#define FRPC_CALL_SLOTS         6
#define FRPC_NESTED(se)         (4 + (se)->slot / 2)
#define FRPC_LOGBUF(se)         (6 + (se)->slot / 2)

static struct {
	vm_offset_t base, second, context;
	uint32_t page, pages, match, ctx, old_smr, old_s2cr;
	bool on, passed;
	vm_offset_t tables;
} mmu;
static vm_offset_t pool;

static uint32_t
mrd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static void
mwr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile ("dsb sy");
}

static uint64_t
dsp_address(const void *at)
{
	return mmu.passed ? kvtophys((vm_offset_t)at) : (FRPC_IOVA + ((vm_offset_t)at - pool)) | FRPC_CB_BITS;
}

static bool
mmu_on(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;

	mmu.base = ml_io_map(SMMU_PHYS, 0x100000);
	if (mmu.base == 0) {
		return false;
	}
	scr0 = mrd(mmu.base + SMMU_SCR0);
	idr0 = mrd(mmu.base + SMMU_IDR0);
	idr1 = mrd(mmu.base + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if (scr0 & 1u << 3) {
		say("frpc: the SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	mmu.page = idr1 >> 31 ? 0x10000 : 0x1000;
	mmu.pages = 2u << (idr1 >> 28 & 7);
	if (2 * mmu.pages * mmu.page != 0x100000) {
		say("frpc: the SMMU is not the megabyte this expects\n");
		return false;
	}
	mmu.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = mrd(mmu.base + SMMU_SMR(n)), s2cr = mrd(mmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ FRPC_STREAM) & ~mask) == 0) {
				if ((s2cr >> 16 & 3) == 1) {
					say("frpc: match register %u lets stream %#x through untranslated (s2cr %#x): physical addresses\n", n,
					    FRPC_STREAM, s2cr);
					mmu.passed = true;
					return true;
				}
				if (smr == (1u << 31 | FRPC_STREAM)) {
					/* ours, as a kernel before this one left it */
					mmu.match = n;
					mmu.old_smr = 0;
					mmu.old_s2cr = 0x200ff;
					mmu.ctx = s2cr & 0xff;
					used[mmu.ctx] = false;
					say("frpc: match register %u is ours from a kernel before this one (context %u): taken over\n", n, mmu.ctx);
					goto have_context;
				}
				say("frpc: match register %u covers stream %#x already (s2cr %#x): it is somebody's\n", n, FRPC_STREAM, s2cr);
				return false;
			}
		} else if (mmu.match == ~0u) {
			mmu.match = n;
			mmu.old_smr = smr;
			mmu.old_s2cr = s2cr;
		}
	}
	for (mmu.ctx = 0; mmu.ctx < contexts && used[mmu.ctx]; mmu.ctx++) {
	}
	if (mmu.match == ~0u || mmu.ctx >= contexts) {
		say("frpc: the SMMU has no match register or no context left\n");
		return false;
	}
have_context:
	mmu.second = mmu.base + mmu.page;
	mmu.context = mmu.base + (mmu.pages + mmu.ctx) * mmu.page;
	if (mmu.old_s2cr != 0x200ff && (mrd(mmu.context + SMMU_CB_SCTLR) & 1)) {
		say("frpc: context %u is switched on with no stream through it: somebody's\n", mmu.ctx);
		return false;
	}
	if (kmem_alloc(kernel_map, &mmu.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	{
		uint64_t *root = (uint64_t *)mmu.tables, *second = (uint64_t *)(mmu.tables + 4096), *third = (uint64_t *)(mmu.tables + 8192);
		uint64_t table_root;

		root[FRPC_IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
		second[FRPC_IOVA >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
		for (unsigned page = 0; page < FRPC_POOL / 4096; page++) {
			third[(FRPC_IOVA >> 12 & 0x1ff) + page] = kvtophys(pool + 4096 * page) | FRPC_PTE;
		}
		FlushPoC_DcacheRegion(mmu.tables, 3 * 4096);
		table_root = kvtophys(mmu.tables) | (uint64_t)mmu.ctx << 48;
		say("frpc: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our tables for %u KB at %#llx, match register %u sends stream "
		    "%#x through it\n", mmu.ctx, FRPC_POOL / 1024, FRPC_IOVA, mmu.match, FRPC_STREAM);
		mwr(mmu.context + SMMU_CB_SCTLR, 0);
		mwr(mmu.second + SMMU_CBA2R(mmu.ctx), 1);
		mwr(mmu.second + SMMU_CBAR(mmu.ctx), SMMU_CBAR_STAGE_1);
		mwr(mmu.context + SMMU_CB_TCR2, SMMU_TCR2);
		mwr(mmu.context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
		mwr(mmu.context + SMMU_CB_TTBR0, (uint32_t)table_root);
		mwr(mmu.context + SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
		mwr(mmu.context + SMMU_CB_TTBR1, 0);
		mwr(mmu.context + SMMU_CB_TTBR1 + 4, 0);
		mwr(mmu.context + SMMU_CB_MAIR0, SMMU_MAIR0);
		mwr(mmu.context + SMMU_CB_MAIR1, 0);
		mwr(mmu.context + SMMU_CB_FSR, SMMU_FSR_ALL);
		mwr(mmu.context + SMMU_CB_TLBIASID, mmu.ctx);
		mwr(mmu.context + SMMU_CB_TLBSYNC, 1);
		for (unsigned ms = 0; mrd(mmu.context + SMMU_CB_TLBSTATUS) & 1; ms++) {
			if (ms == 100) {
				say("frpc: the SMMU does not finish forgetting old translations\n");
				return false;
			}
			sleep_ms(1);
		}
		mwr(mmu.context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
		mwr(mmu.base + SMMU_S2CR(mmu.match), mmu.ctx);
		mwr(mmu.base + SMMU_SMR(mmu.match), 1u << 31 | FRPC_STREAM);
		if (mrd(mmu.base + SMMU_SMR(mmu.match)) != (1u << 31 | FRPC_STREAM) || (mrd(mmu.base + SMMU_S2CR(mmu.match)) & 0x300ff) != mmu.ctx ||
		    !(mrd(mmu.context + SMMU_CB_SCTLR) & 1)) {
			say("frpc: THE SMMU DID NOT TAKE IT: stream %#x is not through context %u (smr %#x s2cr %#x)\n", FRPC_STREAM, mmu.ctx,
			    mrd(mmu.base + SMMU_SMR(mmu.match)), mrd(mmu.base + SMMU_S2CR(mmu.match)));
			return false;
		}
	}
	mmu.on = true;
	say("frpc: stream %#x goes through context %u of the SMMU, our pool at %#llx (addresses for the DSP | %#llx)\n", FRPC_STREAM,
	    mmu.ctx, FRPC_IOVA, FRPC_CB_BITS);
	return true;
}

static void
mmu_faults(const char *when)
{
	uint32_t status;

	if (!mmu.on) {
		return;
	}
	status = mrd(mmu.context + SMMU_CB_FSR);
	if ((status & SMMU_FSR_ALL) == 0) {
		return;
	}
	say("frpc: THE SMMU REFUSED THE DSP (%s): context %u status %#x address %#llx syndrome %#x\n", when, mmu.ctx, status,
	    (uint64_t)mrd(mmu.context + SMMU_CB_FAR + 4) << 32 | mrd(mmu.context + SMMU_CB_FAR), mrd(mmu.context + SMMU_CB_FSYNR0));
	mwr(mmu.context + SMMU_CB_FSR, status);
}

/*
 * Our match register still sends stream 0x541 through our context? Another driver that restores registers "as it found them" can
 * write over it (the camera did, 2026-11-07, instantnoodle_camera.c fp_put_back_matches()); then every access of the DSP's to our pool
 * matches nothing. Checked before each call; put back once noticed.
 */
static void
mmu_keep(void)
{
	static unsigned put_back;

	if (!mmu.on || mmu.passed || mmu.match == ~0u) {
		return;
	}
	if (mrd(mmu.base + SMMU_SMR(mmu.match)) == (1u << 31 | FRPC_STREAM) && (mrd(mmu.base + SMMU_S2CR(mmu.match)) & 0xff) == mmu.ctx) {
		return;
	}
	lck_mtx_lock(&hk_smmu_lock);
	say("frpc: match register %u was written over (smr %#x s2cr %#x): ours again (%u)\n", mmu.match, mrd(mmu.base + SMMU_SMR(mmu.match)),
	    mrd(mmu.base + SMMU_S2CR(mmu.match)), ++put_back);
	mwr(mmu.base + SMMU_S2CR(mmu.match), mmu.ctx);
	mwr(mmu.base + SMMU_SMR(mmu.match), 1u << 31 | FRPC_STREAM);
	lck_mtx_unlock(&hk_smmu_lock);
}

/* ---- invocations ---------------------------------------------------------------------------------------- */

#define SC(method, in, out)     ((uint32_t)((method) & 0x1f) << 24 | (uint32_t)((in) & 0xff) << 16 | (uint32_t)((out) & 0xff) << 8)
#define SC_METHOD(sc)           ((sc) >> 24 & 0x1f)
#define SC_IN(sc)               ((sc) >> 16 & 0xff)
#define SC_OUT(sc)              ((sc) >> 8 & 0xff)
#define SC_HANDLES(sc)          (((sc) >> 4 & 0xf) + ((sc) & 0xf))
#define FRPC_TGID               0x5c5
#define FRPC_MAX_ARGS           16

#define AEE_EFAILED             1
#define AEE_ENOMEMORY           2
#define AEE_EBADPARM            0x0e
#define AEE_EUNSUPPORTED        0x14
#define AEE_EINVHANDLE          0x2c
#define AEE_ENOSUCHMOD          0x3e
#define AEE_EFILE               0x201

struct frpc_msg {
	int32_t pid, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
};

struct frpc_arg {
	void *ptr;
	uint32_t length;
};

static struct {
	volatile bool answered;
	volatile int32_t result;
	uint64_t ctx;
} slots[FRPC_CALL_SLOTS];

/*
 * Two sessions, as Android has two daemons: the root process (pd 0: what Android's sscrpcd without an argument serves --
 * LOWI's configuration, seen) and the SENSORS process (pd 2, "sscrpcd sensorspd": SEE, its registry and its drivers).
 * Each its own process id, listener and slots.
 */
struct frpc_session {
	const char *name;
	uint32_t pd;
	int32_t tgid;
	int slot;                       /* its listener's; slot + 1 for its other calls */
	volatile bool listening;
	uint8_t *in_bufs, *out_bufs, *scratch;
};
static struct frpc_session sessions[2] = {
	{ .name = "the root process", .pd = 0, .tgid = 0x5c5, .slot = 0 },
	{ .name = "the sensors process", .pd = 2, .tgid = 0x5c7, .slot = 2 },
};
static uint32_t next_ctx = 1, answers, strays;
static volatile bool listening, leaving;

void
hk_fastrpc_input(const uint8_t *data, uint32_t length)
{
	uint64_t ctx;
	int32_t result;

	if (length < 12) {
		say("frpc: %u bytes from the DSP that are no answer\n", length);
		return;
	}
	memcpy(&ctx, data, 8);
	memcpy(&result, data + 8, 4);
	answers++;
	for (int s = 0; s < FRPC_CALL_SLOTS; s++) {
		if (slots[s].ctx == ctx && !slots[s].answered) {
			slots[s].result = result;
			__asm__ volatile ("dmb ish");
			slots[s].answered = true;
			return;
		}
	}
	if (strays++ < 10) {
		say("frpc: an answer for context %#llx, which nobody waits for (result %d)\n", ctx, result);
	}
}

/* One call: `handle`, scalars `sc`, its buffers in `args` (in-buffers first). Answers 0 (the call's own result in *result) or an errno. */
static int
frpc_call(const struct frpc_session *session, int slot, bool kernel, uint32_t handle, uint32_t sc, struct frpc_arg *args, int32_t *result,
    uint32_t wait_ms)
{
	uint8_t *page = (uint8_t *)(pool + (vm_offset_t)slot * FRPC_HALF);
	uint32_t n = SC_IN(sc) + SC_OUT(sc), meta, at;
	uint64_t *rpra, *pages;
	uint32_t *list;
	uint32_t offsets[FRPC_MAX_ARGS];
	struct frpc_msg msg;

	if (n > FRPC_MAX_ARGS || SC_HANDLES(sc) != 0) {
		return EINVAL;
	}
	mmu_keep();
	meta = n * (16 + 8 + 16) + 16 * 8 + 64 * 4;
	bzero(page, meta);
	rpra = (uint64_t *)page;
	list = (uint32_t *)(page + n * 16);
	pages = (uint64_t *)(page + n * 24);
	at = (meta + 127) & ~127u;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t len = args[i].length;

		list[2 * i + 1] = i;
		rpra[2 * i + 1] = len;
		if (len == 0) {
			continue;
		}
		at = (at + 127) & ~127u;
		if (at + len > FRPC_HALF) {
			return E2BIG;
		}
		offsets[i] = at;
		list[2 * i] = 1;
		rpra[2 * i] = dsp_address(page + at);
		pages[2 * i] = dsp_address(page + (at & ~4095u));
		pages[2 * i + 1] = (uint64_t)(((at + len - 1) >> 12) - (at >> 12) + 1) * 4096;
		if (i < SC_IN(sc)) {
			memcpy(page + at, args[i].ptr, len);
		} else {
			bzero(page + at, len);
		}
		at += len;
	}
	FlushPoC_DcacheRegion((vm_offset_t)page, at);
	/*
	 * A context of its own: never one a call still waiting holds (the other listener's next2 waits for as long as its DSP
	 * process has nothing to ask -- and after 255 calls the numbers came round to it, and the answer went to the wrong
	 * listener, which then waited for ever: 2026-11-07, the registry stopped after its 257th call). With the PD in its
	 * low bits, as the DSP gives it back.
	 */
	lck_mtx_lock(&frpc_lock);
	for (bool taken = true; taken;) {
		next_ctx = next_ctx >= 255 ? 1 : next_ctx + 1;
		taken = false;
		for (int k = 0; k < FRPC_CALL_SLOTS; k++) {
			taken = taken || (k != slot && !slots[k].answered && (slots[k].ctx >> 4) == next_ctx);
		}
	}
	slots[slot].ctx = (uint64_t)next_ctx << 4 | session->pd;
	slots[slot].answered = false;
	lck_mtx_unlock(&frpc_lock);
	msg.pid = kernel ? 0 : session->tgid;
	msg.tid = session->tgid + 1 + (slot & 1);
	msg.ctx = slots[slot].ctx;          /* (its low bits the PD: 0 root, 2 sensors) */
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = dsp_address(page);
	msg.size = ((uint64_t)at + 4095) & ~4095ull;
	if (hk_slpi_fastrpc_send(&msg, sizeof(msg)) != 0) {
		return EIO;
	}
	for (uint32_t waited = 0; !slots[slot].answered; waited++) {
		if (leaving || (wait_ms != 0 && waited >= wait_ms)) {
			mmu_faults("an unanswered call");
			slots[slot].ctx = ~0ull;
			return ETIMEDOUT;
		}
		delay_for_interval(waited < 50 ? 100 : 1000, NSEC_PER_USEC);
		if (waited >= 50) {
			waited += 0;
		}
	}
	FlushPoC_DcacheRegion((vm_offset_t)page, at);
	for (uint32_t i = SC_IN(sc); i < n; i++) {
		if (args[i].length != 0) {
			memcpy(args[i].ptr, page + offsets[i], args[i].length);
		}
	}
	*result = slots[slot].result;
	return 0;
}

/* ---- the files: apps_std on our vnodes ------------------------------------------------------------------ */

#define FRPC_FILES              32
#define FRPC_DIRS               8
#define FRPC_NAMES              2048
#define WRITABLE_ROOT           "/private/var/hackios/sensors/"
#define IMAGE_ROOT              "/hackios/sensors/"

static struct {
	vnode_t vp;
	off_t pos;
	bool eof, writes;
	char path[200];
} files[FRPC_FILES];
static struct {
	bool used;
	uint32_t count, next;
	char (*names)[256];      /* FRPC_NAMES of them */
} dirs[FRPC_DIRS];
static uint32_t opens, reads, writes, misses;

/* `needle` somewhere in `text` (the kernel's libc has no strstr) */
static bool
has(const char *text, const char *needle)
{
	size_t n = strlen(needle);

	for (; *text; text++) {
		if (strncmp(text, needle, n) == 0) {
			return true;
		}
	}
	return false;
}

/* "a/b/../c" as "a/c", "./" dropped, in place; false if it climbs out of its root */
static bool
normalize(char *path)
{
	char out[256];
	size_t o = 0;
	const char *p = path;

	while (*p) {
		const char *slash = p;
		size_t n;

		while (*slash && *slash != '/') {
			slash++;
		}
		n = (size_t)(slash - p);
		if (n == 2 && p[0] == '.' && p[1] == '.') {
			if (o <= 1) {
				return false;
			}
			o--;                     /* (the slash before the last component) */
			while (o > 0 && out[o - 1] != '/') {
				o--;
			}
		} else if (!(n == 1 && p[0] == '.') && !(n == 0 && o > 0)) {
			if (o + n + 1 >= sizeof(out)) {
				return false;
			}
			memcpy(out + o, p, n);
			o += n;
			if (*slash) {
				out[o++] = '/';
			}
		} else if (n == 0 && o == 0) {
			out[o++] = '/';
		}
		p = *slash ? slash + 1 : slash;
	}
	if (o > 1 && out[o - 1] == '/') {
		o--;
	}
	out[o] = 0;
	strlcpy(path, out, 256);
	return true;
}

static uint32_t failed_count;
static char failed[8][160];

static void
note_failure(const char *what, const char *path)
{
	snprintf(failed[failed_count++ % 8], sizeof(failed[0]), "%s %s", what, path);
}

/* The DSP's path as ours, relative to both roots ("persist/sensors/registry/..."); false for what is not served. */
static bool
map_path(const char *dsp_in, char *rel, size_t size)
{
	char dsp[256];

	strlcpy(dsp, dsp_in, sizeof(dsp));
	if (!normalize(dsp)) {
		return false;
	}
	static const char *const prefixes[][2] = {
		{ "/mnt/vendor/persist/", "persist/" }, { "/persist/", "persist/" }, { "/vendor/", "vendor/" },
		{ "/odm/", "odm/" }, { "/data/vendor/sensors/", "data/" }, { "/mnt/vendor/", "mnt/" },
	};

	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i][0]);

		if (strncmp(dsp, prefixes[i][0], n) == 0 && !has(dsp, "..")) {
			snprintf(rel, size, "%s%s", prefixes[i][1], dsp + n);
			return true;
		}
		/* (and the directory itself, without its slash) */
		if (strncmp(dsp, prefixes[i][0], n - 1) == 0 && dsp[n - 1] == 0) {
			snprintf(rel, size, "%.*s", (int)strlen(prefixes[i][1]) - 1, prefixes[i][1]);
			return true;
		}
	}
	return false;
}

/* one directory, `leaf` in `parent` (as hk_apfs_check.c makes its own); 0 if it is there afterwards */
static int
make_dir(const char *parent, const char *leaf)
{
	vfs_context_t ctx = vfs_context_kernel();
	struct componentname cn;
	struct vnode_attr va;
	vnode_t dvp = NULLVP, vp = NULLVP;
	char name[256];
	int error;

	strlcpy(name, leaf, sizeof(name));
	error = vnode_lookup(parent, 0, &dvp, ctx);
	if (error) {
		return error;
	}
	bzero(&cn, sizeof(cn));
	cn.cn_nameiop = CREATE;
	cn.cn_flags = ISLASTCN;
	cn.cn_context = ctx;
	cn.cn_pnbuf = name;
	cn.cn_pnlen = (int)sizeof(name);
	cn.cn_nameptr = name;
	cn.cn_namelen = (int)strlen(name);
	VATTR_INIT(&va);
	VATTR_SET(&va, va_type, VDIR);
	VATTR_SET(&va, va_mode, 0755);
	error = VNOP_MKDIR(dvp, &vp, &cn, &va, ctx);
	if (error == 0 && vp != NULLVP) {
		vnode_put(vp);
	}
	vnode_put(dvp);
	return error == EEXIST ? 0 : error;
}

/* every directory of `path` up to (not with) its last component, made where missing: mkdir -p of its dirname */
static int
make_dirs(const char *path, bool with_last)
{
	char parent[260] = "/";
	const char *p = path + 1;
	int error = 0;

	while (*p) {
		const char *slash = p;
		char leaf[256];
		size_t n;

		while (*slash && *slash != '/') {
			slash++;
		}
		if (*slash == 0 && !with_last) {
			break;
		}
		n = (size_t)(slash - p);
		if (n == 0 || n >= sizeof(leaf)) {
			return EINVAL;
		}
		memcpy(leaf, p, n);
		leaf[n] = 0;
		{
			vnode_t vp = NULLVP;
			char full[260];

			snprintf(full, sizeof(full), "%s%s%s", parent, strcmp(parent, "/") == 0 ? "" : "/", leaf);
			if (vnode_lookup(full, 0, &vp, vfs_context_kernel()) == 0) {
				vnode_put(vp);
			} else if ((error = make_dir(parent, leaf)) != 0) {
				return error;
			}
			strlcpy(parent, full, sizeof(parent));
		}
		if (*slash == 0) {
			break;
		}
		p = slash + 1;
	}
	return 0;
}

static int
lookup_either(const char *rel, vnode_t *vp, char *full, size_t size)
{
	vfs_context_t ctx = vfs_context_kernel();

	snprintf(full, size, WRITABLE_ROOT "%s", rel);
	if (vnode_lookup(full, 0, vp, ctx) == 0) {
		return 0;
	}
	snprintf(full, size, IMAGE_ROOT "%s", rel);
	return vnode_lookup(full, 0, vp, ctx);
}

/*
 * (2026-11-13) For finding why SEE registers no magnetometer: every name the DSP opens (kern.hackios.fastrpc_opens) and its
 * own log lines (slpi_dsplog=1; kern.hackios.dsplog: those that speak of a magnetometer or a failure, then the last ones), kept
 * whole here because they fall in the middle of the boot's log, which the loader does not keep. Read with hk_sysctl.
 */
static char trace_opens[12288], trace_dsplog[12288], trace_dsplast[4096];
static uint32_t trace_opens_n, trace_dsplog_n, trace_dsplast_at;

static bool
trace_has_ci(const char *s, uint32_t length, const char *word)
{
	size_t w = strlen(word);

	for (uint32_t i = 0; i + w <= length; i++) {
		size_t j;

		for (j = 0; j < w; j++) {
			char c = s[i + j];

			if (c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			if (c != word[j]) {
				break;
			}
		}
		if (j == w) {
			return true;
		}
	}
	return false;
}

static void
trace_append(char *buffer, size_t size, uint32_t *used, const char *text, uint32_t length)
{
	if (*used + length + 2 >= size) {
		return;
	}
	memcpy(buffer + *used, text, length);
	*used += length;
	buffer[(*used)++] = '\n';
	buffer[*used] = 0;
}

static void
trace_dsp_line(const char *text, uint32_t length)
{
	static const char *const words[] = { "mag", "mmc", "ak09", "akm", "fail", "err", "probe", "not found", "absent" };

	while (length > 0 && (text[length - 1] == 0 || text[length - 1] == '\n')) {
		length--;
	}
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (trace_has_ci(text, length, words[i])) {
			trace_append(trace_dsplog, sizeof(trace_dsplog), &trace_dsplog_n, text, length > 300 ? 300 : length);
			break;
		}
	}
	/* the last lines, a ring of whole lines */
	if (length > 200) {
		length = 200;
	}
	if (trace_dsplast_at + length + 2 >= sizeof(trace_dsplast)) {
		trace_dsplast_at = 0;
	}
	memcpy(trace_dsplast + trace_dsplast_at, text, length);
	trace_dsplast_at += length;
	trace_dsplast[trace_dsplast_at++] = '\n';
	trace_dsplast[trace_dsplast_at] = 0;
}

static int
trace_sysctl(__unused struct sysctl_oid *oidp, void *arg1, __unused int arg2, struct sysctl_req *req)
{
	const char *text = arg1;

	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

static int
std_fopen(const char *name, const char *mode, int32_t *out)
{
	vfs_context_t ctx = vfs_context_kernel();
	char rel[200], full[260];
	bool wants_write = mode[0] == 'w' || mode[0] == 'a' || has(mode, "+");
	int fmode = wants_write ? FWRITE | FREAD : FREAD, error;
	vnode_t vp = NULLVP;
	int slot;

	opens++;
	{
		const char *shown = name, *last = NULL;

		for (const char *c = name; *c; c++) {
			if (*c == '/') {
				last = c;
			}
		}
		if (last != NULL && trace_has_ci(name, (uint32_t)strlen(name), "/registry/")) {
			shown = last + 1;
		}
		trace_append(trace_opens, sizeof(trace_opens), &trace_opens_n, shown, (uint32_t)strnlen(shown, 120));
	}
	if (!map_path(name, rel, sizeof(rel))) {
		note_failure("not served", name);
		if (misses++ < 40) {
			say("frpc: fopen \"%s\" \"%s\": not a path of the sensors\n", name, mode);
		}
		return AEE_EFILE;
	}
	for (slot = 0; slot < FRPC_FILES && files[slot].vp != NULLVP; slot++) {
	}
	if (slot == FRPC_FILES) {
		return AEE_EFILE;
	}
	if (wants_write) {
		snprintf(full, sizeof(full), WRITABLE_ROOT "%s", rel);
		make_dirs(full, false);         /* (init makes the tree only on a new data volume: made here when missing) */
		if (mode[0] == 'w') {
			fmode |= O_CREAT | O_TRUNC;
		} else if (mode[0] == 'a') {
			fmode |= O_CREAT | O_APPEND;
		}
		error = vnode_open(full, fmode, 0644, 0, &vp, ctx);
		if (error == ENOENT && mode[0] == 'r') {
			/* r+ of a file only the image has: written as ours from now on */
			error = EROFS;
		}
	} else {
		vnode_t probe = NULLVP;

		error = lookup_either(rel, &probe, full, sizeof(full));
		if (error == 0) {
			vnode_put(probe);
			error = vnode_open(full, fmode, 0, 0, &vp, ctx);
		}
	}
	if (error != 0) {
		note_failure(wants_write ? "write" : "read", name);
		if (misses++ < 40) {
			say("frpc: fopen \"%s\" \"%s\": %s: error %d\n", name, mode, full, error);
		}
		return AEE_EFILE;
	}
	files[slot].vp = vp;
	files[slot].pos = mode[0] == 'a' ? 0 : 0;
	files[slot].eof = false;
	files[slot].writes = wants_write;
	strlcpy(files[slot].path, full, sizeof(files[slot].path));
	*out = slot + 3;
	if (opens <= 30) {
		say("frpc: fopen \"%s\" \"%s\" = %d (%s)\n", name, mode, slot + 3, full);
	}
	return 0;
}

static int
file_of(int32_t f)
{
	return f >= 3 && f < FRPC_FILES + 3 && files[f - 3].vp != NULLVP ? f - 3 : -1;
}

static off_t
file_size(vnode_t vp)
{
	struct vnode_attr va;

	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	return vnode_getattr(vp, &va, vfs_context_kernel()) == 0 ? (off_t)va.va_data_size : 0;
}

static int
std_fclose(int32_t f)
{
	int i = file_of(f);

	if (i < 0) {
		return AEE_EBADPARM;
	}
	vnode_close(files[i].vp, files[i].writes ? FWRITE | FREAD : FREAD, vfs_context_kernel());
	files[i].vp = NULLVP;
	return 0;
}

static int
std_fread(int32_t f, uint8_t *buf, uint32_t len, int32_t *got, int32_t *eof)
{
	int i = file_of(f), resid = 0, error;
	vfs_context_t ctx = vfs_context_kernel();

	if (i < 0) {
		return AEE_EBADPARM;
	}
	reads++;
	error = vn_rdwr(UIO_READ, files[i].vp, (caddr_t)buf, (int)len, files[i].pos, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
	    vfs_context_proc(ctx));
	if (error != 0) {
		return AEE_EFILE;
	}
	*got = (int32_t)(len - (uint32_t)resid);
	files[i].pos += *got;
	files[i].eof = *got < (int32_t)len;
	*eof = files[i].eof;
	return 0;
}

static int
std_fwrite(int32_t f, const uint8_t *buf, uint32_t len, int32_t *put, int32_t *err)
{
	int i = file_of(f), resid = 0, error;
	vfs_context_t ctx = vfs_context_kernel();

	if (i < 0 || !files[i].writes) {
		return AEE_EBADPARM;
	}
	writes++;
	error = vn_rdwr(UIO_WRITE, files[i].vp, (caddr_t)(uintptr_t)buf, (int)len, files[i].pos, UIO_SYSSPACE, IO_UNIT,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	*put = (int32_t)(len - (uint32_t)resid);
	*err = error != 0;
	files[i].pos += *put;
	if (writes <= 10) {
		say("frpc: fwrite %u bytes to %s: %d\n", len, files[i].path, error);
	}
	return error != 0 ? AEE_EFILE : 0;
}

static int
std_exists(const char *name, bool *exists)
{
	char rel[200], full[260];
	vnode_t vp = NULLVP;

	*exists = map_path(name, rel, sizeof(rel)) && lookup_either(rel, &vp, full, sizeof(full)) == 0;
	if (vp != NULLVP) {
		vnode_put(vp);
	}
	return 0;
}

/*
 * frename(old, new): SEE writes a registry file under another name and renames it into place. No rename here that takes a
 * kernel path, and the files are a few KB: the old one copied to the new name in our tree, then the old one removed.
 */
static int
std_frename(const char *from, const char *to)
{
	vfs_context_t ctx = vfs_context_kernel();
	char rel_from[200], rel_to[200], full_from[260], full_to[260];
	vnode_t in = NULLVP, out = NULLVP, probe = NULLVP;
	uint8_t *data = NULL;
	off_t size;
	int resid = 0, error;

	if (!map_path(from, rel_from, sizeof(rel_from)) || !map_path(to, rel_to, sizeof(rel_to)) ||
	    lookup_either(rel_from, &probe, full_from, sizeof(full_from)) != 0) {
		note_failure("rename", from);
		return AEE_EFILE;
	}
	vnode_put(probe);
	if ((error = vnode_open(full_from, FREAD, 0, 0, &in, ctx)) != 0) {
		return AEE_EFILE;
	}
	size = file_size(in);
	if (size < 0 || size > (1 << 20) || (data = kalloc_data((vm_size_t)size + 1, Z_WAITOK)) == NULL) {
		vnode_close(in, FREAD, ctx);
		return AEE_EFILE;
	}
	error = vn_rdwr(UIO_READ, in, (caddr_t)data, (int)size, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_close(in, FREAD, ctx);
	snprintf(full_to, sizeof(full_to), WRITABLE_ROOT "%s", rel_to);
	make_dirs(full_to, false);
	if (error == 0 && (error = vnode_open(full_to, FWRITE | O_CREAT | O_TRUNC, 0644, 0, &out, ctx)) == 0) {
		error = vn_rdwr(UIO_WRITE, out, (caddr_t)data, (int)size, 0, UIO_SYSSPACE, IO_UNIT, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		vnode_close(out, FWRITE, ctx);
	}
	kfree_data(data, (vm_size_t)size + 1);
	if (error != 0) {
		note_failure("rename", to);
		return AEE_EFILE;
	}
	if (strncmp(full_from, WRITABLE_ROOT, strlen(WRITABLE_ROOT)) == 0) {
		unlink1(ctx, NULLVP, CAST_USER_ADDR_T(full_from), UIO_SYSSPACE, 0);
	}
	return 0;
}

/* stat: { tsz, dev, ino, mode, nlink, rdev, size, atime, atimensec, mtime, mtimensec, ctime, ctimensec } */
static int
std_stat(const char *name, uint8_t out[96])
{
	char rel[200], full[260];
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	uint64_t size;
	uint32_t mode, nlink = 1;
	int64_t t;

	bzero(out, 96);
	if (!map_path(name, rel, sizeof(rel)) || lookup_either(rel, &vp, full, sizeof(full)) != 0) {
		return AEE_EFILE;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_modify_time);
	vnode_getattr(vp, &va, vfs_context_kernel());
	mode = (vnode_isdir(vp) ? S_IFDIR : S_IFREG) | (va.va_mode & 07777);
	vnode_put(vp);
	size = va.va_data_size;
	t = va.va_modify_time.tv_sec;
	memcpy(out + 24, &mode, 4);
	memcpy(out + 28, &nlink, 4);
	memcpy(out + 40, &size, 8);
	memcpy(out + 48, &t, 8);
	memcpy(out + 64, &t, 8);
	memcpy(out + 80, &t, 8);
	return 0;
}

/* a directory's names, both roots' (ours first), into a table of ours */
static void
dir_collect(const char *full, int d)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	char buffer[2048];

	if (vnode_lookup(full, 0, &vp, ctx) != 0) {
		return;
	}
	for (off_t offset = 0;;) {
		uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
		int eof = 0, count = 0, error;

		if (uio == NULL) {
			break;
		}
		uio_addiov(uio, CAST_USER_ADDR_T(buffer), sizeof(buffer));
		error = VNOP_READDIR(vp, uio, 0, &eof, &count, ctx);
		offset = uio_offset(uio);
		{
			size_t used = sizeof(buffer) - (size_t)uio_resid(uio);

			for (size_t at = 0; error == 0 && at + 8 <= used;) {
				struct dirent *e = (struct dirent *)(buffer + at);

				if (e->d_reclen == 0) {
					break;
				}
				if (e->d_namlen > 0 && strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
					bool known = false;

					for (uint32_t k = 0; k < dirs[d].count && !known; k++) {
						known = strcmp(dirs[d].names[k], e->d_name) == 0;
					}
					if (!known && dirs[d].count < FRPC_NAMES) {
						strlcpy(dirs[d].names[dirs[d].count++], e->d_name, 256);
					}
				}
				at += e->d_reclen;
			}
		}
		uio_free(uio);
		if (error != 0 || eof || count == 0) {
			break;
		}
	}
	vnode_put(vp);
}

static int
std_opendir(const char *name, uint64_t *out)
{
	char rel[200], full[260];
	int d;

	if (!map_path(name, rel, sizeof(rel))) {
		return AEE_EFILE;
	}
	for (d = 0; d < FRPC_DIRS && dirs[d].used; d++) {
	}
	if (d == FRPC_DIRS) {
		return AEE_EFILE;
	}
	if (dirs[d].names == NULL) {
		dirs[d].names = kalloc_data(FRPC_NAMES * 256, Z_WAITOK | Z_ZERO);
		if (dirs[d].names == NULL) {
			return AEE_EFILE;
		}
	}
	dirs[d].count = dirs[d].next = 0;
	snprintf(full, sizeof(full), WRITABLE_ROOT "%s", rel);
	dir_collect(full, d);
	snprintf(full, sizeof(full), IMAGE_ROOT "%s", rel);
	dir_collect(full, d);
	dirs[d].used = true;
	*out = (uint64_t)d + 1;
	say("frpc: opendir \"%s\": %u names\n", name, dirs[d].count);
	return 0;
}

/* readdir: DIRENT { int ino; char name[255]; } (260 bytes, padded) then eof */
static int
std_readdir(uint64_t dir, uint8_t out[264])
{
	int d = (int)dir - 1;
	int32_t ino, eof;

	bzero(out, 264);
	if (d < 0 || d >= FRPC_DIRS || !dirs[d].used) {
		return AEE_EBADPARM;
	}
	if (dirs[d].next >= dirs[d].count) {
		eof = 1;
		memcpy(out + 260, &eof, 4);
		return 0;
	}
	ino = (int32_t)dirs[d].next + 1;
	memcpy(out, &ino, 4);
	strlcpy((char *)out + 4, dirs[d].names[dirs[d].next++], 255);
	return 0;
}

/* ---- the reverse calls: unpack, serve, pack ------------------------------------------------------------- */

#define FRPC_APPS_STD           0x100
#define FRPC_APPS_MEM           0x101
#define FRPC_APPS_MSG           0x102
#define IN_CAPACITY             (64 * 1024)

static uint32_t reverse_calls, unsupported;

static uint32_t
u32_at(const struct frpc_arg *a, uint32_t offset)
{
	uint32_t v = 0;

	if (a->length >= offset + 4) {
		memcpy(&v, (uint8_t *)a->ptr + offset, 4);
	}
	return v;
}

/* a string argument: NUL-terminated within its buffer, or "" */
static const char *
string_of(const struct frpc_arg *a)
{
	if (a->length == 0 || ((const char *)a->ptr)[a->length - 1] != 0) {
		return "";
	}
	return (const char *)a->ptr;
}

static int32_t apps_mem(struct frpc_session *se, uint32_t method, uint32_t nin, uint32_t nout, struct frpc_arg *in,
    struct frpc_arg *out);

static int32_t
serve(struct frpc_session *se, uint32_t handle, uint32_t sc, struct frpc_arg *in, struct frpc_arg *out)
{
	uint32_t method = SC_METHOD(sc), nin = SC_IN(sc), nout = SC_OUT(sc);

	reverse_calls++;
	if (handle == 0) {
		/* apps_remotectl: 0 open(name; handle, dlerror, nErr), 1 close(handle; error, nErr) */
		if (method == 0 && nin == 2 && nout == 2 && out[0].length >= 8) {
			const char *name = string_of(&in[1]);
			uint32_t h = 0, err = 0;

			h = strcmp(name, "apps_std") == 0 ? FRPC_APPS_STD : strcmp(name, "apps_mem") == 0 ? FRPC_APPS_MEM :
			    strcmp(name, "adspmsgd_apps") == 0 ? FRPC_APPS_MSG : 0;
			if (h == 0) {
				err = AEE_ENOSUCHMOD;
			}
			memcpy(out[0].ptr, &h, 4);
			memcpy((uint8_t *)out[0].ptr + 4, &err, 4);
			say("frpc: the DSP opens \"%s\": %s\n", name, h ? "served" : "NOT SERVED");
			return 0;
		}
		if (method == 1) {
			return 0;
		}
		return AEE_EUNSUPPORTED;
	}
	if (handle == FRPC_APPS_MSG) {
		/* adspmsgd_apps: what the DSP logs; said as it comes (a string is all a message has that matters here) */
		for (uint32_t i = 1; i < nin; i++) {
			if (in[i].length > 1 && reverse_calls < 2000) {
				say("frpc: dsp: %.*s\n", (int)in[i].length, (const char *)in[i].ptr);
			}
			if (in[i].length > 1) {
				trace_dsp_line((const char *)in[i].ptr, in[i].length);
			}
		}
		return 0;
	}
	if (handle == FRPC_APPS_MEM) {
		return apps_mem(se, method, nin, nout, in, out);
	}
	if (handle != FRPC_APPS_STD) {
		if (unsupported++ < 30) {
			say("frpc: a call to handle %#x, method %u (%u in, %u out): not served\n", handle, method, nin, nout);
		}
		return AEE_EINVHANDLE;
	}
	if (method == 31 && nin >= 1) {
		method = u32_at(&in[0], 0);
	}
	switch (method) {
	case 0:         /* fopen(name, mode; FILE) */
		if (nin == 3 && nout == 1 && out[0].length >= 4) {
			int32_t f = 0;
			int r = std_fopen(string_of(&in[1]), string_of(&in[2]), &f);

			memcpy(out[0].ptr, &f, 4);
			return r;
		}
		break;
	case 2: case 11: case 14: case 23:      /* fflush, rewind, clearerr, fsync */
		if (method == 11) {
			int i = file_of((int32_t)u32_at(&in[0], 0));

			if (i >= 0) {
				files[i].pos = 0;
				files[i].eof = false;
			}
		}
		return 0;
	case 3:         /* fclose */
		return std_fclose((int32_t)u32_at(&in[0], 0));
	case 4:         /* fread(FILE, len; buf; got, eof) */
		if (nin == 1 && nout == 2 && out[0].length >= 8) {
			int32_t got = 0, eof = 0;
			uint32_t len = MIN(u32_at(&in[0], 4), out[1].length);
			int r = std_fread((int32_t)u32_at(&in[0], 0), out[1].ptr, len, &got, &eof);

			memcpy(out[0].ptr, &got, 4);
			memcpy((uint8_t *)out[0].ptr + 4, &eof, 4);
			return r;
		}
		break;
	case 5:         /* fwrite(FILE, len, buf; put, error) */
		if (nin == 2 && nout == 1 && out[0].length >= 8) {
			int32_t put = 0, err = 0;
			int r = std_fwrite((int32_t)u32_at(&in[0], 0), in[1].ptr, MIN(u32_at(&in[0], 4), in[1].length), &put, &err);

			memcpy(out[0].ptr, &put, 4);
			memcpy((uint8_t *)out[0].ptr + 4, &err, 4);
			return r;
		}
		break;
	case 8: case 12: case 13: {     /* ftell, feof, ferror (FILE; int) */
		int i = file_of((int32_t)u32_at(&in[0], 0));
		int32_t v = 0;

		if (i < 0 || nout < 1 || out[0].length < 4) {
			return AEE_EBADPARM;
		}
		v = method == 8 ? (int32_t)files[i].pos : method == 12 ? files[i].eof : 0;
		memcpy(out[0].ptr, &v, 4);
		return 0;
	}
	case 9: {       /* fseek(FILE, offset, whence) */
		int i = file_of((int32_t)u32_at(&in[0], 0));
		int32_t offset = (int32_t)u32_at(&in[0], 4);
		uint32_t whence = u32_at(&in[0], 8);

		if (i < 0) {
			return AEE_EBADPARM;
		}
		files[i].pos = whence == 0 ? offset : whence == 1 ? files[i].pos + offset : file_size(files[i].vp) + offset;
		files[i].eof = false;
		return 0;
	}
	case 10: {      /* flen(FILE; uint64) */
		int i = file_of((int32_t)u32_at(&in[0], 0));
		uint64_t len;

		if (i < 0 || nout < 1 || out[0].length < 8) {
			return AEE_EBADPARM;
		}
		len = (uint64_t)file_size(files[i].vp);
		memcpy(out[0].ptr, &len, 8);
		return 0;
	}
	case 15:        /* print_string */
		if (nin == 2) {
			say("frpc: dsp says: %s\n", string_of(&in[1]));
		}
		return 0;
	case 16:        /* getenv: nothing is set here */
		return AEE_EFAILED;
	case 19:        /* fopen_with_env(env, delim, name, mode; FILE): the name as it is */
		if (nin == 5 && nout == 1 && out[0].length >= 4) {
			int32_t f = 0;
			int r = std_fopen(string_of(&in[3]), string_of(&in[4]), &f);

			memcpy(out[0].ptr, &f, 4);
			return r;
		}
		break;
	case 22:        /* fileExists(path; bool) */
		if (nin == 2 && nout == 1 && out[0].length >= 1) {
			bool exists = false;

			std_exists(string_of(&in[1]), &exists);
			*(uint8_t *)out[0].ptr = exists;
			return 0;
		}
		break;
	case 24: {      /* fremove(path): ours only */
		char rel[200], full[260];

		if (nin != 2 || !map_path(string_of(&in[1]), rel, sizeof(rel))) {
			return AEE_EFILE;
		}
		snprintf(full, sizeof(full), WRITABLE_ROOT "%s", rel);
		unlink1(vfs_context_kernel(), NULLVP, CAST_USER_ADDR_T(full), UIO_SYSSPACE, 0);
		return 0;
	}
	case 26:        /* opendir(path; DIR) */
		if (nin == 2 && nout == 1 && out[0].length >= 8) {
			uint64_t d = 0;
			int r = std_opendir(string_of(&in[1]), &d);

			memcpy(out[0].ptr, &d, 8);
			return r;
		}
		break;
	case 27: {      /* closedir(DIR) */
		uint64_t d = (uint64_t)u32_at(&in[0], 0) | (uint64_t)u32_at(&in[0], 4) << 32;

		if (d >= 1 && d <= FRPC_DIRS) {
			dirs[d - 1].used = false;
		}
		return 0;
	}
	case 28:        /* readdir(DIR; DIRENT, eof) */
		if (nin == 1 && nout == 1 && out[0].length >= 264) {
			uint64_t d = (uint64_t)u32_at(&in[0], 0) | (uint64_t)u32_at(&in[0], 4) << 32;

			return std_readdir(d, out[0].ptr);
		}
		break;
	case 29: {      /* mkdir(path, mode): in our tree */
		char rel[200], full[260];

		if (nin != 2 || !map_path(string_of(&in[1]), rel, sizeof(rel))) {
			return AEE_EFILE;
		}
		snprintf(full, sizeof(full), WRITABLE_ROOT "%s", rel);
		return make_dirs(full, true) == 0 ? 0 : AEE_EFILE;
	}
	case 30:        /* rmdir */
		return 0;
	case 31 + 0:    /* (never: 31 is the escape) */
		break;
	default:
		if (method == 31 + 0) {
			break;
		}
		break;
	}
	if (method == 33 && nin == 3) {
		/* frename(mid, len, len, old, new) */
		return std_frename(string_of(&in[1]), string_of(&in[2]));
	}
	if (method == 31 || (SC_METHOD(sc) == 31 && method == 31)) {
		/* stat(mid, len, path; STAT) */
		if (nin == 2 && nout == 1 && out[0].length >= 96) {
			return std_stat(string_of(&in[1]), out[0].ptr);
		}
	}
	if (unsupported++ < 40) {
		say("frpc: apps_std method %u (%u in, %u out): not served\n", method, nin, nout);
	}
	return AEE_EUNSUPPORTED;
}

/*
 * (2026-11-13) apps_mem (quic/fastrpc apps_mem_imp.c / apps_mem_skel.c): memory the DSP asks the AP for. Only request_map64
 * (method 2: {heapid, lflags, rflags, pad, vin u64, len u64}; {vapps u64, vadsp u64}) for one buffer a session, the one
 * adspmsgd's init3 wants for the DSP's log (256 KB): slot FRPC_LOGBUF of the pool, already in the SMMU, mapped into the DSP
 * as Linux fastrpc_req_mmap does -- a kernel call on handle 1, INIT_MMAP (method 4): {client_id, flags, vaddr, num} and one
 * physical page {addr, size} in, {vaddr} out. The call goes on the session's FRPC_NESTED slot: its other slot is the one
 * waiting for init3. vapps is our kernel address of it (adspmsgd's reader finds the ring there), vadsp the DSP's.
 * request_unmap64 (3): said, nothing undone. The rest: not supported.
 */
static struct {
	uint8_t *buffer;
	uint32_t size, read_at;
	uint64_t vadsp;
} logbufs[2];

static int32_t
apps_mem(struct frpc_session *se, uint32_t method, uint32_t nin, uint32_t nout, struct frpc_arg *in, struct frpc_arg *out)
{
	int k = se->slot / 2;

	if (method == 2 && nin == 1 && nout == 1 && in[0].length >= 32 && out[0].length >= 16) {
		int32_t heapid = (int32_t)u32_at(&in[0], 0), result = -1;
		uint32_t lflags = u32_at(&in[0], 4), rflags = u32_at(&in[0], 8);
		uint64_t vin, len, vapps, rsp = 0;
		struct { int32_t client_id; uint32_t flags; uint64_t vaddr; int32_t num, pad; } req;
		struct { uint64_t addr, size; } page;
		struct frpc_arg a[3];
		int error;

		memcpy(&vin, (const uint8_t *)in[0].ptr + 16, 8);
		memcpy(&len, (const uint8_t *)in[0].ptr + 24, 8);
		say("frpc: %s: apps_mem request_map64(heap %d, lflags %#x, rflags %#x, vin %#llx, %llu bytes)\n", se->name, heapid, lflags,
		    rflags, vin, len);
		if (logbufs[k].buffer != NULL || len == 0 || len > FRPC_HALF) {
			return AEE_ENOMEMORY;
		}
		logbufs[k].buffer = (uint8_t *)(pool + (vm_offset_t)FRPC_LOGBUF(se) * FRPC_HALF);
		logbufs[k].size = (uint32_t)len;
		bzero(logbufs[k].buffer, FRPC_HALF);
		FlushPoC_DcacheRegion((vm_offset_t)logbufs[k].buffer, FRPC_HALF);
		req = (typeof(req)){ .client_id = se->tgid, .flags = rflags, .vaddr = 0, .num = sizeof(page) };
		page.addr = dsp_address(logbufs[k].buffer);
		page.size = (len + 4095) & ~4095ull;
		a[0] = (struct frpc_arg){ &req, 24 };
		a[1] = (struct frpc_arg){ &page, sizeof(page) };
		a[2] = (struct frpc_arg){ &rsp, 8 };
		error = frpc_call(se, FRPC_NESTED(se), true, 1, SC(4, 2, 1), a, &result, 10000);
		say("frpc: %s: INIT_MMAP of %llu bytes at %#llx: %d, result %#x, DSP address %#llx\n", se->name, page.size, page.addr, error,
		    result, rsp);
		if (error != 0 || result != 0) {
			logbufs[k].buffer = NULL;
			return error != 0 ? AEE_EFAILED : result;
		}
		logbufs[k].vadsp = rsp;
		vapps = (uint64_t)(uintptr_t)logbufs[k].buffer;
		memcpy(out[0].ptr, &vapps, 8);
		memcpy((uint8_t *)out[0].ptr + 8, &rsp, 8);
		return 0;
	}
	if (method == 3) {
		say("frpc: %s: apps_mem request_unmap64: left mapped\n", se->name);
		return 0;
	}
	say("frpc: %s: apps_mem method %u (%u in, %u out): not served\n", se->name, method, nin, nout);
	return AEE_EUNSUPPORTED;
}

/*
 * adspmsgd's ring (quic/fastrpc adspmsgd.c): the buffer's last 4 bytes are the DSP's write position, the rest NUL-ended
 * messages going round. Read every 100 ms, each message to trace_dsp_line() (kern.hackios.dsplog) and, the first 400, said.
 */
static void
logbuf_thread(void *p, __unused wait_result_t w)
{
	struct frpc_session *se = p;
	int k = se->slot / 2;
	char message[300];
	unsigned said = 0;

	for (;;) {
		uint8_t *b = logbufs[k].buffer;
		uint32_t ring, at, write;

		delay_for_interval(100, NSEC_PER_MSEC);
		if (b == NULL || leaving) {
			if (leaving) {
				break;
			}
			continue;
		}
		ring = logbufs[k].size - 4;
		FlushPoC_DcacheRegion((vm_offset_t)b, logbufs[k].size);
		memcpy(&write, b + ring, 4);
		if (write >= ring) {
			continue;
		}
		at = logbufs[k].read_at;
		while (at != write) {
			uint32_t n = 0;

			while (at != write && b[at] == 0) {
				at = (at + 1) % ring;
			}
			while (at != write && b[at] != 0 && n < sizeof(message) - 1) {
				message[n++] = (char)b[at];
				at = (at + 1) % ring;
			}
			message[n] = 0;
			if (n > 0) {
				trace_dsp_line(message, n);
				if (said++ < 400) {
					say("frpc: dsp %s: %s\n", se->pd ? "sns" : "root", message);
				}
			}
		}
		logbufs[k].read_at = at;
	}
	thread_terminate(current_thread());
}

static bool
listener_init(struct frpc_session *se)
{
	int32_t result = 0;
	int32_t tgid = se->tgid;
	struct frpc_arg a[4];
	int error;

	/* attach: handle 1, INIT_ATTACH, our process id, as the kernel (pid 0 in the message) */
	a[0] = (struct frpc_arg){ &tgid, 4 };
	error = frpc_call(se, se->slot + 1, true, 1, SC(0, 1, 0), a, &result, 5000);
	say("frpc: attach to %s (pd %u): %d, result %d\n", se->name, se->pd, error, result);
	if (error != 0 || result != 0) {
		return false;
	}
	/* the listener's init2: handle 3, method 3, nothing */
	error = frpc_call(se, se->slot + 1, false, 3, SC(3, 0, 0), a, &result, 5000);
	say("frpc: %s: adsp_listener_init2: %d, result %d\n", se->name, error, result);
	return error == 0 && result == 0;
}

static void
listener_thread(void *p, __unused wait_result_t w)
{
	struct frpc_session *se = p;
	uint8_t *in_bufs = se->in_bufs, *out_bufs = se->out_bufs, *scratch = se->scratch;
	uint32_t ctx = 0, handle = 0, sc = 0, requested = 0, out_length = 0;
	int32_t result = -1;

	se->listening = listening = true;
	while (!leaving) {
		uint32_t prim_in[4] = { ctx, (uint32_t)result, out_length, IN_CAPACITY }, prim_out[4] = { 0 };
		struct frpc_arg a[4] = { { prim_in, 16 }, { out_bufs, out_length }, { prim_out, 16 }, { in_bufs, IN_CAPACITY } };
		int32_t call_result = 0;
		int error;

		if (result != 0) {
			a[1].length = 0;
			prim_in[2] = 0;
		}
		error = frpc_call(se, se->slot, false, 3, SC(4, 2, 2), a, &call_result, 0);
		if (error != 0 || call_result != 0) {
			say("frpc: %s: adsp_listener_next2: %d, result %#x: the listener stops\n", se->name, error, call_result);
			break;
		}
		ctx = prim_out[0];
		handle = prim_out[1];
		sc = prim_out[2];
		requested = prim_out[3];
		if (requested > IN_CAPACITY) {
			say("frpc: a reverse call with %u bytes of in-buffers, more than %u\n", requested, IN_CAPACITY);
			result = AEE_EBADPARM;
			out_length = 0;
			continue;
		}
		{
			struct frpc_arg in[FRPC_MAX_ARGS], out[FRPC_MAX_ARGS];
			uint32_t nin = SC_IN(sc), nout = SC_OUT(sc), at = 0, sat = 0, put = 0;
			bool bad = nin > FRPC_MAX_ARGS || nout > FRPC_MAX_ARGS || SC_HANDLES(sc) != 0;

			for (uint32_t i = 0; !bad && i < nin; i++) {
				uint32_t len;

				if (at + 4 > requested) {
					bad = true;
					break;
				}
				memcpy(&len, in_bufs + at, 4);
				at += 4;
				if (len) {
					at = (at + 7) & ~7u;
				}
				if (at + len > requested) {
					bad = true;
					break;
				}
				in[i] = (struct frpc_arg){ in_bufs + at, len };
				at += len;
			}
			for (uint32_t i = 0; !bad && i < nout; i++) {
				uint32_t len;

				if (at + 4 > requested) {
					bad = true;
					break;
				}
				memcpy(&len, in_bufs + at, 4);
				at += 4;
				sat = (sat + 7) & ~7u;
				if (sat + len > IN_CAPACITY) {
					bad = true;
					break;
				}
				out[i] = (struct frpc_arg){ scratch + sat, len };
				bzero(scratch + sat, len);
				sat += len;
			}
			if (bad) {
				say("frpc: a reverse call I cannot unpack: handle %#x, sc %#x, %u bytes\n", handle, sc, requested);
				result = AEE_EBADPARM;
				out_length = 0;
				continue;
			}
			result = serve(se, handle, sc, in, out);
			{
				static int trace = -1;
				static uint32_t traced;

				if (trace < 0) {
					uint32_t v = 0;

					trace = PE_parse_boot_argn("slpi_frpc_trace", &v, sizeof(v)) && v != 0;
				}
				if (trace && traced++ < 600) {
					uint32_t m = SC_METHOD(sc) == 31 && nin >= 1 ? u32_at(&in[0], 0) : SC_METHOD(sc);

					say("frpc: %s < h%#x m%u in%u out%u%s%.60s -> %d\n", se->pd ? "sns" : "root", handle, m, nin, nout,
					    nin >= 2 ? " " : "", nin >= 2 ? string_of(&in[1]) : "", result);
				}
			}
			/* pack: len, align 8, bytes, for every out-buffer */
			for (uint32_t i = 0; i < nout; i++) {
				memcpy(out_bufs + put, &out[i].length, 4);
				put += 4;
				if (out[i].length) {
					put = (put + 7) & ~7u;
					memcpy(out_bufs + put, out[i].ptr, out[i].length);
					put += out[i].length;
				}
			}
			out_length = put;
		}
	}
	se->listening = false;
	thread_terminate(current_thread());
}

/* one session: attached, its listener running, and registered as the default listener of that process */
static bool
session_up(struct frpc_session *se)
{
	thread_t thread;
	int32_t result = 0;
	char name[] = "adsp_default_listener";
	uint32_t prim_in[2] = { sizeof(name), 255 }, prim_out[2] = { 0 };
	char dlerror[256] = "";
	struct frpc_arg a[4];
	int error;

	if ((se->in_bufs = kalloc_data(IN_CAPACITY, Z_WAITOK | Z_ZERO)) == NULL ||
	    (se->out_bufs = kalloc_data(IN_CAPACITY + 4096, Z_WAITOK | Z_ZERO)) == NULL ||
	    (se->scratch = kalloc_data(IN_CAPACITY, Z_WAITOK | Z_ZERO)) == NULL) {
		say("frpc: no memory\n");
		return false;
	}
	if (!listener_init(se)) {
		mmu_faults("an attach");
		say("frpc: %s DID NOT TAKE US\n", se->name);
		return false;
	}
	if (kernel_thread_start(listener_thread, se, &thread) != KERN_SUCCESS) {
		return false;
	}
	thread_deallocate(thread);
	sleep_ms(20);
	/* remotectl_open("adsp_default_listener"): handle 0, method 0 ({len, dlerror len}, name; {handle, nErr}, dlerror) */
	a[0] = (struct frpc_arg){ prim_in, 8 };
	a[1] = (struct frpc_arg){ name, sizeof(name) };
	a[2] = (struct frpc_arg){ prim_out, 8 };
	a[3] = (struct frpc_arg){ dlerror, 255 };
	error = frpc_call(se, se->slot + 1, false, 0, SC(0, 2, 2), a, &result, 10000);
	say("frpc: %s: remotectl_open(\"%s\"): %d, result %#x, handle %#x, nErr %#x%s%s\n", se->name, name, error, result, prim_out[0],
	    prim_out[1], dlerror[0] ? ", " : "", dlerror);
	if (error == 0 && result == 0 && prim_out[1] == 0) {
		/* register: that handle, method 0, nothing */
		error = frpc_call(se, se->slot + 1, false, prim_out[0], SC(0, 0, 0), a, &result, 20000);
		say("frpc: %s: adsp_default_listener_register: %d, result %#x\n", se->name, error, result);
	}
	mmu_faults("a registration");
	say("frpc: %s HAS A LISTENER%s\n", se->name, se->listening ? "" : " (NOT LISTENING)");
	{
		/*
		 * Boot-arg slpi_dsplog=1: the DSP's own log lines (its FARF messages: what a sensor driver says when its part does not
		 * answer) to us, as Android's adspmsgd asks for them (fastrpc log_config.c): adspmsgd_adsp1, the DSP's static handle 5,
		 * method 2 init2(), no arguments; they come back as adspmsgd_apps log() calls (FRPC_APPS_MSG, said as "frpc: dsp:").
		 */
		uint32_t dsplog = 0;

		if (PE_parse_boot_argn("slpi_dsplog", &dsplog, sizeof(dsplog)) && dsplog != 0) {
			error = frpc_call(se, se->slot + 1, false, 5, SC(2, 0, 0), a, &result, 10000);
			say("frpc: %s: adspmsgd_adsp1 init2 (the DSP's log to us): %d, result %#x\n", se->name, error, result);
			if (error != 0 || result != 0) {
				/*
				 * (2026-11-13) init3 on the same static handle, as Android's adspmsgd_init does first: {heapid 0, ion_flags
				 * RPCMEM_HEAP_DEFAULT, filter 0x1f001f, pad, buf_size u64 256 KB}; {buff_addr u64}. The DSP gets its buffer from
				 * us through apps_mem (above) meanwhile; logbuf_thread reads it.
				 */
				uint32_t i3_in[6] = { 0, 0x80000000u, 0x1f001f, 0, 256 * 1024, 0 };
				uint64_t i3_out = 0;
				thread_t reader;

				a[0] = (struct frpc_arg){ i3_in, 24 };
				a[1] = (struct frpc_arg){ &i3_out, 8 };
				error = frpc_call(se, se->slot + 1, false, 5, SC(4, 1, 1), a, &result, 15000);
				say("frpc: %s: adspmsgd_adsp1 init3: %d, result %#x, buffer %#llx\n", se->name, error, result, i3_out);
				if (error == 0 && result == 0 && kernel_thread_start(logbuf_thread, se, &reader) == KERN_SUCCESS) {
					thread_deallocate(reader);
				}
			}
			if (error != 0 || result != 0) {
				/* a remote_handle64 interface (quic/fastrpc adspmsgd_adsp1.h): opened by its name in a user process */
				char mname[] = "adspmsgd_adsp1";
				uint32_t m_in[2] = { sizeof(mname), 255 }, m_out[2] = { 0 };

				dlerror[0] = 0;
				a[0] = (struct frpc_arg){ m_in, 8 };
				a[1] = (struct frpc_arg){ mname, sizeof(mname) };
				a[2] = (struct frpc_arg){ m_out, 8 };
				a[3] = (struct frpc_arg){ dlerror, 255 };
				error = frpc_call(se, se->slot + 1, false, 0, SC(0, 2, 2), a, &result, 10000);
				say("frpc: %s: remotectl_open(\"%s\"): %d, result %#x, handle %#x, nErr %#x%s%s\n", se->name, mname, error, result,
				    m_out[0], m_out[1], dlerror[0] ? ", " : "", dlerror);
				if (error == 0 && result == 0 && m_out[1] == 0) {
					error = frpc_call(se, se->slot + 1, false, m_out[0], SC(2, 0, 0), a, &result, 10000);
					say("frpc: %s: adspmsgd_adsp1 init2 on handle %#x: %d, result %#x\n", se->name, m_out[0], error, result);
				}
			}
			/*
			 * (2026-11-13) And which of its FARF levels it sends: none until asked, as Android's log_config.c asks when it finds
			 * a "<process>.farf" file (quic/fastrpc): remotectl_open("adsp_current_process"), then set_logging_params2(mask,
			 * no files), method 6 ({mask, files 0}, the files' lengths: none), or set_logging_params (method 2, a 16-bit mask).
			 * The mask: slpi_dsplog's value when above 1, else 0x1f (every level).
			 */
			{
				char pname[] = "adsp_current_process";
				uint32_t open_in[2] = { sizeof(pname), 255 }, open_out[2] = { 0 }, log_in[2] = { dsplog > 1 ? dsplog : 0x1f, 0 };
				uint32_t none = 0;

				dlerror[0] = 0;
				a[0] = (struct frpc_arg){ open_in, 8 };
				a[1] = (struct frpc_arg){ pname, sizeof(pname) };
				a[2] = (struct frpc_arg){ open_out, 8 };
				a[3] = (struct frpc_arg){ dlerror, 255 };
				error = frpc_call(se, se->slot + 1, false, 0, SC(0, 2, 2), a, &result, 10000);
				say("frpc: %s: remotectl_open(\"%s\"): %d, result %#x, handle %#x, nErr %#x%s%s\n", se->name, pname, error, result,
				    open_out[0], open_out[1], dlerror[0] ? ", " : "", dlerror);
				if (error == 0 && result == 0 && open_out[1] == 0) {
					a[0] = (struct frpc_arg){ log_in, 8 };
					a[1] = (struct frpc_arg){ &none, 0 };
					error = frpc_call(se, se->slot + 1, false, open_out[0], SC(6, 2, 0), a, &result, 10000);
					say("frpc: %s: set_logging_params2(%#x): %d, result %#x\n", se->name, log_in[0], error, result);
					if (error != 0 || result != 0) {
						error = frpc_call(se, se->slot + 1, false, open_out[0], SC(2, 2, 0), a, &result, 10000);
						say("frpc: %s: set_logging_params(%#x): %d, result %#x\n", se->name, log_in[0], error, result);
					}
				}
			}
		}
	}
	return true;
}

static void
start_thread(__unused void *p, __unused wait_result_t w)
{
	uint32_t which = 3;     /* slpi_frpc_pd: bit 0 the root process, bit 1 the sensors process */

	PE_parse_boot_argn("slpi_frpc_pd", &which, sizeof(which));
	if (kmem_alloc(kernel_map, &pool, FRPC_POOL, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("frpc: no memory\n");
		thread_terminate(current_thread());
		return;
	}
	lck_mtx_lock(&hk_smmu_lock);
	bool mapped = mmu_on();
	lck_mtx_unlock(&hk_smmu_lock);
	if (!mapped) {
		say("frpc: the DSP cannot see our memory: no FastRPC\n");
		thread_terminate(current_thread());
		return;
	}
	/* the channel, as soon as the DSP has opened it and we have answered (hk_slpi.c's step 9) */
	for (unsigned waited = 0; !hk_slpi_fastrpc_ready(); waited++) {
		if (waited >= 120000) {
			say("frpc: the DSP did not open its FastRPC channel in two minutes\n");
			thread_terminate(current_thread());
			return;
		}
		sleep_ms(1);
	}
	sleep_ms(5);
	/* the sensors' first: SEE reads its registry as it starts */
	if (which & 2) {
		session_up(&sessions[1]);
	}
	if (which & 1) {
		session_up(&sessions[0]);
	}
	say("frpc: listeners up: reverse calls so far %u, files opened %u\n", reverse_calls, opens);
	thread_terminate(current_thread());
}

static int
frpc_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[2048];
	int n = snprintf(text, sizeof(text), "listeners: root %s, sensors %s; answers %u (strays %u); reverse calls %u, opens %u, reads %u, "
	    "writes %u, misses %u, not served %u\nlast failures:\n", sessions[0].listening ? "on" : "off", sessions[1].listening ? "on" : "off",
	    answers, strays, reverse_calls, opens, reads, writes, misses, unsupported);

	for (uint32_t i = 0; i < 8 && i < failed_count && n < (int)sizeof(text); i++) {
		n += snprintf(text + n, sizeof(text) - (size_t)n, "  %s\n", failed[(failed_count - 1 - i) % 8]);
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, fastrpc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, frpc_sysctl, "A",
    "FastRPC with the sensor DSP: the listeners, what the DSP asked of our files");
SYSCTL_PROC(_kern_hackios, OID_AUTO, fastrpc_opens, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, trace_opens, 0, trace_sysctl, "A",
    "every name the sensor DSP opened, in order (registry files by their own name)");
SYSCTL_PROC(_kern_hackios, OID_AUTO, dsplog, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, trace_dsplog, 0, trace_sysctl, "A",
    "the sensor DSP's log lines (slpi_dsplog=1) of a magnetometer or a failure");
SYSCTL_PROC(_kern_hackios, OID_AUTO, dsplog_last, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, trace_dsplast, 0, trace_sysctl, "A",
    "the sensor DSP's last log lines (a ring)");

bool
hk_fastrpc_listening(void)
{
	return listening;
}

void
hk_fastrpc_start(void)
{
	thread_t thread;
	uint32_t on = 1;

	PE_parse_boot_argn("slpi_frpc", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(start_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
