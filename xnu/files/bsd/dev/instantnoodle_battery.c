/*
 * The battery of the OnePlus 8 (instantnoodle, SM8250), for the power source iOS reads
 * (hk_battery_report() in hk_battery.c, then iokit/Kernel/HKBattery.cpp).
 *
 * On redwood the numbers come from the audio DSP's battery manager. This phone has none: its
 * Android reads a TI fuel gauge of OnePlus's own choosing, "ti,bq27541-battery" at I2C address
 * 0x55 on serial engine 16 (i2c@888000, pins gpio48 data / gpio49 clock, SOUTH tile; Android's
 * live device tree), with OnePlus's driver drivers/oneplus/power/supply/qcom/bq27541_fuelgauger.c
 * (android_kernel_oneplus_sm8250). The register map is that driver's: the bq27541 one, or the
 * bq27411 one when the gauge says it is one (control subcommand DEVICE_TYPE, 0x0421).
 *
 * Whether a charger is on the USB port: the PM8150B's charger (SPMI slave 2), USBIN's real-time
 * status (0x1310, bit 4 "plugged in") and the charger's state (0x1006 [2:0]: Linux smb5-reg.h),
 * read through the buttons' SPMI code.
 *
 * Only reads, and one control subcommand at the start (asking the gauge its type, which changes
 * nothing). Nothing is told to the charger: it charges as the boot chain left it set up. The
 * OnePlus fast-charge MCU on the same bus (0x26, "Warp") is left alone; without it the phone
 * charges at the PM8150B's normal rate.
 *
 * On with board=instantnoodle; battery=0 turns it off. Every 5 s; a line in the log every minute.
 *
 * An empty battery shuts the system down in order (reboot_kernel(RB_HALT): every program told to end, the
 * filesystems synced and unmounted, the loader switches the phone off) before the PMIC cuts the power in the middle
 * of a write: no charger and the gauge at 0% or the cell under BATTERY_EMPTY_MV, three readings in a row (15 s),
 * as Android's healthd shuts down at the gauge's 0%. battery_shutdown=0: only said; battery_empty_mv=N another
 * threshold (for trying it: one above the cell's voltage, the cable out).
 *
 * Every ten minutes and when the cable goes in or out, what the charger says of itself (only reads, PM8150B, Linux smb5-reg.h):
 * the port's kind (APSD), the input current limit in force (DCDC ICL_STATUS) and the configured ones, the power path,
 * charging enabled, the battery's temperature state -- for telling why it charges or does not (a computer's port
 * gives 500 mA in "USB 500" mode, less than a busy phone takes).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/hk_input.h>
#include <sys/reboot.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

SYSCTL_DECL(_kern_hackios);


#define GAUGE_ADDR              0x55
#define DEVICE_TYPE_BQ27411     0x0421
#define SUBCMD_DEVICE_TYPE      0x0001
#define REG_CNTL                0x00
#define FLAG_FC                 (1u << 9)       /* bq27541 FLAGS: full charge */

#define PM8150B_CHGR            0x210           /* slave 2, peripheral 0x10 */
#define PM8150B_USBIN           0x213
#define CHGR_STATUS_1           0x06            /* [2:0] (smb5-reg.h) inhibit, trickle, pre, full-on, taper, terminate, pause, disable */
#define CHG_TERMINATE           5
#define CHG_PAUSE               6
#define USBIN_RT_STS            0x10
#define USBIN_PLUGIN            (1u << 4)
#define PM8150B_MISC            0x216
#define PM8150B_DCDC            0x211
#define BATTERY_EMPTY_MV        3300
#define BATTERY_LOW_PERCENT     10

extern int reboot_kernel(int howto, char *message);
static uint32_t empty_readings, shutdown_on = 1, empty_mv = BATTERY_EMPTY_MV;
static volatile int battery_last_percent = -1, battery_last_external = -1;

/*
 * The charger's input current. The firmware leaves the charger in "USB 500" mode (USBIN_ICL_OPTIONS 0x1366: USB51_MODE,
 * not USBIN_MODE_CHG): right for a computer's port, and a wall charger gets 500 mA from it as well. As OnePlus's own
 * smb5 does for the port's kind (APSD 0x1308; smb5-lib.c smblib_set_icl_current() with SW_OVERRIDE_HC_MODE and its
 * DEFAULT_CDP_MA / DEFAULT_DCP_MA): the limit into USBIN_CURRENT_LIMIT_CFG (0x1370, 50 mA a step, qpnp-smb5.c's
 * usb_icl) -- 1500 mA for a charging downstream port, 2000 mA for a dedicated charger --, high-current mode
 * (USBIN_MODE_CHG), no command override (CMD_ICL_OVERRIDE 0x1342 bit 0 clear) and the limit kept after the port's
 * detection (USBIN_LOAD_CFG 0x1365 ICL_OVERRIDE_AFTER_APSD). The charger's AICL lowers it by itself where the adapter
 * cannot give that much. A computer's port (SDP) is left as the firmware set it. The firmware's values come back when
 * the cable comes out, so that the next cable starts as the firmware would have it. charge_icl=0: never touched.
 */
#define USBIN_APSD_RESULT       0x08
#define USBIN_CMD_ICL_OVERRIDE  0x42
#define USBIN_LOAD_CFG          0x65
#define USBIN_ICL_OPTIONS       0x66
#define USBIN_ICL_CFG           0x70
#define DCDC_AICL_ICL_STATUS    0x08
#define PM8150B_TYPEC           0x215
#define TYPEC_SNK_STATUS        0x06
#define SNK_RP_3P0              (1u << 1)
#define SNK_RP_1P5              (1u << 2)
#define APSD_CDP                (1u << 2)
#define APSD_DCP                (1u << 3)
#define ICL_OVERRIDE            (1u << 0)
#define ICL_OVERRIDE_AFTER_APSD (1u << 4)
#define USBIN_MODE_CHG          (1u << 0)
#define ICL_STEP_MA             50
#define ICL_CDP_MA              1500
#define ICL_DCP_MA              2000

static uint32_t icl_on = 1;
static struct {
	bool saved, applied;
	uint8_t options, load, cfg, override;
} icl;

static int
icl_write(uint8_t options, uint8_t load, uint8_t cfg, uint8_t override)
{
	return hk_spmi_write(PM8150B_USBIN, USBIN_ICL_CFG, cfg) | hk_spmi_write(PM8150B_USBIN, USBIN_LOAD_CFG, load) |
	       hk_spmi_write(PM8150B_USBIN, USBIN_CMD_ICL_OVERRIDE, override) |
	       hk_spmi_write(PM8150B_USBIN, USBIN_ICL_OPTIONS, options);
}

static void
icl_follow(bool plugged)
{
	uint8_t apsd = 0, now = 0;
	uint32_t ma;
	int err;

	if (!icl_on) {
		return;
	}
	if (!icl.saved) {
		icl.saved = hk_spmi_read(PM8150B_USBIN, USBIN_ICL_OPTIONS, &icl.options, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_LOAD_CFG, &icl.load, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_ICL_CFG, &icl.cfg, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_CMD_ICL_OVERRIDE, &icl.override, 1) == 0;
		if (!icl.saved) {
			return;
		}
	}
	if (!plugged) {
		if (icl.applied) {
			err = icl_write(icl.options, icl.load, icl.cfg, icl.override);
			icl.applied = false;
			printf("battery: the cable is out: the charger's input limit as the firmware had it%s\n", err ? ": NOT ALL WRITTEN" : "");
		}
		return;
	}
	if (icl.applied || hk_spmi_read(PM8150B_USBIN, USBIN_APSD_RESULT, &apsd, 1) != 0) {
		return;
	}
	/*
	 * (2026-11-12) And what the port says over Type-C (TYPE_C_SNK_STATUS 0x1506: Rp 3.0 A bit 1, 1.5 A bit 2): a USB-C
	 * computer's port or charger advertises 3 A there whatever its BC1.2 kind (a Mac's: CDP and 3 A, image 370). The larger
	 * of the two; a port that says neither (SDP, default Rp) is left at the firmware's 500 mA.
	 */
	{
		uint8_t snk = 0;
		uint32_t rp = 0;

		if (hk_spmi_read(PM8150B_TYPEC, TYPEC_SNK_STATUS, &snk, 1) == 0) {
			rp = (snk & SNK_RP_3P0) ? 3000 : (snk & SNK_RP_1P5) ? 1500 : 0;
		}
		ma = (apsd & APSD_DCP) ? ICL_DCP_MA : (apsd & APSD_CDP) ? ICL_CDP_MA : 0;
		if (rp > ma) {
			ma = rp;
		}
		if (ma == 0) {
			return;
		}
	}
	err = icl_write(icl.options | USBIN_MODE_CHG, icl.load | ICL_OVERRIDE_AFTER_APSD, (uint8_t)(ma / ICL_STEP_MA),
	    icl.override & ~ICL_OVERRIDE);
	icl.applied = true;
	delay_for_interval(500, NSEC_PER_MSEC);
	hk_spmi_read(PM8150B_DCDC, DCDC_AICL_ICL_STATUS, &now, 1);
	printf("battery: a %s: the charger's input limit to %u mA (high-current mode)%s; %u mA after AICL\n",
	    (apsd & APSD_DCP) ? "charger (DCP)" : (apsd & APSD_CDP) ? "charging port (CDP)" : "port with Type-C current", ma,
	    err ? ": NOT ALL WRITTEN" : "", now * ICL_STEP_MA);
}

/*
 * (2026-11-12) Charging itself. The boot chain leaves the PM8150B's charging command off (CHARGING_ENABLE_CMD 0x1042 bit 0
 * clear; the charge state then reads 7, "disable"), and nothing turned it on: with the cable in the phone ran from USB and the
 * battery stayed where it was, all day at 46% (image 370: status1 0x47, chg_en_cmd 0). Android's smb5 votes it on at its start
 * (qpnp-smb5.c smb5_init_hw: "enable the charging path", smblib_chg_disable_vote_callback writes the bit).
 *
 * (2026-11-13) And when it must not charge: Android's OnePlus charger code is not here, so its limits are, every reading
 * (5 s), in charge_control(). Charging is on only while all of these hold, and off (the phone then runs from USB, the cell
 * left alone) as soon as one does not:
 *   - the gauge answered this reading (no reading, no charging);
 *   - the cell under charge_limit percent (95 by default; on again at 5 below), under CHARGE_MAX_MV (on again under
 *     CHARGE_RESUME_MV), between CHARGE_MIN_DC and CHARGE_MAX_DC tenths of a degree (on again 2 degrees inside them):
 *     OnePlus's device tree (qpnp-smb5: hot_bat_decidegc 50 C stops, warm 45 C lowers the float, cool 5 C lowers the
 *     current), taken stricter, since nothing here lowers the current by temperature;
 *   - less than CHARGE_MAX_S of charging since the cable went in (OnePlus's max_chg_time_sec, 10 h): then not again
 *     before the cable comes out.
 * The PMIC's own limits under that: the float voltage (fv_cfg 0x1070, 3.6 V + 10 mV a step) and the fast-charge current
 * (fcc_cfg 0x1061, 50 mA a step) no higher than CHARGE_FV_MV and CHARGE_FCC_MA (the firmware's own 4.35 V and 2 A; a higher
 * value is lowered, a lower one kept); its termination current and hardware JEITA as the firmware set them.
 * And should this thread stop (a hung kernel, a panic, the CrashDump mode), the PMIC's bark-bite watchdog (smb5-reg.h
 * SNARL_BARK_BITE_WD_CFG 0x1653, WD_CFG 0x1651, pet BARK_BITE_WDOG_PET 0x1643; set up as smb5_init_hw does): petted every
 * reading, it barks after 32 s unpetted and bites 8 s later, which turns the charging off in the PMIC itself.
 * charge_enable=0: none of this, the charger left as it is. charge_limit=N: another cap (50 to 100).
 * charge_wd_test=1: the watchdog not petted after the first minute (for seeing it bite: status1 should leave 1-3); =2: not
 * petted from the first minute to the 150th second (whether charging comes back after a bite).
 */
#define CHGR_CHARGING_ENABLE_CMD        0x42
#define CHARGING_ENABLE_CMD_BIT         (1u << 0)
#define CHGR_FCC_CFG                    0x61
#define CHGR_FV_CFG                     0x70
#define CHARGE_FV_MV                    4350
#define CHARGE_FCC_MA                   2000
#define CHARGE_MAX_MV                   4400
#define CHARGE_RESUME_MV                4300
#define CHARGE_MIN_DC                   50      /* 5.0 C */
#define CHARGE_MAX_DC                   450     /* 45.0 C */
#define CHARGE_MAX_S                    (10 * 3600)
#define MISC_WDOG_PET                   0x43
#define MISC_WD_CFG                     0x51
#define MISC_SNARL_BARK_BITE_WD_CFG     0x53
#define WD_TIMER_EN_ON_PLUGIN           (1u << 1)
#define WD_TRIGGER_AFP_EN               (1u << 7)
#define WD_BITE_DISABLE_CHARGING        (1u << 7)
#define WD_SNARL_8S                     0x70
#define WD_BARK_32S                     (1u << 2)       /* ilog2(32 / 16) << 2 */
#define WD_BITE_8S                      0x3

static uint32_t charge_enable_on = 1, charge_limit = 95, charge_wd_test;
static struct {
	bool allowed, held, timed_out, wd_ready;
	uint64_t plugged_at;                    /* mach_absolute_time() when the cable went in, 0 while out */
	const char *why;
	char line[256];
} chg = { .allowed = true, .why = "" };
SYSCTL_STRING(_kern_hackios, OID_AUTO, charge_control, CTLFLAG_RD | CTLFLAG_LOCKED, chg.line, sizeof(chg.line),
    "the charging limits: whether it may charge, why not");

static void
charge_wd_setup(void)
{
	uint8_t cfg = 0, wd = 0;

	if (hk_spmi_read(PM8150B_MISC, MISC_SNARL_BARK_BITE_WD_CFG, &cfg, 1) || hk_spmi_read(PM8150B_MISC, MISC_WD_CFG, &wd, 1)) {
		printf("battery: the PMIC's charging watchdog cannot be read: not set up\n");
		return;
	}
	hk_spmi_write(PM8150B_MISC, MISC_WDOG_PET, 1);
	hk_spmi_write(PM8150B_MISC, MISC_SNARL_BARK_BITE_WD_CFG, WD_BITE_DISABLE_CHARGING | WD_SNARL_8S | WD_BARK_32S | WD_BITE_8S);
	hk_spmi_write(PM8150B_MISC, MISC_WD_CFG, (uint8_t)((wd & ~WD_TRIGGER_AFP_EN) | WD_TIMER_EN_ON_PLUGIN));
	hk_spmi_write(PM8150B_MISC, MISC_WDOG_PET, 1);
	printf("battery: the PMIC's charging watchdog: wd_cfg %#x -> ", wd);
	hk_spmi_read(PM8150B_MISC, MISC_WD_CFG, &wd, 1);
	printf("%#x, bark/bite cfg %#x -> ", wd, cfg);
	hk_spmi_read(PM8150B_MISC, MISC_SNARL_BARK_BITE_WD_CFG, &cfg, 1);
	printf("%#x (charging off 40 s after this thread stops)\n", cfg);
	chg.wd_ready = true;
}

/* a PMIC limit no higher than max_value: lowered when the firmware set it higher */
static void
charge_cap_register(uint8_t reg, uint8_t max_value, const char *name)
{
	uint8_t v = 0;

	if (hk_spmi_read(PM8150B_CHGR, reg, &v, 1) == 0 && v > max_value) {
		hk_spmi_write(PM8150B_CHGR, reg, max_value);
		printf("battery: the charger's %s %#x lowered to %#x\n", name, v, max_value);
	}
}

/* b NULL: the gauge did not answer */
static void
charge_control(const struct hk_battery_state *b, unsigned tick)
{
	uint8_t cmd = 0;
	bool plugged, want;
	int percent = 0, mv = 0, dc = 0;
	uint64_t now = mach_absolute_time(), ns = 0;
	const char *why = NULL;

	if (!charge_enable_on) {
		return;
	}
	if (!chg.wd_ready) {
		charge_wd_setup();
		charge_cap_register(CHGR_FV_CFG, (CHARGE_FV_MV - 3600) / 10, "float voltage");
		charge_cap_register(CHGR_FCC_CFG, CHARGE_FCC_MA / 50, "fast-charge current");
	}
	if (chg.wd_ready && !(charge_wd_test == 1 && tick >= 12) && !(charge_wd_test == 2 && tick >= 12 && tick < 30)) {
		hk_spmi_write(PM8150B_MISC, MISC_WDOG_PET, 1);
	}
	plugged = b != NULL ? b->external : true;
	if (b != NULL && !b->external) {
		chg.plugged_at = 0;
		chg.timed_out = false;
		chg.held = false;
	} else if (chg.plugged_at == 0) {
		chg.plugged_at = now;
	}
	absolutetime_to_nanoseconds(now - chg.plugged_at, &ns);

	if (b == NULL) {
		why = "the gauge does not answer";
	} else {
		percent = b->percent_hundredths / 100;
		mv = b->voltage_uv / 1000;
		dc = b->temperature_hundredths / 10;
		if (ns / NSEC_PER_SEC >= CHARGE_MAX_S) {
			chg.timed_out = true;
		}
		/* the cap, with its 5 points back */
		if (percent >= (int)charge_limit) {
			chg.held = true;
		} else if (percent <= (int)charge_limit - 5) {
			chg.held = false;
		}
		if (chg.timed_out) {
			why = "10 h of charging (until the cable comes out)";
		} else if (chg.held) {
			why = "the charge limit";
		} else if (mv >= CHARGE_MAX_MV || (!chg.allowed && chg.why != NULL && strcmp(chg.why, "the cell's voltage") == 0 &&
		    mv >= CHARGE_RESUME_MV)) {
			why = "the cell's voltage";
		} else if (dc >= CHARGE_MAX_DC || (!chg.allowed && chg.why != NULL && strcmp(chg.why, "too warm") == 0 &&
		    dc >= CHARGE_MAX_DC - 20)) {
			why = "too warm";
		} else if (dc < CHARGE_MIN_DC || (!chg.allowed && chg.why != NULL && strcmp(chg.why, "too cold") == 0 &&
		    dc < CHARGE_MIN_DC + 20)) {
			why = "too cold";
		}
	}
	want = why == NULL;
	if (!plugged && want) {
		chg.allowed = true;
		chg.why = "";
		return;                         /* (nothing to charge from: the bit is left as it is until the next cable) */
	}
	if (hk_spmi_read(PM8150B_CHGR, CHGR_CHARGING_ENABLE_CMD, &cmd, 1) == 0 &&
	    ((cmd & CHARGING_ENABLE_CMD_BIT) != 0) != want) {
		hk_spmi_write(PM8150B_CHGR, CHGR_CHARGING_ENABLE_CMD,
		    (uint8_t)(want ? (cmd | CHARGING_ENABLE_CMD_BIT) : (cmd & ~CHARGING_ENABLE_CMD_BIT)));
		cmd = 0xff;
		hk_spmi_read(PM8150B_CHGR, CHGR_CHARGING_ENABLE_CMD, &cmd, 1);
	}
	/*
	 * A bite stays: the charger goes on reading "disable" (status1 7) with the command set, even once petted again (image 377,
	 * charge_wd_test=2). Charging allowed, the watchdog petted for two readings, the command set and the charger disabled:
	 * the command cleared and set again, which starts it over.
	 */
	{
		static unsigned petted_ticks;
		uint8_t st = 0;
		bool petted = chg.wd_ready && !(charge_wd_test == 1 && tick >= 12) && !(charge_wd_test == 2 && tick >= 12 && tick < 30);

		petted_ticks = petted ? petted_ticks + 1 : 0;
		if (want && plugged && petted_ticks >= 2 && cmd != 0xff && (cmd & CHARGING_ENABLE_CMD_BIT) &&
		    hk_spmi_read(PM8150B_CHGR, CHGR_STATUS_1, &st, 1) == 0 && (st & 7) == 7) {
			hk_spmi_write(PM8150B_CHGR, CHGR_CHARGING_ENABLE_CMD, (uint8_t)(cmd & ~CHARGING_ENABLE_CMD_BIT));
			delay_for_interval(20, NSEC_PER_MSEC);
			hk_spmi_write(PM8150B_CHGR, CHGR_CHARGING_ENABLE_CMD, cmd);
			delay_for_interval(200, NSEC_PER_MSEC);
			hk_spmi_read(PM8150B_CHGR, CHGR_STATUS_1, &st, 1);
			printf("battery: the charger was disabled with charging allowed (a watchdog bite?): the command set again, state now %u\n",
			    st & 7);
		}
	}
	if (want != chg.allowed || (why != NULL && chg.why != NULL && strcmp(why, chg.why) != 0)) {
		printf("battery: charging %s%s%s (%d%%, %d mV, %d.%d C; CHARGING_ENABLE_CMD %#x)\n", want ? "on" : "OFF",
		    want ? "" : ": ", want ? "" : why, percent, mv, dc / 10, (dc < 0 ? -dc : dc) % 10, cmd);
	}
	chg.allowed = want;
	chg.why = want ? "" : why;
	snprintf(chg.line, sizeof(chg.line), "charging %s%s; limit %u%%, %d%% %d mV %d.%d C, plugged %llu s, watchdog %s",
	    want ? "allowed" : "OFF: ", want ? "" : why, charge_limit, percent, mv, dc / 10, (dc < 0 ? -dc : dc) % 10,
	    chg.plugged_at ? (unsigned long long)(ns / NSEC_PER_SEC) : 0ULL,
	    !chg.wd_ready ? "not set up" : (charge_wd_test == 1 && tick >= 12) || (charge_wd_test == 2 && tick >= 12 && tick < 30) ?
	    "NOT PETTED (test)" : "petted");
}

/* For the system's sleep (bsd/dev/hk_power.c): the battery low and no charger at the last reading. */
bool instantnoodle_battery_low(void);

bool
instantnoodle_battery_low(void)
{
	return battery_last_external == 0 && battery_last_percent >= 0 && battery_last_percent <= BATTERY_LOW_PERCENT;
}

/*
 * (2026-11-12) The last account, for sysctl kern.hackios.charger_pm8150b (hk_sysctl on the phone): the log's tail is a minute or two
 * of a busy run, and the account is said only every ten minutes.
 */
static char charger_line[768];
static int battery_stage;               /* how far the thread has come: 1 started, 2 gauge asked, 3 in its loop (+ ticks * 10) */
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_stage, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_stage, 0, "the OnePlus gauge thread's progress");
SYSCTL_STRING(_kern_hackios, OID_AUTO, charger_pm8150b, CTLFLAG_RD | CTLFLAG_LOCKED, charger_line, sizeof(charger_line),
    "the PM8150B charger's registers, the last reading");

/* The charger's own account (reads only): a line */
static void
charger_say(void)
{
	static const struct {
		uint16_t id;
		uint8_t reg;
		const char *name;
	} regs[] = {
		{ PM8150B_CHGR, 0x06, "status1" }, { PM8150B_CHGR, 0x07, "status2" }, { PM8150B_CHGR, 0x42, "chg_en_cmd" },
		{ PM8150B_CHGR, 0x61, "fcc_cfg" }, { PM8150B_CHGR, 0x70, "fv_cfg" }, { PM8150B_USBIN, 0x07, "apsd_status" },
		{ PM8150B_USBIN, 0x08, "apsd_result" }, { PM8150B_USBIN, 0x66, "icl_options" }, { PM8150B_USBIN, 0x70, "icl_cfg" },
		{ PM8150B_CHGR, 0x0d, "status7_temp" }, { PM8150B_USBIN, 0x42, "icl_override" }, { PM8150B_USBIN, 0x65, "load_cfg" },
		{ PM8150B_DCDC, 0x06, "icl_max" }, { PM8150B_DCDC, 0x07, "icl_status" }, { PM8150B_DCDC, 0x08, "aicl_icl" },
		{ PM8150B_DCDC, 0x0a, "aicl" },
		{ PM8150B_DCDC, 0x0b, "power_path" }, { PM8150B_MISC, 0x51, "wd_cfg" },
		/* (2026-11-12) the Type-C port (TYPEC 0x1500, smb5-reg.h): what the source advertises (SNK_STATUS: 3A bit 1, 1.5A
		 * bit 2, default bit 3) and the port's state; USBIN's interrupt status; the charger's enable bits */
		{ 0x215, 0x06, "typec_snk" }, { 0x215, 0x07, "typec_snk_dbg" }, { 0x215, 0x0b, "typec_misc" },
		{ PM8150B_USBIN, 0x10, "usbin_rt" }, { PM8150B_CHGR, 0x08, "status3" }, { PM8150B_CHGR, 0x0e, "chgr_rt" },
		{ PM8150B_CHGR, 0x60, "chgr_cfg" }, { PM8150B_CHGR, 0x52, "pre_cfg" }, { PM8150B_MISC, 0x40, "misc_cmd" },
		/* (2026-11-13) the safety limits the PMIC keeps itself: hardware JEITA (0x1090), the safety timers (0x10a0, its
		 * fast-charge one 0x10a2), the termination (0x1050 chgr_cfg1), the watchdog (0x1653) and MISC's interrupt status */
		{ PM8150B_CHGR, 0x90, "jeita_en" }, { PM8150B_CHGR, 0xa0, "safety_en" }, { PM8150B_CHGR, 0xa2, "fast_timer" },
		{ PM8150B_CHGR, 0x50, "chgr_cfg1" }, { PM8150B_MISC, 0x53, "wd_bark_bite" }, { PM8150B_MISC, 0x10, "misc_rt" },
	};
	char line[768];
	size_t n = 0;

	n += snprintf(line + n, sizeof(line) - n, "battery: charger");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]) && n < sizeof(line); i++) {
		uint8_t v = 0;

		if (hk_spmi_read(regs[i].id, regs[i].reg, &v, 1) == 0) {
			n += snprintf(line + n, sizeof(line) - n, " %s %#x", regs[i].name, v);
		} else {
			n += snprintf(line + n, sizeof(line) - n, " %s -", regs[i].name);
		}
	}
	printf("%s\n", line);
	strlcpy(charger_line, line, sizeof(charger_line));
}

/* An empty battery, three readings in a row with no charger: the system shut down in order. */
static void
empty_check(const struct hk_battery_state *b)
{
	int mv = b->voltage_uv / 1000, percent = b->percent_hundredths / 100;

	battery_last_percent = percent;
	battery_last_external = b->external;
	if (b->external || (percent > 0 && mv >= (int)empty_mv)) {
		empty_readings = 0;
		return;
	}
	if (++empty_readings == 3) {
		printf("battery: EMPTY (%d%%, %d mV, no charger, three readings in a row): %s\n", percent, mv,
		    shutdown_on ? "the system shuts down" : "not shut down (battery_shutdown=0)");
		if (shutdown_on) {
			reboot_kernel(RB_HALT, (char *)"battery empty");
		}
	}
}

static struct instantnoodle_i2c bus = {
	.name = "se16",
	.sda = { INSTANTNOODLE_TLMM_SOUTH, 48 },
	.scl = { INSTANTNOODLE_TLMM_SOUTH, 49 },
	.pull = INSTANTNOODLE_PULL_UP,          /* as the boot chain leaves the pins (0x7): no answer without it */
};

/* the gauge's registers, by its type (OnePlus's gauge_set_cmd_addr()) */
static struct {
	const char *name;
	uint8_t temp, volt, flags, rm, fcc, ai, soc, cycles, design;
} map = { "bq27541", 0x06, 0x08, 0x0a, 0x10, 0x12, 0x14, 0x2c, 0x2a, 0x3c };

static int
word(uint8_t reg, int *value)
{
	uint8_t b[2];
	int err = instantnoodle_i2c_read(&bus, GAUGE_ADDR, reg, b, 2);

	if (err == 0) {
		*value = b[0] | b[1] << 8;
	}
	return err;
}

static int
device_type(void)
{
	uint8_t cmd[3] = { REG_CNTL, SUBCMD_DEVICE_TYPE & 0xff, SUBCMD_DEVICE_TYPE >> 8 };
	int type = -1;

	if (instantnoodle_i2c_write(&bus, GAUGE_ADDR, cmd, sizeof(cmd)) != 0) {
		return -1;
	}
	delay(66);                                      /* as OnePlus's driver waits */
	return word(REG_CNTL, &type) == 0 ? type : -1;
}

/* one reading; false when the gauge did not answer */
static bool
sample(struct hk_battery_state *b, int *flags_out, uint8_t *charger_state)
{
	int soc, mv, ma, temp_dk, flags = 0, rm = 0, fcc = 0, cycles = 0, design = 0;
	uint8_t usbin = 0, chgr = 0xff;

	if (word(map.soc, &soc) || word(map.volt, &mv) || word(map.ai, &ma) || word(map.temp, &temp_dk)) {
		return false;
	}
	word(map.flags, &flags);
	word(map.rm, &rm);
	word(map.fcc, &fcc);
	if (map.cycles) {
		word(map.cycles, &cycles);
	}
	if (map.design) {
		word(map.design, &design);
	}
	ma = (int16_t)ma;                               /* TI's sign: more than 0 while the battery takes charge */
	bzero(b, sizeof(*b));
	b->size = sizeof(*b);
	b->percent_hundredths = (soc > 100 ? 100 : soc) * 100;
	b->voltage_uv = mv * 1000;
	b->current_ua = ma * 1000;
	b->temperature_hundredths = temp_dk * 10 - 27315;       /* tenths of a kelvin */
	b->present = 1;
	b->charge_uah = rm * 1000;
	b->full_uah = fcc * 1000;
	b->design_uah = design > 1000 && design < 10000 ? design * 1000 : 0;
	b->cycles = cycles < 10000 ? cycles : 0;

	if (hk_spmi_read(PM8150B_USBIN, USBIN_RT_STS, &usbin, 1) == 0) {
		b->external = (usbin & USBIN_PLUGIN) != 0;
	} else {
		b->external = ma > 0;                   /* the charger cannot be asked: taking charge says it is there */
	}
	if (hk_spmi_read(PM8150B_CHGR, CHGR_STATUS_1, &chgr, 1) == 0) {
		chgr &= 7;
	}
	/* Linux POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	if (!b->external) {
		b->status = 2;
	} else if (soc >= 100 || (flags & FLAG_FC) || chgr == CHG_TERMINATE) {
		b->status = 4;
	} else if (ma > 0 || (chgr >= 1 && chgr < CHG_TERMINATE)) {
		b->status = 1;
	} else {
		b->status = 3;
	}
	*flags_out = flags;
	*charger_state = chgr;
	return true;
}

/*
 * One reading of the gauge now, for the power meter (bsd/dev/hk_power.c: the cell's charge before and after every
 * sleep, drain_bench=). The cell's remaining charge in uAh (the gauge counts in mAh), its average current in mA
 * (TI's sign: more than 0 while the cell takes charge), its voltage, whether a charger is in. False before the gauge
 * answered once or when it does not. The bus is locked per transfer: the thread's reads and this one's may mix.
 */
static volatile bool gauge_ready;

bool instantnoodle_battery_meter(int *charge_uah, int *avg_ma, int *mv, int *external);

bool
instantnoodle_battery_meter(int *charge_uah, int *avg_ma, int *mv, int *external)
{
	struct hk_battery_state b;
	int flags;
	uint8_t chgr;

	if (!gauge_ready || !sample(&b, &flags, &chgr)) {
		return false;
	}
	*charge_uah = b.charge_uah;
	*avg_ma = b.current_ua / 1000;
	*mv = b.voltage_uv / 1000;
	*external = b.external;
	return true;
}

void hk_power_bench_start(void);        /* bsd/dev/hk_power.c */

/*
 * (2026-11-12) The Warp (VOOC) fast-charge MCU, only looked at: an STM8S at 0x26 on this bus (the 8 Pro's device tree:
 * "oplus,stm8s-fastcg"), with the USB data switch on gpio 82, its reset on gpio 10, the clock on gpio 131 and the data on
 * gpio 129 (north, north, south, south: Linux pinctrl-sm8250.c). Whether it answers its address (an empty write: nothing is
 * told to it) and how its lines are set, for kern.hackios.warp. Nothing drives it yet: without the AP's half of the handshake
 * the adapter stays at 5 V and the PM8150B charges as on any charger.
 */
static char warp_line[256];
SYSCTL_STRING(_kern_hackios, OID_AUTO, warp, CTLFLAG_RD | CTLFLAG_LOCKED, warp_line, sizeof(warp_line),
    "the Warp fast-charge MCU: whether it answers, its lines");

static void
warp_look(void)
{
	static const struct { struct instantnoodle_pin p; const char *name; } pins[] = {
		{ { INSTANTNOODLE_TLMM_NORTH, 82 }, "switch" }, { { INSTANTNOODLE_TLMM_NORTH, 10 }, "reset" },
		{ { INSTANTNOODLE_TLMM_SOUTH, 131 }, "clock" }, { { INSTANTNOODLE_TLMM_SOUTH, 129 }, "data" },
	};
	size_t n = 0;
	int answer = instantnoodle_i2c_write(&bus, 0x26, NULL, 0);

	n += snprintf(warp_line + n, sizeof(warp_line) - n, "warp: the MCU at 0x26 %s;", answer == 0 ? "answers" : "does not answer");
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]) && n < sizeof(warp_line); i++) {
		n += snprintf(warp_line + n, sizeof(warp_line) - n, " %s gpio%u cfg %#x level %d", pins[i].name, pins[i].p.pin,
		    instantnoodle_gpio_cfg(pins[i].p), instantnoodle_gpio_level(pins[i].p));
	}
	printf("%s\n", warp_line);
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct hk_battery_state b;
	unsigned failed = 0;
	int said_external = -1;
	int type, flags;
	uint8_t chgr;

	battery_stage = 1;
	printf("battery: bq27541 at %#x on %s (gpio%u/%u): pins cfg %#x %#x\n", GAUGE_ADDR, bus.name, bus.sda.pin,
	    bus.scl.pin, instantnoodle_gpio_cfg(bus.sda), instantnoodle_gpio_cfg(bus.scl));
	instantnoodle_i2c_recover(&bus);
	printf("battery: lines let go: sda %d scl %d\n", instantnoodle_gpio_level(bus.sda), instantnoodle_gpio_level(bus.scl));
	type = device_type();
	if (type < 0) {
		char found[200] = "";

		/* who is on this bus at all: an address and nothing else, which writes nothing */
		for (uint8_t a = 0x08; a < 0x78; a++) {
			if (instantnoodle_i2c_write(&bus, a, NULL, 0) == 0) {
				snprintf(found + strlen(found), sizeof(found) - strlen(found), " %#x", a);
			}
		}
		printf("battery: no gauge; on %s answer:%s\n", bus.name, found[0] ? found : " nobody");
	}
	if (type == DEVICE_TYPE_BQ27411) {
		map.name = "bq27411";
		map.temp = 0x02; map.volt = 0x04; map.flags = 0x06; map.rm = 0x0c; map.fcc = 0x0e;
		map.ai = 0x10; map.soc = 0x1c; map.cycles = 0; map.design = 0;
	}
	printf("battery: the gauge says device type %#x: %s register map\n", type, map.name);
	gauge_ready = type >= 0;
	battery_stage = 2;
	warp_look();
	if (gauge_ready) {
		hk_power_bench_start();
	}

	for (unsigned tick = 0;; tick++) {
		battery_stage = 3 + 10 * (int)tick;
		if (sample(&b, &flags, &chgr)) {
			failed = 0;
			hk_battery_report(&b);
			empty_check(&b);
			icl_follow(b.external);
			charge_control(&b, tick);
			if (tick % 120 == 0 || b.external != said_external || (b.external && tick % 12 == 0)) {
				said_external = b.external;     /* (every ten minutes, and when the cable goes in or out) */
				charger_say();
			}
			if (tick % 12 == 0) {
				printf("battery: %d%%, %d mV, %d mA, %d.%02d C, %d/%d mAh (design %d), %d cycles, flags %#x; "
				    "USB %s, charger state %u, status %d (%u bus errors)\n", b.percent_hundredths / 100,
				    b.voltage_uv / 1000, b.current_ua / 1000, b.temperature_hundredths / 100,
				    (b.temperature_hundredths < 0 ? -b.temperature_hundredths : b.temperature_hundredths) % 100,
				    b.charge_uah / 1000, b.full_uah / 1000, b.design_uah / 1000, b.cycles, flags,
				    b.external ? "plugged" : "unplugged", chgr, b.status, bus.errors);
			}
		} else if (charge_control(NULL, tick), ++failed == 1 || failed % 12 == 0) {
			printf("battery: the gauge at %#x does not answer (%u times in a row)\n", GAUGE_ADDR, failed);
			instantnoodle_i2c_recover(&bus);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
instantnoodle_battery_init(void)
{
	uint32_t enabled = 1;
	thread_t thread;

	if (!instantnoodle_board()) {
		return;
	}
	PE_parse_boot_argn("battery", &enabled, sizeof(enabled));
	PE_parse_boot_argn("battery_shutdown", &shutdown_on, sizeof(shutdown_on));
	PE_parse_boot_argn("battery_empty_mv", &empty_mv, sizeof(empty_mv));  /* (a test: above the cell's voltage, unplugged) */
	PE_parse_boot_argn("charge_icl", &icl_on, sizeof(icl_on));
	PE_parse_boot_argn("charge_enable", &charge_enable_on, sizeof(charge_enable_on));
	PE_parse_boot_argn("charge_limit", &charge_limit, sizeof(charge_limit));
	PE_parse_boot_argn("charge_wd_test", &charge_wd_test, sizeof(charge_wd_test));
	if (charge_limit < 50 || charge_limit > 100) {
		charge_limit = 95;
	}
	if (!enabled) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
