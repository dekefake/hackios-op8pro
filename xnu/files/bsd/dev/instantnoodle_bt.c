/*
 * The OnePlus 8 Pro's Bluetooth controller (instantnoodlep, SM8250): the hardware side only, a ladder of boot-args like the
 * camera's. It is the QCA6390's Bluetooth half (the Wi-Fi half is bsd/dev/instantnoodle_wifi.c: one chip, two functions, shared
 * rails), reached over a UART. What iOS makes of it is another matter (BlueTool wants a Broadcom controller; see
 * INSTANTNOODLEP-PLAN.md addendum 36): this file brings up the part that is the same whatever iOS thinks.
 *
 * What this phone has (its live device tree, the Lineage kernel's drivers/bluetooth/{bluetooth-power.c,hci_qca.c,btqca.c},
 * drivers/tty/serial/msm_geni_serial.c, include/linux/qcom-geni-se.h, drivers/clk/qcom/gcc-kona.c, pinctrl-kona.c):
 *
 *   chip       QCA6390 (Wi-Fi on PCIe 0, Bluetooth on a UART, both behind WLAN_EN gpio20 / BT_EN gpio21)
 *   UART       qcom,qup_uart@998000, "qcom,msm-geni-serial-hs": serial engine 6 of QUPv3 wrapper 0 (wrapper at 0x9c0000, its
 *              AHB clocks gcc_qupv3_wrap_0_m_ahb_clk / s_ahb_clk), GPIO 16 CTS, 17 RFR, 18 TX, 19 RX, all function 1 (qup6), the
 *              north tile of the TLMM; its interrupts are the engine's SPI 0x25f and a wake-up on GPIO 19 (RX), wake-up byte 0xfd.
 *              Android's driver runs it in FIFO mode (it reads SE_GENI_DMA_MODE_EN and sets it up only for DMA).
 *   BT_EN      gpio21 (bt-reset-gpio): low 50 ms, then high, then 50 ms; gpio124 (bt-sw-ctrl) is an INPUT that the controller drives
 *              high when it is up (bluetooth-power.c reads it and complains when it is not)
 *   rails      the QCA6390's, shared with Wi-Fi: pm8150 S6 0.95 V (aon, RPMh "smpa6"), pm8009 S2 0.95 V (dig, "smpf2"), pm8150 S5
 *              1.9-2.04 V (rfa1, "smpa5"), pm8150a S8 1.35 V (rfa2, "smpc8"), and Bluetooth's own: pm8150 L16 3.024-3.304 V (asd,
 *              "ldoa16", 10 mA)
 *
 * The firmware (Qualcomm's "ROME" TLV, backed up in the phone's bluetooth partition: htbtfw20.tlv 205836 bytes, patch type 1,
 * download mode 3, product 0x10, rom build 0x200, patch version 0x4509; htnv20.bin 5815 bytes, NVM type 2, 51 tags; version file
 * "BTFM.HST.2.0.0.c3-00476-QCACHROMZ-1") goes in over HCI vendor commands, as btqca.c does it: read the version (opcode 0xfc00,
 * parameter 0x19; the answer has product, patch version, ROM version, SoC id), send the patch file whole in segments of 243 bytes
 * (0xfc00, parameter 0x1e, length, data; mode 3 acknowledges only the last segment), 10 ms, the NVM file the same way (with tag 17's
 * byte 0 or'ed 0x80 and its byte 2 the baud code, tag 27's byte 0 or'ed 1, if inband sleep is wanted), HCI_Reset. The baud rate is
 * changed with the vendor command 0xfc48 (one byte: 0 115200, 7 460800, 10 921600, 13 2000000, 14 3000000, 17 3200000) and the
 * host follows 300 ms later. None of that is here yet: it needs the UART's FIFO path, which is the next rung.
 *
 * Nothing is done unless a boot-arg asks for it:
 *
 *   bt=1   a look, READING ONLY: the engine's three clocks (their halt bits and the vote register), the six pins, and -- only if all
 *          three clocks are running, because a register of a block whose clock is stopped is a bus error that takes the phone to
 *          CrashDump -- the engine's registers: the firmware revision (whose protocol field says what the engine's firmware is: 2 is
 *          UART), the status, whether its FIFO interface is disabled, the DMA mode, the hardware parameters
 *   bt=2   ... then the chip powered on as Android powers it (rails, BT_EN low 50 ms, high, 50 ms), the engine's clocks voted, its
 *          four pins given to qup6, the engine's registers read again, and the controller's sw-ctrl line looked at; then everything put
 *          back. Refused when wifi= is also on (the rails and the vote register are the Wi-Fi driver's then). Not done: a byte on the UART.
 *
 * Every write is announced first and read back. The GCC's vote register (0x52008) is changed only in the bits of this engine's three
 * clocks (6 and 7: the wrapper's AHB clocks, 16: the engine's own) and only when they were off, and given back the same way.
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
#define GCC_VOTE                0x52008         /* APCS's branch votes (gcc-kona.c: enable_reg of the QUPv3 branches) */
#define VOTE_M_AHB              (1u << 6)       /* gcc_qupv3_wrap_0_m_ahb_clk */
#define VOTE_S_AHB              (1u << 7)       /* gcc_qupv3_wrap_0_s_ahb_clk */
#define VOTE_S6                 (1u << 16)      /* gcc_qupv3_wrap0_s6_clk */
#define VOTE_MINE               (VOTE_M_AHB | VOTE_S_AHB | VOTE_S6)
#define HALT_M_AHB              0x17004
#define HALT_S_AHB              0x17008
#define HALT_S6                 0x1772c
#define RCG_S6                  0x17730         /* command; configuration at +4 */
#define CBCR_OFF                (1u << 31)

#define SE_PHYS                 0x00998000UL    /* qcom,qup_uart@998000 */
#define SE_SIZE                 0x4000UL
/* registers of the serial engine (include/linux/qcom-geni-se.h) */
#define GENI_INIT_CFG_REVISION  0x000
#define GENI_FORCE_DEFAULT_REG  0x020
#define GENI_CGC_CTRL           0x028
#define SE_GENI_STATUS          0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_CLK_CTRL_RO        0x060
#define GENI_IF_FIFO_DISABLE_RO 0x064
#define GENI_FW_REVISION_RO     0x068
#define GENI_FW_S_REVISION_RO   0x06c
#define SE_GENI_CLK_SEL         0x07c
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_IOS             0x908
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define FW_REV_PROTOCOL(x)      (((x) >> 8) & 0xff)

#define PIN(n)                  ((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, (n) })
#define PIN_CTS                 PIN(16)
#define PIN_RFR                 PIN(17)
#define PIN_TX                  PIN(18)
#define PIN_RX                  PIN(19)
#define PIN_WLAN_EN             PIN(20)
#define PIN_BT_EN               PIN(21)
#define PIN_SW_CTRL             PIN(124)
#define FUNCTION_QUP6           1

static volatile uint32_t *gcc17, *gcc52, *se;
static uint32_t level;

static uint32_t
rd(volatile uint32_t *page, uint32_t offset)
{
	return page[(offset & 0xfff) / 4];
}

static void
show_pin(struct instantnoodle_pin p, const char *what)
{
	uint32_t cfg = instantnoodle_gpio_cfg(p);

	say("bt: pin %u (%s): function %u, pull %u, drive %u mA, %s, level %u\n", p.pin, what, (cfg >> 2) & 0xf, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & (1u << 9)) ? "output" : "input", instantnoodle_gpio_level(p) ? 1u : 0u);
}

static void
show_pins(void)
{
	show_pin(PIN_CTS, "UART CTS");
	show_pin(PIN_RFR, "UART RFR");
	show_pin(PIN_TX, "UART TX");
	show_pin(PIN_RX, "UART RX");
	show_pin(PIN_WLAN_EN, "WLAN_EN, for context");
	show_pin(PIN_BT_EN, "BT_EN");
	show_pin(PIN_SW_CTRL, "sw-ctrl, the controller drives it");
}

/* true: the engine's three clocks all run (a halt bit of 0 in each branch's status) */
static bool
clocks_run(bool say_it)
{
	uint32_t m = rd(gcc17, HALT_M_AHB), s = rd(gcc17, HALT_S_AHB), e = rd(gcc17, HALT_S6), vote = rd(gcc52, GCC_VOTE);
	/*
	 * The slave AHB clock has hardware clock gating (gcc-kona.c: hwcg_reg 0x17008, hwcg_bit 1): once voted it still reads "off"
	 * (bit 31) while nothing uses it and wakes by itself at the first access to the wrapper, as Linux relies on. So: voted and
	 * gated counts as running. (The first run read 0x80000002 after the vote and called the clocks "not coming on".)
	 */
	bool s_ok = !(s & CBCR_OFF) || ((s & 2u) && (vote & VOTE_S_AHB));
	bool run = !(m & CBCR_OFF) && s_ok && !(e & CBCR_OFF);

	if (say_it) {
		say("bt: GCC: wrapper 0 master AHB clock %#010x (%s), slave AHB clock %#010x (%s), engine 6 clock %#010x (%s); vote register "
		    "%#010x (the three bits: %u%u%u); engine 6's root generator cmd %#010x cfg %#010x\n", m, (m & CBCR_OFF) ? "off" : "running",
		    s, (s & CBCR_OFF) ? ((s & 2u) && (vote & VOTE_S_AHB) ? "gated, wakes on use" : "off") : "running", e, (e & CBCR_OFF) ? "off" : "running", vote, (vote & VOTE_M_AHB) ? 1u : 0u,
		    (vote & VOTE_S_AHB) ? 1u : 0u, (vote & VOTE_S6) ? 1u : 0u, rd(gcc17, RCG_S6), rd(gcc17, RCG_S6 + 4));
	}
	return run;
}

static void
show_engine(void)
{
	uint32_t fw, sfw, fifo, dma;

	if (!clocks_run(false)) {
		say("bt: the engine's clocks are not all running: its registers are not read (a bus error would follow)\n");
		return;
	}
	fw = rd(se, GENI_FW_REVISION_RO);
	sfw = rd(se, GENI_FW_S_REVISION_RO);
	fifo = rd(se, GENI_IF_FIFO_DISABLE_RO);
	dma = rd(se, SE_GENI_DMA_MODE_EN);
	say("bt: engine 6: init cfg revision %#010x, firmware revision %#010x (protocol %u: %s, version %u), slave firmware %#010x\n",
	    rd(se, GENI_INIT_CFG_REVISION), fw, FW_REV_PROTOCOL(fw), FW_REV_PROTOCOL(fw) == 2 ? "UART" : FW_REV_PROTOCOL(fw) == 0 ? "none" :
	    "NOT UART", fw & 0xff, sfw);
	say("bt: engine 6: status %#010x, clock control %#010x, FIFO interface disabled %#x, DMA mode %#x, hardware parameters %#010x %#010x, "
	    "main clock cfg %#010x, IOS %#010x\n", rd(se, SE_GENI_STATUS), rd(se, GENI_CLK_CTRL_RO), fifo, dma, rd(se, SE_HW_PARAM_0),
	    rd(se, SE_HW_PARAM_1), rd(se, GENI_SER_M_CLK_CFG), rd(se, SE_GENI_IOS));
}

/* ---- bt=2 ---- */

static bool did_vote, did_pins;
static uint32_t voted_bits;

static bool
power_up(void)
{
	uint32_t before = rd(gcc52, GCC_VOTE);

	say("bt: bt=2: the QCA6390's Bluetooth powered on as Android does it\n");
	say("bt: next: RPMh rails smpa6 950, smpf2 950, smpa5 1900, smpc8 1350 (the radio's, shared with Wi-Fi) and ldoa16 3300 mV (Bluetooth's own)\n");
	if (!instantnoodle_supply("smpa6", 950, true) || !instantnoodle_supply("smpf2", 950, true) ||
	    !instantnoodle_supply("smpa5", 1900, true) || !instantnoodle_supply("smpc8", 1350, true) ||
	    !instantnoodle_supply("ldoa16", 3300, true)) {
		say("bt: RPMh did not take a rail's vote\n");
		return false;
	}
	did_vote = true;
	IOSleep(2);
	say("bt: next: BT_EN (pin 21) low for 50 ms, then high, then 50 ms\n");
	instantnoodle_gpio_output(PIN_BT_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
	IOSleep(50);
	instantnoodle_gpio_output(PIN_BT_EN, true, INSTANTNOODLE_PULL_NONE, 2);
	IOSleep(50);
	instantnoodle_gpio_input(PIN_SW_CTRL, INSTANTNOODLE_PULL_NONE);
	say("bt: the controller's sw-ctrl line (pin 124) is %u (Android wants it high once the controller is up; with no firmware loaded it may not be)\n",
	    instantnoodle_gpio_level(PIN_SW_CTRL) ? 1u : 0u);
	voted_bits = VOTE_MINE & ~before;
	say("bt: next: the vote register's bits %#x (the engine's clocks that are off: %#010x) set\n", voted_bits, before);
	if (voted_bits != 0) {
		gcc52[(GCC_VOTE & 0xfff) / 4] = before | voted_bits;
		did_vote = true;
	}
	for (int i = 0; i < 100 && !clocks_run(false); i++) {
		IOSleep(1);
	}
	clocks_run(true);
	if (!clocks_run(false)) {
		say("bt: the engine's clocks do not come on\n");
		return false;
	}
	say("bt: next: pins 16-19 to the engine (function %u)\n", FUNCTION_QUP6);
	instantnoodle_gpio_function(PIN_CTS, FUNCTION_QUP6, INSTANTNOODLE_PULL_NONE, 2);
	instantnoodle_gpio_function(PIN_RFR, FUNCTION_QUP6, INSTANTNOODLE_PULL_NONE, 2);
	instantnoodle_gpio_function(PIN_TX, FUNCTION_QUP6, INSTANTNOODLE_PULL_UP, 2);
	instantnoodle_gpio_function(PIN_RX, FUNCTION_QUP6, INSTANTNOODLE_PULL_NONE, 2);
	did_pins = true;
	return true;
}

static void
power_down(void)
{
	say("bt: power down: pins back, BT_EN low, the engine's clocks given back\n");
	if (did_pins) {
		instantnoodle_gpio_input(PIN_CTS, INSTANTNOODLE_PULL_NONE);
		instantnoodle_gpio_input(PIN_RFR, INSTANTNOODLE_PULL_DOWN);
		instantnoodle_gpio_input(PIN_TX, INSTANTNOODLE_PULL_UP);
		instantnoodle_gpio_input(PIN_RX, INSTANTNOODLE_PULL_DOWN);
	}
	instantnoodle_gpio_output(PIN_BT_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
	if (voted_bits != 0) {
		gcc52[(GCC_VOTE & 0xfff) / 4] = rd(gcc52, GCC_VOTE) & ~voted_bits;
	}
	if (did_vote) {
		(void)instantnoodle_supply("ldoa16", 0, false);
		/*
		 * The radio's own four rails stay: they are Wi-Fi's too, and an off vote is not one among several here (it is this
		 * driver's request to RPMh, and the Wi-Fi driver's is the same requester). They go with the run.
		 */
	}
	say("bt: everything put back (vote register %#010x)\n", rd(gcc52, GCC_VOTE));
}

static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20, wifi = 0;

	(void)PE_parse_boot_argn("bt_after", &secs, sizeof(secs));
	IOSleep(secs * 1000);           /* (the system up first) */
	say("bt: bt=%u: the OnePlus 8 Pro's Bluetooth controller (the QCA6390), %s\n", level, level >= 2 ?
	    "powered on, the UART engine looked at" : "a look, reading only");
	gcc17 = (volatile uint32_t *)ml_io_map(GCC_PHYS + 0x17000, 0x1000);
	gcc52 = (volatile uint32_t *)ml_io_map(GCC_PHYS + 0x52000, 0x1000);
	se = (volatile uint32_t *)ml_io_map(SE_PHYS, SE_SIZE);
	clocks_run(true);
	show_pins();
	show_engine();
	if (level >= 2) {
		if (PE_parse_boot_argn("wifi", &wifi, sizeof(wifi)) && wifi != 0) {
			say("bt: bt=2 is refused with wifi= on: the rails and the vote register are the Wi-Fi driver's then\n");
		} else {
			if (power_up()) {
				show_engine();
			}
			power_down();
			show_pins();
		}
	}
	say("bt: done\n");
	thread_terminate(current_thread());
}

void
instantnoodlep_bt_init(void)
{
	thread_t thread;

	if (!instantnoodlep_board() || !PE_parse_boot_argn("bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 2) {
		say("bt: the driver does bt=1 (a look) and bt=2 (power on, the engine looked at) so far\n");
		level = 2;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
