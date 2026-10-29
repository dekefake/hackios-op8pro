/*
 * OnePlus 8 Pro ("instantnoodlep"): the main camera's lens motor (autofocus).
 *
 * What it is [source: the module file /odm/lib64/camera/com.qti.sensormodule.semco_imx689.bin, read with the symbol parser of
 * tools/camsensor.py]: its "actuatorDriver" is named "imx689_ak7377" (an AKM AK7377 voice-coil driver), slave 0x18 in 8 bits = 0x0c on the
 * module's CCI master 0 bus (the one of the sensor, 0x1a, and the OIS controller, 0x24). The module's own OIS, an ON Semiconductor LC898128
 * ("ofilm_imx689_lc898128_ois"), moves the lens in X and Y; it has nothing to do with the focus and is NOT touched here (Android's driver
 * reprograms its flash when its firmware version differs: not something to do blind).
 *
 * Its registers [sources: the module file's actuator tables; the Linux ak7375 driver (drivers/media/i2c/ak7375.c), the same family, whose
 * constants these are; observed here: the dump of registers 0x00..0xff after power-up, `cam_actdump=1`: reg 0x02 = 0x20, reg 0x03 = 0x13,
 * all others 0x00]:
 *   0x02  mode: 0x20 sleep (what it reads after power-up), 0x40 standby, 0x00 active. Android's initSettings for this module: write 0x02 = 0x40,
 *         1 ms, write 0x02 = 0x00, 1 ms (and its deInitSettings are empty).
 *   0x00 and 0x01  the position, one 16-bit big-endian word written at register 0x00: the code left-justified (the module file's
 *         registerParam: 2 data bytes, register 0x00, shift 6, i.e. a 10-bit code << 6; the Linux driver's 12 bits << 4 is the same word). The
 *         module file's regions: step bound 0x3f6 = 1014 at the macro end, 0 at infinity. The VCM is open loop: no read-back of where the
 *         lens is, only of what was written.
 * Direction [observed on the phone, 2026-10-27, boot lens1: the userland's contrast sweep through this driver found a close scene sharp at
 * codes 48..276 and a far scene at 756 (sharpness 22.6), and the owner's "stuck at close focus" is the lens with no current]: code 0 (no
 * current, where the lens rests) is the CLOSEST focus, a higher code is further. iOS' positions (hk_camera.h, the userland's af_*) are the
 * other way, 0 furthest ... 1023 closest, so the code is LENS_MAX - position (held to LENS_MAX, which is under the module's own limit of
 * 1014: kept off the stop). Where exactly infinity lies (somewhere about 756..1000) is not measured; the scan (cam_lensscan=1) is meant to.
 *
 * Safety: the chip is read before anything is written (reg 0x02 has to be one of 0x20/0x40/0x00, the position 0), every step of the mode
 * sequence is read back, the first position written is read back, and any surprise leaves the lens driver off for the session (said in the
 * log). Positions are reached in small steps with a pause between them, as redwood's lens_* does: a lens thrown across its travel knocks
 * against its stop.
 *
 * Boot-args (all experiments, bsd/dev/instantnoodle_camera.c calls this from the /dev/hk-camera path only):
 *   cam_focus=N      the lens to iOS' position N (0 furthest .. 1000) once the camera is up (the viewfinder at that focus)
 *   cam_lensscan=1   a scan: after the first frames the lens goes 0..LENS_MAX in steps of 32 while the frames' sharpness (a gradient sum in the
 *                    middle of the picture, normalised by its level) is measured and logged ("lens scan: code C sharpness S": codes, 0 closest), then the sharpest
 *                    place is kept; the focus commands from iOS are ignored meanwhile. The poster at about 1 m: the peak is where it is sharp.
 *   cam_lensmax=N    LENS_MAX (the soft limit)
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/hk_camera.h>
#include <sys/instantnoodle.h>

extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

#define LENS_ADDRESS    0x0c
#define LENS_POSITION   0x00            /* and 0x01 */
#define LENS_MODE       0x02
#define LENS_SLEEP      0x20
#define LENS_STANDBY    0x40
#define LENS_ACTIVE     0x00
#define LENS_MAX_CODE   1014            /* the module file's macro step bound */
#define LENS_STEP       24              /* codes a step; 2 ms each: the whole travel in about 85 ms */
#define LENS_STEP_MS    2

LCK_GRP_DECLARE(lens_group, "hk-lens-8pro");
LCK_MTX_DECLARE(lens_lock, &lens_group);

static struct instantnoodle_i2c *lens_bus;
static bool lens_up, lens_dead, lens_checked;
static uint32_t lens_at;                /* the code last written */
static uint32_t lens_max = 1000;        /* the largest code, which is the furthest focus; iOS' position 0 */

static struct {
	bool on, done;
	unsigned frames, index, wait, measured;
	uint64_t gradient, level;
	uint32_t best_pos;
	uint64_t best, sharpness[40];
	uint32_t pos[40];
	unsigned count;
} scan;
#define SCAN_FIRST      90              /* frames before the scan starts (the exposure settles) */
#define SCAN_SETTLE     8               /* frames after a move before measuring */
#define SCAN_MEASURE    3
#define SCAN_STEP       32

static bool
lens_read(uint8_t reg, uint8_t *value)
{
	return instantnoodle_i2c_read(lens_bus, LENS_ADDRESS, reg, value, 1) == 0;
}

static bool
lens_write_mode(uint8_t mode)
{
	uint8_t out[2] = { LENS_MODE, mode };

	return instantnoodle_i2c_write(lens_bus, LENS_ADDRESS, out, 2) == 0;
}

static bool
lens_write_code(uint32_t code)
{
	uint16_t word = (uint16_t)(code << 6);
	uint8_t out[3] = { LENS_POSITION, (uint8_t)(word >> 8), (uint8_t)word };

	if (instantnoodle_i2c_write(lens_bus, LENS_ADDRESS, out, 3) == 0) {
		return true;
	}
	instantnoodle_i2c_recover(lens_bus);
	return instantnoodle_i2c_write(lens_bus, LENS_ADDRESS, out, 3) == 0;
}

static void
lens_fail(const char *what)
{
	say("camera: the lens motor (AK7377 at 0x0c): %s: no focus for the rest of this session\n", what);
	lens_dead = true;
	lens_up = false;
}

/* The lens to `target' a few codes at a time (the lock held). */
static void
lens_go(uint32_t target)
{
	while (lens_up && lens_at != target) {
		uint32_t next = lens_at < target ? (target - lens_at > LENS_STEP ? lens_at + LENS_STEP : target) :
		    (lens_at - target > LENS_STEP ? lens_at - LENS_STEP : target);

		if (!lens_write_code(next)) {
			lens_fail("a position write was not acknowledged");
			return;
		}
		lens_at = next;
		if (!lens_checked) {
			uint8_t high = 0xff, low = 0xff;

			lens_checked = true;
			if (!lens_read(LENS_POSITION, &high) || !lens_read(LENS_POSITION + 1, &low) ||
			    (uint32_t)(high << 2 | low >> 6) != next) {
				say("camera: the lens motor: position %u written, read back %02x %02x\n", next, high, low);
				lens_fail("the first position read back differently");
				return;
			}
			say("camera: the lens motor: the first position written (%u) reads back right (%02x %02x)\n", next, high, low);
		}
		if (lens_at != target) {
			IOSleep(LENS_STEP_MS);
		}
	}
}

void instantnoodle_lens_init(struct instantnoodle_i2c *bus);
void instantnoodle_lens_set(uint32_t position);
uint32_t instantnoodle_lens_position(void);
void instantnoodle_lens_park(void);
void instantnoodle_lens_down(void);
void instantnoodle_lens_frame(const void *frame, uint32_t stride, uint32_t width, uint32_t height);

/* The chip read and brought from sleep to active (once per power-up of the module; idempotent). The lens stays where it rests (code 0). */
void
instantnoodle_lens_init(struct instantnoodle_i2c *bus)
{
	uint8_t mode = 0xff, high = 0xff, low = 0xff, back = 0xff;
	uint32_t value = 0;

	lck_mtx_lock(&lens_lock);
	if (lens_up || lens_dead || bus == NULL) {
		lck_mtx_unlock(&lens_lock);
		return;
	}
	lens_bus = bus;
	if (!lens_read(LENS_MODE, &mode) || !lens_read(LENS_POSITION, &high) || !lens_read(LENS_POSITION + 1, &low)) {
		say("camera: the lens motor (AK7377 at 0x0c) does not answer: no focus\n");
		lens_dead = true;
		lck_mtx_unlock(&lens_lock);
		return;
	}
	say("camera: the lens motor (AK7377 at 0x0c): mode %#04x, position regs %02x %02x\n", mode, high, low);
	if ((mode != LENS_SLEEP && mode != LENS_STANDBY && mode != LENS_ACTIVE) || high != 0 || low != 0) {
		lens_fail("not the state a power-up leaves it in (mode 0x20, position 0)");
		lck_mtx_unlock(&lens_lock);
		return;
	}
	/* Android's own initSettings: standby, 1 ms, active, 1 ms; each read back */
	if (mode != LENS_ACTIVE) {
		if (!lens_write_mode(LENS_STANDBY)) {
			lens_fail("the standby write was not acknowledged");
			lck_mtx_unlock(&lens_lock);
			return;
		}
		IOSleep(1);
		if (!lens_read(LENS_MODE, &back) || back != LENS_STANDBY) {
			say("camera: the lens motor: mode standby written, read back %#04x\n", back);
			lens_fail("the mode did not take");
			lck_mtx_unlock(&lens_lock);
			return;
		}
		if (!lens_write_mode(LENS_ACTIVE)) {
			lens_fail("the active write was not acknowledged");
			lck_mtx_unlock(&lens_lock);
			return;
		}
		IOSleep(1);
		if (!lens_read(LENS_MODE, &back) || back != LENS_ACTIVE) {
			say("camera: the lens motor: mode active written, read back %#04x\n", back);
			lens_fail("the mode did not take");
			lck_mtx_unlock(&lens_lock);
			return;
		}
	}
	if (PE_parse_boot_argn("cam_lensmax", &value, sizeof(value)) && value > 0 && value <= LENS_MAX_CODE) {
		lens_max = value;
	}
	lens_at = 0;
	lens_up = true;
	lens_checked = false;
	say("camera: the lens motor (AK7377) is active, the lens at code 0 (closest), codes 0..%u (iOS positions %u..0)\n", lens_max, lens_max);
	if (PE_parse_boot_argn("cam_lensscan", &value, sizeof(value)) && value) {
		bzero(&scan, sizeof(scan));
		scan.on = true;
		say("camera: the lens scan is on (cam_lensscan): it starts after %u frames\n", SCAN_FIRST);
	}
	if (PE_parse_boot_argn("cam_focus", &value, sizeof(value))) {
		lens_go(lens_max - (value > lens_max ? lens_max : value));
		say("camera: cam_focus=%u: the lens %s at code %u\n", value, lens_up ? "is" : "FAILED to get", lens_at);
	}
	lck_mtx_unlock(&lens_lock);
}

/* iOS' position: 0 furthest, 1023 closest (the code is the other way round, see the top). Ignored while the scan has the lens. */
void
instantnoodle_lens_set(uint32_t position)
{
	lck_mtx_lock(&lens_lock);
	if (lens_up && !(scan.on && !scan.done)) {
		lens_go(lens_max - (position > lens_max ? lens_max : position));
	}
	lck_mtx_unlock(&lens_lock);
}

/* Where the lens was told to be, in iOS' units; HK_CAMERA_NO_FOCUS without a working motor. */
uint32_t
instantnoodle_lens_position(void)
{
	return lens_up ? lens_max - lens_at : HK_CAMERA_NO_FOCUS;
}

/* The camera stops streaming: the lens to code 0 (no current in the coil while nobody looks: the closest focus), the motor left active. */
void
instantnoodle_lens_park(void)
{
	lck_mtx_lock(&lens_lock);
	scan.on = false;
	lens_go(0);
	lck_mtx_unlock(&lens_lock);
}

/* The module is about to lose its power: the lens to 0, the motor to standby (Linux' ak7375 does the same), and the next power-up starts again. */
void
instantnoodle_lens_down(void)
{
	lck_mtx_lock(&lens_lock);
	scan.on = false;
	if (lens_up) {
		lens_go(0);
		if (lens_up) {
			(void)lens_write_mode(LENS_STANDBY);
		}
	}
	lens_up = false;
	lens_dead = false;
	lck_mtx_unlock(&lens_lock);
}

/*
 * The scan's eyes: a finished RAW10 frame (packed: four pixels' high bytes, then a byte of their low bits). The sharpness is the sum of the
 * differences of same-colour neighbours (two pixels apart) over the middle 800 x 400 pixels, per unit of their level.
 */
static void
scan_measure(const uint8_t *frame, uint32_t stride, uint32_t width, uint32_t height)
{
	uint64_t gradient = 0, level = 0;
	uint32_t rows = 400, y0 = (height - rows) / 2, first = (width / 2 - 400) / 4 * 5, bytes = 800 / 4 * 5;

	for (uint32_t y = y0; y < y0 + rows; y++) {
		const uint8_t *row = frame + (uint64_t)y * stride + first;

		for (uintptr_t line = (uintptr_t)row & ~63ul; line < (uintptr_t)row + bytes; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		for (uint32_t g = 0; g < bytes; g += 5) {
			int a = row[g], b = row[g + 1], c = row[g + 2], d = row[g + 3];

			gradient += (uint64_t)(a > c ? a - c : c - a) + (uint64_t)(b > d ? b - d : d - b);
			level += (uint64_t)(a + b + c + d);
		}
	}
	scan.gradient += gradient;
	scan.level += level;
}

void
instantnoodle_lens_frame(const void *frame, uint32_t stride, uint32_t width, uint32_t height)
{
	if (!scan.on || scan.done || !lens_up) {
		return;
	}
	if (scan.frames++ < SCAN_FIRST) {
		return;
	}
	if (scan.wait > 0) {
		scan.wait--;
		return;
	}
	scan_measure(frame, stride, width, height);
	if (++scan.measured < SCAN_MEASURE) {
		return;
	}
	{
		uint32_t pos = scan.index * SCAN_STEP > lens_max ? lens_max : scan.index * SCAN_STEP;
		uint64_t sharpness = scan.gradient * 100000 / (scan.level + 1);

		say("camera: lens scan: code %u sharpness %llu\n", pos, sharpness);
		if (scan.count < 40) {
			scan.pos[scan.count] = pos;
			scan.sharpness[scan.count++] = sharpness;
		}
		if (sharpness > scan.best) {
			scan.best = sharpness;
			scan.best_pos = pos;
		}
		scan.gradient = scan.level = 0;
		scan.measured = 0;
		if (pos >= lens_max) {
			say("camera: lens scan done: the sharpest place is code %u (sharpness %llu)\n", scan.best_pos, scan.best);
			scan.done = true;
			lck_mtx_lock(&lens_lock);
			lens_go(scan.best_pos);
			lck_mtx_unlock(&lens_lock);
			for (unsigned i = 0; i < scan.count; i += 8) {
				char line[160];
				int at = 0;

				for (unsigned k = i; k < i + 8 && k < scan.count; k++) {
					at += snprintf(line + at, sizeof(line) - (size_t)at, " %u:%llu", scan.pos[k], scan.sharpness[k]);
				}
				say("camera: lens scan summary:%s\n", line);
			}
			return;
		}
		scan.index++;
		pos = scan.index * SCAN_STEP > lens_max ? lens_max : scan.index * SCAN_STEP;
		scan.wait = SCAN_SETTLE;
		lck_mtx_lock(&lens_lock);
		lens_go(pos);
		lck_mtx_unlock(&lens_lock);
	}
}
