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
#include <mach/mach_time.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <sys/hk_camera.h>
#include <sys/instantnoodle.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);
extern void IODelay(unsigned int microseconds);
extern uint64_t kvtophys(vm_offset_t va);
/* the main camera's lens motor (bsd/dev/instantnoodle_lens.c) */
void instantnoodle_lens_init(struct instantnoodle_i2c *bus);
void instantnoodle_lens_set(uint32_t position);
uint32_t instantnoodle_lens_position(void);
void instantnoodle_lens_park(void);
void instantnoodle_lens_down(void);
void instantnoodle_lens_frame(const void *frame, uint32_t stride, uint32_t width, uint32_t height);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);

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

/*
 * The loader's log ring is 32 KB and the system's own output (bluetoothd in some images) pushes the lines out within seconds:
 * the results are remembered here and said again, a few times, at the end.
 */
static char kept[5000];
static unsigned kept_n;
static void keep(const char *format, ...) __printflike(1, 2);

static void
keep(const char *format, ...)
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
	if (kept_n + strlen(line) < sizeof(kept)) {
		memcpy(kept + kept_n, line, strlen(line));
		kept_n += (unsigned)strlen(line);
	}
}

/*
 * "egpio": a TLMM pin whose configuration register has bit 11 set ("egpio present", read-only) is driven by the TLMM only when
 * bit 12 ("egpio enable") is set as well; Linux does that when it muxes such a pin to GPIO (pinctrl-msm.c msm_pinmux_set_mux:
 * `if (val & BIT(egpio_present)) val |= BIT(egpio_enable)`). instantnoodle_gpio_output() rewrites the whole register and leaves it
 * out: the camera's GPIO 156 (the IMX689's VANA analog switch, config 0xa00) then read LOW however it was driven, 2026-10-27.
 * cam_out() and cam_in() are the shared functions plus that bit.
 */
static void
egpio_fix(struct instantnoodle_pin p)
{
	instantnoodle_gpio_egpio_enable(p);
}

static void
cam_out(struct instantnoodle_pin p, bool high, unsigned pull, unsigned ma)
{
	instantnoodle_gpio_output(p, high, pull, ma);
	egpio_fix(p);
}

static void
cam_in(struct instantnoodle_pin p, unsigned pull)
{
	instantnoodle_gpio_input(p, pull);
	egpio_fix(p);
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
	{ { N, 105 }, "CCI1 master 0 data (slot 3)" }, { { N, 106 }, "CCI1 master 0 clock (slot 3)" },
	{ { N, 107 }, "CCI1 master 1 data (slot 2, IMX471)" }, { { N, 108 }, "CCI1 master 1 clock (slot 2, IMX471)" },
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


/* ---- a read-only look at the PMICs' regulator blocks (cam=2 compares it before and after the power-up) ----
 *
 * RPMh takes the votes and says nothing about what the regulators then do. The PMIC arbiter's observer channels (as
 * hk_buttons.c's hk_spmi_read(), which keeps a table of eight peripherals only) read the registers of every regulator
 * block: TYPE/SUBTYPE at 0x04/0x05, STATUS1 at 0x08 (bit 7: output good), the voltage setting at 0x40/0x41, the mode at
 * 0x45 and the enable at 0x46 (bit 7), for LDOs and switchers alike (Linux qcom_spmi-regulator.c). Nothing is written.
 */
#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_CHANNELS            512
#define OBS_COMMAND             0x00
#define OBS_STATUS              0x08
#define OBS_DATA0               0x18
#define OBS_DATA1               0x1c
#define OBS_READ                (1u << 27)

extern void hk_spmi_lock(void);
extern void hk_spmi_unlock(void);

static volatile uint8_t *arb_core, *arb_observer;
static uint16_t arb_channel_of[ARB_CHANNELS], arb_channel_ppid[ARB_CHANNELS];
static unsigned arb_channels;

static uint32_t
arb_rd(volatile uint8_t *base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static bool
arb_ready(void)
{
	if (arb_channels != 0) {
		return true;
	}
	arb_core = (volatile uint8_t *)ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = (volatile uint8_t *)ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (arb_core == NULL || arb_observer == NULL || arb_rd(arb_core, 0) < 0x50000000u || arb_rd(arb_core, 0) >= 0x70000000u) {
		return false;
	}
	for (unsigned n = 0; n < ARB_CHANNELS; n++) {
		uint32_t map = arb_rd(arb_core, ARB_CHANNEL(n));

		if (map != 0 && arb_channels < ARB_CHANNELS) {
			arb_channel_ppid[arb_channels] = (map >> 8) & 0xfff;
			arb_channel_of[arb_channels] = (uint16_t)n;
			arb_channels++;
		}
	}
	return arb_channels != 0;
}

/* `count` (1-8) registers of peripheral `ppid` (slave << 8 | block), from `reg`: false if nobody answers */
static bool
pmic_read(uint16_t ppid, uint8_t reg, uint8_t *bytes, unsigned count)
{
	unsigned channel = ARB_CHANNELS;
	uint32_t status = 0, data[2];

	for (unsigned i = 0; i < arb_channels; i++) {
		if (arb_channel_ppid[i] == ppid) {
			channel = arb_channel_of[i];
			break;
		}
	}
	if (channel == ARB_CHANNELS) {
		return false;
	}
	volatile uint8_t *obs = arb_observer + 0x80UL * channel;
	uint64_t start = mach_absolute_time(), interval;

	nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	*(volatile uint32_t *)(obs + OBS_COMMAND) = OBS_READ | (uint32_t)reg << 4 | (count - 1);
	do {
		status = arb_rd(obs, OBS_STATUS);
	} while (!(status & 1) && mach_absolute_time() - start < interval);
	data[0] = arb_rd(obs, OBS_DATA0);
	data[1] = count > 4 ? arb_rd(obs, OBS_DATA1) : 0;
	hk_spmi_unlock();
	if (status != 1) {
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return true;
}

#define RAIL_MAX        800
struct rail {
	uint16_t ppid;
	uint8_t id[8], set[8];                  /* registers 0x04-0x0b and 0x40-0x47 */
};
static struct rail rails_before[RAIL_MAX], rails_after[RAIL_MAX];
static unsigned rails_before_n, rails_after_n;

static unsigned
rails_snapshot(struct rail *into)
{
	unsigned n = 0;

	if (!arb_ready()) {
		return 0;
	}
	for (unsigned sid = 0; sid < 0xc; sid++) {
		for (unsigned block = 0; block < 0xb0; block++) {
			uint16_t ppid = (uint16_t)(sid << 8 | block);
			bool wanted = (block >= 0x14 && block < 0x30) || (block >= 0x40 && block < 0x60) || (block >= 0xa0 && block < 0xb0);

			if (!wanted || n == RAIL_MAX || !pmic_read(ppid, 0x04, into[n].id, 8)) {
				continue;
			}
			if (!pmic_read(ppid, 0x40, into[n].set, 8)) {
				memset(into[n].set, 0xee, 8);
			}
			into[n].ppid = ppid;
			n++;
		}
	}
	return n;
}

static void
rails_show_changes(void)
{
	unsigned shown = 0;

	rails_after_n = rails_snapshot(rails_after);
	keep("camera: PMIC regulator blocks seen: %u before, %u now\n", rails_before_n, rails_after_n);
	for (unsigned i = 0; i < rails_after_n; i++) {
		const struct rail *b = NULL, *a = &rails_after[i];

		for (unsigned j = 0; j < rails_before_n && b == NULL; j++) {
			if (rails_before[j].ppid == a->ppid) {
				b = &rails_before[j];
			}
		}
		/* the status (0x08 = id[4]) and the settings, not the free-running bytes of other kinds of block */
		if (b == NULL || (b->id[4] == a->id[4] && memcmp(b->set, a->set, 8) == 0) || shown >= 40) {
			continue;
		}
		shown++;
		keep("camera: rail s%x/%02x type %02x/%02x: status %02x>%02x, volt %02x%02x>%02x%02x, mode %02x>%02x, enable %02x>%02x\n", a->ppid >> 8,
		    a->ppid & 0xff, a->id[0], a->id[1], b->id[4], a->id[4], b->set[1], b->set[0], a->set[1], a->set[0], b->set[5], a->set[5],
		    b->set[6], a->set[6]);
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
	uint16_t id_reg;                        /* the 16-bit register that holds it (Sony: 0x16; Samsung S5K3M5: 0) */
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
	{ "IMX689", 0x1a, 0x0689, 0x16, 0, { W, 145 }, { { W, 156 }, { S, 26 } }, 2, "ldof1", 1104, "smpa5", 2040, 93, { N, 101 }, { N, 102 } },
	/* (the IMX586's VANA is PM8009 GPIO 1 and it has CUSTOM_REG1 smpa5 and CUSTOM_REG2 ldoc7 2.8 V as well: not powered right yet) */
	{ "IMX586", 0x10, 0x0586, 0x16, 1, { W, 144 }, { { W, 159 }, { W, 159 } }, 1, "ldof2", 1100, NULL, 0, 0, { N, 103 }, { N, 104 } },
	/* slot 2, the front IMX471 (device tree cam-sensor@2: MCLK3 gpio 97, reset 78, AVDD switch 24, pm8009 l3 1056 mV). Its bus is master 1 of the SECOND CCI
	 * (0xac50000: "CCI_I2C_DATA3/CLK3" = gpio 107/108; the CCI0 pins 103/104 it was scanned on before 2026-10-27 20:00 belong to the IMX586/GC5035
	 * modules: the 0x50.. EEPROMs that answered there were theirs). Slot 3 (S5K3M5) is master 0 of the same CCI: gpio 105/106 (cci2_active). */
	{ "IMX471", 0x10, 0x0471, 0x16, 3, { N, 78 }, { { S, 24 }, { S, 24 } }, 1, "ldof3", 1056, NULL, 0, 0, { N, 107 }, { N, 108 } },
	/* slot 3, the S5K3M5 (cam-sensor@3: MCLK2 gpio 96, reset 90, VANA switch 139, pm8009 l4 1056 mV, CCI master 0; id 0x30d5 at register 0) */
	{ "S5K3M5", 0x2d, 0x30d5, 0x0000, 2, { S, 90 }, { { W, 139 }, { W, 139 } }, 1, "ldof4", 1056, NULL, 0, 0, { N, 105 }, { N, 106 } },
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
	/*
	 * What the previous sensor's power-up did is not this one's (cam_slot=99 powers several one after the other): a stale did_vaf made
	 * the IMX471's power-down drive "its" VAF pin, which is 0 -- NORTH tile pin 0 is not a pin of ours -- and the phone went to CrashDump
	 * (2026-10-27, twice, the IMX586 and the IMX471 right after their power-ups).
	 */
	did_vote = did_gcc = did_gdsc = did_mclk = did_vio = did_vdig = did_custom = did_vaf = did_bob = false;
	say("camera: cam=2: the %s, Android's own order: the pins' default state, VANA, custom, VDIG, VIO, VAF, clocks, MCLK, reset high\n", s->name);
	say("camera: dbg: reset pin %u (tile %#llx) is read first: cfg %#x\n", s->reset.pin, (unsigned long long)s->reset.tile, instantnoodle_gpio_cfg(s->reset));
	instantnoodle_gpio_output(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	say("camera: dbg: reset pin written by the shared function: cfg %#x\n", instantnoodle_gpio_cfg(s->reset));
	egpio_fix(s->reset);
	say("camera: dbg: reset pin after egpio_fix: cfg %#x\n", instantnoodle_gpio_cfg(s->reset));
	cam_out(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	/*
	 * GPIO 26 (the device tree's custom GPIO, "CAM_VANA_2", the second analog switch). Android requests it from its gpio-req-tbl with flags 0
	 * (output, initially LOW; no step of the IMX689's sequence touches it), but the pinctrl "cam_default" state is selected AFTER that
	 * request (cam_sensor_util_power_up: cam_sensor_util_request_gpio_table, then pinctrl_select_state), and that state says
	 * output-high with a pull-up, so on Android the pin ends HIGH (suspend state: output-low). The runs before 2026-10-27 12:00 kept it LOW
	 * (the OIS and the actuator answered, the sensor and the module's EEPROM, which sit behind the switches, did not) or drove it high
	 * while the VAF rail was still off: the combination of Android's end state was never tried. cam_g26=0 gives the old behaviour.
	 */
	{
		uint32_t g26 = 1;

		(void)PE_parse_boot_argn("cam_g26", &g26, sizeof(g26));
		for (unsigned i = 0; i < s->vanas; i++) {
			if (s->vana[i].pin == 26) {
				say("camera: next: pin 26 (CAM_VANA_2) %s (Android's pinctrl active state: output-high, pull-up)\n", g26 ? "HIGH" : "LOW");
				cam_out(s->vana[i], g26 != 0, g26 ? INSTANTNOODLE_PULL_UP : INSTANTNOODLE_PULL_NONE, 2);
			}
		}
	}
	say("camera: next: VANA: the BOB (RPMh bobc1, the device tree's cam_vana: 3.008 V at the least) and the analog switch, pin %u, high\n", s->vana[0].pin);
	did_bob = instantnoodle_supply("bobc1", 3008, true);
	say("camera: the BOB vote: %s\n", did_bob ? "taken" : "NOT taken by RPMh");
	/* the front sensor's AVDD switch (gpio 24) is "cam_sensor_front_avdd_active": 16 mA, output high, pull-up (the others: 2 mA, no bias) */
	if (s->vana[0].pin == 24) {
		cam_out(s->vana[0], true, INSTANTNOODLE_PULL_UP, 16);
	} else {
		cam_out(s->vana[0], true, INSTANTNOODLE_PULL_NONE, 2);
	}
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
		cam_out((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }, true, INSTANTNOODLE_PULL_NONE, 2);
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
	cam_out(s->reset, true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(15);
	return true;
}

static bool
scan_bus(const char *when)
{
	char list[160] = "";
	unsigned seen = 0;
	bool module = false;

	for (unsigned a = 0x08; a <= 0x77; a++) {
		uint8_t one;

		if (instantnoodle_i2c_read16(&bus, (uint8_t)a, 0, &one, 1) == 0) {
			seen++;
			module = module || a == sensor->address || a == 0x50;
			snprintf(list + strlen(list), sizeof(list) - strlen(list), " %#04x", a);
		}
		instantnoodle_i2c_recover(&bus);
	}
	keep("camera: addresses answering on this bus (7-bit), %s:%s%s\n", when, seen ? list : " none", seen > 12 ? " (so many: the bus is stuck)" : "");
	return module;
}

static void
show_control_pins(const char *when)
{
	const struct sensor *s = sensor;

	keep("camera: pin cfg %s: reset %#x, VANA(%u) %#x (level %u), pin 26 %#x, VAF %#x\n", when, instantnoodle_gpio_cfg(s->reset), s->vana[0].pin,
	    instantnoodle_gpio_cfg(s->vana[0]), instantnoodle_gpio_level(s->vana[0]) ? 1u : 0u,
	    instantnoodle_gpio_cfg((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_SOUTH, 26 }),
	    (s->vaf_pin ? instantnoodle_gpio_cfg((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }) : 0u));

	keep("camera: pins %s: reset(%u) %u, VANA(%u) %u, pin 26 %u, VAF(%u) %u, MCLK(%u) cfg %#x\n", when, s->reset.pin, instantnoodle_gpio_level(s->reset) ? 1u : 0u,
	    s->vana[0].pin, instantnoodle_gpio_level(s->vana[0]) ? 1u : 0u,
	    instantnoodle_gpio_level((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_SOUTH, 26 }) ? 1u : 0u, s->vaf_pin,
	    (s->vaf_pin ? instantnoodle_gpio_level((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }) ? 1u : 0u : 0u), 94 + s->mclk,
	    instantnoodle_gpio_cfg((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, 94 + s->mclk }));
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
	bus.fast = true;                /* (the sensor's tables are 3000 writes: at the pins' own speed, not 50 kHz) */
	say("camera: the CCI's pins before the transfer: data (pin %u) %u, clock (pin %u) %u\n", s->sda.pin, instantnoodle_gpio_level(s->sda) ? 1u : 0u,
	    s->scl.pin, instantnoodle_gpio_level(s->scl) ? 1u : 0u);
	show_control_pins("after power-up");
	if (!instantnoodle_gpio_level(s->vana[0])) {
		/* the analog switch's pin reads LOW after it was driven high: how does it answer to other settings? */
		for (unsigned ma = 2; ma <= 16; ma *= 2) {
			cam_out(s->vana[0], true, INSTANTNOODLE_PULL_NONE, ma);
			IOSleep(2);
			keep("camera: pin 156 driven high at %u mA: cfg %#x, level %u\n", ma, instantnoodle_gpio_cfg(s->vana[0]),
			    instantnoodle_gpio_level(s->vana[0]) ? 1u : 0u);
		}
		cam_in(s->vana[0], INSTANTNOODLE_PULL_NONE);
		IOSleep(2);
		keep("camera: pin 156 released, no pull: cfg %#x, level %u\n", instantnoodle_gpio_cfg(s->vana[0]), instantnoodle_gpio_level(s->vana[0]) ? 1u : 0u);
		cam_out(s->vana[0], true, INSTANTNOODLE_PULL_NONE, 2);
	}
	scan_bus("15 ms after reset high");
	IOSleep(100);
	scan_bus("115 ms");
	say("camera: next: a reset pulse with MCLK running\n");
	cam_out(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	IOSleep(5);
	cam_out(s->reset, true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(30);
	if (!scan_bus("30 ms after the pulse")) {
		/* the other state of pin 26, the second analog switch, with a reset pulse behind it (no power cycle) */
		struct instantnoodle_pin g26 = { INSTANTNOODLE_TLMM_SOUTH, 26 };
		bool was = instantnoodle_gpio_level(g26);

		keep("camera: next: pin 26 %s, then a reset pulse\n", was ? "LOW" : "HIGH");
		cam_out(g26, !was, was ? INSTANTNOODLE_PULL_NONE : INSTANTNOODLE_PULL_UP, 2);
		IOSleep(5);
		cam_out(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
		IOSleep(5);
		cam_out(s->reset, true, INSTANTNOODLE_PULL_NONE, 2);
		IOSleep(30);
		show_control_pins("with pin 26 flipped");
		scan_bus("pin 26 flipped, 30 ms after the pulse");
	}
	for (int tries = 0; tries < 3; tries++) {
		int err = instantnoodle_i2c_read16(&bus, s->address, s->id_reg, id, 2);

		keep("camera: chip ID of the %s at I2C %#04x, register %#06x: %s %02x%02x (it should be %#06x)\n", s->name, s->address, s->id_reg,
		    err ? "NO ANSWER" : "answers", id[0], id[1], s->id);
		if (err == 0) {
			break;
		}
		instantnoodle_i2c_recover(&bus);
	}
	{
		uint32_t dump = 0;

		if (PE_parse_boot_argn("cam_actdump", &dump, sizeof(dump)) && dump && s->vaf_pin != 0) {
			/* the lens driver at 0x0c: every register 0x00..0xff read one at a time (to tell the chip) */
			char line[160];
			int at = 0;

			for (unsigned r = 0; r < 256; r++) {
				uint8_t v = 0;
				int e = instantnoodle_i2c_read(&bus, 0x0c, (uint8_t)r, &v, 1);

				at += snprintf(line + at, sizeof(line) - (size_t)at, e == 0 ? " %02x" : " --", v);
				if ((r & 15) == 15) {
					say("camera: actuator 0x0c regs %02x..%02x:%s\n", r - 15, r, line);
					at = 0;
				}
			}
		}
	}
	cam_in(s->sda, INSTANTNOODLE_PULL_UP);
	cam_in(s->scl, INSTANTNOODLE_PULL_UP);
}

static void
power_down(void)
{
	const struct sensor *s = sensor;

	say("camera: power down: reset low, MCLK off, power domain off, clocks and supplies back\n");
	cam_out(s->reset, false, INSTANTNOODLE_PULL_DOWN, 2);
	IOSleep(1);
	if (did_mclk) {
		camcc[MCLK_CBCR(s->mclk) / 4] = camcc_rd(MCLK_CBCR(s->mclk)) & ~1u;
		cam_in((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, 94 + s->mclk }, INSTANTNOODLE_PULL_DOWN);
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
	if (did_vaf && s->vaf_pin != 0) {
		cam_out((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, s->vaf_pin }, false, INSTANTNOODLE_PULL_DOWN, 2);
		(void)instantnoodle_supply("ldof5", 0, false);
	}
	if (did_custom) {
		(void)instantnoodle_supply(s->custom, 0, false);   /* (smpa5 is shared with the Wi-Fi radio's RF rail: this gives back only this vote) */
	}
	if (did_vdig) {
		(void)instantnoodle_supply(s->vdig, 0, false);
	}
	for (unsigned i = 0; i < s->vanas; i++) {
		cam_out(s->vana[i], false, INSTANTNOODLE_PULL_DOWN, 2);
		cam_in(s->vana[i], INSTANTNOODLE_PULL_DOWN);
	}
	if (did_vio) {
		(void)instantnoodle_supply("ldof7", 0, false);
	}
	cam_in(s->reset, INSTANTNOODLE_PULL_DOWN);
	say("camera: everything put back (GCC camera AHB clock %#010x)\n", gcc_rd(GCC_CAMERA_AHB_CBCR));
}


/*
 * ---- cam=3: the frame path, rung A: the CSID's own test pattern through RDI 0 and IFE-lite 0 into memory ------------------------
 * docs/research/camera-frame-path-sm8250.md is the specification (every register there has its source in Qualcomm's driver for this
 * phone). No PHY, no sensor traffic: it shows that the clocks, CSID-lite 0, IFE-lite 0 (bus version 3), the write master and the
 * memory path work. Nothing is read from a block before its clocks are on (a read of a block with its clock off is a bus error and a
 * CrashDump); every step says what it is about to do in the loader's log first. cam_step=N stops after step N (default: all).
 */
#define CPAS_PHYS       0x0ac40000UL
#define LITE_PHYS       0x0acd9000UL    /* IFE-lite 0; CSID-lite 0 is at +0x200 of the same mapping */
#define SMMU_PHYS       0x15000000UL
#define CS(offset)      (0x200 + (offset))

static volatile uint32_t *cpas_regs, *lite_regs, *smmu_regs;
static uint32_t cam_level;                    /* (defined with its value below) */
extern uint64_t loader_recording[3];
struct fp_header {                  /* what the loader hands to the host (tools/camraw.py reads it) */
	char magic[8]; uint32_t width, height, stride, format, bayer, frames, frame_bytes, source; char sensor[16]; uint32_t last;
};
#define FP_HEADER_BYTES 4096
static uint32_t fp_steps = 99;
static vm_offset_t fp_keep;                  /* frame memory below 4 GB, taken at the start of the boot (cam >= 3) */
static vm_size_t fp_keep_bytes;

static inline uint32_t lrd(uint32_t offset) { return lite_regs[offset / 4]; }
static inline void lwr(uint32_t offset, uint32_t value) { lite_regs[offset / 4] = value; __asm__ volatile ("dsb sy" ::: "memory"); }
static inline void ccw(uint32_t offset, uint32_t value) { camcc[offset / 4] = value; __asm__ volatile ("dsb sy" ::: "memory"); }

static bool
fp_step(unsigned n, const char *what)
{
	if (n > fp_steps) {
		say("camera: stopped before step %u (%s): cam_step=%u\n", n, what, fp_steps);
		return false;
	}
	say("camera: step %u: %s\n", n, what);
	return true;
}

/* camcc's PLL0 (Lucid), kona's values (camcc-kona.c cam_cc_pll0_config), the enable flow of clk_lucid_pll_configure + alpha_pll_lucid_enable */
static bool fp_pll_on;
static bool
fp_pll0_up(void)
{
	static const uint32_t config[][2] = {
		{ 0x04, 0x3e }, { 0x08, 0x44 }, { 0x40, 0x8000 }, { 0x18, 0x20485699 }, { 0x1c, 0x00002261 }, { 0x20, 0x329a699c },
		{ 0x0c, 0x00003100 }, { 0x10, 0x00000805 }, { 0x14, 0x00000000 },
	};
	uint32_t mode = camcc_rd(0);

	say("camera: PLL0 mode %#x before\n", mode);
	if (mode & 1) {
		say("camera: camcc's PLL0 is running already (somebody else's): left alone\n");
		return true;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		ccw(config[i][0], config[i][1]);
	}
	fp_pll_on = true;
	ccw(0, camcc_rd(0) | 1u << 23);                 /* update bypass */
	ccw(0, camcc_rd(0) & ~1u);                      /* outputs off */
	ccw(0x38, 0);                                   /* standby */
	ccw(0, camcc_rd(0) | 1u << 2);                  /* out of reset */
	ccw(0x38, 1);                                   /* run */
	for (int i = 0; i < 1000 && !(camcc_rd(0) & 1u << 31); i++) {
		IODelay(1);
	}
	if (!(camcc_rd(0) & 1u << 31)) {
		say("camera: PLL0 does not lock (mode %#x)\n", camcc_rd(0));
		return false;
	}
	ccw(0x0c, camcc_rd(0x0c) | 7);                  /* the three outputs */
	ccw(0, camcc_rd(0) | 1);
	say("camera: PLL0 locked (mode %#x, user_ctl %#x)\n", camcc_rd(0), camcc_rd(0x0c));
	return true;
}

static void
fp_pll0_down(void)
{
	if (fp_pll_on) {
		ccw(0, camcc_rd(0) & ~1u);
		ccw(0x0c, camcc_rd(0x0c) & ~7u);
		ccw(0x38, 0);
		ccw(0, camcc_rd(0) & ~(1u << 2));
		fp_pll_on = false;
		say("camera: PLL0 off (mode %#x)\n", camcc_rd(0));
	}
}

static struct { const char *name; uint32_t cmd, source, divider; } fp_rcgs[] = {      /* (not const: the CSIPHY timer is the front sensor's when that runs, fpd_phy_clocks) */
	{ "slow_ahb 80 MHz", 0x7058, 2, 14 }, { "camnoc_axi 300 MHz", 0xc0f8, 2, 3 }, { "cphy_rx 400 MHz", 0xa068, 3, 1 },
	{ "ife_lite_csid 400 MHz", 0xc01c, 3, 1 }, { "ife_lite 400 MHz", 0xc000, 3, 1 }, { "csi0phytimer 300 MHz", 0x6000, 2, 3 },
};
static uint32_t fp_rcg_found[6];
static bool fp_rcg_set[6];

static struct { const char *name; bool gcc; uint32_t cbcr; bool poll; } fp_branches[] = {
	{ "gcc camera_hf_axi", true, 0xb02c, true }, { "gcc camera_sf_axi", true, 0xb030, true },
	{ "cpas_ahb", false, 0xc0f4, true }, { "core_ahb", false, 0xc150, false }, { "camnoc_axi", false, 0xc114, true },
	{ "ife_lite_csid", false, 0xc034, true }, { "ife_lite_cphy_rx", false, 0xc03c, true }, { "ife_lite_ahb", false, 0xc040, true },
	{ "ife_lite_axi", false, 0xc044, true }, { "ife_lite", false, 0xc018, true },
	{ "csiphy0", false, 0x601c, true }, { "csi0phytimer", false, 0x6018, true },
};
static bool fp_branch_set[12];

static bool
fp_clocks_up(void)
{
	for (unsigned i = 0; i < 2; i++) {                     /* the GCC's AXI branches first */
		uint32_t value = gcc_rd(fp_branches[i].cbcr);

		if (!(value & 1)) {
			gcc[fp_branches[i].cbcr / 4] = value | 1;
			fp_branch_set[i] = true;
		}
		for (int w = 0; w < 500 && (gcc_rd(fp_branches[i].cbcr) & CBCR_OFF); w++) {
			IODelay(10);
		}
		say("camera: %s: %#x\n", fp_branches[i].name, gcc_rd(fp_branches[i].cbcr));
	}
	if (!fp_pll0_up()) {
		return false;
	}
	for (unsigned i = 0; i < 6; i++) {
		uint32_t cfg = fp_rcgs[i].cmd + 4;

		fp_rcg_found[i] = camcc_rd(cfg);
		ccw(cfg, (fp_rcg_found[i] & ~0x371fu) | fp_rcgs[i].source << 8 | fp_rcgs[i].divider);
		fp_rcg_set[i] = true;
		ccw(fp_rcgs[i].cmd, camcc_rd(fp_rcgs[i].cmd) | 1u);
		for (int w = 0; w < 500 && (camcc_rd(fp_rcgs[i].cmd) & 1u); w++) {
			IODelay(10);
		}
		say("camera: RCG %s: cmd %#x cfg %#x (was %#x)\n", fp_rcgs[i].name, camcc_rd(fp_rcgs[i].cmd), camcc_rd(cfg), fp_rcg_found[i]);
		if (camcc_rd(fp_rcgs[i].cmd) & 1u) {
			say("camera: the RCG %s does not take its configuration: the rails may be too low\n", fp_rcgs[i].name);
			return false;
		}
	}
	for (unsigned i = 2; i < 12; i++) {
		uint32_t value = camcc_rd(fp_branches[i].cbcr);

		if (!(value & 1)) {
			ccw(fp_branches[i].cbcr, value | 1);
			fp_branch_set[i] = true;
		}
		if (fp_branches[i].poll) {
			for (int w = 0; w < 500 && (camcc_rd(fp_branches[i].cbcr) & CBCR_OFF); w++) {
				IODelay(10);
			}
		}
		say("camera: branch %s: %#x%s\n", fp_branches[i].name, camcc_rd(fp_branches[i].cbcr),
		    (camcc_rd(fp_branches[i].cbcr) & CBCR_OFF) ? " STILL OFF" : "");
	}
	return true;
}

static void
fp_clocks_down(void)
{
	for (int i = 11; i >= 2; i--) {
		if (fp_branch_set[i]) {
			ccw(fp_branches[i].cbcr, camcc_rd(fp_branches[i].cbcr) & ~1u);
			fp_branch_set[i] = false;
		}
	}
	for (int i = 5; i >= 0; i--) {
		if (fp_rcg_set[i]) {
			ccw(fp_rcgs[i].cmd + 4, fp_rcg_found[i]);
			ccw(fp_rcgs[i].cmd, camcc_rd(fp_rcgs[i].cmd) | 1u);
			fp_rcg_set[i] = false;
		}
	}
	fp_pll0_down();
	for (int i = 1; i >= 0; i--) {
		if (fp_branch_set[i]) {
			gcc[fp_branches[i].cbcr / 4] = gcc_rd(fp_branches[i].cbcr) & ~1u;
			fp_branch_set[i] = false;
		}
	}
}

/*
 * The frame's memory behind the IFE's stream: the apps SMMU faults a stream nobody matches (sCR0.USFCFG) and the kernel has no memory below
 * 4 GB (the write master's address has 32 bits), so a free match register and a free context bank (never one a valid match register uses)
 * send the IFE streams (kona: 0x800 with mask 0x441, camera DT "msm_cam_smmu_ife") through tables of ours: FP_IOVA -> the buffer.
 * Ported from the POCO X5 Pro's hk_camera.c smmu_find/smmu_up (the same SMMU, its global registers behind hk_smmu_lock).
 */
extern lck_mtx_t hk_smmu_lock;
#define FP_IOVA                 0x40000000ULL
/*
 * Match registers and contexts below these are left to the others that take the lowest free ones when they start: the sound (match register 5,
 * context 4, at its first sound) and the Wi-Fi. With the camera up first (Camera.app opened before the first sound), the camera had taken
 * context 4 and match registers 5.., the sound then found register 7 free (its SMR refused to the camera, its S2CR left at context 4) and the
 * audio DSP gave up ("audio_process", CrashDump) as the first sound began: the layout of the runs that worked (sound first, the camera on
 * context 6 and match registers 7..) is now the only one.
 */
#define FP_FIRST_MATCH          7
#define FP_FIRST_CTX            6
#define FP_STREAM               0x800
#define FP_STREAM_MASK          0x441
#define FP_PTE                  (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define SM_SMR(n)               (0x800 + 4 * (n))
#define SM_S2CR(n)              (0xc00 + 4 * (n))
#define SM_CBAR(n)              (4 * (n))
#define SM_CBA2R(n)             (0x800 + 4 * (n))
#define SM_CBAR_STAGE_1         (1u << 16 | 3u << 8 | 0xfu << 12)
#define SM_TCR_39               (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SM_TCR2                 (1u | 1u << 4 | 7u << 15)
#define SM_MAIR0                0xf404ff44
#define SM_FSR_ALL              0xc00001fe
#define SM_SCTLR_ON             (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)

static volatile uint32_t *fp_ctx;
static const uint32_t fp_streams[8] = { 0x800, 0x801, 0x840, 0x841, 0xc00, 0xc01, 0xc40, 0xc41 };   /* the device tree's eight, one match register each */
static uint32_t fp_match[8], fp_old_smr[8], fp_old_s2cr[8], fp_want[8], fp_nmatch, fp_ctx_n;   /* fp_want: what we wrote (only those are put back) */
static bool fp_smmu_on;
static vm_offset_t fp_tables;
static vm_size_t fp_tables_bytes;

static bool
fp_put(volatile uint32_t *base, uint32_t offset, uint32_t value, uint32_t check)
{
	base[offset / 4] = value;
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((base[offset / 4] & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, base[offset / 4]);
		return false;
	}
	return true;
}

/*
 * What fp_smmu_up() takes for one stream, given back: the page tables (and the context bank's page mapping kept for the next
 * start, fp_ctx_map()). Each start of the camera took both anew and nothing gave them back; with the sensor DSP's mappings beside them (slpi=1) the kernel's map
 * ran out after a few camera starts and switches: "kmem_alloc(..., 16384, 0x100021): failed with 3" in io_map() under
 * fp_smmu_up(), a panic, the kernel's return (2026-11-07, RAM image 182, switching to the front camera).
 */
/* A context bank's page, mapped once for all the camera's starts (io_map()'s range is small and nothing in it comes back) */
static volatile uint32_t *
fp_ctx_map(uint64_t phys, uint32_t n)
{
	static volatile uint32_t *mapped[256];

	if (n >= 256) {
		return NULL;
	}
	if (mapped[n] == NULL) {
		mapped[n] = (volatile uint32_t *)ml_io_map(phys, 0x1000);
	}
	return mapped[n];
}

static void
fp_smmu_release(void)
{
	if (fp_ctx != NULL) {
		for (unsigned waited = 0; (fp_ctx[0x7f4 / 4] & 1) && waited < 1000; waited++) {
			IODelay(10);            /* (the context's TLB sync: its tables are still the SMMU's until it is done) */
		}
		/* (the mapping stays: io_map()'s are permanent -- ml_io_unmap() of one panicked, "Attempting to remove permanent
		 * VM map entry", RAM image 183 -- and fp_ctx_map() hands the same one out again) */
	}
	if (fp_tables != 0) {
		kmem_free(kernel_map, fp_tables, fp_tables_bytes);
		fp_tables = 0;
		fp_tables_bytes = 0;
	}
}

static bool
fp_smmu_up(vm_offset_t buffer, uint64_t bytes)
{
	bool used[256] = { false };
	uint32_t idr0 = smmu_regs[0x20 / 4], idr1 = smmu_regs[0x24 / 4], pages, pagesize, contexts = idr1 & 0xff;
	uint32_t thirds = (uint32_t)((bytes + 0x1fffff) >> 21), npages = (uint32_t)(bytes >> 12);
	uint64_t *root, *second, table_root;
	volatile uint32_t *second_page = smmu_regs + 0x1000 / 4;

	pagesize = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	if (pagesize != 0x1000 || 2 * pages * pagesize != 0x100000) {
		say("camera: the SMMU's layout is not the one known here (page %#x, %u pages)\n", pagesize, pages);
		return false;
	}
	fp_smmu_release();      /* (anything a failed start left) */
	lck_mtx_lock(&hk_smmu_lock);
	fp_nmatch = 0;
	for (uint32_t n = 0; n < (idr0 & 0xff) && SM_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu_regs[SM_SMR(n) / 4], s2cr = smmu_regs[SM_S2CR(n) / 4];

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			for (unsigned k = 0; k < 8; k++) {
				if (((smr & 0x7fff) ^ fp_streams[k]) & ~(smr >> 16 & 0x7fff)) {
					continue;
				}
				say("camera: SMMU match register %u (%#x) is somebody's for the IFEs' stream %#x: left alone\n", n, smr, fp_streams[k]);
				lck_mtx_unlock(&hk_smmu_lock);
				return false;
			}
		} else if (fp_nmatch < 8 && n >= FP_FIRST_MATCH) {
			fp_match[fp_nmatch] = n;
			fp_old_smr[fp_nmatch] = smr;
			fp_old_s2cr[fp_nmatch] = s2cr;
			fp_want[fp_nmatch] = 0;
			fp_nmatch++;
		}
	}
	for (fp_ctx_n = FP_FIRST_CTX; fp_ctx_n < contexts && used[fp_ctx_n]; fp_ctx_n++) {
	}
	if (fp_nmatch < 8 || fp_ctx_n >= contexts) {
		say("camera: the SMMU has no 8 match registers (%u free) or no context left\n", fp_nmatch);
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	fp_ctx = fp_ctx_map(SMMU_PHYS + (uint64_t)(pages + fp_ctx_n) * pagesize, fp_ctx_n);
	if (fp_ctx == NULL) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	if (fp_ctx[0] & 1) {
		say("camera: SMMU context %u is switched on although no stream uses it: somebody's\n", fp_ctx_n);
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	fp_tables_bytes = (2 + thirds) * 4096;
	if (kmem_alloc(kernel_map, &fp_tables, fp_tables_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for page tables\n");
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	root = (uint64_t *)fp_tables;
	second = (uint64_t *)(fp_tables + 4096);
	root[FP_IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(fp_tables + (2 + t) * 4096);

		second[(FP_IOVA >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t q = 0; q < 512 && t * 512 + q < npages; q++) {
			third[q] = kvtophys(buffer + 4096ull * (t * 512 + q)) | FP_PTE;
		}
	}
	for (vm_offset_t line = fp_tables; line < fp_tables + fp_tables_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(fp_tables) | (uint64_t)fp_ctx_n << 48;
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match registers %u.. send the IFE's eight streams through it\n", fp_ctx_n,
	    bytes >> 10, FP_IOVA, fp_match[0]);
	if (!fp_put(fp_ctx, 0x00, 0, ~0u) || !fp_put(second_page, SM_CBA2R(fp_ctx_n), 1, 1) ||
	    !fp_put(second_page, SM_CBAR(fp_ctx_n), SM_CBAR_STAGE_1, 0x30000) || !fp_put(fp_ctx, 0x10, SM_TCR2, 0) ||
	    !fp_put(fp_ctx, 0x30, SM_TCR_39, ~0u) || !fp_put(fp_ctx, 0x20, (uint32_t)table_root, ~0u) ||
	    !fp_put(fp_ctx, 0x24, (uint32_t)(table_root >> 32), ~0u) || !fp_put(fp_ctx, 0x28, 0, ~0u) || !fp_put(fp_ctx, 0x2c, 0, ~0u) ||
	    !fp_put(fp_ctx, 0x38, SM_MAIR0, ~0u) || !fp_put(fp_ctx, 0x3c, 0, ~0u)) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	fp_ctx[0x58 / 4] = SM_FSR_ALL;
	fp_ctx[0x610 / 4] = fp_ctx_n;
	fp_ctx[0x7f0 / 4] = 1;
	for (unsigned waited = 0; fp_ctx[0x7f4 / 4] & 1; waited++) {
		if (waited > 1000) {
			say("camera: the SMMU does not finish forgetting\n");
			lck_mtx_unlock(&hk_smmu_lock);
			return false;
		}
		IODelay(10);
	}
	if (!fp_put(fp_ctx, 0x00, SM_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	fp_smmu_on = true;
	unsigned accepted = 0, kept = 0;

	/*
	 * A register the hypervisor refuses (its SMR reads 0: it keeps some of the IFE's streams to itself) is given back at once and
	 * forgotten: it still reads free, so the next driver to start takes it, and putting back "what was found" at the camera's stop
	 * would wipe that driver's stream out. Seen 2026-11-07: the sensor DSP's FastRPC (hk_fastrpc.c) took match register 10, refused
	 * here, after the camera's first stream; the camera's stop then wrote 0 over it, the DSP's stream 0x541 matched nothing any more,
	 * and the next opening of Camera.app ended the kernel's run.
	 */
	for (unsigned k = 0; k < 8; k++) {
		uint32_t n = fp_match[k], want = 1u << 31 | fp_streams[k];

		smmu_regs[SM_S2CR(n) / 4] = fp_ctx_n;
		smmu_regs[SM_SMR(n) / 4] = want;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (smmu_regs[SM_SMR(n) / 4] == want && (smmu_regs[SM_S2CR(n) / 4] & 0xff) == fp_ctx_n) {
			accepted++;
			fp_match[kept] = n;
			fp_old_smr[kept] = fp_old_smr[k];
			fp_old_s2cr[kept] = fp_old_s2cr[k];
			fp_want[kept] = want;
			kept++;
		} else {
			say("camera: stream %#x: match register %u refused (smr reads %#x, s2cr %#x): given back\n", fp_streams[k], n,
			    smmu_regs[SM_SMR(n) / 4], smmu_regs[SM_S2CR(n) / 4]);
			smmu_regs[SM_SMR(n) / 4] = fp_old_smr[k];
			smmu_regs[SM_S2CR(n) / 4] = fp_old_s2cr[k];
			__asm__ volatile ("dsb sy" ::: "memory");
		}
	}
	fp_nmatch = kept;
	say("camera: the hypervisor took %u of the IFE's 8 streams\n", accepted);
	if (accepted == 0) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	lck_mtx_unlock(&hk_smmu_lock);
	return true;
}

/* Our match registers back as they were found: only those that still say what we wrote (fp_want); one that says something else is somebody's now.
 * (No logging: the kernel's quiesce on the way back to the loader calls this too.) */
static void
fp_put_back_matches(void)
{
	for (unsigned k = 0; k < fp_nmatch; k++) {
		uint32_t n = fp_match[k];

		if (fp_want[k] == 0 || smmu_regs[SM_SMR(n) / 4] != fp_want[k]) {
			continue;       /* never written by us, or somebody's now */
		}
		smmu_regs[SM_SMR(n) / 4] = fp_old_smr[k];
		smmu_regs[SM_S2CR(n) / 4] = fp_old_s2cr[k];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
fp_smmu_faults(void)
{
	if (!fp_smmu_on) {
		return;
	}
	uint32_t status = fp_ctx[0x58 / 4], global = smmu_regs[0x48 / 4];

	if ((status & SM_FSR_ALL) == 0 && global == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u)\n", fp_ctx_n);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n", fp_ctx_n,
	    status, (uint64_t)fp_ctx[0x64 / 4] << 32 | fp_ctx[0x60 / 4], fp_ctx[0x68 / 4], global, smmu_regs[0x54 / 4]);
}

static void
fp_smmu_down(void)
{
	if (!fp_smmu_on && fp_nmatch == 0) {
		return;
	}
	fp_smmu_faults();
	lck_mtx_lock(&hk_smmu_lock);
	fp_put_back_matches();
	if (fp_smmu_on) {
		fp_ctx[0x00] = 0;
		fp_ctx[0x610 / 4] = fp_ctx_n;
		fp_ctx[0x7f0 / 4] = 1;
	}
	lck_mtx_unlock(&hk_smmu_lock);
	fp_smmu_on = false;
	fp_nmatch = 0;
	fp_smmu_release();
	say("camera: the SMMU's match register and context put back as they were found\n");
}

/* generated from op8pro-data/lineage-kernel/techpack/camera/.../cam_csiphy_1_2_1_hwreg.h csiphy_3ph_v1_2_1_reg (trios 0..2) and the first row of data_rate_delta_table_1_2_1 */
struct fp_cphy_reg { uint16_t address; uint8_t data, type; };    /* type: 0 plain, 1 the settle count's low byte, 2 its high byte */
static const struct fp_cphy_reg fp_cphy_trio[3][34] = {
	{ { 0x0990, 0x00, 0 }, { 0x0994, 0x00, 0 }, { 0x0998, 0x00, 0 }, { 0x0990, 0x08, 0 }, { 0x0994, 0x08, 0 }, { 0x0998, 0x1a, 0 }, { 0x098c, 0xaf, 0 }, { 0x0168, 0xac, 0 }, { 0x0104, 0x06, 0 }, { 0x010c, 0x07, 1 }, { 0x0108, 0x00, 2 }, { 0x0114, 0x20, 0 }, { 0x0150, 0x00, 0 }, { 0x0188, 0x00, 0 }, { 0x018c, 0x7f, 0 }, { 0x0190, 0x7f, 0 }, { 0x0118, 0x3e, 0 }, { 0x011c, 0x41, 0 }, { 0x0120, 0x41, 0 }, { 0x0124, 0x7f, 0 }, { 0x0128, 0x00, 0 }, { 0x012c, 0x00, 0 }, { 0x0160, 0x02, 0 }, { 0x01cc, 0x41, 0 }, { 0x0164, 0x33, 0 }, { 0x01dc, 0x50, 0 }, { 0x09c0, 0x80, 0 }, { 0x09c4, 0x7d, 0 }, { 0x09c8, 0x7f, 0 }, { 0x0984, 0x20, 0 }, { 0x0988, 0x05, 0 }, { 0x0980, 0x60, 0 }, { 0x09b0, 0x01, 0 }, { 0x0800, 0x0e, 0 } },
	{ { 0x0a90, 0x00, 0 }, { 0x0a94, 0x00, 0 }, { 0x0a98, 0x00, 0 }, { 0x0a90, 0x08, 0 }, { 0x0a94, 0x00, 0 }, { 0x0a98, 0x1f, 0 }, { 0x0a8c, 0xbf, 0 }, { 0x0368, 0xac, 0 }, { 0x0304, 0x06, 0 }, { 0x030c, 0x07, 1 }, { 0x0308, 0x00, 2 }, { 0x0314, 0x20, 0 }, { 0x0350, 0x00, 0 }, { 0x0388, 0x00, 0 }, { 0x038c, 0x7f, 0 }, { 0x0390, 0x7f, 0 }, { 0x0318, 0x3e, 0 }, { 0x031c, 0x41, 0 }, { 0x0320, 0x41, 0 }, { 0x0324, 0x7f, 0 }, { 0x0328, 0x00, 0 }, { 0x032c, 0x00, 0 }, { 0x0360, 0x02, 0 }, { 0x03cc, 0x41, 0 }, { 0x0364, 0x33, 0 }, { 0x03dc, 0x50, 0 }, { 0x0ac0, 0x80, 0 }, { 0x0ac4, 0x7d, 0 }, { 0x0ac8, 0x7f, 0 }, { 0x0a84, 0x20, 0 }, { 0x0a88, 0x05, 0 }, { 0x0a80, 0x60, 0 }, { 0x0ab0, 0x01, 0 }, { 0x0800, 0x0e, 0 } },
	{ { 0x0b90, 0x00, 0 }, { 0x0b94, 0x00, 0 }, { 0x0b98, 0x00, 0 }, { 0x0b90, 0x08, 0 }, { 0x0b94, 0x08, 0 }, { 0x0b98, 0x1a, 0 }, { 0x0b8c, 0xaf, 0 }, { 0x0568, 0xac, 0 }, { 0x0504, 0x06, 0 }, { 0x050c, 0x07, 1 }, { 0x0508, 0x00, 2 }, { 0x0514, 0x20, 0 }, { 0x0550, 0x00, 0 }, { 0x0588, 0x00, 0 }, { 0x058c, 0x7f, 0 }, { 0x0590, 0x7f, 0 }, { 0x0518, 0x3e, 0 }, { 0x051c, 0x41, 0 }, { 0x0520, 0x41, 0 }, { 0x0524, 0x7f, 0 }, { 0x0528, 0x00, 0 }, { 0x052c, 0x00, 0 }, { 0x0560, 0x02, 0 }, { 0x05cc, 0x41, 0 }, { 0x0564, 0x33, 0 }, { 0x05dc, 0x50, 0 }, { 0x0bc0, 0x80, 0 }, { 0x0bc4, 0x7d, 0 }, { 0x0bc8, 0x7f, 0 }, { 0x0b84, 0x20, 0 }, { 0x0b88, 0x05, 0 }, { 0x0b80, 0x60, 0 }, { 0x0bb0, 0x01, 0 }, { 0x0800, 0x0e, 0 } },
};
static const struct fp_cphy_reg fp_cphy_rate0[12] = {      /* data_rate_delta_table_1_2_1, the row up to 5.7 Gbit/s a trio */
	{ 0x15c, 0x66, 0 }, { 0x35c, 0x66, 0 }, { 0x55c, 0x66, 0 }, { 0x9b4, 0x03, 0 }, { 0xab4, 0x03, 0 }, { 0xbb4, 0x03, 0 },
	{ 0x144, 0x22, 0 }, { 0x344, 0x22, 0 }, { 0x544, 0x22, 0 }, { 0x16c, 0xad, 0 }, { 0x36c, 0xad, 0 }, { 0x56c, 0xad, 0 },
};

extern const uint8_t *hk_wifi_file(const char *name, uint64_t *size);   /* a file of fw/ in the small image */

/* ---- rung B: the sensor's tables (tools/camsensor.py: "RWCTAB02", as hk_camera.c reads them), the CSIPHY, the refgen ---- */
struct fp_ctab_head { char magic[8]; uint32_t slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes; };
struct fp_ctab_mode { uint32_t width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type; };
struct fp_ctab_entry { uint16_t address, data; uint8_t address_bytes, data_bytes, operation, zero; uint32_t delay_us; };
static const uint8_t *fp_tab;
static uint64_t fp_tab_size;
static const struct fp_ctab_head *fp_tab_head;

static const struct fp_ctab_entry *
fp_tab_find(const char *name, uint32_t *count)
{
	const uint8_t *at, *end = fp_tab + fp_tab_size;
	uint32_t blocks;

	*count = 0;
	if (fp_tab == NULL) {
		return NULL;
	}
	at = (const uint8_t *)(fp_tab_head + 1) + fp_tab_head->modes * sizeof(struct fp_ctab_mode);
	for (int pw = 0; pw < 2; pw++) {
		if (at + 12 > end) {
			return NULL;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct fp_ctab_entry);
	}
	blocks = *(const uint32_t *)at;
	at += 4;
	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct fp_ctab_entry) <= end) {
			*count = n;
			return (const struct fp_ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct fp_ctab_entry);
	}
	return NULL;
}

static bool
fp_tab_load(const char *file)
{
	fp_tab = hk_wifi_file(file, &fp_tab_size);
	if (fp_tab == NULL || fp_tab_size < sizeof(struct fp_ctab_head) || memcmp(fp_tab, "RWCTAB02", 8) != 0) {
		say("camera: no %s (RWCTAB02) in fw/ of the small image\n", file);
		fp_tab = NULL;
		return false;
	}
	fp_tab_head = (const struct fp_ctab_head *)fp_tab;
	say("camera: %s: slave %#x, chip ID %#x at %#x, lane assignment %#x, %u modes\n", file, fp_tab_head->slave, fp_tab_head->id, fp_tab_head->id_address,
	    fp_tab_head->lane_assign, fp_tab_head->modes);
	return true;
}

/* A table written in order over the (bit-banged) bus, one transfer a write; each byte must be acknowledged. */
static bool
fp_sensor_table(const char *name)
{
	uint32_t count;
	const struct fp_ctab_entry *e = fp_tab_find(name, &count);

	if (e == NULL) {
		say("camera: the sensor's tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[4];
		unsigned n = 0;

		if (e[i].operation != 0 || e[i].address_bytes != 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("camera: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes, e[i].data_bytes);
			return false;
		}
		out[n++] = (uint8_t)(e[i].address >> 8);
		out[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			out[n++] = (uint8_t)(e[i].data >> 8);
		}
		out[n++] = (uint8_t)e[i].data;
		if (instantnoodle_i2c_write(&bus, (uint8_t)(fp_tab_head->slave >> 1), out, n) != 0) {
			say("camera: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			return false;
		}
		if (e[i].delay_us != 0) {
			IODelay(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	say("camera: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

static volatile uint32_t *fp_phy, *fp_phy0, *fp_phy2, *fp_phy3, *fp_refgen;   /* fp_phy: the CSIPHY of the sensor that runs (0 the main, 3 the front) */
static bool fp_refgen_set, fp_ldoa9;
static inline void pwr(uint32_t offset, uint32_t value) { fp_phy[offset / 4] = value; }

/* cam_csiphy_reset(), the common registers for three C-PHY trios, the trios' registers with the settle count, the rate row, the interrupt masks. */
static void
fp_csiphy_up(uint32_t settle)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };

	pwr(0x814, 0);
	IODelay(5000);
	pwr(0x818, 0);
	pwr(0x81c, 0);
	pwr(0x800, 1);
	IODelay(1);
	pwr(0x800, 0);
	pwr(0x814, 0x2a);                       /* trios 0, 1, 2: bit 2n + 1 */
	pwr(0x818, 0x01);
	pwr(0x81c, 0x52);                       /* three-phase */
	pwr(0x800, 0x0e);
	for (unsigned t = 0; t < 3; t++) {
		for (unsigned i = 0; i < 34; i++) {
			uint8_t value = fp_cphy_trio[t][i].data;

			if (fp_cphy_trio[t][i].type == 1) {
				value = (uint8_t)settle;
			} else if (fp_cphy_trio[t][i].type == 2) {
				value = (uint8_t)(settle >> 8);
			}
			pwr(fp_cphy_trio[t][i].address, value);
		}
	}
	for (unsigned i = 0; i < 12; i++) {
		pwr(fp_cphy_rate0[i].address, fp_cphy_rate0[i].data);
	}
	for (unsigned i = 0; i < 11; i++) {
		pwr(0x82c + 4 * i, irq_masks[i]);
		pwr(0x858 + 4 * i, 0xff);
	}
	pwr(0x828, 1);
	pwr(0x828, 0);
	say("camera: CSIPHY 0 configured: three C-PHY trios, settle count %u\n", settle);
}

static void
fp_csiphy_down(void)
{
	pwr(0x814, 0);
	IODelay(5000);
	pwr(0x818, 0);
	pwr(0x81c, 0);
	pwr(0x800, 1);
	IODelay(1);
	pwr(0x800, 0);
}

/*
 * cam_csiphy_config_dev() for the front IMX471: four D-PHY lanes (csiphy_2ph_v1_2_1_reg of Android's cam_csiphy_1_2_1_hwreg.h, lane_mask 0x1f:
 * data lanes at lane_pos 0, 2, 3, 4 and the clock lane at lane_pos 1; common registers 0x814 = 0xd5, 0x818 = 1, 0x81c = 2, 0x800 = 2). The
 * entries marked "do not program" there (0x0c, 0x28, 0x24 of the data lanes) are left out as Android's switch does. No rate table for D-PHY.
 * `settle` is the lane's HS-settle count (CamX's value is not in the module file: sweep by cam_fsettle).
 */
static void
fp_csiphy_up_dphy(uint32_t settle)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	static const struct { uint16_t b, m; bool clock; } lanes[5] = {
		{ 0x000, 0x900, false }, { 0x700, 0xc80, true }, { 0x200, 0xa00, false }, { 0x400, 0xb00, false }, { 0x600, 0xc00, false },
	};

	pwr(0x814, 0);
	IODelay(5000);
	pwr(0x818, 0);
	pwr(0x81c, 0);
	pwr(0x800, 1);
	IODelay(1);
	pwr(0x800, 0);
	pwr(0x814, 0xd5);                       /* four data lanes (bits 0, 2, 4, 6) and the clock lane (0x80) */
	pwr(0x818, 0x01);
	pwr(0x81c, 0x02);
	pwr(0x800, 0x02);
	for (unsigned l = 0; l < 5; l++) {
		const uint32_t b = lanes[l].b, m = lanes[l].m;

		pwr(b + 0x30, 0x00);
		pwr(m + 0x00, 0x05);
		pwr(m + 0x08, 0x10);
		pwr(m + 0x04, 0x00);
		pwr(m + 0x04, 0x03);
		pwr(b + 0x04, 0x0c);
		pwr(b + 0x2c, 0x01);
		pwr(b + 0x34, 0x07);
		pwr(b + 0x10, 0x02);
		pwr(b + 0x1c, 0x08);
		pwr(b + 0x3c, 0xb8);
		pwr(b + 0x08, settle & 0xff);
		pwr(b + 0x00, lanes[l].clock ? 0x80 : 0x8d);
		if (lanes[l].clock) {
			pwr(b + 0x0c, 0xa5);
		}
		pwr(b + 0x38, lanes[l].clock ? 0x1f : 0xfe);
		pwr(b + 0x14, 0x60);
		if (lanes[l].clock) {
			pwr(b + 0x28, 0x04);
		}
		pwr(0x800, 0x02);
		pwr(0x884, 0x01);
	}
	for (unsigned i = 0; i < 11; i++) {
		pwr(0x82c + 4 * i, irq_masks[i]);
		pwr(0x858 + 4 * i, 0xff);
	}
	pwr(0x828, 1);
	pwr(0x828, 0);
	say("camera: CSIPHY %u configured: four D-PHY lanes, settle count %u\n", fp_phy == fp_phy2 ? 2u : 3u, settle);
}

static void
fp_show_phy(const char *when)
{
	say("camera: [%s] CSIPHY irq status %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", when, fp_phy[0x8b0 / 4] & 0xff,
	    fp_phy[0x8b4 / 4] & 0xff, fp_phy[0x8b8 / 4] & 0xff, fp_phy[0x8bc / 4] & 0xff, fp_phy[0x8c0 / 4] & 0xff, fp_phy[0x8c4 / 4] & 0xff,
	    fp_phy[0x8c8 / 4] & 0xff, fp_phy[0x8cc / 4] & 0xff, fp_phy[0x8d0 / 4] & 0xff, fp_phy[0x8d4 / 4] & 0xff, fp_phy[0x8d8 / 4] & 0xff);
}

static void
fp_show(const char *when)
{
	say("camera: [%s] CSID rx irq %#x rdi0 irq %#x packets %u ecc %u crc %u bytes %u/%u rdi0 status %#x | IFE top irq %#x/%#x/%#x bus irq %#x "
	    "camif rdi0 %#x wm0 addr status %#x %#x\n", when, lrd(CS(0x20)), lrd(CS(0x30)), lrd(CS(0x160)), lrd(CS(0x164)), lrd(CS(0x168)),
	    lrd(CS(0x2e0)), lrd(CS(0x2e4)), lrd(CS(0x250)), lrd(0x40), lrd(0x44), lrd(0x48), lrd(0x1a28), lrd(0x1204), lrd(0x1c68), lrd(0x1c6c));
}

static void
fp_tpg(void)
{
	const bool sensor = cam_level == 4;
	uint32_t width = 1600, height = 1200, stride, frame, mode = 4, settle = 0x22;
	vm_offset_t buffer = 0;
	uint64_t phys;
	bool fault = false;

	(void)PE_parse_boot_argn("cam_step", &fp_steps, sizeof(fp_steps));
	(void)PE_parse_boot_argn("cam_mode", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("cam_settle", &settle, sizeof(settle));
	if (sensor) {
		width = 2000;           /* the IMX689's mode 4: 2000 x 1128, 4 x 4 binned, RAW10, one stream (docs/research/camera-frame-path-sm8250.md section 6) */
		height = 1128;
	}
	stride = (width * 10 / 8 + 15) & ~15u;
	frame = stride * height;
	/* 1: what the SMMU does with a stream nobody matches (global registers only) */
	if (!fp_step(1, "the apps SMMU's global registers, read only")) {
		return;
	}
	smmu_regs = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x4000);
	uint32_t scr0 = smmu_regs[0], idr0 = smmu_regs[0x20 / 4], idr1 = smmu_regs[0x24 / 4];
	say("camera: SMMU sCR0 %#x (a stream nobody matches %s), IDR0 %#x IDR1 %#x\n", scr0, scr0 & (1u << 10) ? "FAULTS" : "bypasses", idr0, idr1);
	/* 2: rails */
	if (!fp_step(2, "RPMh: MX at SVS_L1 (192), for camcc's PLL0 at 1200 MHz")) {
		return;
	}
	bool mx = instantnoodle_rail_level("mx.lvl", 192);
	say("camera: MX vote: %s\n", mx ? "taken" : "NOT taken by RPMh");
	if (sensor) {
		/* the PHY's 1.2 V supply (pm8150_l9: held by others too) and its reference generator (off even on an idle Android) */
		fp_ldoa9 = instantnoodle_supply("ldoa9", 1200, true);
		fp_refgen = (volatile uint32_t *)ml_io_map(0x088e7000UL, 0x1000);
		say("camera: ldoa9 vote: %s; refgen +0x80 reads %#x\n", fp_ldoa9 ? "taken" : "NOT taken", fp_refgen[0x80 / 4]);
		if (!(fp_refgen[0x80 / 4] & 1)) {
			fp_refgen[0x80 / 4] |= 1;
			fp_refgen_set = true;
			say("camera: refgen enabled: +0x80 reads %#x\n", fp_refgen[0x80 / 4]);
		}
	}
	/* 3-6: clocks */
	if (!fp_step(3, "GCC AXI branches, camcc PLL0, the RCGs and the branches of CSID-lite, IFE-lite and CAMNOC")) {
		goto down;
	}
	if (!fp_clocks_up()) {
		goto down;
	}
	/* 6b: the first reads of the blocks (their clocks are on now) */
	if (!fp_step(4, "the first reads: CPAS top, IFE-lite 0 and CSID-lite 0 versions")) {
		goto down;
	}
	cpas_regs = (volatile uint32_t *)ml_io_map(CPAS_PHYS, 0x1000);
	lite_regs = (volatile uint32_t *)ml_io_map(LITE_PHYS, 0x4000);
	say("camera: about to read CPAS +0 (camera version; Android wants 0x00040800)\n");
	uint32_t cv = cpas_regs[0];
	say("camera: CPAS camera version %#x, cpas version %#x\n", cv, cpas_regs[1]);
	say("camera: about to read IFE-lite 0 +0/+4/+8\n");
	say("camera: IFE-lite 0: hw version %#x, titan version %#x, capability %#x\n", lrd(0), lrd(4), lrd(8));
	say("camera: about to read CSID-lite 0 +0\n");
	say("camera: CSID-lite 0: hw version %#x\n", lrd(CS(0)));
	if (sensor) {
		char name[16];

		if (!fp_step(5, "the sensor's tables from the image: init and the mode") || !fp_tab_load("camera-imx689.tab") || !fp_sensor_table("init")) {
			goto down;
		}
		snprintf(name, sizeof(name), "res%u", mode);
		if (!fp_sensor_table(name)) {
			goto down;
		}
		{
			uint32_t fl = 0, shutter = 0, gain = 0;
			static const struct { uint16_t reg; uint8_t v; } dummy[1] = { { 0, 0 } };

			(void)dummy;
			(void)PE_parse_boot_argn("cam_fl", &fl, sizeof(fl));            /* frame length in lines (0x0340): a longer frame, a longer exposure */
			(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));     /* coarse integration time in lines (0x0202) */
			(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));      /* analog gain code (0x0204): 1024 - 1024 / gain */
			if (fl || shutter || gain) {
				uint8_t w[3][4] = { { 0x01, 0x04, 0, 1 }, { 0, 0, 0, 0 }, { 0x01, 0x04, 0, 0 } };
				uint8_t seq[8][3];
				unsigned n = 0;

				seq[n][0] = 0x01; seq[n][1] = 0x04; seq[n++][2] = 1;                  /* group hold on */
				if (fl) { seq[n][0] = 0x03; seq[n][1] = 0x40; seq[n++][2] = (uint8_t)(fl >> 8); seq[n][0] = 0x03; seq[n][1] = 0x41; seq[n++][2] = (uint8_t)fl; }
				if (shutter) { seq[n][0] = 0x02; seq[n][1] = 0x02; seq[n++][2] = (uint8_t)(shutter >> 8); seq[n][0] = 0x02; seq[n][1] = 0x03; seq[n++][2] = (uint8_t)shutter; }
				if (gain) { seq[n][0] = 0x02; seq[n][1] = 0x04; seq[n++][2] = (uint8_t)(gain >> 8); seq[n][0] = 0x02; seq[n][1] = 0x05; seq[n++][2] = (uint8_t)gain; }
				for (unsigned i = 0; i < n && i < 8; i++) {
					uint8_t out[3] = { seq[i][0], seq[i][1], seq[i][2] };

					if (instantnoodle_i2c_write(&bus, (uint8_t)(fp_tab_head->slave >> 1), out, 3) != 0) {
						say("camera: the exposure write %#x%02x not acknowledged\n", out[0], out[1]);
					}
				}
				uint8_t off[3] = { 0x01, 0x04, 0 };

				(void)instantnoodle_i2c_write(&bus, (uint8_t)(fp_tab_head->slave >> 1), off, 3);        /* group hold off */
				(void)w;
				say("camera: exposure: frame length %u, shutter %u lines, gain code %u\n", fl, shutter, gain);
			}
		}
	}
	/* 5: memory */
	if (!fp_step(5, "memory in one piece for the frame")) {
		goto down;
	}
	if (kmem_alloc_contig(kernel_map, &buffer, round_page(frame + FP_HEADER_BYTES + 4096), PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory in one piece (%u KB)\n", (frame + FP_HEADER_BYTES + 4096) >> 10);
		goto down;
	}
	phys = kvtophys(buffer);
	{
		struct fp_header *h = (struct fp_header *)buffer;

		memcpy(h->magic, "RWCAM001", 8);
		h->width = width;
		h->height = height;
		h->stride = stride;
		h->format = 1;
		h->bayer = 0;                                       /* (the module file says 0 for every mode; "bayer" below corrects it from the picture) */
		h->frames = 0;
		h->frame_bytes = frame;
		h->source = sensor ? 1 : 0;
		strlcpy(h->sensor, sensor ? "IMX689" : "CSID TPG", sizeof(h->sensor));
	}
	for (vm_offset_t line = buffer; line < buffer + FP_HEADER_BYTES + frame; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	say("camera: frame buffer %#lx, physical %#llx; the IFE reaches it at %#llx through the SMMU\n", (unsigned long)buffer, phys, FP_IOVA);
	if (!fp_step(6, "the SMMU: a context of ours for the IFE's streams") || !fp_smmu_up(buffer, round_page(frame + FP_HEADER_BYTES))) {
		goto down;
	}
	/* 8: CSID-lite reset */
	if (!fp_step(7, "CSID-lite 0 reset")) {
		goto down;
	}
	lwr(CS(0x24), 0);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x34 + 0x10 * r), 0);
	}
	lwr(CS(0x78), 1);
	lwr(CS(0x28), 0xfffffff);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x38 + 0x10 * r), 0x7fff);
	}
	lwr(CS(0x80), 1);
	lwr(CS(0x104), 0x80);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x200 + 0x100 * r), 2);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		lwr(CS(0x78), 1);
		lwr(CS(0x80), 1);
		if (hw) {
			lwr(CS(0x74), 1);
			lwr(CS(0x80), 1);
		}
		lwr(CS(0x10), hw ? 0x1e : 0x1);
		if (!hw) {
			lwr(CS(0x74), 1);
			lwr(CS(0x80), 1);
		}
		int w;

		for (w = 0; w < 100000 && !(lrd(CS(0x70)) & 1); w++) {
			IODelay(1);
		}
		say("camera: CSID %s reset %s (top irq %#x)\n", hw ? "hardware" : "registers'", w < 100000 ? "done" : "DID NOT FINISH", lrd(CS(0x70)));
		lwr(CS(0x78), 1);
		lwr(CS(0x80), 1);
	}
	/* 9: IFE-lite reset and init */
	if (!fp_step(8, "IFE-lite 0 reset and bus init")) {
		goto down;
	}
	lwr(0x28, 0x00020000);
	lwr(0x0c, 2);
	int w;

	for (w = 0; w < 100000 && !(lrd(0x40) & (1u << 17)); w++) {
		IODelay(1);
	}
	say("camera: IFE-lite reset %s (irq status0 %#x)\n", w < 100000 ? "done" : "DID NOT FINISH", lrd(0x40));
	lwr(0x34, 0xffffffff);
	lwr(0x38, 0xffffffff);
	lwr(0x3c, 0xffffffff);
	lwr(0x24, 1);
	lwr(0x28, 0);
	lwr(0x2c, 0);
	lwr(0x30, 0);
	lwr(0x10, 0xffffffff);
	lwr(0x14, 1);
	lwr(0x18, 1);
	lwr(0x1a08, 0x03ffffff);
	lwr(0x1adc, 0);
	/* 11: CSID-lite RX, the test pattern generator and RDI 0 */
	if (!fp_step(9, "CSID-lite 0: RX, test pattern generator, RDI 0")) {
		goto down;
	}
	{
		uint32_t cfg0 = 0xau << 22 | 0x2bu << 16 | 0u << 27 | 0xfu << 12 | 1u << 2 | 3u;

		lwr(CS(0x100), 3 | 0x3210u << 4);                   /* 4 lanes, D-PHY bit clear, phy_sel 0 */
		lwr(CS(0x104), 1u << 6 | 1u << 2 | 1u);
		lwr(CS(0x604), 0xa | 1u << 10);
		lwr(CS(0x608), 0x3ffu << 12 | 0x740);
		lwr(CS(0x60c), 0x12345678);
		lwr(CS(0x610), width << 16 | height);
		lwr(CS(0x614), 0x2b);
		lwr(CS(0x618), (3u - 1) << 16 | 0x8);
		lwr(CS(0x640), 1u << 5);
		lwr(CS(0x648), 0);
		lwr(CS(0x200), cfg0);
		lwr(CS(0x204), 2);
		lwr(CS(0x2b0), 9);
		lwr(CS(0x20c), 0);
		lwr(CS(0x210), 1);
		lwr(CS(0x214), 0);
		lwr(CS(0x218), 1);
		lwr(CS(0x224), 0);
		lwr(CS(0x228), 1);
		lwr(CS(0x22c), 0);
		lwr(CS(0x230), 1);
		lwr(CS(0x208), 0);
		lwr(CS(0x200), cfg0 | 1u << 31);
		lwr(CS(0x208), 1);
		lwr(CS(0x34), 1u << 1 | 1u << 2 | 1u << 9 | 1u << 12 | 1u << 15 | 1u << 17);
	}
	if (sensor) {
		const uint32_t sensor_cfg0 = 0u << 22 | 0x2bu << 16 | 0xfu << 12 | 1u << 2 | 3u;

		lwr(CS(0x208), 0);
		lwr(CS(0x100), 0x01002102);                         /* three lanes (trios), lane assignment 0x210, phy 0, C-PHY */
		lwr(CS(0x104), 0x41);
		lwr(CS(0x200), sensor_cfg0);
		lwr(CS(0x200), sensor_cfg0 | 1u << 31);
		lwr(CS(0x208), 1);
		lwr(CS(0x24), (1u << 11) | (1u << 12) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 24));
	}
	say("camera: CSID RX_CFG0 %#x RDI0_CFG0 %#x TPG %#x/%#x/%#x\n", lrd(CS(0x100)), lrd(CS(0x200)), lrd(CS(0x604)), lrd(CS(0x610)), lrd(CS(0x640)));
	/* 10: write master 0 */
	if (!fp_step(10, "IFE-lite 0: write master 0 (RDI 0), frame mode")) {
		goto down;
	}
	lwr(0x1c64, 0xf);
	lwr(0x1c0c, 0xffff);
	lwr(0x1c18, 0);
	lwr(0x1c14, stride);
	lwr(0x1c30, 0);
	lwr(0x1c34, 1);
	lwr(0x1c38, 0);
	lwr(0x1c3c, 1);
	lwr(0x1c04, (uint32_t)FP_IOVA + FP_HEADER_BYTES);
	lwr(0x1c08, frame);
	lwr(0x1c00, 0x00010001);
	lwr(0x1a18, 1u << 4 | 1u << 0 | 0xc0000000u);
	lwr(0x20, 1);
	lwr(0x74, 1);
	say("camera: WM0 cfg %#x addr %#x stride %u frame %u\n", lrd(0x1c00), lrd(0x1c04), lrd(0x1c14), lrd(0x1c08));
	fp_show("configured");
	/* 12: the generator */
	if (!fp_step(11, "start the test pattern generator, wait for the frame")) {
		goto down;
	}
	if (sensor) {
		fp_phy = (volatile uint32_t *)ml_io_map(0x0ac6a000UL, 0x2000);
		fp_csiphy_up(settle);
		if (!fp_sensor_table("on")) {
			goto down;
		}
		IOSleep(30);
	} else {
		lwr(CS(0x600), 4u << 20 | 0x80u << 8 | 3u << 4 | 7);
	}
	for (unsigned ms = 0; ms < (sensor ? 5000u : 2000u); ms += 100) {
		IOSleep(100);
		if (lrd(0x1a28) & (1u << 4)) {
			say("camera: write master 0 says its frame is done after %u ms\n", ms + 100);
			if (sensor) {
				IOSleep(400);                           /* (a few more frames: the sensor's first are not its best) */
			}
			break;
		}
		if (ms == 0 || ms == 900) {
			fp_show("waiting");
			if (sensor) {
				fp_show_phy("waiting");
			}
		}
	}
	fp_show("after the wait");
	if (sensor) {
		fp_show_phy("after the wait");
		(void)fp_sensor_table("off");
		IOSleep(30);
		fp_csiphy_down();
	}
	lwr(CS(0x600), 0);
	lwr(0x1c00, 0);
	lwr(0x20, 1);
	for (vm_offset_t line = buffer + FP_HEADER_BYTES; line < buffer + FP_HEADER_BYTES + 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	{
		const uint8_t *q = (const uint8_t *)buffer + FP_HEADER_BYTES;

		say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", q[0], q[1],
		    q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
	}
	{
		struct fp_header *h = (struct fp_header *)buffer;

		h->frames = 1;
		h->last = 0;
		for (vm_offset_t line = buffer; line < buffer + FP_HEADER_BYTES; line += 64) {
			__asm__ volatile ("dc cvac, %0" : : "r" (line));
		}
		loader_recording[0] = kvtophys(buffer);
		loader_recording[1] = FP_HEADER_BYTES + frame;
		loader_recording[2] = (uint64_t)buffer;
		say("camera: the frame is left for the host (xl camera): %llu KB at physical %#llx\n", loader_recording[1] >> 10, loader_recording[0]);
	}
	if (sensor) {
		/* the first line unpacked (MIPI RAW10: four pixels in five bytes): is it a picture? */
		const uint8_t *q = (const uint8_t *)buffer;
		uint32_t lo = 1023, hi = 0;
		uint64_t sum = 0, n = 0;

		for (vm_offset_t line = buffer + FP_HEADER_BYTES + stride * 400; line < buffer + FP_HEADER_BYTES + stride * 402 + 64; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		q = (const uint8_t *)buffer + FP_HEADER_BYTES + stride * 400;
		for (uint32_t x = 0; x + 5 <= width * 10 / 8; x += 5) {
			for (unsigned k = 0; k < 4; k++) {
				uint32_t v = (uint32_t)q[x + k] << 2 | (q[x + 4] >> (2 * k) & 3);

				lo = v < lo ? v : lo;
				hi = v > hi ? v : hi;
				sum += v;
				n++;
			}
		}
		say("camera: line 400 of the sensor's frame: %llu pixels, min %u max %u mean %u\n", n, lo, hi, n ? (uint32_t)(sum / n) : 0);
	}
	(void)fault;
down:
	if (fp_refgen_set) {
		fp_refgen[0x80 / 4] &= ~1u;
		fp_refgen_set = false;
	}
	if (fp_ldoa9) {
		(void)instantnoodle_supply("ldoa9", 0, false);
		fp_ldoa9 = false;
	}
	fp_smmu_down();
	fp_clocks_down();
	if (mx) {
		(void)instantnoodle_rail_level("mx.lvl", 0);
	}
	say("camera: frame path rung A done\n");
}


/*
 * ---- cam=5: /dev/hk-camera for iOS: the IMX689's mode 4 (2000 x 1128 RAW10, 4 x 4 binned), frame after frame ------------------------
 * The contract is bsd/sys/hk_camera.h (userland/src/hk_camera.h): read() waits for the newest frame not read yet and returns a
 * struct hk_camera_frame and the frame's bytes; write() takes commands (exposure, focus, sensor, a picture for the host). The first
 * read powers the sensor and the frame path up (the sensor's tables take seconds), frames go into a ring of three buffers behind the
 * SMMU context of fp_smmu_up(), a second without a read ends it. The path itself is rung B (cam=4). The gain register iOS sees is a
 * synthetic linear one (256 = 1x): Sony's code is 1024 - 1024 / gain. Nothing here is verified on the phone yet.
 */
#define FPD_RING        3
/*
 * The stream that runs now: the main IMX689's mode 4 or the front IMX471's mode 1 (2328 x 1748, 2 x 2 binned, 4 D-PHY lanes, 30 fps: the
 * module's res1; the line is 2560 clocks of ~206.9 MHz = 12372 ns, the frame 2685 lines). The macros below are fields of fpd_m, so that
 * the main camera's code keeps its text; fpd_m changes only with the sensor, under fpd_lock, with every slot empty.
 */
struct fpd_mode { uint32_t width, height, stride, frame, line_ns, fl_min, fl_default, bayer, sensor; const char *name; };
/*
 * The main camera (2026-11-12): the module's res1, 4000 x 3000 (the 48 MP quad array binned 2 x 2, the full field of view), 3 C-PHY trios,
 * a line of 11856 clocks of the sensor's 1100.8 MHz = 10770 ns, 3094 lines for 30 fps. Until then it was res4, 2000 x 1128 (4 x 4 binned and
 * cropped to 16:9, up to 240 fps): a photograph was a 1504 x 1128 crop blown up to 4032 x 3024, and a room's light needed 16x gain and
 * more. cam_main=4 is the old mode (FPD_MODE_MAIN4).
 */
#define FPD_MODE_MAIN   { 4000, 3000, 5008, 5008 * 3000, 10770, 3094, 3094, 3, 1, "IMX689 (iOS)" }
#define FPD_MODE_MAIN4  { 2000, 1128, 2512, 2512 * 1128, 3517, 1184, 8000, 3, 1, "IMX689 (iOS)" }      /* 3872 clocks of the sensor's 1100.8 MHz */
#define FPD_MODE_FRONT  { 2328, 1748, 2912, 2912 * 1748, 12372, 2685, 2685, 3, 2, "IMX471 (iOS)" }
/*
 * (2026-11-12) The telephoto S5K3M5 (slot 3, CSIPHY 2, CCI1 master 0): its module's res3, 2104 x 1560 (2 x 2 binned), four D-PHY lanes at
 * 578.568 Mpixel/s (1.45 Gbit/s a lane), a line of 4848 clocks, 1656 lines for 60 fps -- 10059 ns a line; streamed at 30 fps (3312 lines).
 * Its Bayer order is GRBG (Samsung's readout; checked by eye on the first frames). Gain is the SMIA code, 32 a unit (fpd_gain_code).
 */
#define FPD_MODE_TELE   { 2104, 1560, 2640, 2640 * 1560, 10059, 1656, 3312, 1, 3, "S5K3M5 (iOS)" }
static struct fpd_mode fpd_m = FPD_MODE_MAIN;
#define FPD_WIDTH       (fpd_m.width)
#define FPD_HEIGHT      (fpd_m.height)
#define FPD_STRIDE      (fpd_m.stride)
#define FPD_FRAME       (fpd_m.frame)
#define FPD_LINE_NS     (fpd_m.line_ns)
#define FPD_FL_MIN      (fpd_m.fl_min)
#define FPD_FL_MAX      65535
#define FPD_MARGIN      12
#define FPD_GAIN_UNITY  256
#define FPD_GAIN_MAX    4096
#define FPD_FL_DEFAULT  8000
LCK_GRP_DECLARE(fpd_group, "hk-camera-8pro");
LCK_MTX_DECLARE(fpd_lock, &fpd_group);

struct fpd_slot { uint64_t sequence, time; uint32_t lines, gain, frame_lines, focus; int state; };   /* 0 free, 1 being written, 2 ready, 3 being read */
static struct fpd_slot fpd_slot[FPD_RING];
static uint32_t fpd_lens_before = HK_CAMERA_NO_FOCUS;       /* the lens at the end of the frame before (fpd_stream) */
static vm_offset_t fpd_ring;
static vm_size_t fpd_place;
static bool fpd_wanted, fpd_broken, fpd_streaming, fpd_up_now, fpd_made;
static vm_offset_t fpd_keep;
static uint64_t fpd_sequence, fpd_given, fpd_last_read_ms;
static uint32_t fpd_sensor_index = 1;   /* the camera iOS asked for last (HK_CAMERA_SENSOR): 1 the main one, 2 the front one, 3 the telephoto */
/* (2026-11-12) the main camera's mode iOS asked for last (HK_CAMERA_SENSOR's b): 1 its 4000 x 3000 (photographs), 4 its 2000 x 1128
 * (video: 16:9 and near 1080p, a quarter of the pixels to make into pictures); 0 until asked: boot-arg cam_main's, else 1 */
static uint32_t fpd_main_res, fpd_cur_res;
static uint32_t fpd_cur;                /* the camera that is powered (fpd_up_now): 1, 2 or 3 */
static bool fpd_tele_failed;            /* the S5K3M5 did not come up: its place is the main sensor's frames (as the front's) */
static bool fpd_switch, fpd_front_failed;   /* fpd_switch: the stream stops for the other sensor; fpd_front_failed: the IMX471 did not come up, its place is the main sensor's frames */
static uint64_t fpd_auto_until;         /* cam_auto: the run's own request (see fpd_thread) */
static uint64_t fpd_again_at;           /* cam_auto_again=1: after the first stream, the kernel asks again from the standby (the resume path) */
static uint32_t fpd_rx_cfg0 = 0x01002102;   /* CSID RX_CFG0: the main sensor's three C-PHY trios on CSIPHY 0 */
static uint32_t fpd_lines = 1120, fpd_gain = FPD_GAIN_UNITY, fpd_fl = FPD_FL_DEFAULT;
static struct { uint32_t lines, gain, frame_lines; bool pending; } fpd_ask;

/* the sensor that serves camera `index`: a front or telephoto that did not come up is served by the main one */
static uint32_t
fpd_serving(uint32_t index)
{
	return index == 2 && !fpd_front_failed ? 2u : index == 3 && !fpd_tele_failed ? 3u : 1u;
}

static uint64_t
fpd_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

static uint32_t
fpd_gain_code(uint32_t gain)
{
	gain = gain < FPD_GAIN_UNITY ? FPD_GAIN_UNITY : gain > FPD_GAIN_MAX ? FPD_GAIN_MAX : gain;
	if (fpd_m.sensor == 3) {
		return gain / 8;        /* Samsung's SMIA analogue gain: 0x20 a unit, 16x at 0x200 (FPD_GAIN_MAX) */
	}
	return 1024 - 1024 * FPD_GAIN_UNITY / gain;
}

/* The sensor's exposure registers, with group hold (0x0104): frame length 0x0340, coarse integration 0x0202, analog gain code 0x0204. */
static void
fpd_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	uint32_t fl = frame_lines ? frame_lines : (lines + FPD_MARGIN > fpd_m.fl_default ? lines + FPD_MARGIN : fpd_m.fl_default);
	uint32_t code;
	static const uint8_t hold_on[3] = { 0x01, 0x04, 1 }, hold_off[3] = { 0x01, 0x04, 0 };

	fl = fl < FPD_FL_MIN ? FPD_FL_MIN : fl > FPD_FL_MAX ? FPD_FL_MAX : fl;
	lines = lines < 8 ? 8 : lines > fl - FPD_MARGIN ? fl - FPD_MARGIN : lines;
	gain = gain < FPD_GAIN_UNITY ? FPD_GAIN_UNITY : gain > FPD_GAIN_MAX ? FPD_GAIN_MAX : gain;
	code = fpd_gain_code(gain);
	const uint8_t seq[6][3] = {
		{ 0x03, 0x40, (uint8_t)(fl >> 8) }, { 0x03, 0x41, (uint8_t)fl }, { 0x02, 0x02, (uint8_t)(lines >> 8) }, { 0x02, 0x03, (uint8_t)lines },
		{ 0x02, 0x04, (uint8_t)(code >> 8) }, { 0x02, 0x05, (uint8_t)code },
	};
	uint8_t addr = (uint8_t)(fp_tab_head->slave >> 1);

	(void)instantnoodle_i2c_write(&bus, addr, hold_on, 3);
	for (unsigned i = 0; i < 6; i++) {
		(void)instantnoodle_i2c_write(&bus, addr, seq[i], 3);
	}
	(void)instantnoodle_i2c_write(&bus, addr, hold_off, 3);
	fpd_fl = fl;
	fpd_lines = lines;
	fpd_gain = gain;
}

/* the register sequences of rung A/B, as they ran on the phone */
static void
fpd_csid_reset(void)
{
	lwr(CS(0x24), 0);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x34 + 0x10 * r), 0);
	}
	lwr(CS(0x78), 1);
	lwr(CS(0x28), 0xfffffff);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x38 + 0x10 * r), 0x7fff);
	}
	lwr(CS(0x80), 1);
	lwr(CS(0x104), 0x80);
	for (unsigned r = 0; r < 4; r++) {
		lwr(CS(0x200 + 0x100 * r), 2);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		lwr(CS(0x78), 1);
		lwr(CS(0x80), 1);
		if (hw) {
			lwr(CS(0x74), 1);
			lwr(CS(0x80), 1);
		}
		lwr(CS(0x10), hw ? 0x1e : 0x1);
		if (!hw) {
			lwr(CS(0x74), 1);
			lwr(CS(0x80), 1);
		}
		for (int w = 0; w < 100000 && !(lrd(CS(0x70)) & 1); w++) {
			IODelay(1);
		}
		lwr(CS(0x78), 1);
		lwr(CS(0x80), 1);
	}
}

static void
fpd_ife_init(void)
{
	lwr(0x28, 0x00020000);
	lwr(0x0c, 2);
	for (int w = 0; w < 100000 && !(lrd(0x40) & (1u << 17)); w++) {
		IODelay(1);
	}
	lwr(0x34, 0xffffffff);
	lwr(0x38, 0xffffffff);
	lwr(0x3c, 0xffffffff);
	lwr(0x24, 1);
	lwr(0x28, 0);
	lwr(0x2c, 0);
	lwr(0x30, 0);
	lwr(0x10, 0xffffffff);
	lwr(0x14, 1);
	lwr(0x18, 1);
	lwr(0x1a08, 0x03ffffff);
	lwr(0x1adc, 0);
}

static void
fpd_csid_sensor(void)
{
	const uint32_t cfg0 = 0u << 22 | 0x2bu << 16 | 0xfu << 12 | 1u << 2 | 3u;

	lwr(CS(0x100), fpd_rx_cfg0);
	lwr(CS(0x104), 0x41);
	lwr(CS(0x200), cfg0);
	lwr(CS(0x204), 2);
	lwr(CS(0x2b0), 9);
	lwr(CS(0x20c), 0);
	lwr(CS(0x210), 1);
	lwr(CS(0x214), 0);
	lwr(CS(0x218), 1);
	lwr(CS(0x224), 0);
	lwr(CS(0x228), 1);
	lwr(CS(0x22c), 0);
	lwr(CS(0x230), 1);
	lwr(CS(0x208), 0);
	lwr(CS(0x200), cfg0 | 1u << 31);
	lwr(CS(0x208), 1);
	lwr(CS(0x34), 1u << 1 | 1u << 2 | 1u << 9 | 1u << 12 | 1u << 15 | 1u << 17);
	lwr(CS(0x24), (1u << 11) | (1u << 12) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 24));
}

static void
fpd_wm(uint32_t iova)
{
	lwr(0x1c64, 0xf);
	lwr(0x1c0c, 0xffff);
	lwr(0x1c18, 0);
	lwr(0x1c14, FPD_STRIDE);
	lwr(0x1c30, 0);
	lwr(0x1c34, 1);
	lwr(0x1c38, 0);
	lwr(0x1c3c, 1);
	lwr(0x1c04, iova);
	lwr(0x1c08, FPD_FRAME);
	lwr(0x1c00, 0x00010001);
	lwr(0x1a18, 1u << 4 | 1u << 0 | 0xc0000000u);
	lwr(0x20, 1);
	lwr(0x74, 1);
}

static bool fpd_mx;

/* The front IMX471's chip ID, the short way (the probe's read_id() also scans the bus three times and flips pins); a bus scan only when it does not answer. */
static bool
fpd_read_id_quick(void)
{
	const struct sensor *s = sensor;
	uint8_t id[2] = { 0, 0 };

	bus.name = "cci";
	bus.sda = s->sda;
	bus.scl = s->scl;
	bus.pull = INSTANTNOODLE_PULL_UP;
	bus.fast = true;
	for (int tries = 0; tries < 3; tries++) {
		int err = instantnoodle_i2c_read16(&bus, s->address, s->id_reg, id, 2);

		say("camera: chip ID of the %s at I2C %#04x, register %#06x: %s %02x%02x (it should be %#06x)\n", s->name, s->address, s->id_reg,
		    err ? "NO ANSWER" : "answers", id[0], id[1], s->id);
		if (err == 0) {
			return ((uint32_t)id[0] << 8 | id[1]) == s->id;
		}
		instantnoodle_i2c_recover(&bus);
		IOSleep(10);
	}
	show_control_pins("front, no answer");
	scan_bus("front, no answer");
	return false;
}

/* The CSIPHY n's own clocks (the timer's root generator and branch, the PHY's branch): the camcc has them 0x20 apart (csi0phytimer 0x6000/0x6018, csiphy0 0x601c). */
static void
fpd_phy_clocks(unsigned phy)
{
	fp_rcgs[5].cmd = 0x6000 + 0x20 * phy;
	fp_branches[10].cbcr = 0x601c + 0x20 * phy;
	fp_branches[11].cbcr = 0x6018 + 0x20 * phy;
	fp_phy = phy == 3 ? fp_phy3 : phy == 2 ? fp_phy2 : fp_phy0;
}

static uint32_t fpd_settle_now = 0x22;                  /* the settle count the running stream's PHY got */

static uint32_t
fpd_settle(void)
{
	uint32_t v = 0x1a;      /* (the HS-settle window of a ~500 Mbit/s lane is 97..165 ns: 26 counts of 5 ns, a guess: cam_fsettle) */

	(void)PE_parse_boot_argn("cam_fsettle", &v, sizeof(v));
	return v;
}

/* the sensor's PHY: the main one's three C-PHY trios or the front's or the telephoto's four D-PHY lanes */
static void
fpd_phy_up(uint32_t settle)
{
	if (fpd_cur != 1) {
		fp_csiphy_up_dphy(settle);
	} else {
		fp_csiphy_up(settle);
	}
}

static void fpd_stop_stream(void);
static bool fpd_streaming_hw;

/*
 * cam_fsweep=1 (the front sensor's bring-up as a measurement): the lane's settle count is the one number the module file does not give. For
 * each candidate the CSID, the IFE's write master and the PHY are set up, the sensor streams for up to two seconds, and the CSID's and the IFE's
 * counters are said; the first count that gets a frame (IFE bus irq bit 4) is the one used afterwards (else `first`); cam_fsweep=2 tries all.
 */
static uint32_t
fpd_front_sweep(uint32_t first, bool all)
{
	static const uint32_t list[] = { 0x1a, 0x14, 0x20, 0x10, 0x28, 0x30, 0x0c, 0x38 };
	uint32_t best = 0;

	for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]) + 1; i++) {
		uint32_t v = i == 0 ? first : list[i - 1];
		bool got = false;

		fpd_csid_reset();
		fpd_ife_init();
		fpd_csid_sensor();
		fpd_wm((uint32_t)FP_IOVA);
		fpd_phy_up(v);
		if (!fp_sensor_table("on")) {
			break;
		}
		fpd_streaming_hw = true;
		for (int w = 0; w < 2000 && !got; w++) {
			got = (lrd(0x1a28) & (1u << 4)) != 0;
			if (!got) {
				IOSleep(1);
			}
		}
		keep("camera: front sweep: settle %#x: %s\n", v, got ? "A FRAME" : "no frame");
		fp_show("sweep");
		fp_show_phy("sweep");
		fpd_stop_stream();
		if (got && best == 0) {
			best = v;
			if (!all) {
				break;          /* cam_fsweep=2 goes on through the whole list: the range that works */
			}
		}
	}
	return best != 0 ? best : first;
}

/*
 * Everything up for camera `which` (1 the main IMX689, 2 the front IMX471), the sensor streaming into slot 0; false (and everything back down by the
 * caller) when something does not work.
 */
static bool
fpd_up(unsigned which)
{
	const bool front = which == 2, tele = which == 3;
	uint32_t main_res = 1;

	(void)PE_parse_boot_argn("cam_main", &main_res, sizeof(main_res));
	if (fpd_main_res != 0) {
		main_res = fpd_main_res;
	}
	const bool main4 = main_res == 4;
	const struct fpd_mode mode = front ? (struct fpd_mode)FPD_MODE_FRONT : tele ? (struct fpd_mode)FPD_MODE_TELE :
	    main4 ? (struct fpd_mode)FPD_MODE_MAIN4 : (struct fpd_mode)FPD_MODE_MAIN;
	const char *name = front ? "IMX471" : tele ? "S5K3M5" : "IMX689";
	uint32_t settle = 0x22;

	(void)PE_parse_boot_argn("cam_settle", &settle, sizeof(settle));
	if (front) {
		settle = fpd_settle();
	} else if (tele) {
		/* the HS-settle window of a 1.45 Gbit/s lane is 89..152 ns: 0x24 counts of the 300 MHz timer, in its middle (cam_tsettle) */
		settle = 0x24;
		(void)PE_parse_boot_argn("cam_tsettle", &settle, sizeof(settle));
	}
	if (!gcc) {
		gcc = (volatile uint32_t *)ml_io_map(GCC_PHYS, GCC_SIZE);
	}
	lck_mtx_lock(&fpd_lock);
	fpd_m = mode;
	fpd_cur_res = front || tele ? 0 : main4 ? 4 : 1;
	fpd_rx_cfg0 = front ? 0x00332103 : tele ? 0x00232103 : 0x01002102;   /* front: 4 lanes | lane assignment 0x3210 << 4 | PHY 3 << 20 | D-PHY; tele: the same on PHY 2; main: 3 trios, 0x210, PHY 0, C-PHY */
	fpd_lines = front ? 1500 : tele ? 1600 : main4 ? 1120 : 1500;
	fpd_gain = FPD_GAIN_UNITY;
	fpd_fl = mode.fl_default;
	fpd_ask.pending = false;
	lck_mtx_unlock(&fpd_lock);
	fpd_cur = which;
	sensor = &sensors[front ? 2 : tele ? 3 : 0];
	if (!power_up()) {
		return false;
	}
	fpd_up_now = true;
	if (front || tele) {
		if (!fpd_read_id_quick()) {
			return false;
		}
	} else {
		read_id();
	}
	if (!fp_tab_load(front ? "camera-imx471.tab" : tele ? "camera-s5k3m5.tab" : "camera-imx689.tab")) {
		return false;
	}
	if (!fp_phy0) {
		fp_phy0 = (volatile uint32_t *)ml_io_map(0x0ac6a000UL, 0x2000);
		fp_phy2 = (volatile uint32_t *)ml_io_map(0x0ac6e000UL, 0x2000);   /* (kona: CSIPHY n at 0x0ac6a000 + 0x2000 n) */
		fp_phy3 = (volatile uint32_t *)ml_io_map(0x0ac70000UL, 0x2000);
	}
	fpd_phy_clocks(front ? 3 : tele ? 2 : 0);
	fpd_mx = instantnoodle_rail_level("mx.lvl", 192);
	fp_ldoa9 = instantnoodle_supply("ldoa9", 1200, true);
	if (!fp_refgen) {
		fp_refgen = (volatile uint32_t *)ml_io_map(0x088e7000UL, 0x1000);
	}
	if (!(fp_refgen[0x80 / 4] & 1)) {
		fp_refgen[0x80 / 4] |= 1;
		fp_refgen_set = true;
	}
	if (!fp_clocks_up()) {
		return false;
	}
	if (!lite_regs) {
		lite_regs = (volatile uint32_t *)ml_io_map(LITE_PHYS, 0x4000);
		smmu_regs = (volatile uint32_t *)ml_io_map(SMMU_PHYS, 0x4000);
	}
	if (!fp_sensor_table("init") || !fp_sensor_table(front ? "res1" : tele ? "res3" : main4 ? "res4" : "res1")) {
		return false;
	}
	fpd_exposure(fpd_lines, fpd_gain, fpd_fl);
	if (!fp_smmu_up(fpd_ring, (uint64_t)fpd_place * FPD_RING)) {
		return false;
	}
	if (front) {
		uint32_t sweep = 0;

		if (PE_parse_boot_argn("cam_fsweep", &sweep, sizeof(sweep)) && sweep) {
			settle = fpd_front_sweep(settle, sweep > 1);
		}
	}
	fpd_settle_now = settle;
	fpd_csid_reset();
	fpd_ife_init();
	fpd_csid_sensor();
	fpd_wm((uint32_t)FP_IOVA);
	fpd_phy_up(settle);
	if (!fp_sensor_table("on")) {
		return false;
	}
	fpd_streaming_hw = true;
	if (!front && !tele) {
		instantnoodle_lens_init(&bus);           /* the main camera's lens motor (instantnoodle_lens.c; cam_focus=N and cam_lensscan=1 are its) */
	}
	say("camera: /dev/hk-camera: the %s is streaming into slot 0\n", name);
	return true;
}

/* From the standby: only what fpd_stop_stream() took away (the sensor keeps its registers in software standby). */
static bool
fpd_resume(void)
{
	fpd_exposure(fpd_lines, fpd_gain, fpd_fl);
	fpd_csid_reset();
	fpd_ife_init();
	fpd_csid_sensor();
	fpd_wm((uint32_t)FP_IOVA);
	fpd_phy_up(fpd_settle_now);
	if (!fp_sensor_table("on")) {
		return false;
	}
	fpd_streaming_hw = true;
	say("camera: /dev/hk-camera: the %s is streaming again (from the standby)\n", fpd_cur == 2 ? "IMX471" : fpd_cur == 3 ? "S5K3M5" : "IMX689");
	return true;
}


/* The sensor stops, and the CSID, the IFE's write master and the PHY with it; rails, clocks and the SMMU context stay (the standby of fpd_thread). */
static void
fpd_stop_stream(void)
{
	if (!fpd_streaming_hw) {
		return;
	}
	if (fpd_cur == 1) {
		instantnoodle_lens_park();              /* (no current in the lens motor's coil while nobody looks) */
	}
	if (fp_tab != NULL) {
		(void)fp_sensor_table("off");
		IOSleep(30);
	}
	if (lite_regs != NULL) {
		lwr(0x1c00, 0);
		lwr(0x20, 1);
		lwr(CS(0x200), 0);
		lwr(CS(0x100), 0);
	}
	if (fp_phy != NULL) {
		fp_csiphy_down();
	}
	fpd_streaming_hw = false;
}

static void
fpd_down(void)
{
	if (!fpd_up_now) {
		return;
	}
	fpd_stop_stream();
	if (fpd_cur == 1) {
		instantnoodle_lens_down();
	}
	fp_smmu_down();
	fp_clocks_down();
	if (fp_refgen_set) {
		fp_refgen[0x80 / 4] &= ~1u;
		fp_refgen_set = false;
	}
	if (fp_ldoa9) {
		(void)instantnoodle_supply("ldoa9", 0, false);
		fp_ldoa9 = false;
	}
	if (fpd_mx) {
		(void)instantnoodle_rail_level("mx.lvl", 0);
		fpd_mx = false;
	}
	power_down();
	fpd_up_now = false;
	say("camera: /dev/hk-camera: everything is down again\n");
}

/* On a panic's way out and at the run's end: the frame path must stop writing into memory that is about to be somebody else's. */
void instantnoodlep_camera_quiesce(void);
void
instantnoodlep_camera_quiesce(void)
{
	if (!fpd_up_now || lite_regs == NULL) {
		return;
	}
	/*
	 * (2026-11-12) The sensor stops sending first and the IFE's last write is let finish before its SMMU context goes: the
	 * phone went to CrashDump when a run was ended (iphont stop) with the telephoto streaming into iOS (cam_lock=1), the
	 * context switched off right behind the write master. One frame (40 ms) is the longest a write can be in flight.
	 */
	if (fpd_streaming_hw && fp_tab_head != NULL) {
		static const uint8_t off[3] = { 0x01, 0x00, 0x00 };     /* MIPI's mode_select 0x0100 = 0: software standby */

		(void)instantnoodle_i2c_write(&bus, (uint8_t)(fp_tab_head->slave >> 1), off, 3);
	}
	lwr(0x1c00, 0);
	lwr(0x20, 1);
	lwr(CS(0x200), 0);
	lwr(CS(0x100), 0);
	IODelay(40000);
	if (fp_phy != NULL) {
		fp_csiphy_down();
	}
	fp_put_back_matches();
	if (fp_smmu_on) {
		fp_ctx[0x00] = 0;
	}
}

static void
fpd_stream(void)
{
	int cur = 0;
	uint64_t last_frame_ms = fpd_ms();

	lck_mtx_lock(&fpd_lock);
	fpd_slot[0].state = 1;
	fpd_streaming = true;
	fpd_lens_before = HK_CAMERA_NO_FOCUS;   /* (the first frame's lens is not known: MOVED) */
	if (fpd_auto_until != 0 && fpd_auto_until < 1000) {
		fpd_auto_until = fpd_ms() + fpd_auto_until * 1000;
	}
	lck_mtx_unlock(&fpd_lock);
	for (;;) {
		uint32_t done = lrd(0x1a28);

		if (fpd_auto_until != 0 && fpd_ms() < fpd_auto_until) {
			fpd_last_read_ms = fpd_ms();            /* (cam_auto: nobody reads, the stream runs its seconds) */
		}

		if (done & (1u << 4)) {
			int next = -1;

			lwr(0x1a20, done);
			lwr(0x1a30, 1);
			int finished = cur;
			/* the lens where it was while this frame was exposed: at the frame before's end and now (HK_CAMERA_FOCUS_STAMPED) */
			uint32_t lens_now = fpd_cur == 1 ? instantnoodle_lens_position() : HK_CAMERA_NO_FOCUS, lens_stamp = HK_CAMERA_NO_FOCUS;

			if (lens_now != HK_CAMERA_NO_FOCUS) {
				lens_stamp = HK_CAMERA_FOCUS_STAMPED | lens_now | (lens_now != fpd_lens_before ? HK_CAMERA_FOCUS_MOVED : 0);
			}
			fpd_lens_before = lens_now;
			lck_mtx_lock(&fpd_lock);
			fpd_slot[cur].sequence = ++fpd_sequence;
			absolutetime_to_nanoseconds(mach_absolute_time(), &fpd_slot[cur].time);
			fpd_slot[cur].time = mach_absolute_time();
			fpd_slot[cur].lines = fpd_lines;
			fpd_slot[cur].gain = fpd_gain;
			fpd_slot[cur].frame_lines = fpd_fl;
			fpd_slot[cur].focus = lens_stamp;
			fpd_slot[cur].state = 2;
			for (int i = 0; i < FPD_RING; i++) {            /* a free place first, else the oldest finished one that is not being read */
				if (i != cur && fpd_slot[i].state == 0) {
					next = i;
					break;
				}
			}
			for (int i = 0; next < 0 && i < FPD_RING; i++) {
				if (i != cur && fpd_slot[i].state == 2 && (next < 0 || fpd_slot[i].sequence < fpd_slot[next].sequence)) {
					next = i;
				}
			}
			if (next < 0) {
				next = cur;                             /* (all in use: write over the one just finished) */
			}
			fpd_slot[next].state = 1;
			cur = next;
			lck_mtx_unlock(&fpd_lock);
			lwr(0x1c04, (uint32_t)FP_IOVA + (uint32_t)(cur * fpd_place));
			lwr(0x20, 1);
			wakeup(&fpd_sequence);
			last_frame_ms = fpd_ms();
			if (fpd_cur == 1) {
				instantnoodle_lens_frame((const void *)(fpd_ring + (vm_offset_t)finished * fpd_place), FPD_STRIDE, FPD_WIDTH, FPD_HEIGHT);     /* (the lens scan's eyes; nothing without cam_lensscan=1) */
			}
		} else {
			IOSleep(1);
		}
		if (fpd_ask.pending) {
			uint32_t a = fpd_ask.lines, b = fpd_ask.gain, c = fpd_ask.frame_lines;

			fpd_ask.pending = false;
			fpd_exposure(a, b, c);
		}
		if (fpd_ms() - last_frame_ms > 1500) {
			say("camera: /dev/hk-camera: no frame for 1.5 s (csid rx irq %#x rdi %#x, ife bus irq %#x): the camera is switched off\n", lrd(CS(0x20)),
			    lrd(CS(0x30)), lrd(0x1a28));
			if (fpd_cur == 2 || fpd_cur == 3) {
				/* the front or telephoto sensor streams nothing: the main sensor's frames serve its place from now on */
				if (fpd_cur == 2) {
					fpd_front_failed = true;
				} else {
					fpd_tele_failed = true;
				}
				fpd_switch = true;
			} else {
				fpd_broken = true;
			}
			break;
		}
		if (fpd_ms() - fpd_last_read_ms > 5000) {
			say("camera: /dev/hk-camera: nobody reads: down\n");
			break;
		}
		if (fpd_switch) {
			say("camera: /dev/hk-camera: the other sensor is asked for: this one stops\n");
			break;
		}
	}
	if (fpd_keep != 0) {
		int newest = -1;

		lck_mtx_lock(&fpd_lock);
		for (int i = 0; i < FPD_RING; i++) {
			if (fpd_slot[i].state == 2 && (newest < 0 || fpd_slot[i].sequence > fpd_slot[newest].sequence)) {
				newest = i;
			}
		}
		lck_mtx_unlock(&fpd_lock);
		if (newest >= 0) {
			struct fp_header *h = (struct fp_header *)fpd_keep;
			vm_offset_t at = fpd_ring + (vm_offset_t)newest * fpd_place;

			for (vm_offset_t line = at & ~63ul; line < at + FPD_FRAME; line += 64) {
				__asm__ volatile ("dc civac, %0" : : "r" (line));
			}
			__asm__ volatile ("dsb sy");
			bzero(h, FP_HEADER_BYTES);
			memcpy(h->magic, "RWCAM001", 8);
			h->width = FPD_WIDTH; h->height = FPD_HEIGHT; h->stride = FPD_STRIDE; h->format = 1; h->bayer = fpd_m.bayer; h->frames = 1;
			h->frame_bytes = FPD_FRAME; h->source = 1;
			strlcpy(h->sensor, fpd_m.name, sizeof(h->sensor));
			memcpy((void *)(fpd_keep + FP_HEADER_BYTES), (const void *)at, FPD_FRAME);
			for (vm_offset_t line = fpd_keep; line < fpd_keep + FP_HEADER_BYTES + FPD_FRAME; line += 64) {
				__asm__ volatile ("dc cvac, %0" : : "r" (line));
			}
			__asm__ volatile ("dsb sy");
			loader_recording[0] = kvtophys(fpd_keep);
			loader_recording[1] = FP_HEADER_BYTES + FPD_FRAME;
			loader_recording[2] = (uint64_t)fpd_keep;
			say("camera: /dev/hk-camera: the last frame (sequence %llu) is left for the host (xl camera)\n", fpd_slot[newest].sequence);
		}
	}
	lck_mtx_lock(&fpd_lock);
	fpd_streaming = false;
	for (int i = 0; i < FPD_RING; i++) {
		fpd_slot[i].state = 0;
	}
	lck_mtx_unlock(&fpd_lock);
	wakeup(&fpd_sequence);
}

static void
fpd_thread(__unused void *parameter, __unused wait_result_t result)
{
	{
		/* cam_auto=N (1 main, 2 front, 3 telephoto), cam_after=S: the camera is asked for by the kernel itself S seconds after the boot (a run without iOS's reads);
		 * it streams cam_auto_secs (default 8) and leaves its last frame for the host (xl camera) */
		uint32_t which = 0, after = 25;

		if (PE_parse_boot_argn("cam_auto", &which, sizeof(which)) && (which == 1 || which == 2 || which == 3)) {
			uint32_t secs = 8;

			uint32_t again = 0;

			(void)PE_parse_boot_argn("cam_auto_again", &again, sizeof(again));
			fpd_again_at = again ? 1 : 0;           /* (1: armed; the stream's end sets the time) */
			(void)PE_parse_boot_argn("cam_after", &after, sizeof(after));
			(void)PE_parse_boot_argn("cam_auto_secs", &secs, sizeof(secs));
			IOSleep(after * 1000);
			say("camera: cam_auto=%u: the kernel asks for the %s camera now (streams %u s after it is up)\n", which, which == 2 ? "front" : which == 3 ? "telephoto" : "main", secs);
			lck_mtx_lock(&fpd_lock);
			fpd_sensor_index = which;
			fpd_wanted = true;
			fpd_auto_until = secs;                  /* (seconds; made absolute when the stream starts) */
			lck_mtx_unlock(&fpd_lock);
		}
	}
	for (;;) {
		bool ok;
		unsigned want;

		lck_mtx_lock(&fpd_lock);
		while (!fpd_wanted || fpd_broken) {
			struct timespec ts = { 1, 0 };

			(void)msleep(&fpd_wanted, &fpd_lock, PZERO, "hk-camera-wait", &ts);
		}
		fpd_last_read_ms = fpd_ms();
		fpd_switch = false;
		want = fpd_serving(fpd_sensor_index);
		lck_mtx_unlock(&fpd_lock);
		if (fpd_up_now && (fpd_cur != want || (want == 1 && fpd_main_res != 0 && fpd_cur_res != fpd_main_res))) {
			say("camera: /dev/hk-camera: the camera changes: everything down, then the %s\n", want == 2 ? "IMX471 (front)" :
			    want == 3 ? "S5K3M5 (telephoto)" : fpd_main_res == 4 ? "IMX689 (main) in its 2000x1128 mode" :
			    "IMX689 (main) in its 4000x3000 mode");
			fpd_down();
		}
		ok = fpd_up_now ? fpd_resume() : fpd_up(want);
		if (!ok && (want == 2 || want == 3)) {
			/* the front or telephoto sensor does not come up: its place is served with the main sensor's frames, so that the app does not wait for ever */
			say("camera: /dev/hk-camera: the %s camera did not come up: the main sensor's frames serve its place\n", want == 2 ? "front" : "telephoto");
			fpd_stop_stream();
			fpd_down();
			if (want == 2) {
				fpd_front_failed = true;
			} else {
				fpd_tele_failed = true;
			}
			ok = fpd_up(1);
		}
		if (ok) {
			fpd_stream();
		} else {
			say("camera: /dev/hk-camera: the camera did not come up\n");
			fpd_broken = true;
		}
		fpd_stop_stream();
		lck_mtx_lock(&fpd_lock);
		if (!fpd_switch) {
			fpd_wanted = false;
		}
		if (!fpd_broken && !fpd_switch) {
			/* the standby: a minute in which the next read finds everything but the stream in place (the viewfinder opens at once) */
			uint64_t until = fpd_ms() + 60000;

			if (fpd_again_at == 1) {
				fpd_again_at = fpd_ms() + 12000;
			}
			while (!fpd_wanted && fpd_ms() < until) {
				struct timespec ts = { 1, 0 };

				if (fpd_again_at > 1 && fpd_ms() > fpd_again_at) {
					say("camera: cam_auto_again: the kernel asks again, from the standby\n");
					fpd_again_at = 0;
					fpd_auto_until = 6;
					fpd_wanted = true;
					break;
				}
				(void)msleep(&fpd_wanted, &fpd_lock, PZERO, "hk-camera-standby", &ts);
			}
		}
		bool again = fpd_wanted && !fpd_broken;

		lck_mtx_unlock(&fpd_lock);
		if (!again) {
			fpd_down();
			say("camera: /dev/hk-camera: standby over, everything down\n");
		}
		wakeup(&fpd_sequence);
	}
}

static int
fpd_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&fpd_lock);
	fpd_wanted = true;
	fpd_last_read_ms = fpd_ms();
	lck_mtx_unlock(&fpd_lock);
	wakeup(&fpd_wanted);
	return 0;
}

static int
fpd_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
fpd_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hk_camera_frame f;
	struct fpd_mode m;
	uint64_t since = fpd_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&fpd_lock);
	fpd_last_read_ms = fpd_ms();
	if (!fpd_wanted && !fpd_streaming && !fpd_broken) {
		fpd_wanted = true;
		wakeup(&fpd_wanted);
	}
	for (;;) {
		if (fpd_broken) {
			lck_mtx_unlock(&fpd_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < FPD_RING; i++) {
			if (fpd_slot[i].state == 2 && fpd_slot[i].sequence > fpd_given && (b < 0 || fpd_slot[i].sequence > fpd_slot[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (fpd_ms() - since > (fpd_streaming ? 3000u : 20000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&fpd_lock);
			return EAGAIN;
		}
		if (!fpd_wanted && !fpd_streaming) {
			fpd_wanted = true;
			wakeup(&fpd_wanted);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };

		error = msleep(&fpd_sequence, &fpd_lock, PZERO | PCATCH, "hk-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&fpd_lock);
			return error;
		}
		fpd_last_read_ms = fpd_ms();
	}
	fpd_slot[b].state = 3;
	fpd_given = fpd_slot[b].sequence;
	m = fpd_m;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HK_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = m.width;
	f.height = m.height;
	f.stride = m.stride;
	f.format = 1;
	f.bayer = m.bayer;                      /* (BGGR: the module file's order 0 in CamX's key; not yet checked against a colour picture) */
	f.black = 64;
	f.sequence = fpd_slot[b].sequence;
	f.time = fpd_slot[b].time;
	f.exposure_lines = fpd_slot[b].lines;
	f.line_ns = m.line_ns;
	f.gain = fpd_slot[b].gain;
	f.gain_unity = FPD_GAIN_UNITY;
	f.gain_max = FPD_GAIN_MAX;
	f.frame_lines = fpd_slot[b].frame_lines;
	f.frame_lines_min = m.fl_min;
	f.frame_lines_max = FPD_FL_MAX;
	f.margin = FPD_MARGIN;
	f.sensor = fpd_sensor_index;            /* the camera asked for (see fpd_sensor_index) */
	f.focus = fpd_slot[b].focus;            /* (the main camera's lens motor, as it was while the frame was exposed; HK_CAMERA_NO_FOCUS without one) */
	lck_mtx_unlock(&fpd_lock);
	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = fpd_ring + (vm_offset_t)b * fpd_place;
		uint32_t n = (uint32_t)MIN((uint64_t)m.frame, (uint64_t)uio_resid(uio));

		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {         /* (what the IFE wrote went past the processor's caches) */
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		error = uiomove((caddr_t)at, (int)n, uio);
	}
	lck_mtx_lock(&fpd_lock);
	if (fpd_slot[b].state == 3) {
		fpd_slot[b].state = 2;
	}
	fpd_last_read_ms = fpd_ms();
	lck_mtx_unlock(&fpd_lock);
	return error;
}

static int
fpd_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hk_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == HK_CAMERA_EXPOSURE) {
		lck_mtx_lock(&fpd_lock);
		fpd_ask.lines = c.a;
		fpd_ask.gain = c.b;
		fpd_ask.frame_lines = c.c;
		fpd_ask.pending = true;
		lck_mtx_unlock(&fpd_lock);
		return 0;
	}
	if (c.command == HK_CAMERA_FOCUS) {
		if (fpd_cur == 1) {
			instantnoodle_lens_set(c.a);
		}
		return 0;
	}
	if (c.command == HK_CAMERA_SENSOR) {
		if (c.a != 1 && c.a != 2 && c.a != 3) {
			return EINVAL;
		}
		{
			uint32_t lock = 0;

			if (PE_parse_boot_argn("cam_lock", &lock, sizeof(lock)) && lock) {
				return 0;               /* cam_lock=1: the sensor stays the one cam_auto asked for (the run's frame for the host is its own) */
			}
		}
		/* the other camera: the stream stops, everything goes down and the next frames are the other sensor's (fpd_thread); a front camera
		 * that did not come up is served with the main sensor's frames (fpd_front_failed), so that the app does not wait for ever */
		lck_mtx_lock(&fpd_lock);
		if (fpd_sensor_index != c.a) {
			fpd_sensor_index = c.a;
			fpd_switch = fpd_up_now && fpd_cur != fpd_serving(c.a);
		}
		/* (2026-11-12) the main camera's mode: b 4 its 2000 x 1128, 1 its 4000 x 3000; another while it streams restarts it */
		if (c.a == 1 && (c.b == 1 || c.b == 4) && fpd_main_res != c.b) {
			fpd_main_res = c.b;
			if (fpd_up_now && fpd_cur == 1 && fpd_cur_res != c.b) {
				fpd_switch = true;
			}
		}
		lck_mtx_unlock(&fpd_lock);
		wakeup(&fpd_sequence);
		return 0;
	}
	return EINVAL;
}

static const struct cdevsw fpd_cdevsw = {
	.d_open = fpd_open, .d_close = fpd_close, .d_read = fpd_read, .d_write = fpd_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* At the start of the boot (the memory is still in large pieces; mediaserverd looks for cameras once, when it starts): the node and the ring. */
static void
fpd_init(void)
{
	thread_t thread;
	int major;

	{
		static const struct fpd_mode main_mode = FPD_MODE_MAIN, front_mode = FPD_MODE_FRONT, tele_mode = FPD_MODE_TELE;

		fpd_place = round_page(MAX(MAX(main_mode.frame, front_mode.frame), tele_mode.frame));   /* (a slot holds any sensor's frame) */
	}
	if (kmem_alloc_contig(kernel_map, &fpd_ring, fpd_place * FPD_RING, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: /dev/hk-camera: no %lu KB in one piece for its ring\n", (unsigned long)(fpd_place * FPD_RING) >> 10);
		return;
	}
	for (vm_offset_t line = fpd_ring; line < fpd_ring + fpd_place * FPD_RING; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (kmem_alloc_contig(kernel_map, &fpd_keep, round_page(FP_HEADER_BYTES + fpd_place), PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		fpd_keep = 0;
	}
	major = cdevsw_add(-1, &fpd_cdevsw);
	fpd_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hk-camera") != NULL;
	say("camera: /dev/hk-camera %s: ring of %u x %lu KB at physical %#llx\n", fpd_made ? "made" : "NOT MADE", FPD_RING, (unsigned long)fpd_place >> 10,
	    kvtophys(fpd_ring));
	if (fpd_made && kernel_thread_start(fpd_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
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
		uint32_t slot = 0, first, last;

		(void)PE_parse_boot_argn("cam_slot", &slot, sizeof(slot));     /* 0..3 one sensor; 99: every sensor, one after the other */
		first = slot == 99 ? 0 : (slot >= sizeof(sensors) / sizeof(sensors[0]) ? 0 : slot);
		last = slot == 99 ? sizeof(sensors) / sizeof(sensors[0]) - 1 : first;
		for (uint32_t one = first; one <= last; one++) {
			if (slot == 99 && one == 1) {
				/* the IMX586 (slot 1) needs PM8009 GPIO 1, smpa5 and ldoc7 as well and is not powered right yet: its probe ended in
				 * a CrashDump on 2026-10-27 right after the first bus scan; only by cam_slot=1 */
				say("camera: slot 1 (IMX586) is left out of cam_slot=99 (use cam_slot=1)\n");
				continue;
			}
			sensor = &sensors[one];
			rails_before_n = rails_snapshot(rails_before);
			say("camera: PMIC regulator blocks before the power-up: %u\n", rails_before_n);
			if (power_up()) {
				rails_show_changes();
				read_id();
				if (cam_level == 3 || cam_level == 4) {
					fp_tpg();
				}
			}
			power_down();
		}
		show_gcc();
		show_pins();
	}
	for (int again = 0; again < 15; again++) {
		/* (the results, again, for whoever polls the loader's ring) */
		IOSleep(3000);
		for (unsigned i = 0, from = 0; i < kept_n; i++) {
			if (kept[i] == '\n') {
				char one[240];
				unsigned n = i + 1 - from < sizeof(one) - 1 ? i + 1 - from : sizeof(one) - 1;

				memcpy(one, kept + from, n);
				one[n] = 0;
				for (const char *c = one; *c != 0; c++) {
					loader_log_putc(*c);
				}
				from = i + 1;
			}
		}
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
	if (level == 5) {
		fpd_init();                     /* cam=5: the device for iOS (the loader's cam=5 also gives SpringBoard the camera node); no probe */
		return;
	}
	cam_level = level == 3 ? 3 : level == 4 ? 4 : (level > 2 ? 2 : level);
	if (level == 3) {
		/* the frame memory, in one piece below 4 GB (max_pnum in pages of PAGE_SIZE), now, while the system's memory is still in large pieces */
		uint32_t mb = 8;

		(void)PE_parse_boot_argn("cam_keep_mb", &mb, sizeof(mb));
		fp_keep_bytes = (vm_size_t)(mb > 32 ? 32 : mb) << 20;
		if (kmem_alloc_contig(kernel_map, &fp_keep, fp_keep_bytes, PAGE_MASK, (ppnum_t)((0x100000000ULL >> PAGE_SHIFT) - 1), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("camera: no %u MB in one piece below 4 GB at the start of the boot\n", mb);
			fp_keep = 0;
			fp_keep_bytes = 0;
		} else {
			say("camera: %u MB for frames at the start of the boot: physical %#llx\n", mb, kvtophys(fp_keep));
		}
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
