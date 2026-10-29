/*
 * The system's power state, between the drivers that each have a part of it.
 *
 * The screen: the framebuffer (iokit/Kernel/MiniIOMobileFramebuffer.cpp) puts the panel to sleep when backboardd
 * asks for the display's power off (IOMobileFramebufferRequestPowerChange 0, when the screen locks or dims out) and
 * wakes it for 1, and says so here: the processors' governor lowers the memory's bandwidth while the screen is off
 * and raises everything at once when it comes back (bsd/dev/hk_cpufreq.c); the touch screen's chip is held in
 * reset meanwhile (bsd/dev/instantnoodle_touch.c: iOS turns the digitizer off with the screen).
 *
 * The system's sleep, on the OnePlus 8 (board=instantnoodle; syssleep=0 refuses it, as every other board does).
 * iOS's powerd asks for it within seconds of the screen going off when nothing holds an assertion against it,
 * and IOPMrootDomain takes it to IOCPUSleepKernel(): every processor but the boot one is switched off through
 * the firmware (PSCI CPU_OFF, GenericARMCPU::quiesceCPU()), and on the boot processor GenericARMCPU::haltCPU()
 * comes here, in thread context. What is done, in order:
 *   - before the other processors go (hk_power_will_sleep()): the audio path parked, the Wi-Fi radio off (the
 *     network left, the firmware stopped, the PCIe link down: bsd/dev/hk_wifi.c; back on a thread of its own after
 *     the wake), the panel dark if iOS had left it lit (lit again by hk_power_did_wake(), once they are back);
 *   - every group of cores at its lowest row, the memory's vote at its floor (hk_cpufreq_sleep());
 *   - every interrupt but the wake's disabled at the distributor, the wake's -- the PMIC arbiter's, for the power
 *     key (bsd/dev/hk_buttons.c) -- routed here; this processor's interrupts masked;
 *   - mach_absolute_time() frozen, as an iPhone's stops while it sleeps (ml_sleep_timebase_freeze());
 *   - WFI, with the virtual timer set for the next of: SLEEP_BEAT_SECONDS (the loader's heartbeat: a watching
 *     loader takes a kernel silent for 50 s as hung and resets the phone; and a look at the charger's cable),
 *     the wake iOS scheduled (IOPMrootDomain's "wake" setting: GenericARMPE.cpp, hk_power_set_wake()), or, when
 *     the key's interrupt could not be set up, SLEEP_POLL_MS (the key's state read then instead);
 *   - woken by the key, the time iOS asked for, or the cable put in or taken out; everything undone in reverse,
 *     the clock going on from where it stopped (the calendar is made up by clock_wakeup_calendar() from the
 *     hardware counter, which counted on), and the reason handed to IOPMrootDomain as the wake's.
 * A press that woke the system is told to iOS even when it was over before the keys were looked at again
 * (hk_buttons_woken()): that is what lights the screen, as the side button does on an iPhone.
 *
 * Not reached: the SoC's own lowest state (CX and the memory collapsed by the always-on processor): it needs this
 * processor and the loader's core 0 power-collapsed with the cluster (PSCI CPU_SUSPEND), sleep and wake sets for
 * RPMh, and UEFI's resident drivers (the storage's, the loader's USB) letting go of their votes. What is reached is
 * every core but two switched off, those two in WFI/WFE at their lowest speed, no timer of the kernel or of any
 * program running, the panel asleep and the memory at its floor.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

/* bsd/dev/hk_buttons.c */
extern bool hk_buttons_wake_armed(void);
extern int hk_buttons_wake_check(void);
extern bool hk_buttons_power_held(void);
extern void hk_buttons_woken(void);
extern void hk_buttons_kick(void);
/* iokit/Kernel/MiniIOMobileFramebuffer.cpp */
extern bool hk_display_on(void);
extern uint32_t hk_display_frames(void);
extern uint32_t hk_display_backlight(uint64_t * sent);
extern void hk_display_power(bool on);
/* bsd/dev/hk_cpufreq.c */
extern void hk_cpufreq_sleep(bool asleep);
/* iokit/Kernel/HKGPU.cpp */
extern void hk_gpu_speed_kick(void);
/* bsd/dev/hk_sound.c */
extern void hk_sound_park(void);
extern int hk_gpu_power_bits(void);
extern void hk_gpu_sleep_begin(void);           /* iokit/Kernel/HKGPU.cpp: the GPU block given back, its lock held */
extern void hk_gpu_sleep_end(void);
/* bsd/dev/instantnoodle_battery.c */
extern bool instantnoodle_battery_low(void);
/* bsd/dev/instantnoodle_touch.c */
extern void instantnoodle_touch_display(bool on);
/* osfmk/arm64 */
extern void gic_v3_sleep_enter(uint32_t wake_intid);
extern void gic_v3_sleep_exit(uint32_t wake_intid);
extern bool gic_v3_spi_pending(uint32_t intid);
extern void gic_v3_spi_mask(uint32_t intid);
extern uint64_t ml_sleep_timebase_freeze(void);
extern void ml_sleep_timebase_thaw(uint64_t frozen);
extern uint64_t ml_get_hwclock(void);
extern void loader_live_beat(void);
extern void loader_return_wake(void);           /* osfmk/arm64/loader_return.c: the timed return, as wall time */
extern bool ml_cpu_online(int cpu_id);
extern void loader_log_putc(char c);
extern void PEGetUTCTimeOfDay(clock_sec_t *secs, clock_usec_t *usecs);

#define SLEEP_BEAT_SECONDS      30
#define SLEEP_POLL_MS           40      /* (two looks fit in the shortest press in the logs, 80 ms) */
#define SLEEP_CABLE_SECONDS     2       /* the cable looked at this often while polling the key */
#define SLEEP_LOW_SECONDS       600     /* the battery low: awake this often to read the gauge (an empty one shuts down) */
#define ARB_INTID               (481 + 32)
#define PM8150B_USBIN           0x213   /* (as bsd/dev/instantnoodle_battery.c) */
#define USBIN_RT_STS            0x10
#define USBIN_PLUGIN            (1u << 4)
#define CNTV_CTL_ENABLE         (1u << 0)
#define CNTV_CTL_ISTATUS        (1u << 2)

static int power_display_on = 1;
static uint64_t power_display_offs;
static uint32_t sleep_enabled;          /* boot-arg syssleep, by default on with board=instantnoodle */
static uint32_t gpu_sleep_enabled = 1;  /* boot-arg gpu_sleep=0: the GPU block stays powered through a sleep */
static uint64_t sleep_max;              /* boot-arg sleep_max: seconds; 0, none */
static uint64_t sleep_count, sleep_seconds_total, sleep_wake_at;  /* sleep_wake_at: calendar seconds, 0 none */
static char sleep_last[128] = "never slept";
static uint64_t sleep_timer;            /* sysctl kern.hackios.sleep_timer: the next sleep's own wake, seconds (once) */
static int sleep_timer_key;             /* sysctl kern.hackios.sleep_timer_key: ... and that wake is the power key's: a user's (once) */

SYSCTL_DECL(_kern_hackios);
SYSCTL_INT(_kern_hackios, OID_AUTO, display_on, CTLFLAG_RD | CTLFLAG_LOCKED, &power_display_on, 0,
    "whether the panel is lit (0: asleep, the display's power off)");
static int
sysctl_frames(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t frames = hk_display_frames();

	return SYSCTL_OUT(req, &frames, sizeof(frames));
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, frames, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_frames, "I",
    "swaps of the window server so far (its frames: a rate over 30 a second is a home screen that is alive)");
static int
sysctl_backlight(__unused struct sysctl_oid *oidp, __unused void *arg1, int arg2, struct sysctl_req *req)
{
	uint64_t sent = 0;
	uint32_t value = hk_display_backlight(&sent);
	uint64_t out = arg2 ? sent : value;

	return SYSCTL_OUT(req, &out, sizeof(out));
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, backlight, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_backlight, "Q",
    "the panel's brightness value as iOS last set it (1..2047; 0: iOS has not set it)");
SYSCTL_PROC(_kern_hackios, OID_AUTO, backlight_sent, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 1, sysctl_backlight, "Q",
    "how many brightness writes the panel's DSI link has taken");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, display_offs, CTLFLAG_RD | CTLFLAG_LOCKED, &power_display_offs,
    "how many times the panel was put to sleep");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, sleeps, CTLFLAG_RD | CTLFLAG_LOCKED, &sleep_count, "how many times the system slept");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, sleep_seconds, CTLFLAG_RD | CTLFLAG_LOCKED, &sleep_seconds_total,
    "seconds spent asleep, all sleeps together");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, sleep_timer, CTLFLAG_RW | CTLFLAG_LOCKED, &sleep_timer,
    "seconds after which the next sleep wakes by itself (the kernel's own timer, once; 0: none). Tests use it instead of "
    "asking powerd for a wake, which it does not give for less than about 10 s");
SYSCTL_INT(_kern_hackios, OID_AUTO, sleep_timer_key, CTLFLAG_RW | CTLFLAG_LOCKED, &sleep_timer_key, 0,
    "1: the next sleep's timer wake counts as a press of the power key (iOS wakes fully and lights the screen, as for a user), "
    "0: as a timer wake (a dark wake: the screen stays off)");
SYSCTL_STRING(_kern_hackios, OID_AUTO, sleep_last, CTLFLAG_RD | CTLFLAG_LOCKED, sleep_last, sizeof(sleep_last),
    "the last sleep: how long, and what woke it");

static void
note(const char *format, ...) __printflike(1, 2);

/* the loader's log only: with this processor's interrupts masked the console is not to be written */
static void
note(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* From the framebuffer, in thread context: the panel has gone to sleep, or is about to light. */
void hk_power_display(bool on);

void
hk_power_display(bool on)
{
	power_display_on = on;
	if (!on) {
		power_display_offs++;
	}
	hk_cpufreq_display(on);
	instantnoodle_touch_display(on);
	if (on) {
		hk_gpu_speed_kick();
	}
}

/* Whether this platform sleeps for real (IOPMrootDomain's syssleep, GenericARMPE). */
bool hk_power_sleep_supported(void);

bool
hk_power_sleep_supported(void)
{
	static bool decided;
	char board[32] = "";

	if (!decided) {
		decided = true;
		/* on the OnePlus 8 only: the 8 Pro ("instantnoodlep") has not been given system sleep by default, because the second sleep of a run hangs or returns to the loader (2026-10-29: the first sleep passes, Wi-Fi does not come back, the modem link is lost); syssleep=1 asks for it */
		sleep_enabled = PE_parse_boot_argn("board", board, sizeof(board)) && strcmp(board, "instantnoodle") == 0;
		PE_parse_boot_argn("syssleep", &sleep_enabled, sizeof(sleep_enabled));
		PE_parse_boot_argn("sleep_max", &sleep_max, sizeof(sleep_max));
		PE_parse_boot_argn("gpu_sleep", &gpu_sleep_enabled, sizeof(gpu_sleep_enabled));
	}
	return sleep_enabled != 0;
}

/* The wake iOS has asked for, in seconds from now (IOPMrootDomain's "wake" setting, GenericARMPE.cpp); 0: none. */
void hk_power_set_wake(uint64_t seconds_from_now);

void
hk_power_set_wake(uint64_t seconds_from_now)
{
	clock_sec_t now;
	clock_usec_t us;

	PEGetUTCTimeOfDay(&now, &us);
	sleep_wake_at = seconds_from_now != 0 ? (uint64_t)now + seconds_from_now : 0;
}

/*
 * The PMIC's real-time clock (PM8150 slave 0, peripheral 0x60, the Linux pm8xxx-rtc register layout of "qcom,pm8941-rtc": the four bytes of the seconds at 0x48, least
 * significant first: reading the first latches the rest; 0x46 bit 7 says it counts): the phone's clock, as Android keeps it, for the time at boot. 0: not readable.
 */
uint32_t hk_rtc_seconds(void);

uint32_t
hk_rtc_seconds(void)
{
	uint8_t ctrl = 0, a[4], b[4];

	if (hk_spmi_read(0x060, 0x46, &ctrl, 1) != 0 || !(ctrl & 0x80)) {
		return 0;
	}
	for (unsigned tries = 0; tries < 4; tries++) {
		if (hk_spmi_read(0x060, 0x48, a, 4) != 0 || hk_spmi_read(0x060, 0x48, b, 4) != 0) {
			return 0;
		}
		if (memcmp(a, b, 4) == 0) {
			return (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24;
		}
	}
	return 0;
}

static int
sysctl_rtc_seconds(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int seconds = (int)hk_rtc_seconds();

	return SYSCTL_OUT(req, &seconds, sizeof(seconds));
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, rtc_seconds, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_rtc_seconds, "I",
    "the PMIC's real-time clock in seconds (it counts through power-off but is not the date: the date is kept by userland/src/hk_time.c)");

static int
cable_in(void)
{
	uint8_t usbin = 0;
	char board[32] = "";

	if (!PE_parse_boot_argn("board", board, sizeof(board)) || (strcmp(board, "instantnoodle") != 0 && strcmp(board, "instantnoodlep") != 0)) {
		return -1;              /* (the PMIC's arbiter is the OnePlus 8's: nothing at that address in QEMU) */
	}
	return hk_spmi_read(PM8150B_USBIN, USBIN_RT_STS, &usbin, 1) == 0 ? (usbin & USBIN_PLUGIN) != 0 : -1;
}

/*
 * The power meter: what a sleep costs, read from the battery's gauge (bsd/dev/instantnoodle_battery.c) as the system
 * goes to sleep and as it comes back. The gauge counts the cell's charge in mAh, so a figure needs minutes of sleep
 * to mean anything (1 mAh is 12 mA over 5 minutes); `kern.hackios.power_meter` adds every interval up. Nothing is
 * read during the sleep itself (the I2C bus's delays use the clock that is frozen then). An interval with the
 * charger in at either end is counted apart, not as a drain: the charger's current is in the gauge's.
 *   power: meter: asleep|awake S s: A -> B mAh, N uAh drawn = M mA on average; the gauge's own average at its end, mV, cable
 * Awake intervals under 20 s are only added up.
 */
extern bool instantnoodle_battery_meter(int *charge_uah, int *avg_ma, int *mv, int *external);

static struct {
	bool ok;
	int uah, ma, mv, ext;
	uint64_t hw;
} meter_mark;
static struct {
	uint64_t ms[2];                 /* [0] awake, [1] asleep: milliseconds with the cable out */
	int64_t uah[2];                 /* drawn from the cell in them (less than 0: it took charge) */
	uint32_t n[2], cable[2];        /* intervals counted, intervals not counted (a charger in) */
} meter_sum;

static void
meter_close(int state)                  /* the interval that ends now: 0 awake, 1 asleep */
{
	int uah, ma, mv, ext;
	uint64_t hw = ml_get_hwclock(), freq = __builtin_arm_rsr64("CNTFRQ_EL0");
	bool ok = instantnoodle_battery_meter(&uah, &ma, &mv, &ext);

	if (ok && meter_mark.ok && freq != 0) {
		uint64_t ms = (hw - meter_mark.hw) * 1000 / freq;
		int64_t drawn = (int64_t)meter_mark.uah - uah;
		bool plugged = ext || meter_mark.ext;

		if (plugged) {
			meter_sum.cable[state]++;
		} else {
			meter_sum.ms[state] += ms;
			meter_sum.uah[state] += drawn;
			meter_sum.n[state]++;
		}
		if (state == 1 || ms >= 20000) {
			printf("power: meter: %s %llu.%03llu s: %d -> %d mAh, %lld uAh drawn = %lld mA on average; the gauge's own average %d mA "
			    "(%d at the start), %d mV; cable %s\n", state ? "asleep" : "awake", ms / 1000, ms % 1000, meter_mark.uah / 1000,
			    uah / 1000, drawn, ms != 0 ? drawn * 3600 / (int64_t)ms : 0, -ma, -meter_mark.ma, mv, plugged ? "IN (no drain figure)" : "out");
		}
	}
	meter_mark.ok = ok;
	if (ok) {
		meter_mark.uah = uah;
		meter_mark.ma = ma;
		meter_mark.mv = mv;
		meter_mark.ext = ext;
		meter_mark.hw = hw;
	}
}

static int
sysctl_power_meter(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[256];
	size_t n = 0;

	for (int s = 0; s < 2; s++) {
		uint64_t ms = meter_sum.ms[s];

		n += snprintf(text + n, sizeof(text) - n, "%s%s %llu s, %lld uAh drawn, %lld mA (%u intervals, %u with the cable in)",
		    s ? "; " : "", s ? "asleep" : "awake", ms / 1000, meter_sum.uah[s], ms != 0 ? meter_sum.uah[s] * 3600 / (int64_t)ms : 0,
		    meter_sum.n[s], meter_sum.cable[s]);
	}
	return SYSCTL_OUT(req, text, n + 1);
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, power_meter, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_power_meter, "A",
    "the cell's charge drawn awake and asleep, by the gauge, over every interval since the boot (cable out only)");

/*
 * The drain bench (boot-arg drain_bench=1; the run for finding what a sleeping phone costs, `docs/subsystems/power.md`):
 * a thread that, drain_after seconds (default 90: iOS up) after the gauge answers, waits for the cable to come out
 * (the gauge shows the charger's current with it in; up to drain_cable seconds, default 600), looks at the awake
 * drain for drain_awake seconds (default 30), then puts the system to sleep as IOPMSleepSystem() does and, with
 * drain_force=1 (default), asks again whenever the system has been awake for drain_gap seconds (default 60) with
 * the panel dark, so that iOS's own dark wakes are part of the figure. With sleep_max=S every sleep is at most S
 * seconds. After drain_count sleeps (default 3) the totals are said ("bench: DONE"), and the bench goes on asking
 * unless drain_count was reached with drain_stop=1 (then the bench ends and iOS sleeps as it will).
 */
extern void hk_power_request_sleep(void);

static void
bench_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t after = 90, cable_wait = 600, awake_s = 30, force = 1, gap = 60, count = 3, stop = 0;
	uint64_t last_sleeps;
	uint32_t waited = 0, awake_for = 0;

	PE_parse_boot_argn("drain_after", &after, sizeof(after));
	PE_parse_boot_argn("drain_cable", &cable_wait, sizeof(cable_wait));
	PE_parse_boot_argn("drain_awake", &awake_s, sizeof(awake_s));
	PE_parse_boot_argn("drain_force", &force, sizeof(force));
	PE_parse_boot_argn("drain_gap", &gap, sizeof(gap));
	PE_parse_boot_argn("drain_count", &count, sizeof(count));
	PE_parse_boot_argn("drain_stop", &stop, sizeof(stop));
	printf("bench: on: %u s, then the cable out (up to %u s), %u s awake, sleeps (%u, forced %u, gap %u s)\n", after, cable_wait,
	    awake_s, count, force, gap);
	delay_for_interval(after * 1000, NSEC_PER_MSEC);
	while (cable_in() == 1 && waited < cable_wait) {
		if (waited % 30 == 0) {
			printf("bench: WAITING FOR THE CABLE TO COME OUT (%u s)\n", waited);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
		waited += 5;
	}
	printf("bench: the cable is %s after %u s\n", cable_in() == 1 ? "STILL IN: no drain figure" : "out", waited);
	delay_for_interval(15000, NSEC_PER_MSEC);               /* (the unplugging's own wakeups settle) */
	meter_close(0);
	delay_for_interval(awake_s * 1000, NSEC_PER_MSEC);
	meter_close(0);                                         /* (the awake interval says itself, from 20 s) */
	printf("bench: awake with the panel %s for %u s, above\n", power_display_on ? "lit" : "dark", awake_s);
	last_sleeps = sleep_count;
	hk_power_request_sleep();
	printf("bench: the system's sleep asked for\n");
	for (;;) {
		delay_for_interval(5000, NSEC_PER_MSEC);
		if (sleep_count != last_sleeps) {
			last_sleeps = sleep_count;
			awake_for = 0;
			if (sleep_count >= count) {
				printf("bench: DONE after %llu sleeps: %s\n", sleep_count, sleep_last);
				if (stop) {
					return;
				}
			}
			continue;
		}
		awake_for += 5;
		if (force && awake_for >= gap && !power_display_on) {
			awake_for = 0;
			printf("bench: awake for %u s with the panel dark: asked again\n", gap);
			hk_power_request_sleep();
		}
	}
}

void hk_power_bench_start(void);

void
hk_power_bench_start(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("drain_bench", &on, sizeof(on));
	if (on && kernel_thread_start(bench_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Before the other processors are switched off and after they are back (IOCPUSleepKernel()), in thread context with
 * every processor running: what needs drivers' interrupts, which are routed to all of them. The audio path parked
 * (its commands wait for the DSP's answers); the panel dark if iOS had left it lit, and lit again after (its push
 * waits for the display's done) -- done on the boot processor alone, a push after the wake waited out its timeout.
 */
static bool sleep_lit;

void hk_power_will_sleep(void);
void hk_power_did_wake(void);

void
hk_power_will_sleep(void)
{
	if (!hk_power_sleep_supported()) {
		return;
	}
	hk_sound_park();
	hk_wifi_sleep();
	sleep_lit = hk_display_on();
	if (sleep_lit) {
		hk_display_power(false);
	}
	if (gpu_sleep_enabled) {
		hk_gpu_sleep_begin();
	}
	meter_close(0);
}

void
hk_power_did_wake(void)
{
	meter_close(1);
	hk_gpu_sleep_end();             /* (before the panel: its first frame may be the GPU's) */
	hk_buttons_kick();
	hk_wifi_wake();
	if (sleep_lit) {
		sleep_lit = false;
		hk_display_power(true);
	}
}

/*
 * The system sleeps, on the boot processor (GenericARMCPU::haltCPU()), every other processor switched off. Returns
 * what woke it ("pwrkey", "rtc", "usb"), or NULL when this platform does not sleep (back at once, as before).
 */
const char *hk_power_sleep(void);

const char *
hk_power_sleep(void)
{
	const char *reason = NULL;
	uint64_t frozen, began, now, freq = __builtin_arm_rsr64("CNTFRQ_EL0"), wake_hw = 0, polled_cable = 0, ns;
	uint64_t wakes = 0, beats = 0, unrecognized = 0, timer_s;
	uint32_t wake_intid, cpus_off = 0, cpus = ml_get_topology_info()->num_cpus;
	bool irq, low = false, limited = false, timed = false, key_wake;
	int cable;
	clock_sec_t cal;
	clock_usec_t us;
	boolean_t interrupts;

	if (!hk_power_sleep_supported() || freq == 0) {
		return NULL;
	}
	for (uint32_t i = 0; i < cpus; i++) {
		cpus_off += !ml_cpu_online((int)i);
	}
	timer_s = sleep_timer;
	key_wake = timer_s != 0 && sleep_timer_key != 0;
	sleep_timer = 0;
	sleep_timer_key = 0;
	hk_cpufreq_sleep(true);
	cable = cable_in();
	irq = hk_buttons_wake_armed();
	PEGetUTCTimeOfDay(&cal, &us);
	printf("sleep: the system sleeps (%s; the panel %s; the cable %s; %u of %u cores off; wake: %s)\n",
	    irq ? "the power key's interrupt wakes it" : "the power key looked at every 40 ms",
	    sleep_lit ? "lit by iOS: dark for the sleep" : "dark", cable < 0 ? "not known" : cable ? "in" : "out",
	    cpus_off, cpus, timer_s != 0 ? "the kernel's timer" : sleep_wake_at > (uint64_t)cal ? "iOS's, in some seconds" :
	    sleep_wake_at != 0 ? "iOS's, already past" : "none asked for");
	{
		int gpu = hk_gpu_power_bits();

		printf("sleep: the GPU block: %s\n", gpu < 0 ? "not kept" : gpu == 3 ? "CX and GX (the 3D core) powered" : gpu == 1 ? "CX powered, GX off" : "off");
	}
	if (sleep_wake_at != 0) {
		note("sleep: iOS asked for a wake at calendar second %llu, now %llu\n", sleep_wake_at, (uint64_t)cal);
	}

	interrupts = ml_set_interrupts_enabled(FALSE);
	if (irq) {
		(void)hk_buttons_wake_check();  /* (the key's release before the sleep is no reason to end it at once) */
	}
	wake_intid = irq ? ARB_INTID : 0;
	gic_v3_sleep_enter(wake_intid);
	frozen = ml_sleep_timebase_freeze();
	began = ml_get_hwclock();
	if (sleep_wake_at > (uint64_t)cal) {
		wake_hw = began + (sleep_wake_at - (uint64_t)cal) * freq;
	}
	if (instantnoodle_battery_low() && (wake_hw == 0 || wake_hw > began + SLEEP_LOW_SECONDS * freq)) {
		wake_hw = began + SLEEP_LOW_SECONDS * freq;
		low = true;
	}
	if (timer_s != 0 && (wake_hw == 0 || wake_hw > began + timer_s * freq)) {
		wake_hw = began + timer_s * freq;
		timed = true;
	}
	/* sleep_max=S, for a timed run from the computer: its end (loader_return_after) is a timer, and none runs here */
	if (sleep_max != 0 && (wake_hw == 0 || wake_hw > began + sleep_max * freq)) {
		wake_hw = began + sleep_max * freq;
		limited = true;
	}
	for (;;) {
		uint64_t next;

		now = ml_get_hwclock();
		next = now + (irq ? SLEEP_BEAT_SECONDS * freq : SLEEP_POLL_MS * freq / 1000);
		if (wake_hw != 0 && wake_hw < next) {
			next = wake_hw;
		}
		__builtin_arm_wsr64("CNTV_CVAL_EL0", next);
		__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_ENABLE);
		__asm__ volatile ("isb sy\n\tdsb sy" ::: "memory");
		__builtin_arm_wfi();
		wakes++;

		if (irq && gic_v3_spi_pending(ARB_INTID)) {
			int key = hk_buttons_wake_check();

			if (key == 1 || hk_buttons_power_held()) {
				reason = "pwrkey";
				break;
			}
			if (key == 0 && ++unrecognized == 64) {     /* (2: a volume key's, acknowledged: no wake, as an iPhone) */
				/* raised, and nothing of the power key's in it: the key looked at from here on */
				gic_v3_spi_mask(ARB_INTID);
				irq = false;
				note("sleep: the arbiter's interrupt stays raised with nothing of the power key's: the key is looked at\n");
			}
		}
		irq = irq && hk_buttons_wake_armed();          /* (given up for another channel's: polled from here) */
		now = ml_get_hwclock();
		if (!irq && hk_buttons_power_held()) {
			reason = "pwrkey";
			break;
		}
		if (!(__builtin_arm_rsr64("CNTV_CTL_EL0") & CNTV_CTL_ISTATUS)) {
			continue;               /* (not the timer: something else ended the WFI) */
		}
		if (wake_hw != 0 && now >= wake_hw) {
			reason = limited ? "sleep_max" : timed ? (key_wake ? "pwrkey" : "timer") : low ? "battery" : "rtc";
			break;
		}
		if (irq || now - polled_cable >= SLEEP_CABLE_SECONDS * freq) {
			int in = cable_in();

			polled_cable = now;
			if (in >= 0 && cable >= 0 && in != cable) {
				reason = "usb";
				break;
			}
		}
		if (irq || (now - began) / freq >= (beats + 1) * SLEEP_BEAT_SECONDS) {
			beats++;
			loader_live_beat();
		}
	}
	now = ml_get_hwclock();
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_ENABLE);  /* as the kernel keeps it; its next deadline is set next */
	ml_sleep_timebase_thaw(frozen);
	gic_v3_sleep_exit(wake_intid);
	loader_live_beat();
	absolutetime_to_nanoseconds(now - began, &ns);
	note("sleep: woken by %s after %llu.%03llu s (%llu wakes of the processor)\n", reason, ns / NSEC_PER_SEC,
	    ns / NSEC_PER_MSEC % 1000, wakes);
	ml_set_interrupts_enabled(interrupts);

	sleep_count++;
	sleep_seconds_total += ns / NSEC_PER_SEC;
	snprintf(sleep_last, sizeof(sleep_last), "%llu.%03llu s, woken by %s%s, %u of %u cores off", ns / NSEC_PER_SEC,
	    ns / NSEC_PER_MSEC % 1000, reason, timed && key_wake ? " (the timer, as a key)" : "", cpus_off, cpus);
	if (strcmp(reason, "rtc") == 0) {
		sleep_wake_at = 0;
	}
	hk_cpufreq_sleep(false);
	loader_return_wake();
	if (strcmp(reason, "pwrkey") == 0) {
		hk_buttons_woken();
	}
	printf("sleep: awake again: %s\n", sleep_last);
	return reason;
}
