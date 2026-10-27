/*
 * The OnePlus 8 Pro's cameras (instantnoodlep, SM8250 "kona", Titan 480), the first rung of the ladder hk_camera.c
 * climbs for the POCO X5 Pro: boot-arg cam=1, a look, READING ONLY. Nothing here writes a register.
 *
 * What this phone has (its live device tree, the camera nodes of Android's kona-oem-camera-instantnoodlep_t0.dtsi, the
 * Lineage kernel's camcc-kona.c / gcc-kona.c / pinctrl-kona.c, and Android's own kernel log of 2026-10-27, which
 * probes all five): five sensors on two CCIs (0xac4f000, 0xac50000), six CSIPHYs, CSID480/IFE480:
 *
 *   slot  sensor            slave  id      CCI master   CSIPHY  MCLK  reset  supplies besides vio (ldof7, 1.8 V)
 *   0     IMX689  (main)    0x34   0x0689  CCI0 m0      0       94    145    vana: BOB + gpio 156 (+ gpio 26), vdig ldof1
 *   1     IMX586  (ultra)   0x20   0x0586  CCI0 m1      1       95    144    vana: BOB + gpio 159, vdig ldof2
 *   2     IMX471  (front)   0x20   0x0471  CCI1 m1      3       97    78     avdd gpio 24
 *   3     S5K3M5  (tele)    0x5a   0x30d5  CCI1 m0      2       96    90     vana gpio 139, vdig ldof4
 *   4     GC5035  (5 MP)    0x6e   0x5035  CCI0 m1      4       98    92     vdig smpa4 (gpio 74)
 *
 * (the slots and addresses are Android's; "m0/m1" are the device tree's cci-master, the two buses of each CCI: slots 0, 1
 * and 4 are on the CCI at 0xac4f000 (GPIO 101/102 and 103/104 as cci_i2c), slots 2 and 3 on the one at 0xac50000). The camera clocks are the camera clock controller's (camcc, 0xad00000),
 * reached only when the GCC's camera AHB clock is on (Linux's gcc_camera_ahb_clk, 0xb008 in the GCC): the look
 * reads the GCC first and leaves the camcc alone if that clock is off, because a register of a block whose bus clock is
 * stopped is a bus error that takes the phone to CrashDump.
 *
 * cam=1 prints: the GCC camera clocks; the camcc's power domains (titan_top, ife_0, ife_1, ipe_0, ipe_1, bps), the
 * branch clocks of the CCIs, the AHB clocks and the seven MCLKs, the MCLK root generators' state; the pins of every
 * sensor (function, pull, drive, level) and of the two CCIs. cam_after=N (default 20) seconds of waiting first.
 *
 * cam=2 (cam_slot=0 or 1, default 0): ONE sensor powered up, its chip ID read, powered down again. Every write is
 * announced first and read back where it can be. The order is the usual one of these sensors (their module files'
 * own sequences are in Android's "Parameter Parser V2.0.0" files, which tools/camsensor.py does not read yet): reset
 * low; the I/O supply (RPMh ldof7, 1.8 V); the analog supply (its GPIO switch; the BOB is Android's and stays); the
 * digital supply (RPMh ldof1/ldof2); the camera's clocks (MMCX rail at its lowest level, the GCC's camera AHB clock, the
 * titan_top power domain, the sensor's MCLK at the crystal's 19.2 MHz: what the device tree asks); reset high; 10 ms;
 * the chip ID over I2C (bit-banged on the CCI's two pins, as the other drivers here do theirs) at the sensor's
 * address, 16-bit register 0x0016 (Sony's model ID: 0x0689 for the IMX689, 0x0586 for the IMX586).
 * Power-down: the reverse, and everything put back as it was found.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/instantnoodle.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* to the loader's log as well: the start of a run's kernel log may not reach the host */
static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x30000UL
#define GCC_CAMERA_AHB_CBCR     0xb008          /* gcc-kona.c: gcc_camera_ahb_clk */
#define GCC_CAMERA_XO_CBCR      0xb02c
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x20000UL
#define CBCR_OFF                (1u << 31)      /* the clock is not running */

static volatile uint32_t *gcc, *camcc;

static uint32_t
gcc_rd(uint32_t offset)
{
	return gcc[offset / 4];
}

static uint32_t
camcc_rd(uint32_t offset)
{
	return camcc[offset / 4];
}

/* the sensors' pins: tile by Linux's pinctrl-kona.c (PINGROUP's second argument) */
#define W INSTANTNOODLE_TLMM_WEST
#define S INSTANTNOODLE_TLMM_SOUTH
#define N INSTANTNOODLE_TLMM_NORTH
static const struct pin_name {
	struct instantnoodle_pin pin;
	const char *what;
} pins[] = {
	{ { N, 101 }, "CCI0 master 0 data" }, { { N, 102 }, "CCI0 master 0 clock" },
	{ { N, 103 }, "CCI0 master 1 data" }, { { N, 104 }, "CCI0 master 1 clock" },
	{ { N, 94 }, "MCLK0 (slot 0, IMX689)" }, { { W, 145 }, "reset, slot 0" }, { { W, 156 }, "VANA switch, slot 0" },
	{ { S, 26 }, "custom, slot 0" },
	{ { N, 95 }, "MCLK1 (slot 1, IMX586)" }, { { W, 144 }, "reset, slot 1" }, { { W, 159 }, "VANA switch, slot 1" },
	{ { N, 97 }, "MCLK3 (slot 2, IMX471 front)" }, { { N, 78 }, "reset, slot 2" }, { { S, 24 }, "AVDD switch, slot 2" },
	{ { N, 96 }, "MCLK2 (slot 3, S5K3M5)" }, { { S, 90 }, "reset, slot 3" }, { { W, 139 }, "VANA switch, slot 3" },
	{ { N, 98 }, "MCLK4 (slot 4, GC5035)" }, { { N, 92 }, "reset, slot 4" }, { { S, 74 }, "VDIG switch, slot 4" },
};
#undef W
#undef S
#undef N

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = instantnoodle_gpio_cfg(pins[i].pin);

		say("camera: pin %u (%s): function %u, pull %u, drive %u mA, %s, level %u\n", pins[i].pin.pin, pins[i].what,
		    (cfg >> 2) & 0xf, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & (1u << 9)) ? "output" : "input",
		    instantnoodle_gpio_level(pins[i].pin) ? 1u : 0u);
	}
}

static void
show_gcc(void)
{
	uint32_t ahb = gcc_rd(GCC_CAMERA_AHB_CBCR), xo = gcc_rd(GCC_CAMERA_XO_CBCR);

	say("camera: GCC gcc_camera_ahb_clk %#010x (%s), gcc_camera_xo_clk %#010x (%s)\n", ahb, (ahb & CBCR_OFF) ? "OFF" : "on", xo,
	    (xo & CBCR_OFF) ? "off" : "on");
}

static void
show_camcc(void)
{
	static const struct { uint32_t offset; const char *name; } gdscs[] = {
		{ 0xc144, "titan_top" }, { 0xa004, "ife_0" }, { 0xb004, "ife_1" }, { 0x8004, "ipe_0" }, { 0x9004, "ipe_1" },
		{ 0x7004, "bps" },
	};
	static const struct { uint32_t offset; const char *name; } branches[] = {
		{ 0xc0f4, "cpas_ahb" }, { 0xc150, "core_ahb" }, { 0xc114, "camnoc_axi" }, { 0xc16c, "gdsc" }, { 0xc094, "icp_ahb" },
		{ 0xc0d4, "cci_0" }, { 0xc0f0, "cci_1" },
		{ 0x5018, "mclk0" }, { 0x5034, "mclk1" }, { 0x5050, "mclk2" }, { 0x506c, "mclk3" }, { 0x5088, "mclk4" },
		{ 0x50a4, "mclk5" }, { 0x50c0, "mclk6" },
		{ 0x601c, "csiphy0" }, { 0x603c, "csiphy1" }, { 0x605c, "csiphy2" }, { 0x607c, "csiphy3" }, { 0x609c, "csiphy4" },
		{ 0x60bc, "csiphy5" },
		{ 0xa058, "ife_0_csid" }, { 0xb058, "ife_1_csid" }, { 0xc034, "ife_lite_csid" }, { 0xa028, "ife_0" },
	};
	static const struct { uint32_t offset; const char *name; } rcgs[] = {
		{ 0x5000, "mclk0" }, { 0x501c, "mclk1" }, { 0x5038, "mclk2" }, { 0x5054, "mclk3" }, { 0x5070, "mclk4" },
		{ 0x508c, "mclk5" }, { 0x50a8, "mclk6" }, { 0xc0bc, "cci_0" }, { 0xc0d8, "cci_1" },
	};

	if (gcc_rd(GCC_CAMERA_AHB_CBCR) & CBCR_OFF) {
		say("camera: the GCC's camera AHB clock is off: the camcc is not read (a bus error would follow)\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(gdscs) / sizeof(gdscs[0]); i++) {
		uint32_t v = camcc_rd(gdscs[i].offset);

		say("camera: power domain %s (camcc %#06x) = %#010x: %s\n", gdscs[i].name, gdscs[i].offset, v,
		    (v & (1u << 31)) ? "ON" : "off");
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t v = camcc_rd(branches[i].offset);

		say("camera: clock %s (camcc %#06x) = %#010x: %s\n", branches[i].name, branches[i].offset, v,
		    (v & CBCR_OFF) ? "off" : "RUNNING");
	}
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		say("camera: root generator %s (camcc %#06x): cmd %#010x, cfg %#010x\n", rcgs[i].name, rcgs[i].offset,
		    camcc_rd(rcgs[i].offset), camcc_rd(rcgs[i].offset + 4));
	}
}


/* ---- cam=2 ---- */

#define MMCX_LEVEL              64              /* RPMh's "mmcx.lvl" at its lowest level (the device tree's minimum) */
#define TITAN_TOP_GDSCR         0xc144          /* [0] collapse, [31] on */
#define CMD_UPDATE              1u
#define MCLK_RCG(n)             (0x5000 + 0x1c * (n))
#define MCLK_CBCR(n)            (0x5018 + 0x1c * (n))

#define W INSTANTNOODLE_TLMM_WEST
#define S INSTANTNOODLE_TLMM_SOUTH
#define N INSTANTNOODLE_TLMM_NORTH
static const struct sensor {
	const char *name;
	uint8_t address;                        /* 7-bit */
	uint16_t id;
	unsigned mclk;                          /* camcc's MCLK n, on GPIO 94 + n */
	struct instantnoodle_pin reset, vana[2];
	unsigned vanas;
	const char *vdig;                       /* RPMh's name, 0 mV: none */
	unsigned vdig_mv;
	const char *custom;                     /* the device tree's cam_v_custom1 (RPMh's name), or none */
	unsigned custom_mv;
	unsigned vaf_pin;                       /* GPIO of the module's VAF (OIS, actuator), 0: none */
	struct instantnoodle_pin sda, scl;
} sensors[] = {
	{ "IMX689", 0x1a, 0x0689, 0, { W, 145 }, { { W, 156 }, { S, 26 } }, 2, "ldof1", 1104, "smpa5", 2040, 93, { N, 101 }, { N, 102 } },
	{ "IMX586", 0x10, 0x0586, 1, { W, 144 }, { { W, 159 }, { W, 159 } }, 1, "ldof2", 1100, NULL, 0, 0, { N, 103 }, { N, 104 } },
};
#undef W
#undef S
#undef N

static const struct sensor *sensor;
static struct instantnoodle_i2c bus;
static bool did_vote, did_gcc, did_gdsc, did_mclk, did_vio, did_vdig, did_custom, did_vaf, did_bob;
static uint32_t gcc_found;

static void
camcc_poll(uint32_t offset, uint32_t mask, uint32_t want, const char *what)
{
	for (int i = 0; i < 500 && (camcc_rd(offset) & mask) != want; i++) {
		IOSleep(1);
	}
	if ((camcc_rd(offset) & mask) != want) {
		say("camera: %s did not come as asked (camcc %#06x = %#010x)\n", what, offset, camcc_rd(offset));
	}
}

static bool
power_up(void)
{
	const struct sensor *s = sensor;
	uint32_t ahb;

	/*
	 * Android's own sequence for this sensor, out of its module file (tools/camsensor.py, "Parameter Parser V2.0.0"):
	 * IMX689: VANA, CUSTOM_REG1, VDIG, VIO, MCLK (19.2 MHz), RESET high, each followed by 1 ms (CUSTOM_REG1 by none);
	 * and the pinctrl "cam_default" state first (GPIO 26 output high). The module's OIS and actuator share its bus and
	 * are powered too (their nodes: the VAF pin, GPIO 93, and pm8009 ldo5 at 2.8 V; the actuator's BOB is Android's).
	 */
	say("camera: cam=2: the %s, Android's own order: the pins' default state, VANA, custom, VDIG, VIO, VAF, clocks, MCLK, reset high\n", s->name);
	instantnoodle_gpio_output(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	/*
	 * GPIO 26 (the device tree's custom GPIO, "CAM_VANA_2"): its pinctrl state says output-high, but the camera driver then requests
	 * it from its gpio-req-tbl with flags 0 = output, initially LOW, and no step of the IMX689's sequence raises it: it is LOW while
	 * the sensor runs (cam_sensor_util.c, cam_sensor_util_request_gpio_table). The first runs here drove it high.
	 */
	for (unsigned i = 0; i < s->vanas; i++) {
		if (s->vana[i].pin == 26) {
			say("camera: next: pin 26 (CAM_VANA_2) LOW, as Android's request table leaves it\n");
			instantnoodle_gpio_output(s->vana[i], false, INSTANTNOODLE_PULL_NONE, 2);
		}
	}
	say("camera: next: VANA: the BOB (RPMh bobc1, the device tree's cam_vana: 3.008 V at the least) and the analog switch, pin %u, high\n", s->vana[0].pin);
	did_bob = instantnoodle_supply("bobc1", 3008, true);
	say("camera: the BOB vote: %s\n", did_bob ? "taken" : "NOT taken by RPMh");
	instantnoodle_gpio_output(s->vana[0], true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(1);
	if (s->custom != NULL) {
		say("camera: next: RPMh %s (the device tree's cam_v_custom1) %u mV on\n", s->custom, s->custom_mv);
		did_custom = instantnoodle_supply(s->custom, s->custom_mv, true);
		if (!did_custom) {
			say("camera: RPMh did not take the custom supply's vote\n");
			return false;
		}
	}
	say("camera: next: VDIG: RPMh %s %u mV on\n", s->vdig, s->vdig_mv);
	did_vdig = instantnoodle_supply(s->vdig, s->vdig_mv, true);
	if (!did_vdig) {
		say("camera: RPMh did not take the digital supply's vote\n");
		return false;
	}
	IOSleep(1);
	say("camera: next: VIO: RPMh ldof7 1800 mV on\n");
	did_vio = instantnoodle_supply("ldof7", 1800, true);
	if (!did_vio) {
		say("camera: RPMh did not take the I/O supply's vote\n");
		return false;
	}
	IOSleep(1);
	if (s->vaf_pin != 0) {
		say("camera: next: VAF: RPMh ldof5 2800 mV on, pin %u high (the OIS' and the actuator's supply)\n", s->vaf_pin);
		did_vaf = instantnoodle_supply("ldof5", 2800, true);
		instantnoodle_gpio_output((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }, true, INSTANTNOODLE_PULL_NONE, 2);
		IOSleep(2);
	}
	say("camera: next: RPMh mmcx.lvl at level %u\n", MMCX_LEVEL);
	did_vote = instantnoodle_rail_level("mmcx.lvl", MMCX_LEVEL);
	if (!did_vote) {
		say("camera: RPMh did not take the MMCX vote\n");
		return false;
	}
	gcc_found = gcc_rd(GCC_CAMERA_AHB_CBCR);
	say("camera: next: the GCC's camera AHB clock on (was %#010x)\n", gcc_found);
	gcc[GCC_CAMERA_AHB_CBCR / 4] = gcc_found | 1u;
	did_gcc = true;
	for (int i = 0; i < 100 && (gcc_rd(GCC_CAMERA_AHB_CBCR) & CBCR_OFF); i++) {
		IOSleep(1);
	}
	ahb = gcc_rd(GCC_CAMERA_AHB_CBCR);
	say("camera: the GCC's camera AHB clock now %#010x: %s\n", ahb, (ahb & CBCR_OFF) ? "STILL OFF" : "running");
	if (ahb & CBCR_OFF) {
		return false;
	}
	if (!camcc) {
		camcc = (volatile uint32_t *)ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	}
	say("camera: next: the titan_top power domain on\n");
	camcc[TITAN_TOP_GDSCR / 4] = camcc_rd(TITAN_TOP_GDSCR) & ~1u;
	did_gdsc = true;
	camcc_poll(TITAN_TOP_GDSCR, 1u << 31, 1u << 31, "the titan_top power domain");
	say("camera: next: MCLK%u at 19.2 MHz (the crystal), on pin %u\n", s->mclk, 94 + s->mclk);
	camcc[(MCLK_RCG(s->mclk) + 4) / 4] = (camcc_rd(MCLK_RCG(s->mclk) + 4) & ~0x371fu) | 1u;         /* source 0, divider 1 */
	camcc[MCLK_RCG(s->mclk) / 4] = camcc_rd(MCLK_RCG(s->mclk)) | CMD_UPDATE;
	camcc_poll(MCLK_RCG(s->mclk), CMD_UPDATE, 0, "the MCLK's root generator");
	camcc[MCLK_CBCR(s->mclk) / 4] = camcc_rd(MCLK_CBCR(s->mclk)) | 1u;
	did_mclk = true;
	camcc_poll(MCLK_CBCR(s->mclk), CBCR_OFF, 0, "the MCLK's branch");
	instantnoodle_gpio_function((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, 94 + s->mclk }, 1, INSTANTNOODLE_PULL_NONE, 2);
	say("camera: MCLK%u: branch %#010x, root generator cmd %#010x cfg %#010x\n", s->mclk, camcc_rd(MCLK_CBCR(s->mclk)),
	    camcc_rd(MCLK_RCG(s->mclk)), camcc_rd(MCLK_RCG(s->mclk) + 4));
	IOSleep(1);
	say("camera: next: reset high\n");
	instantnoodle_gpio_output(s->reset, true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(15);
	return true;
}

static void
scan_bus(const char *when)
{
	char list[160] = "";
	unsigned seen = 0;

	for (unsigned a = 0x08; a <= 0x77; a++) {
		uint8_t one;

		if (instantnoodle_i2c_read16(&bus, (uint8_t)a, 0, &one, 1) == 0) {
			seen++;
			snprintf(list + strlen(list), sizeof(list) - strlen(list), " %#04x", a);
		}
		instantnoodle_i2c_recover(&bus);
	}
	say("camera: addresses answering on this bus (7-bit), %s:%s%s\n", when, seen ? list : " none", seen > 12 ? " (so many: the bus is stuck)" : "");
}

static void
read_id(void)
{
	const struct sensor *s = sensor;
	uint8_t id[2] = { 0, 0 };

	bus.name = "cci";
	bus.sda = s->sda;
	bus.scl = s->scl;
	bus.pull = INSTANTNOODLE_PULL_UP;
	say("camera: the CCI's pins before the transfer: data (pin %u) %u, clock (pin %u) %u\n", s->sda.pin, instantnoodle_gpio_level(s->sda) ? 1u : 0u,
	    s->scl.pin, instantnoodle_gpio_level(s->scl) ? 1u : 0u);
	scan_bus("15 ms after reset high");
	IOSleep(100);
	scan_bus("115 ms");
	say("camera: next: a reset pulse with MCLK running\\n");
	instantnoodle_gpio_output(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	IOSleep(5);
	instantnoodle_gpio_output(s->reset, true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(30);
	scan_bus("30 ms after the pulse");
	for (int tries = 0; tries < 3; tries++) {
		int err = instantnoodle_i2c_read16(&bus, s->address, 0x0016, id, 2);

		say("camera: chip ID of the %s at I2C %#04x, register 0x0016: %s %02x%02x (it should be %#06x)\n", s->name, s->address,
		    err ? "NO ANSWER" : "answers", id[0], id[1], s->id);
		if (err == 0) {
			break;
		}
		instantnoodle_i2c_recover(&bus);
	}
	instantnoodle_gpio_input(s->sda, INSTANTNOODLE_PULL_UP);
	instantnoodle_gpio_input(s->scl, INSTANTNOODLE_PULL_UP);
}

static void
power_down(void)
{
	const struct sensor *s = sensor;

	say("camera: power down: reset low, MCLK off, power domain off, clocks and supplies back\n");
	instantnoodle_gpio_output(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	IOSleep(1);
	if (did_mclk) {
		camcc[MCLK_CBCR(s->mclk) / 4] = camcc_rd(MCLK_CBCR(s->mclk)) & ~1u;
		instantnoodle_gpio_input((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, 94 + s->mclk }, INSTANTNOODLE_PULL_DOWN);
	}
	if (did_gdsc) {
		camcc[TITAN_TOP_GDSCR / 4] = camcc_rd(TITAN_TOP_GDSCR) | 1u;
		camcc_poll(TITAN_TOP_GDSCR, 1u << 31, 0, "the titan_top power domain's collapse");   /* (it needs the AHB clock and the MMCX rail until it is done) */
	}
	/*
	 * The GCC's camera AHB clock and the MMCX vote are NOT given back unless cam_restore=1: the first cam=2 run, which gave
	 * them back right behind the power domain's collapse, took the phone to CrashDump during this very step. The clock is
	 * harmless on, the vote goes with the run.
	 */
	{
		uint32_t restore = 0;

		(void)PE_parse_boot_argn("cam_restore", &restore, sizeof(restore));
		if (restore && did_gcc) {
			gcc[GCC_CAMERA_AHB_CBCR / 4] = gcc_found;
		}
		if (restore && did_vote) {
			(void)instantnoodle_rail_level("mmcx.lvl", 0);
		}
	}
	if (did_bob) {
		(void)instantnoodle_supply("bobc1", 0, false);   /* (only this vote: the display and Android's own are the BOB's other users) */
	}
	if (did_vaf) {
		instantnoodle_gpio_output((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }, false, INSTANTNOODLE_PULL_DOWN, 2);
		(void)instantnoodle_supply("ldof5", 0, false);
	}
	if (did_custom) {
		(void)instantnoodle_supply(s->custom, 0, false);   /* (smpa5 is shared with the Wi-Fi radio's RF rail: this gives back only this vote) */
	}
	if (did_vdig) {
		(void)instantnoodle_supply(s->vdig, 0, false);
	}
	for (unsigned i = 0; i < s->vanas; i++) {
		instantnoodle_gpio_output(s->vana[i], false, INSTANTNOODLE_PULL_DOWN, 2);
		instantnoodle_gpio_input(s->vana[i], INSTANTNOODLE_PULL_DOWN);
	}
	if (did_vio) {
		(void)instantnoodle_supply("ldof7", 0, false);
	}
	instantnoodle_gpio_input(s->reset, INSTANTNOODLE_PULL_DOWN);
	say("camera: everything put back (GCC camera AHB clock %#010x)\n", gcc_rd(GCC_CAMERA_AHB_CBCR));
}

static uint32_t cam_level = 1;

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;

	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	IOSleep(secs * 1000);           /* (the system up first) */
	say("camera: cam=%u: the OnePlus 8 Pro's cameras, %s\n", cam_level, cam_level >= 2 ? "one sensor powered up, its chip ID read" : "a look, reading only");
	gcc = (volatile uint32_t *)ml_io_map(GCC_PHYS, GCC_SIZE);
	show_gcc();
	show_pins();
	if (!(gcc_rd(GCC_CAMERA_AHB_CBCR) & CBCR_OFF)) {
		camcc = (volatile uint32_t *)ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	}
	show_camcc();
	if (cam_level >= 2) {
		uint32_t slot = 0;

		(void)PE_parse_boot_argn("cam_slot", &slot, sizeof(slot));
		sensor = &sensors[slot > 1 ? 0 : slot];
		if (power_up()) {
			read_id();
		}
		power_down();
		show_gcc();
		show_pins();
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
instantnoodlep_camera_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!instantnoodlep_board() || !PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	cam_level = level > 2 ? 2 : level;
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
