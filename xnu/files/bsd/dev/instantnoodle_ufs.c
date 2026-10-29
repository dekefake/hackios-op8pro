/*
 * The OnePlus 8 Pro's UFS host controller, adopted as the stock bootloader left it (ufs8=1: READ ONLY).
 *
 * This phone's UEFI cannot use the storage: Qualcomm's UFSDxe asserts Device Error when it initialises the
 * controller that ABL has been using (plan addendum 5), so the loader serves no storage and the iOS image
 * is uploaded into RAM at every boot. The controller itself is fine: `ufs_peek=1` (hk_ufshc.c) found it
 * enabled, the device present, the transfer request list ready and running with an empty doorbell, 32 slots,
 * 64-bit addressing, UFSHCI 3.0, ABL's list at 0x9a131000. So this does what hk_ufshc.c does on the POCO X5
 * Pro and nothing more: no reset, no link startup, no UIC command, no interrupt, no clock or power change.
 * With nothing in flight the list is stopped (UTRLRSR 0), UTRLBA/UTRLBAU are pointed at a list of ours (one
 * page of ours, one slot in use), the list is started again; a TEST UNIT READY proves the controller reads
 * it; then READ CAPACITY(10) and READ(10) of the first blocks of logical unit 0 -- the GPT, whose
 * partitions are printed; at the end ABL's list is put back, on every way out.
 *
 * Read only by construction, as hk_ufshc.c: ufs8_permitted() lets TEST UNIT READY, REQUEST SENSE, INQUIRY,
 * READ CAPACITY(10) and READ(10)/READ(16) through, from the device or without data, to logical units 0..7
 * (never a well-known one), and nothing else is ever written into a slot. A write path comes only with the
 * owner's approval, for the port's own partition (docs/TODO.md T40).
 *
 * What the experiment answers: whether the apps SMMU lets the UFS stream DMA to addresses of ours (the
 * SMMU's global fault status is printed before and after), and whether the hypervisor lets the list be
 * swapped. The memory is a kernel page run mapped cached; it is cleaned and invalidated around every command.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include "instantnoodle_ufs_phy.h"
#include "hk_ufshc.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_line(const char *line);   /* (osfmk/arm64/loader_return.c) */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern boolean_t pmap_valid_address(uint64_t addr);
extern void delay(int usec);
#define u8_wait(us)     do { if (u8_late) { for (volatile uint64_t spin_ = 0; spin_ < (uint64_t)(us) * 300; spin_++) { } } else { delay(us); } } while (0)

#define HC_PHYS         0x1d84000UL
#define HC_SIZE         0x3000
#define SMMU_PHYS       0x15000000UL
#define HC_CAP          0x00
#define HC_VER          0x08
#define HC_IS           0x20
#define HC_HCS          0x30
#define HC_HCE          0x34
#define HC_UTRLBA       0x50
#define HC_UTRLBAU      0x54
#define HC_UTRLDBR      0x58
#define HC_UTRLCLR      0x5c
#define HC_UTRLRSR      0x60
#define HC_UTRLCNR      0x64
#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define IS_FATAL        ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))
#define CAP_NUTRS(c)    (((c) & 0x1f) + 1)
#define CAP_64AS        (1u << 24)

#define UTRD_CT_UFS     (1u << 28)
#define UTRD_DD_NONE    (0u << 25)
#define UTRD_DD_IN      (2u << 25)
#define UTRD_DD_OUT     (1u << 25)
#define UTRD_DD_MASK    (3u << 25)
#define OCS_INVALID     0xf
#define UPIU_COMMAND    0x01
#define UPIU_FLAG_READ  0x40
#define UPIU_FLAG_WRITE 0x20
#define UCD_RESPONSE    512
#define UCD_PRDT        1024
#define BLOCK           4096u

#define OFF_UTRL        0
#define OFF_UCD         4096
#define OFF_DATA        8192
#define DATA_BYTES      (64u << 10)
#define AREA_BYTES      (OFF_DATA + DATA_BYTES)

static uint8_t u8_static[AREA_BYTES] __attribute__((aligned(4096)));
static uint64_t u8_wr_lo, u8_wr_hi;             /* the only blocks a WRITE(10) may touch: [lo, hi) of LUN 0; 0, 0 until hk_ufs.c has checked the port's partition: no write at all */
static uint8_t u8_wr_lun;                         /* the LUN of that window: 0, except for the one block of the A/B swap's GPT (instantnoodle_ufs_ab_swap(): LUN 4), for that write alone */
static uint8_t *u8_big;                         /* the data buffer of the backend (instantnoodle_ufs_up): memory of the kernel's, mapped page by page; NULL: the static area's */
static uint32_t u8_big_bytes;
#define U8_DATA         ((uint8_t *)(u8_big != NULL ? (vm_offset_t)u8_big : u8_area + OFF_DATA))
#define U8_DATA_BYTES   (u8_big != NULL ? u8_big_bytes : (uint32_t)DATA_BYTES)
#define PRD_MAX         (256u << 10)
static uint8_t u8_tables[3 * 4096] __attribute__((aligned(4096)));
static uint64_t u8_dma_base;                    /* the IOVA of the area when the SMMU maps it for the controller (ufs8=9); 0: its physical address */
#define U8_DMA(off)     ((u8_dma_base != 0 ? u8_dma_base : u8_area_pa) + (off))
static volatile uint32_t *u8_hc;
static vm_offset_t u8_area;
static uint64_t u8_area_pa;
static bool u8_in;
static uint32_t u8_old_lo, u8_old_hi;

static bool u8_late;                            /* instantnoodle_ufs_down(): the other processors are stopped and one may hold the console's lock: log without it, map nothing, wait by spinning */
extern void loader_log_putc(char c);

static void
u8_say(const char *format, ...) __printflike(1, 2);
static void
u8_say(const char *format, ...)
{
	char line[208], logged[224];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (u8_late) {
		for (const char *c = "ufs8: "; *c != 0; c++) {
			loader_log_putc(*c);
		}
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	printf("ufs8: %s", line);
	snprintf(logged, sizeof(logged), "ufs8: %s", line);
	loader_log_line(logged);
}

static uint32_t
u8_read(uint32_t reg)
{
	return u8_hc[reg / 4];
}

static void
u8_write(uint32_t reg, uint32_t value)
{
	u8_hc[reg / 4] = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* read only: the operations, the direction (from the device or none) and the logical unit */
static const char *
u8_refusal(const uint8_t *upiu, uint32_t dw0)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;

	if ((dw0 & 0xf0000000u) == (2u << 28)) {
		/* device management: NOP OUT, and the query flag requests of fDeviceInit (idn 1) only: READ FLAG (5), SET FLAG (6) */
		if (dd != UTRD_DD_NONE || (dw0 & 0x0fffffffu) != 0) {
			return "a device management descriptor with data";
		}
		if (upiu[0] == 0x00) {
			return NULL;
		}
		if (upiu[0] == 0x16 && (upiu[12] == 0x05 || upiu[12] == 0x06) && upiu[13] == 0x01) {
			return NULL;
		}
		return "a device management request that is not NOP OUT or a flag request of fDeviceInit";
	}

	if (((dw0 & 0xf0000000u) != UTRD_CT_UFS && (dw0 & 0xf0000000u) != 0) || (dw0 & ~(0xf0000000u | UTRD_DD_MASK | (1u << 24))) != 0) {
		return "a descriptor with more than the command type and direction";
	}
	if (upiu[0] != UPIU_COMMAND || (upiu[1] & ~(UPIU_FLAG_READ | UPIU_FLAG_WRITE)) != 0 || (upiu[1] & UPIU_FLAG_READ && upiu[1] & UPIU_FLAG_WRITE)) {
		return "a UPIU that is not a plain command";
	}
	if ((upiu[1] & UPIU_FLAG_WRITE) != 0 && cdb[0] != 0x2a) {
		return "W set on anything but WRITE(10)";
	}
	if (upiu[2] >= 8 && !(upiu[2] == 0xd0 && cdb[0] == 0x1b)) {
		return "a logical unit that is not 0..7 (the device well-known unit only for START STOP UNIT)";
	}
	if (dd != UTRD_DD_IN && dd != UTRD_DD_NONE && !(dd == UTRD_DD_OUT && cdb[0] == 0x2a)) {
		return "data to the device other than a WRITE(10)";
	}
	switch (cdb[0]) {
	case 0x28: {    /* READ(10) */
		uint32_t blocks = (uint32_t)cdb[7] << 8 | cdb[8];
		uint32_t expected = (uint32_t)upiu[12] << 24 | (uint32_t)upiu[13] << 16 | (uint32_t)upiu[14] << 8 | upiu[15];

		return dd == UTRD_DD_IN && (upiu[1] & UPIU_FLAG_READ) != 0 && blocks != 0 && expected == blocks * BLOCK ? NULL :
		       "a READ(10) that does not read or whose length is not the UPIU's";
	}
	case 0x2a: {    /* WRITE(10): the window's LUN (0; 4 for one block of the A/B swap), FUA only, whole blocks of 4 KiB, inside the window (the port's own partition, past the iOS volume and the cryptexes) */
		uint64_t lba = (uint64_t)cdb[2] << 24 | (uint64_t)cdb[3] << 16 | (uint64_t)cdb[4] << 8 | cdb[5];
		uint32_t blocks = (uint32_t)cdb[7] << 8 | cdb[8];
		uint32_t expected = (uint32_t)upiu[12] << 24 | (uint32_t)upiu[13] << 16 | (uint32_t)upiu[14] << 8 | upiu[15];

		if (upiu[2] != u8_wr_lun || dd != UTRD_DD_OUT || upiu[1] != UPIU_FLAG_WRITE || cdb[1] != 0x08 || cdb[6] != 0 || cdb[9] != 0 || blocks == 0 ||
		    expected != blocks * BLOCK) {
			return "a WRITE(10) that is not of the window's LUN, FUA only, with data of its length";
		}
		if (u8_wr_hi == 0 || lba < u8_wr_lo || lba + blocks > u8_wr_hi) {
			return "a WRITE(10) outside the one window that takes writes";
		}
		return NULL;
	}
	case 0x35:      /* SYNCHRONIZE CACHE(10) of the whole unit */
		return upiu[2] == 0 && dd == UTRD_DD_NONE && cdb[1] == 0 ? NULL : "a SYNCHRONIZE CACHE that is not of LUN 0 without data";
	case 0x00:
		return dd == UTRD_DD_NONE ? NULL : "TEST UNIT READY with data";
	case 0x1b:      /* START STOP UNIT: only power condition ACTIVE (0x10) of the device unit, or START (0x01) of a unit; no data, no IMMED */
		return dd == UTRD_DD_NONE && (upiu[2] == 0xd0 ? cdb[4] == 0x10 : cdb[4] == 0x01) && cdb[1] == 0 ? NULL :
		       "a START STOP UNIT that is not the device unit's ACTIVE or a unit's START";
	case 0x03:
	case 0x12:
	case 0x25:
		return NULL;
	default:
		return "a SCSI operation not on the list";
	}
}

/* one command in slot 0, rung and waited for; the answer in the response UPIU. 0 if the controller says OCS success. */
static int
u8_command2(uint8_t lun, const uint8_t *cdb, uint32_t bytes, int dir, uint8_t *status)
{
	bool in = dir == 1;

	volatile uint32_t *utrd = (volatile uint32_t *)(u8_area + OFF_UTRL);
	volatile uint8_t *ucd = (volatile uint8_t *)(u8_area + OFF_UCD);
	volatile uint32_t *prd = (volatile uint32_t *)(u8_area + OFF_UCD + UCD_PRDT);
	uint8_t upiu[32] = { 0 };
	uint32_t dw0 = UTRD_CT_UFS | (dir == 1 ? UTRD_DD_IN : dir == 2 ? UTRD_DD_OUT : UTRD_DD_NONE), ocs, is;
	uint64_t data_pa = U8_DMA(OFF_DATA), ucd_pa = U8_DMA(OFF_UCD);
	int tries;
	uint32_t nprd = 0;

	upiu[0] = UPIU_COMMAND;
	upiu[1] = in ? UPIU_FLAG_READ : dir == 2 ? UPIU_FLAG_WRITE : 0;
	upiu[2] = lun;
	upiu[3] = 0;
	upiu[12] = (uint8_t)(bytes >> 24);
	upiu[13] = (uint8_t)(bytes >> 16);
	upiu[14] = (uint8_t)(bytes >> 8);
	upiu[15] = (uint8_t)bytes;
	memcpy(upiu + 16, cdb, 16);
	if (u8_refusal(upiu, dw0) != NULL) {
		u8_say("REFUSED (%s): CDB %02x\n", u8_refusal(upiu, dw0), cdb[0]);
		return EPERM;
	}
	if (bytes > U8_DATA_BYTES) {
		return EINVAL;
	}
	bzero((void *)u8_area, OFF_DATA);
	for (int i = 0; i < 32; i++) {
		ucd[i] = upiu[i];
	}
	for (uint32_t at = 0; at < bytes; at += PRD_MAX, nprd++) {
		uint64_t pa = data_pa + at;
		uint32_t len = bytes - at < PRD_MAX ? bytes - at : PRD_MAX;

		prd[4 * nprd] = (uint32_t)pa;
		prd[4 * nprd + 1] = (uint32_t)(pa >> 32);
		prd[4 * nprd + 2] = 0;
		prd[4 * nprd + 3] = len - 1;
	}
	if (bytes != 0 && u8_big == NULL) {
		bzero((void *)U8_DATA, bytes);
	}
	utrd[0] = dw0;
	utrd[1] = 0;
	utrd[2] = OCS_INVALID;
	utrd[3] = 0;
	utrd[4] = (uint32_t)ucd_pa;
	utrd[5] = (uint32_t)(ucd_pa >> 32);
	utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);
	utrd[7] = ((UCD_PRDT / 4) << 16) | nprd;
	FlushPoC_DcacheRegion(u8_area, OFF_DATA);
	if (bytes != 0) {
		FlushPoC_DcacheRegion((vm_offset_t)U8_DATA, bytes);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((u8_read(HC_UTRLDBR) & 1) != 0 || u8_read(HC_UTRLBA) != (uint32_t)U8_DMA(OFF_UTRL)) {
		u8_say("the list is not ours or slot 0 is busy: not rung\n");
		return EBUSY;
	}
	u8_write(HC_UTRLDBR, 1);
	for (tries = 0; tries < 20000 && (u8_read(HC_UTRLDBR) & 1) != 0; tries++) {
		delay(100);
	}
	is = u8_read(HC_IS);
	if ((u8_read(HC_UTRLDBR) & 1) != 0) {
		u8_say("no answer in 2 s (IS %08x): the slot is cleared\n", is);
		u8_write(HC_UTRLCLR, ~1u);
		delay(1000);
		return ETIMEDOUT;
	}
	u8_write(HC_IS, is & 1);
	FlushPoC_DcacheRegion(u8_area, OFF_DATA);
	if (bytes != 0) {
		FlushPoC_DcacheRegion((vm_offset_t)U8_DATA, bytes);
	}
	ocs = utrd[2] & 0xff;
	*status = ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[7];
	if (*status != 0) {
		const volatile uint8_t *r = (const volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE);

		u8_say("SCSI status %02x, sense: length %u, key %x ASC %02x ASCQ %02x (response byte 6: %02x)\n", *status, (uint32_t)r[32] << 8 | r[33],
		    r[36] & 0xf, r[46], r[47], r[6]);
	}
	if (ocs != 0) {
		u8_say("OCS %02x (IS %08x), response UPIU %02x %02x %02x %02x\n", ocs, is,
		    ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[0], ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[1],
		    ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[2], ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[3]);
		return EIO;
	}
	return 0;
}

static int
u8_command(uint8_t lun, const uint8_t *cdb, uint32_t bytes, bool in, uint8_t *status)
{
	return u8_command2(lun, cdb, bytes, in ? 1 : 0, status);
}

static int
u8_swap_in(void)
{
	uint32_t lo = (uint32_t)U8_DMA(OFF_UTRL), hi = (uint32_t)(U8_DMA(0) >> 32);

	u8_old_lo = u8_read(HC_UTRLBA);
	u8_old_hi = u8_read(HC_UTRLBAU);
	u8_write(HC_UTRLRSR, 0);
	delay(200);
	u8_write(HC_UTRLBA, lo);
	u8_write(HC_UTRLBAU, hi);
	u8_write(HC_UTRLRSR, 1);
	delay(200);
	if (u8_read(HC_UTRLBA) != lo || u8_read(HC_UTRLBAU) != hi || (u8_read(HC_UTRLRSR) & 1) == 0) {
		u8_say("the list would not switch: UTRLBA %08x:%08x, run %u\n", u8_read(HC_UTRLBAU), u8_read(HC_UTRLBA),
		    u8_read(HC_UTRLRSR) & 1);
		return EIO;
	}
	u8_in = true;
	return 0;
}

static void
u8_swap_out(void)
{
	u8_write(HC_UTRLRSR, 0);
	u8_wait(200);
	u8_write(HC_UTRLBA, u8_old_lo);
	u8_write(HC_UTRLBAU, u8_old_hi);
	u8_write(HC_UTRLRSR, 1);
	u8_wait(200);
	u8_say("ABL's list put back: UTRLBA %08x:%08x, run %u, doorbell %08x\n", u8_read(HC_UTRLBAU), u8_read(HC_UTRLBA),
	    u8_read(HC_UTRLRSR) & 1, u8_read(HC_UTRLDBR));
	u8_in = false;
}

static uint64_t
u8_le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--) {
		v = v << 8 | p[i];
	}
	return v;
}

static void
u8_gpt(void)
{
	const uint8_t *data = U8_DATA;
	uint8_t cdb[16] = { 0 }, status = 0xff;
	uint32_t entries, size, per_block, shown = 0;
	uint64_t entry_lba;

	cdb[0] = 0x28;
	cdb[5] = 1;
	cdb[8] = 1;
	if (u8_command(0, cdb, BLOCK, true, &status) != 0 || status != 0) {
		u8_say("READ(10) of block 1: failed (status %02x)\n", status);
		return;
	}
	if (memcmp(data, "EFI PART", 8) != 0) {
		u8_say("block 1 of LUN 0 holds no GPT header: %02x %02x %02x %02x %02x %02x %02x %02x\n", data[0], data[1], data[2],
		    data[3], data[4], data[5], data[6], data[7]);
		return;
	}
	entry_lba = u8_le64(data + 72);
	entries = data[80] | data[81] << 8 | data[82] << 16 | (uint32_t)data[83] << 24;
	size = data[84] | data[85] << 8 | data[86] << 16 | (uint32_t)data[87] << 24;
	u8_say("GPT of LUN 0: %u entries of %u bytes at block %llu; disk last block %llu\n", entries, size, entry_lba, u8_le64(data + 48));
	if (size < 128 || size > BLOCK || entries > 256) {
		return;
	}
	per_block = BLOCK / size;
	for (uint32_t first = 0; first < entries; first += per_block * 8) {
		uint32_t blocks = (entries - first + per_block - 1) / per_block;

		if (blocks > 8) {
			blocks = 8;
		}
		bzero(cdb, sizeof(cdb));
		cdb[0] = 0x28;
		cdb[2] = (uint8_t)((entry_lba + first / per_block) >> 24);
		cdb[3] = (uint8_t)((entry_lba + first / per_block) >> 16);
		cdb[4] = (uint8_t)((entry_lba + first / per_block) >> 8);
		cdb[5] = (uint8_t)(entry_lba + first / per_block);
		cdb[7] = 0;
		cdb[8] = (uint8_t)blocks;
		if (u8_command(0, cdb, blocks * BLOCK, true, &status) != 0 || status != 0) {
			u8_say("READ(10) of the entries: failed (status %02x)\n", status);
			return;
		}
		for (uint32_t i = 0; i < blocks * per_block && first + i < entries; i++) {
			const uint8_t *e = data + i * size;
			char name[40];
			uint32_t k;
			bool used = false;

			for (k = 0; k < 16; k++) {
				used = used || e[k] != 0;
			}
			if (!used) {
				continue;
			}
			for (k = 0; k < 36 && (e[56 + 2 * k] | e[57 + 2 * k]) != 0; k++) {
				name[k] = (char)e[56 + 2 * k];
			}
			name[k] = 0;
			u8_say("GPT %3u: blocks %llu..%llu %s\n", first + i + 1, u8_le64(e + 32), u8_le64(e + 40), name);
			shown++;
		}
	}
	u8_say("GPT of LUN 0: %u partitions\n", shown);
}

/*
 * The UFS block of the global clock controller (gcc, 0x100000): the controller's registers answer, but the DMA ends in a bus fatal error (IS bit 17)
 * wherever the memory is, ABL's own included: the AXI clock (or the aggregator's) may have been gated again after ABL used the storage.
 * Printed (CBCR bit 31 is CLK_OFF), and each branch of the list found off is switched on (bit 0): a clock enable, nothing else.
 */
static void
u8_smmu(void)
{
	volatile uint32_t *smmu = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x2000);
	uint32_t idr0 = smmu[0x20 / 4], n = idr0 & 0xff, valid = 0;

	u8_say("apps SMMU: sCR0 %08x, IDR0 %08x (%u stream match groups), IDR1 %08x\n", smmu[0], idr0, n, smmu[0x24 / 4]);
	for (uint32_t i = 0; i < n && 0x800 + 4 * i < 0xc00; i++) {
		uint32_t smr = smmu[(0x800 + 4 * i) / 4], s2cr = smmu[(0xc00 + 4 * i) / 4];

		if ((smr >> 31) != 0) {
			valid++;
			u8_say("smmu smr %2u: stream %04x mask %04x -> s2cr %08x (type %u, context %u)\n", i, smr & 0x7fff, (smr >> 16) & 0x7fff, s2cr,
			    (s2cr >> 16) & 3, s2cr & 0xff);
		}
	}
	u8_say("apps SMMU: %u match registers valid\n", valid);
}

static void
u8_clocks(bool enable)
{
	static const uint32_t cbcr[] = { 0x77004 /* gdsc */, 0x77010 /* axi */, 0x77018 /* ahb */, 0x7701c, 0x77020, 0x77024, 0x7705c /* unipro */, 0x77064 /* ice */,
	    0x7709c /* phy aux */, 0x770a0, 0x770a4, 0x770b0, 0x770cc /* aggre axi */ };
	volatile uint32_t *gcc = (volatile uint32_t *)ml_io_map(0x100000UL + 0x77000, 0x1000);

	for (unsigned i = 0; i < sizeof(cbcr) / sizeof(cbcr[0]); i++) {
		uint32_t v = gcc[(cbcr[i] - 0x77000) / 4];

		u8_say("gcc %05x = %08x%s\n", cbcr[i], v, (cbcr[i] != 0x77004 && (v & 0x80000000u)) ? " (OFF)" : "");
		if (enable && cbcr[i] != 0x77004 && (v & 0x80000000u) && (cbcr[i] == 0x77010 || cbcr[i] == 0x77018 || cbcr[i] == 0x7705c ||
		    cbcr[i] == 0x77064 || cbcr[i] == 0x770cc)) {
			gcc[(cbcr[i] - 0x77000) / 4] = v | 1;
			__asm__ volatile ("dsb sy" ::: "memory");
			delay(100);
			u8_say("gcc %05x switched on: %08x\n", cbcr[i], gcc[(cbcr[i] - 0x77000) / 4]);
		}
	}
}

/*
 * ufs8=2: the same question asked of ABL's own memory. Our list at 0xf8022000 and in kernel memory above 4 GB both ended in IS bit 17
 * (the bus fatal error) with the descriptor never fetched. Here nothing is swapped: ABL's slot 0 (its list, its command descriptor and the
 * data buffer its PRD table names -- all left where ABL put them) is reused, only the command in it is replaced by a READ(10) of block 1
 * of LUN 0 (the GPT header) with the length of ABL's PRD entry. If this reads, the controller's DMA works where ABL had memory and the
 * place of our own memory is the problem; if not, it is the controller's state.
 */
static bool
u8_abl(void)
{
	uint64_t list_pa = (uint64_t)u8_read(HC_UTRLBAU) << 32 | u8_read(HC_UTRLBA), ucd_pa, data_pa;
	volatile uint32_t *list = (volatile uint32_t *)(ml_io_map_unmappable((vm_offset_t)(list_pa & ~(uint64_t)PAGE_MASK), PAGE_SIZE,
	    VM_WIMG_WCOMB) + (list_pa & PAGE_MASK));
	volatile uint8_t *ucd;
	volatile uint32_t *prd;
	volatile uint8_t *data;
	uint32_t dw[8], prd_off, prd_n, count, tries, is, blocks;
	uint8_t upiu[32] = { 0 };

	for (int i = 0; i < 8; i++) {
		dw[i] = list[i];
	}
	u8_say("ABL's slot 0 at %llx: %08x %08x %08x %08x %08x %08x %08x %08x\n", list_pa, dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6],
	    dw[7]);
	ucd_pa = (uint64_t)dw[5] << 32 | dw[4];
	prd_off = (dw[7] >> 16) * 4;
	prd_n = dw[7] & 0xffff;
	if (ucd_pa == 0 || (ucd_pa & 0x7f) != 0 || prd_n != 1 || prd_off < 64 || prd_off > 2048) {
		u8_say("ABL's descriptor is not of a shape this knows (one PRD entry): nothing is done\n");
		return false;
	}
	ucd = (volatile uint8_t *)(ml_io_map_unmappable((vm_offset_t)(ucd_pa & ~(uint64_t)PAGE_MASK), 2 * PAGE_SIZE, VM_WIMG_WCOMB) +
	    (ucd_pa & PAGE_MASK));
	prd = (volatile uint32_t *)(ucd + prd_off);
	data_pa = (uint64_t)prd[1] << 32 | prd[0];
	count = (prd[3] & 0x3ffff) + 1;
	u8_say("ABL's UCD at %llx: UPIU %02x %02x %02x %02x .. CDB %02x; PRD at +%u: data %llx, %u bytes\n", ucd_pa, ucd[0], ucd[1], ucd[2],
	    ucd[3], ucd[16], prd_off, data_pa, count);
	if (count % BLOCK != 0 || count > DATA_BYTES || (data_pa & 3) != 0) {
		u8_say("the data buffer is not a whole number of 4 KB blocks of at most 64 KB: nothing is done\n");
		return false;
	}
	blocks = count / BLOCK;
	data = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)(data_pa & ~(uint64_t)PAGE_MASK), round_page((data_pa & PAGE_MASK) + count),
	    VM_WIMG_WCOMB) + (data_pa & PAGE_MASK);
	upiu[0] = UPIU_COMMAND;
	upiu[1] = UPIU_FLAG_READ;
	upiu[12] = (uint8_t)(count >> 24);
	upiu[13] = (uint8_t)(count >> 16);
	upiu[14] = (uint8_t)(count >> 8);
	upiu[15] = (uint8_t)count;
	upiu[16] = 0x28;
	upiu[21] = 1;                   /* block 1: the GPT header */
	upiu[23] = (uint8_t)(blocks >> 8);
	upiu[24] = (uint8_t)blocks;
	if (u8_refusal(upiu, (dw[0] & ~UTRD_DD_MASK) | UTRD_DD_IN) != NULL) {
		u8_say("REFUSED (%s)\n", u8_refusal(upiu, (dw[0] & ~UTRD_DD_MASK) | UTRD_DD_IN));
		return false;
	}
	for (int i = 0; i < 32; i++) {
		ucd[i] = upiu[i];
	}
	for (uint32_t i = 0; i < count; i += 4) {
		*(volatile uint32_t *)(data + i) = 0;
	}
	list[0] = (dw[0] & ~UTRD_DD_MASK) | UTRD_DD_IN;           /* (ABL's own command type: 0, not 1) */
	list[2] = OCS_INVALID;
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((u8_read(HC_UTRLDBR) & 1) != 0) {
		u8_say("slot 0 is busy\n");
		return false;
	}
	u8_write(HC_UTRLDBR, 1);
	for (tries = 0; tries < 20000 && (u8_read(HC_UTRLDBR) & 1) != 0; tries++) {
		delay(100);
	}
	is = u8_read(HC_IS);
	u8_say("READ(10) of block 1 in ABL's slot: doorbell %08x, OCS %02x, IS %08x, response UPIU %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    u8_read(HC_UTRLDBR), list[2] & 0xff, is, ucd[512], ucd[513], ucd[514], ucd[515], ucd[516], ucd[517], ucd[518], ucd[519]);
	if ((u8_read(HC_UTRLDBR) & 1) != 0) {
		u8_write(HC_UTRLCLR, ~1u);
		delay(1000);
		return false;
	}
	u8_write(HC_IS, is & 1);
	u8_say("what came: %02x %02x %02x %02x %02x %02x %02x %02x\n", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
	return (list[2] & 0xff) == 0;
}

/*
 * ufs8=3: the apps SMMU faults a stream nobody matches (sCR0.USFCFG, bit 10), and none of its 5 valid match registers is the UFS controller's:
 * ABL, which had the storage before this kernel, ran with unmatched streams let through. Two things are tried, each undone before the next and at the end:
 * A) USFCFG cleared (unmatched streams bypass); B) two match registers of a free group sending streams 0xe0..0xef and 0x100..0x10f (the numbers the
 * vendor trees of the neighbouring SoCs use for the UFS controller) through a bypass entry. Each is read back: the hypervisor owns the SMMU and may
 * refuse a write.
 */
static void
u8_smmu_fix(volatile uint32_t *smmu)
{
	uint32_t old = smmu[0], first = ~0u, second = ~0u, n = smmu[0x20 / 4] & 0xff, saved_smr[2] = { 0 }, saved_s2cr[2] = { 0 };
	static const uint32_t streams[2] = { 0x0e0 | (0xf << 16), 0x100 | (0xf << 16) };

	smmu[0] = old & ~(1u << 10);
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("A) sCR0 %08x written %08x, reads %08x\n", old, old & ~(1u << 10), smmu[0]);
	if (u8_abl()) {
		u8_say("A) THE READ WORKS with USFCFG cleared\n");
		smmu[0] = old;
		return;
	}
	smmu[0] = old;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_write(HC_IS, u8_read(HC_IS));
	for (uint32_t i = 8; i < n && 0x800 + 4 * i < 0xc00; i++) {
		if ((smmu[(0x800 + 4 * i) / 4] >> 31) == 0) {
			if (first == ~0u) {
				first = i;
			} else {
				second = i;
				break;
			}
		}
	}
	if (second == ~0u) {
		u8_say("B) no two free match registers\n");
		return;
	}
	for (int k = 0; k < 2; k++) {
		uint32_t i = k == 0 ? first : second;

		saved_smr[k] = smmu[(0x800 + 4 * i) / 4];
		saved_s2cr[k] = smmu[(0xc00 + 4 * i) / 4];
		smmu[(0xc00 + 4 * i) / 4] = 1u << 16;                   /* bypass */
		smmu[(0x800 + 4 * i) / 4] = 1u << 31 | streams[k];
		__asm__ volatile ("dsb sy" ::: "memory");
		u8_say("B) match register %u: smr %08x (written %08x), s2cr %08x\n", i, smmu[(0x800 + 4 * i) / 4], 1u << 31 | streams[k],
		    smmu[(0xc00 + 4 * i) / 4]);
	}
	if (u8_abl()) {
		u8_say("B) THE READ WORKS with streams 0xe0.. and 0x100.. let through\n");
	}
	for (int k = 0; k < 2; k++) {
		uint32_t i = k == 0 ? first : second;

		smmu[(0x800 + 4 * i) / 4] = saved_smr[k];
		smmu[(0xc00 + 4 * i) / 4] = saved_s2cr[k];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("the SMMU put back: sCR0 %08x\n", smmu[0]);
}

/* ---- ufs8=4..7: the controller brought up again, as the Android driver brings it up (ufs-qcom.c, ufshcd.c of the Lineage tree) ---- */

#define HC_IE           0x24
#define HC_UTRIACR      0x4c
#define HC_UTMRLBA      0x70
#define HC_UTMRLBAU     0x74
#define HC_UTMRLRSR     0x80
#define HC_UICCMD       0x90
#define HC_UCMDARG1     0x94
#define HC_UCMDARG2     0x98
#define HC_UCMDARG3     0x9c
#define HC_SYS1CLK_1US  0xc0
#define HC_CFG1         0xdc
#define HC_CFG2         0xe0
#define HC_HWVER        0xe4
#define HCS_UTMRLRDY    (1u << 2)
#define HCS_UCRDY       (1u << 3)
#define IS_UCCS         (1u << 10)
#define UIC_DME_GET     0x01
#define UIC_DME_SET     0x02
#define UIC_LINKSTARTUP 0x16
#define PHY_START       0xc00
#define PHY_SW_RESET    0xc08
#define PHY_LINECFG_DIS 0xd48
#define PHY_PCS_READY   0xd80
#define OFF_UTMRL       2048
#define TLMM_UFS_RESET  0x0f5b8000UL     /* the UFS_RESET pin: control register, and the I/O register (output bit 0) at +4 */

static volatile uint32_t *u8_phy;

/* one UIC command; 0 on success (UCMDARG2's result code), *value is UCMDARG3 (the attribute of a DME_GET) */
static int
u8_uic(uint32_t cmd, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t *value)
{
	int tries;

	for (tries = 0; tries < 200 && (u8_read(HC_HCS) & HCS_UCRDY) == 0; tries++) {
		delay(100);
	}
	if ((u8_read(HC_HCS) & HCS_UCRDY) == 0) {
		u8_say("UIC %02x: the controller is not ready for UIC commands (HCS %08x)\n", cmd, u8_read(HC_HCS));
		return -1;
	}
	u8_write(HC_IS, IS_UCCS);
	u8_write(HC_UCMDARG1, arg1);
	u8_write(HC_UCMDARG2, arg2);
	u8_write(HC_UCMDARG3, arg3);
	u8_write(HC_UICCMD, cmd);
	for (tries = 0; tries < 10000 && (u8_read(HC_IS) & IS_UCCS) == 0; tries++) {
		delay(100);
	}
	if ((u8_read(HC_IS) & IS_UCCS) == 0) {
		u8_say("UIC %02x: no completion in 1 s (HCS %08x, IS %08x)\n", cmd, u8_read(HC_HCS), u8_read(HC_IS));
		return -2;
	}
	if (value != NULL) {
		*value = u8_read(HC_UCMDARG3);
	}
	arg2 = u8_read(HC_UCMDARG2) & 0xff;
	u8_write(HC_IS, IS_UCCS);
	return (int)arg2;
}

static int
u8_dme_set(uint32_t attribute, uint32_t value)
{
	return u8_uic(UIC_DME_SET, attribute << 16, 0, value, NULL);
}

static int
u8_dme_get(uint32_t attribute, uint32_t *value)
{
	return u8_uic(UIC_DME_GET, attribute << 16, 0, 0, value);
}

/* the PHY: reset, the calibration tables, serdes, PCS ready (ufs_qcom_power_up_sequence) */
static int
u8_phy_init(void)
{
	int tries;
	uint32_t v;

	u8_phy = (volatile uint32_t *)ml_io_map(0x1d87000UL, 0x1000);
	u8_say("PHY before: START %08x POWER_DOWN %08x SW_RESET %08x LINECFG %08x PCS_READY %08x\n", u8_phy[PHY_START / 4],
	    u8_phy[(PHY_START + 4) / 4], u8_phy[PHY_SW_RESET / 4], u8_phy[PHY_LINECFG_DIS / 4], u8_phy[PHY_PCS_READY / 4]);
	u8_write(HC_CFG1, u8_read(HC_CFG1) | 2);                /* assert the PHY's reset */
	delay(1100);
	u8_phy[PHY_SW_RESET / 4] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned i = 0; i < sizeof(u8_phy_cal_table_rate_A) / sizeof(u8_phy_cal_table_rate_A[0]); i++) {
		u8_phy[u8_phy_cal_table_rate_A[i].offset / 4] = u8_phy_cal_table_rate_A[i].value;
	}
	for (unsigned i = 0; i < sizeof(u8_phy_cal_table_2nd_lane) / sizeof(u8_phy_cal_table_2nd_lane[0]); i++) {
		u8_phy[u8_phy_cal_table_2nd_lane[i].offset / 4] = u8_phy_cal_table_2nd_lane[i].value;
	}
	u8_phy[PHY_SW_RESET / 4] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_write(HC_CFG1, u8_read(HC_CFG1) & ~2u);              /* deassert it */
	delay(1100);
	v = u8_phy[PHY_START / 4];
	u8_phy[PHY_START / 4] = (v & ~1u) | 1;                  /* start the serdes */
	__asm__ volatile ("dsb sy" ::: "memory");
	for (tries = 0; tries < 100000 && (u8_phy[PHY_PCS_READY / 4] & 1) == 0; tries++) {
		delay(10);
	}
	u8_say("PHY after the tables: PCS_READY %08x (%s)\n", u8_phy[PHY_PCS_READY / 4], (u8_phy[PHY_PCS_READY / 4] & 1) ? "ready" : "NOT READY");
	u8_write(HC_CFG1, u8_read(HC_CFG1) | 1);                /* QUNIPRO_SEL */
	return (u8_phy[PHY_PCS_READY / 4] & 1) ? 0 : -1;
}

/* HCE 0 -> 1 (ufshcd_hba_execute_hce), the PHY between when asked for */
static int
u8_hba_enable(bool with_phy)
{
	int tries;

	if ((u8_read(HC_HCE) & 1) != 0) {
		u8_write(HC_HCE, 0);
		for (tries = 0; tries < 1000 && (u8_read(HC_HCE) & 1) != 0; tries++) {
			delay(10);
		}
		u8_say("HCE cleared: %08x after %d polls\n", u8_read(HC_HCE), tries);
	}
	if (with_phy && u8_phy_init() != 0) {
		return -1;
	}
	u8_write(HC_HCE, 1);
	delay(1000);
	for (tries = 0; tries < 100 && (u8_read(HC_HCE) & 1) == 0; tries++) {
		delay(5000);
	}
	u8_say("HCE set: %08x, HCS %08x, IS %08x, hardware version %08x\n", u8_read(HC_HCE), u8_read(HC_HCS), u8_read(HC_IS), u8_read(HC_HWVER));
	return (u8_read(HC_HCE) & 1) ? 0 : -1;
}

/* ufs_qcom_link_startup_pre_change, DME_LINKSTARTUP, post change */
static int
u8_link_startup(void)
{
	uint32_t v = 0;
	int err, tries;

	u8_write(HC_SYS1CLK_1US, 300);                          /* the core clock's 300 MHz */
	u8_write(HC_CFG2, u8_read(HC_CFG2) | 0xff);             /* REG_UFS_CFG2_CGC_EN_ALL */
	if (u8_phy != NULL) {
		u8_phy[PHY_LINECFG_DIS / 4] &= ~2u;             /* RX LineCfg on */
	}
	err = u8_dme_get(0xd002, &v);
	if (err == 0) {
		v = (((v & ~0xfffu) | 300) << 16) & ~(1u << 8);         /* (as ufs_qcom_set_dme_vs_core_clk_ctrl_clear_div does, controller version 4) */
		err = u8_dme_set(0xd002, v);
	}
	if (err == 0) {
		err = u8_dme_get(0x9007, &v);
	}
	if (err == 0) {
		err = u8_dme_set(0x9007, (v & ~0xfu) | 12);
	}
	if (err == 0) {
		err = u8_dme_set(0x155e, 0);                    /* PA_LOCAL_TX_LCC_ENABLE */
	}
	if (err == 0) {
		err = u8_dme_get(0x9000, &v);
		if (err == 0) {
			err = u8_dme_set(0x9000, v | 0x6000);
		}
	}
	u8_say("link startup: the DME settings before it: %d\n", err);
	for (tries = 0; tries < 3; tries++) {
		err = u8_uic(UIC_LINKSTARTUP, 0, 0, 0, NULL);
		u8_say("DME_LINKSTARTUP: result %d, HCS %08x (device present: %s), IS %08x\n", err, u8_read(HC_HCS), (u8_read(HC_HCS) & HCS_DP) ? "yes" : "no", u8_read(HC_IS));
		if (err == 0 && (u8_read(HC_HCS) & HCS_DP) != 0) {
			break;
		}
		err = -1;
	}
	return err;
}

/* ufshcd_make_hba_operational: the lists, ours, then running */
static void
u8_operational(void)
{
	volatile uint8_t *tm = (volatile uint8_t *)(u8_area + OFF_UTMRL);

	bzero((void *)u8_area, OFF_DATA);
	for (int i = 0; i < 640; i++) {
		tm[i] = 0;
	}
	FlushPoC_DcacheRegion(u8_area, OFF_DATA);
	u8_write(HC_UTRIACR, 0);
	u8_write(HC_UTRLBA, (uint32_t)U8_DMA(OFF_UTRL));
	u8_write(HC_UTRLBAU, (uint32_t)(U8_DMA(0) >> 32));
	u8_write(HC_UTMRLBA, (uint32_t)U8_DMA(OFF_UTMRL));
	u8_write(HC_UTMRLBAU, (uint32_t)(U8_DMA(0) >> 32));
	u8_say("HCS before the lists run: %08x\n", u8_read(HC_HCS));
	u8_write(HC_UTRLRSR, 1);
	u8_write(HC_UTMRLRSR, 1);
	delay(200);
	u8_say("lists: UTRLBA %08x:%08x run %u, UTMRLBA %08x run %u, HCS %08x\n", u8_read(HC_UTRLBAU), u8_read(HC_UTRLBA), u8_read(HC_UTRLRSR) & 1,
	    u8_read(HC_UTMRLBA), u8_read(HC_UTMRLRSR) & 1, u8_read(HC_HCS));
}

/* a device management request (NOP OUT, or a flag request of fDeviceInit) in slot 0; the response's bytes printed; 0 if the controller says success */
static int
u8_devman(const uint8_t *upiu, uint8_t *response)
{
	volatile uint32_t *utrd = (volatile uint32_t *)(u8_area + OFF_UTRL);
	volatile uint8_t *ucd = (volatile uint8_t *)(u8_area + OFF_UCD);
	uint32_t dw0 = 2u << 28, ocs;
	uint64_t ucd_pa = U8_DMA(OFF_UCD);
	int tries;

	if (u8_refusal(upiu, dw0) != NULL) {
		u8_say("REFUSED (%s)\n", u8_refusal(upiu, dw0));
		return EPERM;
	}
	bzero((void *)u8_area, OFF_DATA);
	for (int i = 0; i < 32; i++) {
		ucd[i] = upiu[i];
	}
	utrd[0] = dw0;
	utrd[2] = OCS_INVALID;
	utrd[4] = (uint32_t)ucd_pa;
	utrd[5] = (uint32_t)(ucd_pa >> 32);
	utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);
	utrd[7] = ((UCD_PRDT / 4) << 16) | 0;
	FlushPoC_DcacheRegion(u8_area, OFF_DATA);
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((u8_read(HC_UTRLDBR) & 1) != 0) {
		return EBUSY;
	}
	u8_write(HC_UTRLDBR, 1);
	for (tries = 0; tries < 20000 && (u8_read(HC_UTRLDBR) & 1) != 0; tries++) {
		delay(100);
	}
	if ((u8_read(HC_UTRLDBR) & 1) != 0) {
		u8_write(HC_UTRLCLR, ~1u);
		delay(1000);
		return ETIMEDOUT;
	}
	u8_write(HC_IS, u8_read(HC_IS) & 1);
	FlushPoC_DcacheRegion(u8_area, OFF_DATA);
	ocs = utrd[2] & 0xff;
	for (int i = 0; i < 32; i++) {
		response[i] = ((volatile uint8_t *)(u8_area + OFF_UCD + UCD_RESPONSE))[i];
	}
	return ocs == 0 ? 0 : EIO;
}

/* the device after its reset: NOP OUT until it answers, fDeviceInit set and waited for (ufshcd_probe_hba's device init) */
static int
u8_device_init(void)
{
	uint8_t upiu[32] = { 0 }, resp[32];
	int tries, err = EIO;

	for (tries = 0; tries < 50; tries++) {
		bzero(upiu, sizeof(upiu));
		err = u8_devman(upiu, resp);
		if (err == 0 && resp[0] == 0x20) {
			break;
		}
		if (err == EIO || err == ETIMEDOUT) {
			u8_say("NOP OUT: error %d, IS %08x\n", err, u8_read(HC_IS));
			u8_write(HC_IS, u8_read(HC_IS));
		}
		delay(100000);
	}
	u8_say("NOP OUT after %d tries: error %d, response %02x %02x %02x %02x\n", tries, err, resp[0], resp[1], resp[2], resp[3]);
	if (err != 0) {
		return -1;
	}
	bzero(upiu, sizeof(upiu));
	upiu[0] = 0x16;
	upiu[5] = 0x81;
	upiu[12] = 0x06;                /* SET FLAG */
	upiu[13] = 0x01;                /* fDeviceInit */
	err = u8_devman(upiu, resp);
	u8_say("SET FLAG fDeviceInit: error %d, response %02x %02x %02x %02x .. %02x %02x %02x %02x\n", err, resp[0], resp[1], resp[2], resp[3],
	    resp[12], resp[13], resp[14], resp[15]);
	for (tries = 0; err == 0 && tries < 600; tries++) {
		bzero(upiu, sizeof(upiu));
		upiu[0] = 0x16;
		upiu[5] = 0x01;
		upiu[12] = 0x05;                /* READ FLAG */
		upiu[13] = 0x01;
		err = u8_devman(upiu, resp);
		if (err == 0 && (resp[19] & 1) == 0) {
			u8_say("fDeviceInit cleared after %d reads\n", tries);
			return 0;
		}
		delay(5000);
	}
	u8_say("fDeviceInit: error %d after %d reads (flag %02x)\n", err, tries, resp[19]);
	return -1;
}

/* the UFS_RESET pin: low for 100 us, high again (ufshcd_reset_device) */
static void
u8_device_reset(void)
{
	volatile uint32_t *pin = (volatile uint32_t *)ml_io_map(TLMM_UFS_RESET, 0x1000);

	u8_say("UFS_RESET pin: ctl %08x io %08x\n", pin[0], pin[1]);
	pin[1] = pin[1] & ~1u;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(100);
	pin[1] = pin[1] | 1u;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(100);
	u8_say("UFS_RESET pin pulsed: io %08x\n", pin[1]);
}

/*
 * ufs8=4: read only, writes nothing: the PHY's and the controller's registers as ABL left them.
 * ufs8=5: the controller reset and linked again (HCE 0 -> 1, DME, link startup), the PHY as it is; then TEST UNIT READY / the GPT.
 * ufs8=6: the same with the PHY brought up again (the calibration tables, serdes).
 * ufs8=7: the device reset first (the UFS_RESET pin), then as 6, then NOP OUT and fDeviceInit: the cold start the Android driver does.
 * Each leaves the controller as it brought it up; nothing is put back (the phone's stock bootloader starts the storage afresh at its next boot).
 */
/*
 * ufs8=8: the hypervisor took single-stream match registers of the camera's (0x800, 0x840, 0xc00, 0xc40: the even ones) and refused masked ones and
 * the odd streams. A UFS stream entry is looked for the same way: single-stream registers for candidate stream numbers, S2CR bypass; each read back;
 * then the read in ABL's slot with whatever was accepted; everything put back.
 */
static void
u8_smmu_singles(void)
{
	volatile uint32_t *smmu = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x2000);
	static const uint32_t streams[] = { 0x0e0, 0x0e1, 0x100, 0x101, 0x0a0, 0x0a1, 0x060, 0x061, 0x0c0, 0x120, 0x140, 0x160 };
	uint32_t used[16], saved_smr[16], saved_s2cr[16], nused = 0, n = smmu[0x20 / 4] & 0xff, i = 8;

	for (unsigned k = 0; k < sizeof(streams) / sizeof(streams[0]); k++) {
		while (i < n && (smmu[(0x800 + 4 * i) / 4] >> 31) != 0) {
			i++;
		}
		if (i >= n || 0x800 + 4 * i >= 0xc00) {
			break;
		}
		saved_smr[nused] = smmu[(0x800 + 4 * i) / 4];
		saved_s2cr[nused] = smmu[(0xc00 + 4 * i) / 4];
		smmu[(0xc00 + 4 * i) / 4] = 1u << 16;
		smmu[(0x800 + 4 * i) / 4] = 1u << 31 | streams[k];
		__asm__ volatile ("dsb sy" ::: "memory");
		u8_say("single %2u: stream %04x: smr reads %08x, s2cr %08x -> %s\n", i, streams[k], smmu[(0x800 + 4 * i) / 4], smmu[(0xc00 + 4 * i) / 4],
		    (smmu[(0x800 + 4 * i) / 4] >> 31) ? "ACCEPTED" : "refused");
		used[nused++] = i;
		i++;
	}
	if (u8_abl()) {
		u8_say("THE READ WORKS with the accepted entries\n");
	}
	for (unsigned k = 0; k < nused; k++) {
		smmu[(0x800 + 4 * used[k]) / 4] = saved_smr[k];
		smmu[(0xc00 + 4 * used[k]) / 4] = saved_s2cr[k];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* TEST UNIT READY until the unit answers GOOD: the first commands after a link start report a unit attention (CHECK CONDITION) once each */
static int
u8_ready(void)
{
	uint8_t cdb[16] = { 0 }, status = 0xff;
	int error = EIO;

	for (int i = 0; i < 6; i++) {
		status = 0xff;
		error = u8_command(0, cdb, 0, false, &status);
		u8_say("TEST UNIT READY of LUN 0, try %d: error %d, SCSI status %02x, IS %08x\n", i, error, status, u8_read(HC_IS));
		if (error == 0 && status == 0) {
			return 0;
		}
		if (error != 0) {
			break;
		}
		if (i == 0) {
			uint8_t ssu[16] = { 0 }, st = 0xff;

			ssu[0] = 0x1b;
			ssu[4] = 0x10;
			error = u8_command(0xd0, ssu, 0, false, &st);
			u8_say("START STOP UNIT (power condition ACTIVE) of the device unit: error %d, SCSI status %02x\n", error, st);
			ssu[4] = 0x01;
			st = 0xff;
			error = u8_command(0, ssu, 0, false, &st);
			u8_say("START STOP UNIT (start) of LUN 0: error %d, SCSI status %02x\n", error, st);
		}
		delay(20000);
	}
	return -1;
}

/*
 * ufs8=9: the UFS stream through a context of ours. Mode 8 showed the hypervisor takes single-stream match registers for 0xe0 (the stream the
 * mainline device tree gives the UFS controller), 0xa0, 0xa1 and 0xc0, but keeps the S2CR it wrote itself (type 2: fault; a bypass type is not
 * taken, a context is -- as the camera's streams showed). So, as the camera driver does: a context bank with page tables of ours (the area's
 * 18 pages at IOVA 0x40000000), stream 0xe0 sent through it; the controller is then given IOVAs. Then as ufs8=1: TEST UNIT READY, READ CAPACITY,
 * the GPT; ABL's list put back.
 */
#define U9_IOVA         0x40000000ULL
#define U9_PTE          (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

static bool
u8_smmu_map(volatile uint32_t *smmu, uint32_t *match, uint32_t *context, uint32_t *saved_smr, uint32_t *saved_s2cr)
{
	uint32_t idr1 = smmu[0x24 / 4], n = smmu[0x20 / 4] & 0xff, contexts = idr1 & 0xff, pages = 2u << (idr1 >> 28 & 7), ctx = 6, m = 8;
	volatile uint32_t *second = smmu + 0x1000 / 4;
	volatile uint32_t *cb;
	uint64_t *root = (uint64_t *)u8_tables, *sec = (uint64_t *)(u8_tables + 4096), *third = (uint64_t *)(u8_tables + 8192), root_pa;
	bool used[256] = { false };

	for (uint32_t i = 0; i < n && 0x800 + 4 * i < 0xc00; i++) {
		if ((smmu[(0x800 + 4 * i) / 4] >> 31) != 0) {
			used[smmu[(0xc00 + 4 * i) / 4] & 0xff] = true;
		}
	}
	while (ctx < contexts && used[ctx]) {
		ctx++;
	}
	while (m < n && (smmu[(0x800 + 4 * m) / 4] >> 31) != 0) {
		m++;
	}
	if (ctx >= contexts || m >= n) {
		u8_say("no free context (%u) or match register (%u)\n", ctx, m);
		return false;
	}
	cb = (volatile uint32_t *)ml_io_map(SMMU_PHYS + (uint64_t)(pages + ctx) * 0x1000, 0x1000);
	bzero(u8_tables, sizeof(u8_tables));
	root[(U9_IOVA >> 30) & 0x1ff] = kvtophys((vm_offset_t)sec) | 3;
	sec[(U9_IOVA >> 21) & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (uint32_t q = 0; q < OFF_DATA / 4096; q++) {
		third[q] = kvtophys(u8_area + 4096ul * q) | U9_PTE;
	}
	for (uint32_t q = 0; q < U8_DATA_BYTES / 4096; q++) {
		third[OFF_DATA / 4096 + q] = kvtophys((vm_offset_t)U8_DATA + 4096ul * q) | U9_PTE;
	}
	for (vm_offset_t line = (vm_offset_t)u8_tables; line < (vm_offset_t)u8_tables + sizeof(u8_tables); line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	root_pa = kvtophys((vm_offset_t)root) | (uint64_t)ctx << 48;
	u8_say("SMMU: context %u (%s), tables at %llx, the area's %u pages at IOVA %llx; match register %u\n", ctx, (cb[0] & 1) ? "SWITCHED ON" : "off",
	    kvtophys((vm_offset_t)root), OFF_DATA / 4096 + U8_DATA_BYTES / 4096, U9_IOVA, m);
	cb[0x00 / 4] = 0;
	second[(0x800 + 4 * ctx) / 4] = 1;
	second[(4 * ctx) / 4] = (1u << 16 | 3u << 8 | 0xfu << 12);
	cb[0x10 / 4] = (1u | 1u << 4 | 7u << 15);
	cb[0x30 / 4] = (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23);
	cb[0x20 / 4] = (uint32_t)root_pa;
	cb[0x24 / 4] = (uint32_t)(root_pa >> 32);
	cb[0x28 / 4] = 0;
	cb[0x2c / 4] = 0;
	cb[0x38 / 4] = 0xf404ff44;
	cb[0x3c / 4] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("context registers read back: CBAR %08x CBA2R %08x TCR %08x TTBR0 %08x:%08x MAIR0 %08x\n", second[(4 * ctx) / 4], second[(0x800 + 4 * ctx) / 4],
	    cb[0x30 / 4], cb[0x24 / 4], cb[0x20 / 4], cb[0x38 / 4]);
	cb[0x58 / 4] = 0xc00001fe;
	cb[0x610 / 4] = ctx;
	cb[0x7f0 / 4] = 1;
	for (int w = 0; w < 1000 && (cb[0x7f4 / 4] & 1) != 0; w++) {
		delay(10);
	}
	cb[0x00 / 4] = (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12);
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("context SCTLR reads %08x\n", cb[0]);
	*saved_smr = smmu[(0x800 + 4 * m) / 4];
	*saved_s2cr = smmu[(0xc00 + 4 * m) / 4];
	smmu[(0xc00 + 4 * m) / 4] = ctx;
	smmu[(0x800 + 4 * m) / 4] = 1u << 31 | 0xe0;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("stream 0xe0 -> context %u: smr reads %08x, s2cr reads %08x\n", ctx, smmu[(0x800 + 4 * m) / 4], smmu[(0xc00 + 4 * m) / 4]);
	*match = m;
	*context = ctx;
	u8_dma_base = U9_IOVA;
	return (smmu[(0x800 + 4 * m) / 4] >> 31) != 0 && (smmu[(0xc00 + 4 * m) / 4] & 0xff) == ctx;
}

static void
u8_via_smmu(void)
{
	volatile uint32_t *smmu = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x2000);
	uint8_t cdb[16] = { 0 }, status = 0xff;
	uint32_t m = 0, ctx = 0, saved_smr = 0, saved_s2cr = 0, pages = 2u << ((smmu[0x24 / 4] >> 28) & 7);
	volatile uint32_t *cb;
	int error;

	u8_area = (vm_offset_t)u8_static;
	u8_area_pa = kvtophys(u8_area);
	u8_smmu();
	if (!u8_smmu_map(smmu, &m, &ctx, &saved_smr, &saved_s2cr)) {
		u8_say("the stream entry was not taken: nothing more is done\n");
		u8_dma_base = 0;
		if (m != 0) {           /* (0: it failed before a match register was chosen) */
			smmu[(0x800 + 4 * m) / 4] = saved_smr;
			smmu[(0xc00 + 4 * m) / 4] = saved_s2cr;
		}
		return;
	}
	cb = (volatile uint32_t *)ml_io_map(SMMU_PHYS + (uint64_t)(pages + ctx) * 0x1000, 0x1000);
	if (u8_swap_in() == 0) {
		error = u8_ready();
		if (error == 0) {
			bzero(cdb, sizeof(cdb));
			cdb[0] = 0x25;
			status = 0xff;
			error = u8_command(0, cdb, 8, true, &status);
			if (error == 0 && status == 0) {
				const uint8_t *d = U8_DATA;

				u8_say("READ CAPACITY(10) of LUN 0: last block %u, block size %u\n",
				    (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3],
				    (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7]);
				u8_gpt();
			} else {
				u8_say("READ CAPACITY(10): error %d, status %02x\n", error, status);
			}
		}
		u8_say("context fault status %08x, global %08x\n", cb[0x58 / 4], smmu[0x48 / 4]);
		u8_swap_out();
	}
	u8_dma_base = 0;
	smmu[(0x800 + 4 * m) / 4] = saved_smr;
	smmu[(0xc00 + 4 * m) / 4] = saved_s2cr;
	cb[0] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_say("the SMMU put back\n");
}

static void
u8_bringup(uint32_t mode)
{
	uint8_t cdb[16] = { 0 }, status = 0xff;
	int error;

	u8_phy = (volatile uint32_t *)ml_io_map(0x1d87000UL, 0x1000);
	if (mode == 4) {
		u8_say("PHY: START %08x POWER_DOWN %08x SW_RESET %08x LINECFG %08x PCS_READY %08x\n", u8_phy[PHY_START / 4], u8_phy[(PHY_START + 4) / 4],
		    u8_phy[PHY_SW_RESET / 4], u8_phy[PHY_LINECFG_DIS / 4], u8_phy[PHY_PCS_READY / 4]);
		for (uint32_t o = 0xc0; o < 0x100; o += 16) {
			u8_say("HC %03x: %08x %08x %08x %08x\n", o, u8_read(o), u8_read(o + 4), u8_read(o + 8), u8_read(o + 12));
		}
		for (uint32_t o = 0; o < 0x40; o += 16) {
			u8_say("PHY %03x: %08x %08x %08x %08x\n", 0xc00 + o, u8_phy[(0xc00 + o) / 4], u8_phy[(0xc04 + o) / 4], u8_phy[(0xc08 + o) / 4],
			    u8_phy[(0xc0c + o) / 4]);
		}
		return;
	}
	u8_area = (vm_offset_t)u8_static;
	u8_area_pa = kvtophys(u8_area);
	u8_say("our area at %llx\n", u8_area_pa);
	if (mode == 7) {
		u8_device_reset();
		delay(10000);
	}
	if (u8_hba_enable(mode >= 6) != 0) {
		u8_say("the controller did not come up\n");
		return;
	}
	if (u8_link_startup() != 0) {
		u8_say("no link\n");
		return;
	}
	u8_operational();
	if (mode == 7 && u8_device_init() != 0) {
		return;
	}
	error = u8_command(0, cdb, 0, false, &status);
	u8_say("TEST UNIT READY of LUN 0: error %d, SCSI status %02x, IS %08x\n", error, status, u8_read(HC_IS));
	if (error == 0) {
		bzero(cdb, sizeof(cdb));
		cdb[0] = 0x25;
		status = 0xff;
		error = u8_command(0, cdb, 8, true, &status);
		if (error == 0 && status == 0) {
			const uint8_t *d = U8_DATA;

			u8_say("READ CAPACITY(10) of LUN 0: last block %u, block size %u\n",
			    (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3],
			    (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7]);
			u8_gpt();
		} else {
			u8_say("READ CAPACITY(10): error %d, status %02x\n", error, status);
		}
	}
}

void
instantnoodle_ufs8_run(uint32_t u8_mode)
{
	volatile uint32_t *smmu = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x1000);
	uint32_t cap, ver, hcs, hce, is, db, rsr;
	uint8_t cdb[16] = { 0 }, status = 0xff;
	int error;

	u8_hc = (volatile uint32_t *)ml_io_map(HC_PHYS, HC_SIZE);
	cap = u8_read(HC_CAP);
	ver = u8_read(HC_VER);
	hcs = u8_read(HC_HCS);
	hce = u8_read(HC_HCE);
	is = u8_read(HC_IS);
	db = u8_read(HC_UTRLDBR);
	rsr = u8_read(HC_UTRLRSR);
	u8_say("controller CAP %08x VER %08x HCE %08x HCS %08x IS %08x list %08x:%08x run %u doorbell %08x\n", cap, ver, hce, hcs, is,
	    u8_read(HC_UTRLBAU), u8_read(HC_UTRLBA), rsr & 1, db);
	if ((hce & 1) == 0 || (hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY) || (rsr & 1) == 0 || db != 0 ||
	    CAP_NUTRS(cap) < 32 || (cap & CAP_64AS) == 0 || (is & IS_FATAL) != 0 || ver < 0x200 || ver >= 0x400) {
		u8_say("not as expected: nothing is done\n");
		return;
	}
	if (u8_mode == 9) {
		u8_via_smmu();
		return;
	}
	if (u8_mode == 8) {
		u8_smmu();
		u8_smmu_singles();
		return;
	}
	if (u8_mode >= 4 && u8_mode <= 7) {
		u8_bringup(u8_mode);
		return;
	}
	if (u8_mode == 2 || u8_mode == 3) {
		u8_smmu();
		u8_clocks(u8_mode == 3);
		if (u8_mode == 3) {
			u8_smmu_fix(smmu);
		} else {
			u8_abl();
		}
		return;
	}
	/*
	 * ufs8pa=ADDR: the area in memory of the loader's below 4 GB (the first try, kernel memory at 0x192140000, ended in IS bit 17, the bus
	 * fatal error, with the descriptor never fetched: the UFS DMA may reach only the low 4 GB, or not our memory); the kernel's own memory
	 * otherwise.
	 */
	if (PE_parse_boot_argn("ufs8pa", &u8_area_pa, sizeof(u8_area_pa)) && u8_area_pa != 0) {
		if ((u8_area_pa & PAGE_MASK) != 0 || pmap_valid_address(u8_area_pa) || pmap_valid_address(u8_area_pa + AREA_BYTES - 1)) {
			u8_say("ufs8pa %llx is not page aligned or is memory this kernel manages: nothing is done\n", u8_area_pa);
			return;
		}
		u8_area = ml_io_map_unmappable((vm_offset_t)u8_area_pa, AREA_BYTES, VM_WIMG_WCOMB);
		bzero((void *)u8_area, AREA_BYTES);
	} else {
		/* the kernel image itself is in the loader's memory below 4 GB (pa f7004000..): a static array of the image is a place the DMA may reach */
		u8_area = (vm_offset_t)u8_static;
		u8_area_pa = kvtophys(u8_area);
		if (u8_area_pa >> 32 != 0) {
			u8_say("the static area is at %llx, not below 4 GB\n", u8_area_pa);
		}
	}
	u8_say("our area at %llx (%u KB); SMMU global fault status %08x (syndrome %08x %08x %08x)\n", u8_area_pa, AREA_BYTES >> 10,
	    smmu[0x48 / 4], smmu[0x50 / 4], smmu[0x54 / 4], smmu[0x58 / 4]);
	if (u8_swap_in() != 0) {
		u8_swap_out();
		return;
	}
	error = u8_command(0, cdb, 0, false, &status);
	u8_say("TEST UNIT READY of LUN 0: error %d, SCSI status %02x\n", error, status);
	if (error == 0) {
		bzero(cdb, sizeof(cdb));
		cdb[0] = 0x25;
		status = 0xff;
		error = u8_command(0, cdb, 8, true, &status);
		if (error == 0 && status == 0) {
			const uint8_t *d = U8_DATA;

			u8_say("READ CAPACITY(10) of LUN 0: last block %u, block size %u\n",
			    (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3],
			    (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7]);
			u8_gpt();
		} else {
			u8_say("READ CAPACITY(10): error %d, status %02x\n", error, status);
		}
	}
	u8_say("SMMU global fault status now %08x (syndrome %08x %08x %08x), IS %08x\n", smmu[0x48 / 4], smmu[0x50 / 4], smmu[0x54 / 4],
	    smmu[0x58 / 4], u8_read(HC_IS));
	u8_swap_out();
}

/* ---- the backend: the storage for hk_ufs.c (ufs_8pro=1), read only ---- */

/*
 * What `ufs8=9` proved (the SMMU context for stream 0xe0, our list in the warm controller, START STOP UNIT) kept up for the whole run: the controller's
 * list is ours, a 1 MB data buffer of the kernel's mapped page by page behind the IOVAs, and reads of any alignment answered one command at a time
 * (READ(10), at most 256 blocks of 4 KiB). Still read only: u8_refusal() has no write in it. instantnoodle_ufs_down() puts ABL's list and the SMMU back
 * (hk_ufs_quiesce, before the kernel gives the phone back).
 */
extern lck_mtx_t hk_smmu_lock;
static LCK_GRP_DECLARE(u8_grp, "instantnoodle-ufs");
static LCK_MTX_DECLARE(u8_mtx, &u8_grp);
static bool u8_backend;
static uint32_t u8_back_match, u8_back_ctx, u8_back_smr, u8_back_s2cr;
static volatile uint32_t *u8_smmu_va, *u8_cb_va;      /* mapped once at the start: instantnoodle_ufs_down() maps nothing */
static uint64_t u8_back_blocks;
static uint64_t u8_back_reads, u8_back_bytes, u8_back_errors, u8_back_wbytes;
extern int get_system_inshutdown(void);
static uint32_t u8_shut_lines;                  /* what the backend does while the system shuts down (a hang there is looked for in the log): the first 300 requests */

int
instantnoodle_ufs_up(void)
{
	volatile uint32_t *smmu = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x2000);
	uint32_t cap, ver, hcs, hce, is, db, rsr, pages;
	uint8_t cdb[16] = { 0 }, status = 0xff;
	vm_offset_t big = 0;
	bool mapped;

	if (u8_backend) {
		return 0;
	}
	u8_hc = (volatile uint32_t *)ml_io_map(HC_PHYS, HC_SIZE);
	cap = u8_read(HC_CAP);
	ver = u8_read(HC_VER);
	hcs = u8_read(HC_HCS);
	hce = u8_read(HC_HCE);
	is = u8_read(HC_IS);
	db = u8_read(HC_UTRLDBR);
	rsr = u8_read(HC_UTRLRSR);
	if ((hce & 1) == 0 || (hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY) || (rsr & 1) == 0 || db != 0 ||
	    CAP_NUTRS(cap) < 32 || (cap & CAP_64AS) == 0 || (is & IS_FATAL) != 0 || ver < 0x200 || ver >= 0x400) {
		u8_say("backend: the controller is not as expected (CAP %08x VER %08x HCE %08x HCS %08x IS %08x run %u doorbell %08x)\n", cap, ver, hce, hcs, is, rsr & 1, db);
		return ENXIO;
	}
	if (kmem_alloc(kernel_map, &big, 1u << 20, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		u8_say("backend: no memory for the data buffer\n");
		return ENOMEM;
	}
	u8_big = (uint8_t *)big;
	u8_big_bytes = 1u << 20;
	u8_area = (vm_offset_t)u8_static;
	u8_area_pa = kvtophys(u8_area);
	pages = 2u << ((smmu[0x24 / 4] >> 28) & 7);
	(void)pages;
	lck_mtx_lock(&hk_smmu_lock);
	mapped = u8_smmu_map(smmu, &u8_back_match, &u8_back_ctx, &u8_back_smr, &u8_back_s2cr);
	lck_mtx_unlock(&hk_smmu_lock);
	if (!mapped) {
		u8_say("backend: the stream entry was not taken\n");
		u8_dma_base = 0;
		if (u8_back_match != 0) {
			smmu[(0x800 + 4 * u8_back_match) / 4] = u8_back_smr;
			smmu[(0xc00 + 4 * u8_back_match) / 4] = u8_back_s2cr;
		}
		u8_big = NULL;
		kmem_free(kernel_map, big, 1u << 20);
		return ENXIO;
	}
	if (u8_swap_in() != 0 || u8_ready() != 0) {
		u8_say("backend: the unit did not come ready\n");
		if (u8_in) {
			u8_swap_out();
		}
		u8_dma_base = 0;
		smmu[(0x800 + 4 * u8_back_match) / 4] = u8_back_smr;
		smmu[(0xc00 + 4 * u8_back_match) / 4] = u8_back_s2cr;
		u8_big = NULL;
		kmem_free(kernel_map, big, 1u << 20);
		return EIO;
	}
	cdb[0] = 0x25;
	if (u8_command(0, cdb, 8, true, &status) == 0 && status == 0) {
		const uint8_t *d = U8_DATA;

		u8_back_blocks = ((uint64_t)((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3])) + 1;
	}
	pages = 2u << ((smmu[0x24 / 4] >> 28) & 7);
	u8_smmu_va = smmu;
	u8_cb_va = (volatile uint32_t *)ml_io_map(SMMU_PHYS + (uint64_t)(pages + u8_back_ctx) * 0x1000, 0x1000);
	u8_backend = true;
	u8_say("backend: up: LUN 0 has %llu blocks of 4096 bytes (%llu MB); stream 0xe0 -> context %u (match register %u); data buffer %u KB\n", u8_back_blocks,
	    u8_back_blocks * BLOCK >> 20, u8_back_ctx, u8_back_match, u8_big_bytes >> 10);
	return 0;
}

/* bytes [offset, offset + len) of logical unit 0, whatever their alignment */
int
instantnoodle_ufs_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	int error = 0;

	/* LUN 4 (the boot and firmware partitions) and LUN 5 (the modem's EFS partitions), read only here: the modem's firmware and EFS, bsd/dev/instantnoodle_modem.c; not bounded by LUN 0's size */
	if (!u8_backend || (lun != 0 && lun != 4 && lun != 5) || (lun == 0 && u8_back_blocks != 0 && (offset + len + BLOCK - 1) / BLOCK > u8_back_blocks)) {
		return ENXIO;
	}
	if (get_system_inshutdown() && u8_shut_lines < 300) {
		u8_shut_lines++;
		u8_say("shutdown: read %llu bytes at %llu\n", len, offset);
	}
	lck_mtx_lock(&u8_mtx);
	while (len > 0 && error == 0) {
		uint64_t first = offset / BLOCK, skip = offset % BLOCK, count = (skip + len + BLOCK - 1) / BLOCK, take;
		uint8_t cdb[16] = { 0 }, status = 0xff;

		if (count > u8_big_bytes / BLOCK) {
			count = u8_big_bytes / BLOCK;
		}
		take = count * BLOCK - skip;
		if (take > len) {
			take = len;
		}
		cdb[0] = 0x28;
		cdb[2] = (uint8_t)(first >> 24);
		cdb[3] = (uint8_t)(first >> 16);
		cdb[4] = (uint8_t)(first >> 8);
		cdb[5] = (uint8_t)first;
		cdb[7] = (uint8_t)(count >> 8);
		cdb[8] = (uint8_t)count;
		error = first >> 32 != 0 ? EINVAL : u8_command(lun, cdb, (uint32_t)(count * BLOCK), true, &status);
		if (error == 0 && status != 0) {
			error = EIO;
		}
		if (error == 0) {
			memcpy(dst, U8_DATA + skip, (size_t)take);
			offset += take;
			dst += take;
			len -= take;
			u8_back_bytes += take;
		}
	}
	u8_back_reads++;
	if (error != 0) {
		u8_back_errors++;
	}
	lck_mtx_unlock(&u8_mtx);
	return error;
}

/*
 * The one window that takes writes (hk_ufs.c, ufs_8pro=1 with ufs_write=1, after it checked that the partition is the port's own: named darwin, an APFS
 * container's type GUID, on LUN 0, big enough): blocks [first, end) of LUN 0. Once set it is not widened (a second call only narrows it).
 */
int
instantnoodle_ufs_write_window(uint64_t first, uint64_t end)
{
	if (!u8_backend || first >= end || end > u8_back_blocks || (u8_wr_hi != 0 && (first < u8_wr_lo || end > u8_wr_hi))) {
		return EINVAL;
	}
	u8_wr_lo = first;
	u8_wr_hi = end;
	u8_say("backend: writes are taken for blocks %llu..%llu of LUN 0 (whole blocks, FUA, read back), nothing else\n", first, end - 1);
	return 0;
}

/*
 * Whole blocks [offset, offset + len) of LUN 0 from src, a bufferful at a time: WRITE(10) with FUA (the data is on the flash when the command
 * is answered), then the same blocks read back and compared. u8_refusal() holds the window; a write outside it is refused there, whatever
 * calls this.
 */
int
instantnoodle_ufs_write(uint8_t lun, uint64_t offset, const uint8_t *src, uint64_t len)
{
	int error = 0;

	if (!u8_backend || lun != 0 || offset % BLOCK != 0 || len % BLOCK != 0 || len == 0) {
		return EINVAL;
	}
	if (get_system_inshutdown()) {
		static uint64_t writes, bytes;

		writes++;
		bytes += len;
		if (u8_shut_lines < 40 || writes % 200 == 0) {
			u8_shut_lines++;
			u8_say("shutdown: write %llu bytes at %llu (%llu writes, %llu KB so far)\n", len, offset, writes, bytes >> 10);
		}
	}
	lck_mtx_lock(&u8_mtx);
	while (len > 0 && error == 0) {
		uint64_t first = offset / BLOCK, count = len / BLOCK, take;
		uint8_t cdb[16] = { 0 }, status = 0xff;

		if (count > u8_big_bytes / BLOCK) {
			count = u8_big_bytes / BLOCK;
		}
		take = count * BLOCK;
		memcpy(U8_DATA, src, (size_t)take);
		cdb[0] = 0x2a;
		cdb[1] = 0x08;          /* FUA */
		cdb[2] = (uint8_t)(first >> 24);
		cdb[3] = (uint8_t)(first >> 16);
		cdb[4] = (uint8_t)(first >> 8);
		cdb[5] = (uint8_t)first;
		cdb[7] = (uint8_t)(count >> 8);
		cdb[8] = (uint8_t)count;
		error = first >> 32 != 0 ? EINVAL : u8_command2(0, cdb, (uint32_t)take, 2, &status);
		if (error == 0 && status != 0) {
			error = EIO;
		}
		if (error == 0) {
			bzero(cdb, sizeof(cdb));
			cdb[0] = 0x28;
			cdb[2] = (uint8_t)(first >> 24);
			cdb[3] = (uint8_t)(first >> 16);
			cdb[4] = (uint8_t)(first >> 8);
			cdb[5] = (uint8_t)first;
			cdb[7] = (uint8_t)(count >> 8);
			cdb[8] = (uint8_t)count;
			status = 0xff;
			error = u8_command(0, cdb, (uint32_t)take, true, &status);
			if (error == 0 && (status != 0 || memcmp(U8_DATA, src, (size_t)take) != 0)) {
				u8_say("backend: the blocks written at %llu did not read back the same\n", first);
				error = EIO;
			}
		}
		if (error == 0) {
			offset += take;
			src += take;
			len -= take;
			u8_back_wbytes += take;
		}
	}
	if (error != 0) {
		u8_back_errors++;
	}
	lck_mtx_unlock(&u8_mtx);
	return error;
}

/*
 * The standalone boot menu's "Android of the other slot" (loader: hk_swap_slot=1 in the command line): the A/B bytes of boot_a and boot_b exchanged in the GPT, so that the other slot is the
 * active one when the phone restarts. The loader cannot do it (the UEFI of this port has no block device: 148 handles, none of them storage; owner's log of 2026-10-28), the kernel can:
 * boot_a and boot_b are not on LUN 0, whose GPT the rest of this file reads, but on LUN 4 (sde), which this reads on its own, a block at a time, and writes only here.
 * Careful all the way: LUN 4 must have blocks of 4 KiB; both copies of its GPT must be sound and the same (header and table CRC-32s), exactly one of the two slots active, the two entries found
 * by name, and the active one must hold HackiOS's own boot image (board name "HACKiOS" in the Android header: started any other way, nothing is written); only the attribute byte of those two
 * entries changes; the backup copy is written before the primary one (the table before its header in each), every block is read back as it is written, and a write goes through the same gate as every
 * other (u8_refusal()): the window is the one block being written, on LUN 4, for that write alone, and taken away again.
 */
#define AB_LUN  4

static uint32_t
u8_crc32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;

	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
		}
	}
	return ~crc;
}

static bool
u8_name_is(const uint8_t *e, const char *want)
{
	for (int i = 0; i < 36; i++) {
		uint16_t c = e[56 + 2 * i] | e[57 + 2 * i] << 8;

		if (c != (uint8_t)want[i]) {
			return false;
		}
		if (c == 0) {
			return true;
		}
	}
	return false;
}

static uint64_t u8_ab_blocks;           /* LUN 4's size in blocks of 4 KiB (READ CAPACITY), 0 until instantnoodle_ufs_ab_swap() has asked */

/* one block of LUN 4 */
static int
u8_ab_read(uint64_t lba, uint8_t *dst)
{
	uint8_t cdb[16] = { 0 }, status = 0xff;
	int error;

	if (!u8_backend || lba >= u8_ab_blocks || lba >> 32 != 0) {
		return EINVAL;
	}
	cdb[0] = 0x28;
	cdb[2] = (uint8_t)(lba >> 24);
	cdb[3] = (uint8_t)(lba >> 16);
	cdb[4] = (uint8_t)(lba >> 8);
	cdb[5] = (uint8_t)lba;
	cdb[8] = 1;
	lck_mtx_lock(&u8_mtx);
	error = u8_command(AB_LUN, cdb, BLOCK, true, &status);
	if (error == 0 && status != 0) {
		error = EIO;
	}
	if (error == 0) {
		memcpy(dst, U8_DATA, BLOCK);
	}
	lck_mtx_unlock(&u8_mtx);
	return error;
}

/* one block of LUN 4, written with FUA through the gate (the window: this block, on this LUN, for this write), read back and compared */
static int
u8_ab_write(uint64_t lba, const uint8_t *src)
{
	uint8_t cdb[16] = { 0 }, status = 0xff;
	int error;

	if (!u8_backend || lba >= u8_ab_blocks || lba >> 32 != 0) {
		return EINVAL;
	}
	cdb[0] = 0x2a;
	cdb[1] = 0x08;          /* FUA */
	cdb[2] = (uint8_t)(lba >> 24);
	cdb[3] = (uint8_t)(lba >> 16);
	cdb[4] = (uint8_t)(lba >> 8);
	cdb[5] = (uint8_t)lba;
	cdb[8] = 1;
	lck_mtx_lock(&u8_mtx);
	{
		uint64_t keep_lo = u8_wr_lo, keep_hi = u8_wr_hi;
		uint8_t keep_lun = u8_wr_lun;

		u8_wr_lo = lba;
		u8_wr_hi = lba + 1;
		u8_wr_lun = AB_LUN;
		memcpy(U8_DATA, src, BLOCK);
		error = u8_command2(AB_LUN, cdb, BLOCK, 2, &status);
		u8_wr_lo = keep_lo;
		u8_wr_hi = keep_hi;
		u8_wr_lun = keep_lun;
	}
	if (error == 0 && status != 0) {
		error = EIO;
	}
	if (error == 0) {
		bzero(cdb, sizeof(cdb));
		cdb[0] = 0x28;
		cdb[2] = (uint8_t)(lba >> 24);
		cdb[3] = (uint8_t)(lba >> 16);
		cdb[4] = (uint8_t)(lba >> 8);
		cdb[5] = (uint8_t)lba;
		cdb[8] = 1;
		status = 0xff;
		error = u8_command(AB_LUN, cdb, BLOCK, true, &status);
		if (error == 0 && (status != 0 || memcmp(U8_DATA, src, BLOCK) != 0)) {
			u8_say("ab: the block written at %llu did not read back the same\n", lba);
			error = EIO;
		}
	}
	lck_mtx_unlock(&u8_mtx);
	return error;
}

/* nblocks blocks of LUN 4 from lba */
static int
u8_ab_read_blocks(uint64_t lba, uint32_t nblocks, uint8_t *dst)
{
	for (uint32_t k = 0; k < nblocks; k++) {
		int error = u8_ab_read(lba + k, dst + (uint64_t)k * BLOCK);

		if (error != 0) {
			return error;
		}
	}
	return 0;
}

/* does the partition whose GPT entry is e hold HackiOS's own boot image: an Android boot image of header version 0..2 with the board name "HACKiOS" at byte 48 */
static bool
u8_ab_is_ours(const uint8_t *e)
{
	static uint8_t head[BLOCK];

	if (u8_ab_read(u8_le64(e + 32), head) != 0) {
		return false;
	}
	return memcmp(head, "ANDROID!", 8) == 0 && *(uint32_t *)(head + 40) <= 2 && memcmp(head + 48, "HACKiOS", 8) == 0;
}

int
instantnoodle_ufs_ab_swap(void)
{
	static uint8_t ph[BLOCK], bh[BLOCK], pt[4 * BLOCK], bt[4 * BLOCK], nt[4 * BLOCK], chk[4 * BLOCK];
	uint64_t entries_lba, alt, balt_entries;
	uint32_t count, size, bytes, nblocks, hsize, a = ~0u, b = ~0u, crc;
	uint8_t ba, bb;

	/*
	 * DISABLED 2026-10-28. Exchanging only the A/B attribute bytes of boot_a and boot_b leaves them disagreeing with the type GUIDs that ABL's
	 * `fastboot --set-active` toggles together with the attributes of every slotted partition; the phone then refused to boot (the OnePlus "image
	 * destroyed" screen on both slots) until its partition table was rebuilt over EDL. Nothing below runs; slots are changed with
	 * `fastboot --set-active=a|b`. A new version has to exchange attributes AND type GUIDs of all pairs, with an EDL undo ready.
	 */
	u8_say("ab: the slot exchange is disabled (it corrupted the A/B state on 2026-10-28): nothing written\n");
	if (1) {
		return EPERM;
	}
	if (!u8_backend) {
		return ENXIO;
	}
	{
		uint8_t cdb[16] = { 0 }, status = 0xff;
		int error = ENXIO;

		lck_mtx_lock(&u8_mtx);
		for (int tries = 0; tries < 3 && (error != 0 || status != 0); tries++) {
			status = 0xff;
			error = u8_command(AB_LUN, cdb, 0, false, &status);     /* TEST UNIT READY */
		}
		if (error == 0 && status == 0) {
			cdb[0] = 0x25;
			status = 0xff;
			error = u8_command(AB_LUN, cdb, 8, true, &status);
			if (error == 0 && status == 0) {
				const uint8_t *d = U8_DATA;
				uint32_t block = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];

				u8_ab_blocks = ((uint64_t)((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3])) + 1;
				u8_say("ab: LUN %d has %llu blocks of %u bytes\n", AB_LUN, u8_ab_blocks, block);
				if (block != BLOCK) {
					u8_ab_blocks = 0;
				}
			}
		}
		lck_mtx_unlock(&u8_mtx);
		if (u8_ab_blocks == 0) {
			u8_say("ab: LUN %d did not answer, or its blocks are not 4096 bytes (error %d, SCSI status %02x): nothing written\n", AB_LUN, error, status);
			return EIO;
		}
	}
	if (u8_ab_read(1, ph) != 0 || memcmp(ph, "EFI PART", 8) != 0) {
		u8_say("ab: no GPT header in block 1 of LUN %d\n", AB_LUN);
		return EINVAL;
	}
	hsize = *(uint32_t *)(ph + 12);
	entries_lba = u8_le64(ph + 72);
	alt = u8_le64(ph + 32);
	count = *(uint32_t *)(ph + 80);
	size = *(uint32_t *)(ph + 84);
	bytes = count * size;
	if (hsize < 92 || hsize > 512 || size < 128 || size > BLOCK || bytes == 0 || bytes > sizeof(pt) || alt >= u8_ab_blocks || entries_lba + (bytes + BLOCK - 1) / BLOCK > alt) {
		u8_say("ab: the GPT is not of a shape this knows (header %u, entries %u of %u bytes at %llu, backup at %llu of %llu blocks)\n", hsize, count, size, entries_lba, alt,
		    u8_ab_blocks);
		return EINVAL;
	}
	{
		uint8_t save[512];
		uint32_t want = *(uint32_t *)(ph + 16);

		memcpy(save, ph, 512);
		*(uint32_t *)(save + 16) = 0;
		if (u8_crc32(save, hsize) != want) {
			u8_say("ab: the primary header's CRC is wrong: nothing written\n");
			return EINVAL;
		}
	}
	nblocks = (bytes + BLOCK - 1) / BLOCK;
	if (u8_ab_read_blocks(entries_lba, nblocks, pt) != 0 || u8_crc32(pt, bytes) != *(uint32_t *)(ph + 88)) {
		u8_say("ab: the primary table's CRC is wrong: nothing written\n");
		return EINVAL;
	}
	if (u8_ab_read(alt, bh) != 0 || memcmp(bh, "EFI PART", 8) != 0 || u8_le64(bh + 24) != alt || u8_le64(bh + 32) != 1) {
		u8_say("ab: the backup header is missing\n");
		return EINVAL;
	}
	{
		uint8_t save[512];
		uint32_t want = *(uint32_t *)(bh + 16);

		memcpy(save, bh, 512);
		*(uint32_t *)(save + 16) = 0;
		if (u8_crc32(save, hsize) != want) {
			u8_say("ab: the backup header's CRC is wrong: nothing written\n");
			return EINVAL;
		}
	}
	balt_entries = u8_le64(bh + 72);
	if (balt_entries + nblocks > alt || u8_ab_read_blocks(balt_entries, nblocks, bt) != 0 || memcmp(pt, bt, bytes) != 0 ||
	    u8_crc32(bt, bytes) != *(uint32_t *)(bh + 88)) {
		u8_say("ab: the backup table is not the primary one: nothing written\n");
		return EINVAL;
	}
	for (uint32_t k = 0; k < count; k++) {
		const uint8_t *e = pt + (uint64_t)k * size;

		if (u8_name_is(e, "boot_a")) {
			a = a == ~0u ? k : ~0u - 1;
		}
		if (u8_name_is(e, "boot_b")) {
			b = b == ~0u ? k : ~0u - 1;
		}
	}
	if (a >= count || b >= count) {
		u8_say("ab: boot_a and boot_b not found exactly once\n");
		return EINVAL;
	}
	ba = pt[(uint64_t)a * size + 54];
	bb = pt[(uint64_t)b * size + 54];
	u8_say("ab: boot_a A/B byte %02x, boot_b %02x\n", ba, bb);
	if (((ba & 4) != 0) == ((bb & 4) != 0)) {
		u8_say("ab: not exactly one slot is active: nothing written\n");
		return EINVAL;
	}
	{
		bool ours_a = u8_ab_is_ours(pt + (uint64_t)a * size), ours_b = u8_ab_is_ours(pt + (uint64_t)b * size);

		if (ours_a == ours_b) {
			u8_say("ab: %s HackiOS's image: nothing written\n", ours_a ? "both boot partitions hold" : "neither boot partition holds");
			return EINVAL;
		}
		if (!((ours_a && (ba & 4)) || (ours_b && (bb & 4)))) {
			u8_say("ab: Android's slot is the active one already (HackiOS was not started from its own slot): nothing written, the restart starts Android\n");
			return 0;
		}
	}
	memcpy(nt, pt, bytes);
	nt[(uint64_t)a * size + 54] = bb;
	nt[(uint64_t)b * size + 54] = ba;
	crc = u8_crc32(nt, bytes);
	/* the backup copy first (its table, then its header), then the primary one */
	for (int pass = 0; pass < 2; pass++) {
		uint8_t *h = pass ? ph : bh;
		uint64_t hlba = pass ? 1 : alt, elba = pass ? entries_lba : balt_entries;

		*(uint32_t *)(h + 88) = crc;
		*(uint32_t *)(h + 16) = 0;
		*(uint32_t *)(h + 16) = u8_crc32(h, hsize);
		for (uint32_t k = 0; k < nblocks; k++) {
			if (u8_ab_write(elba + k, nt + (uint64_t)k * BLOCK) != 0) {
				u8_say("ab: the write of the table at block %llu failed (the %s copy)\n", elba + k, pass ? "primary" : "backup");
				return EIO;
			}
		}
		if (u8_ab_write(hlba, h) != 0) {
			u8_say("ab: the write of the header at block %llu failed (the %s copy)\n", hlba, pass ? "primary" : "backup");
			return EIO;
		}
	}
	if (u8_ab_read_blocks(entries_lba, nblocks, chk) != 0 || memcmp(chk, nt, bytes) != 0 || u8_ab_read_blocks(balt_entries, nblocks, chk) != 0 || memcmp(chk, nt, bytes) != 0) {
		u8_say("ab: read back does not match: the phone may need `fastboot --set-active`\n");
		return EIO;
	}
	u8_say("ab: slots exchanged: boot_a %02x, boot_b %02x\n", bb, ba);
	return 0;
}

void
instantnoodle_ufs_down(void)
{
	boolean_t locked;

	if (!u8_backend) {
		return;
	}
	u8_late = true;
	u8_say("backend: down: begins\n");
	locked = lck_mtx_try_lock(&u8_mtx);     /* (from hk_ufs_quiesce, the other processors stopped: a lock one of them holds is not waited for) */
	if (u8_in) {
		u8_swap_out();
	}
	u8_dma_base = 0;
	u8_smmu_va[(0x800 + 4 * u8_back_match) / 4] = u8_back_smr;
	u8_smmu_va[(0xc00 + 4 * u8_back_match) / 4] = u8_back_s2cr;
	u8_cb_va[0] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	u8_backend = false;
	u8_say("backend: down (%llu reads, %llu MB read, %llu MB written, %llu errors); ABL's list and the SMMU put back\n", u8_back_reads, u8_back_bytes >> 20, u8_back_wbytes >> 20, u8_back_errors);
	if (locked) {
		lck_mtx_unlock(&u8_mtx);
	}
}

/* LUN 0's GPT: the used entries, at most max; the number of them */
uint32_t
instantnoodle_ufs_gpt(struct instantnoodle_ufs_part *out, uint32_t max)
{
	static uint8_t header[BLOCK], entries[4 * BLOCK];
	uint32_t count, size, n = 0;
	uint64_t at;

	if (instantnoodle_ufs_read(0, BLOCK, header, BLOCK) != 0 || memcmp(header, "EFI PART", 8) != 0) {
		u8_say("backend: no GPT header in block 1\n");
		return 0;
	}
	at = u8_le64(header + 72);
	count = header[80] | header[81] << 8 | header[82] << 16 | (uint32_t)header[83] << 24;
	size = header[84] | header[85] << 8 | header[86] << 16 | (uint32_t)header[87] << 24;
	if (size < 128 || size > BLOCK || (uint64_t)count * size > sizeof(entries) || instantnoodle_ufs_read(0, at * BLOCK, entries, (uint64_t)count * size) != 0) {
		u8_say("backend: the GPT entries could not be read (%u of %u bytes)\n", count, size);
		return 0;
	}
	for (uint32_t i = 0; i < count && n < max; i++) {
		const uint8_t *e = entries + (uint64_t)i * size;
		bool used = false;
		uint32_t k;

		for (k = 0; k < 16; k++) {
			used = used || e[k] != 0;
		}
		if (!used) {
			continue;
		}
		for (k = 0; k < 35 && (e[56 + 2 * k] | e[57 + 2 * k]) != 0; k++) {
			out[n].name[k] = (char)e[56 + 2 * k];
		}
		out[n].name[k] = 0;
		memcpy(out[n].type, e, 16);
		out[n].first = u8_le64(e + 32);
		out[n].last = u8_le64(e + 40);
		n++;
	}
	return n;
}
