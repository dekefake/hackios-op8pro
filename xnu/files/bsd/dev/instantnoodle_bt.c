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
 * host follows 300 ms later. None of that is here yet: bt=3 below gives the UART's FIFO path and asks the chip two questions.
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
 *   bt=3   ... and between the engine's second look and the putting back: the engine set up as a UART, polling only (what
 *          msm_geni_serial.c's earlycon does: the engine's root generator set to 7.3728 MHz (GPLL0 even, M/N 384/15625; the old value put
 *          back), divider 2 = 115200 baud at 32x oversampling;
 *          CTS ignored; 8N1; one byte per FIFO word), then H4 "HCI_Reset" (01 03 0c 00) sent and the answer read, then the ROME
 *          "read version" (01 00 fc 01 19, btqca.c's EDL_PATCH_VER_REQ_CMD) sent and its answer read. Every byte received is logged.
 *          VERIFIED on the phone (2026-10-27): the controller answers, `04 0e 04 01 03 0c 00` to the reset. NOTE the rates in this file are
 *          written as msm_geni_serial.c computes them (clock / divider / 32) and the line shows what they are NOT: this engine's firmware
 *          samples at 16x, so the real baud is clock / divider / 16 -- the controller answered at the "57600" entry (7.3728 MHz, divider 4)
 *          = 115200 real, and garbled at "115200" (= 230400 real). Read every "baud" label of the sweep as half of the real rate. The ROME
 *          version answer: product 0x10, patch 0x0d2b, ROM 0x0200, SoC 0x400a0200 (the controller runs ROM firmware, no patch of the 0x4509 file).
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
#define GENI_SER_S_CLK_CFG      0x04c
#define SE_GENI_STATUS_M_ACTIVE (1u << 0)
#define SE_GENI_STATUS_S_ACTIVE (1u << 12)
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL_REG  0x634
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_DMA_GENERAL_CFG      0xe30
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_MANUAL_RFR      0x2ac
#define GENI_OUTPUT_CTRL        0x024
#define M_CMD_DONE              (1u << 0)
#define UART_START_TX           1u
#define UART_START_READ         1u
#define UART_PARAM_RFR_OPEN     (1u << 7)
#define OPCODE_SHIFT            27
#define UART_CTS_MASK           (1u << 1)
#define SER_CLK_EN              (1u << 0)
#define CLK_DIV_SHFT            4
#define RX_FIFO_WC_MASK         0x1ffffffu
#define TX_FIFO_WC_MASK         0xfffffffu
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

/* ---- bt=3 ---- */

static void
wr(uint32_t offset, uint32_t value)
{
	se[offset / 4] = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
rse(uint32_t offset)
{
	return se[offset / 4];
}

/*
 * The engine's root generator is NOT in DFS mode here (first bt=3 run: DFS command 0x8020, bit 0 clear, levels all zero) and the boot
 * chain left it on the 19.2 MHz crystal, undivided, with the engine's own divider at 1 (clock cfg 0x11). 19.2 MHz gives no 115200 baud
 * (divider 5: 120000, 4 % off), so it is set to what Linux does for 7.3728 MHz (gcc-kona.c ftbl_gcc_qupv3_wrap0_s0_clk_src: GPLL0's
 * even output, 300 MHz, pre-divider 1, M/N = 384/15625): then the engine's divider 2 is 115200 x 32. Only after GPLL0 is looked at
 * (locked, even output on, post-divider 2): the PLL is Linux's and the bootloader's, it is not touched. The old value is put back.
 */
#define GPLL_MODE               0x00000         /* gcc offsets, GPLL0 (lucid): mode, L, ... user control */
#define GPLL_L                  0x00004
#define GPLL_USER_CTL           0x0000c
#define RCG_CFG                 (RCG_S6 + 4)
#define RCG_M                   (RCG_S6 + 8)
#define RCG_N                   (RCG_S6 + 0xc)
#define RCG_D                   (RCG_S6 + 0x10)
#define RCG_CFG_MASK            0x771fu         /* source 10:8, mode 13:12, divider 4:0 */
#define CMD_UPDATE              1u
static uint32_t saved_cfg, saved_m, saved_n, saved_d;
static bool rcg_changed;

static volatile uint32_t *gcc0;

static bool
rcg_update(void)
{
	gcc17[(RCG_S6 & 0xfff) / 4] = rd(gcc17, RCG_S6) | CMD_UPDATE;
	for (int i = 0; i < 100; i++) {
		if (!(rd(gcc17, RCG_S6) & CMD_UPDATE)) {
			return true;
		}
		IOSleep(1);
	}
	return false;
}

static bool
set_rate(uint32_t m, uint32_t n)
{
	uint32_t mode = rd(gcc0, GPLL_MODE), user = rd(gcc0, GPLL_USER_CTL), cfg = rd(gcc17, RCG_CFG);

	say("bt: engine 6's root generator: DFS command %#010x, cmd %#010x cfg %#010x M %#x N %#x D %#x; GPLL0: mode %#010x L %#x user control %#010x\n",
	    rd(gcc17, RCG_S6 + 0x14), rd(gcc17, RCG_S6), cfg, rd(gcc17, RCG_M), rd(gcc17, RCG_N), rd(gcc17, RCG_D), mode, rd(gcc0, GPLL_L), user);
	if (rd(gcc17, RCG_S6 + 0x14) & 1) {
		say("bt: the root generator is in DFS mode after all: stopping (this code sets the rate itself)\n");
		return false;
	}
	if (!(mode & (1u << 31)) || !(user & 2u) || ((user >> 8) & 0xf) != 1) {
		say("bt: GPLL0 is not locked with its even output on at post-divider 2 (mode %#x, user control %#x): stopping\n", mode, user);
		return false;
	}
	if (!rcg_changed) {
		saved_cfg = cfg; saved_m = rd(gcc17, RCG_M); saved_n = rd(gcc17, RCG_N); saved_d = rd(gcc17, RCG_D);
	}
	say("bt: next: engine 6's root generator to GPLL0 even / 1, M %u N %u = %u Hz (read back after; the old values are kept to put back)\n", m, n,
	    (uint32_t)(300000000ull * m / n));
	gcc17[(RCG_M & 0xfff) / 4] = m;
	gcc17[(RCG_N & 0xfff) / 4] = ~(n - m) & 0xffffu;
	gcc17[(RCG_D & 0xfff) / 4] = ~n & 0xffffu;
	gcc17[(RCG_CFG & 0xfff) / 4] = (cfg & ~RCG_CFG_MASK) | (6u << 8) | (2u << 12) | 1u;
	rcg_changed = true;
	if (!rcg_update()) {
		say("bt: the root generator did not take the update in 100 ms (cmd %#010x)\n", rd(gcc17, RCG_S6));
		return false;
	}
	say("bt: root generator now cmd %#010x cfg %#010x M %#x N %#x D %#x\n", rd(gcc17, RCG_S6), rd(gcc17, RCG_CFG), rd(gcc17, RCG_M),
	    rd(gcc17, RCG_N), rd(gcc17, RCG_D));
	return true;
}

static void
restore_rate(void)
{
	if (rcg_changed) {
		gcc17[(RCG_M & 0xfff) / 4] = saved_m;
		gcc17[(RCG_N & 0xfff) / 4] = saved_n;
		gcc17[(RCG_D & 0xfff) / 4] = saved_d;
		gcc17[(RCG_CFG & 0xfff) / 4] = saved_cfg;
		say("bt: root generator put back: %s (cmd %#010x cfg %#010x)\n", rcg_update() ? "updated" : "update timed out", rd(gcc17, RCG_S6),
		    rd(gcc17, RCG_CFG));
		rcg_changed = false;
	}
}

static bool uart_baud(uint32_t baud, uint32_t m, uint32_t n, uint32_t div);

/* geni_se_init + geni_se_select_mode(FIFO) + the earlycon's register writes (msm_geni_serial_earlycon_setup), 115200 8N1 */
static bool
uart_setup(void)
{
	uint32_t cgc = rse(GENI_CGC_CTRL);

	if (rse(SE_GENI_STATUS) & (SE_GENI_STATUS_M_ACTIVE | SE_GENI_STATUS_S_ACTIVE)) {
		say("bt: a command is active in the engine (status %#010x): stopping\n", rse(SE_GENI_STATUS));
		return false;
	}
	say("bt: next: the engine's registers written as geni_se_init and the earlycon do (first write read back: GENI_CGC_CTRL %#x -> %#x)\n", cgc, cgc | 0x7f);
	wr(GENI_CGC_CTRL, cgc | 0x7f);
	if ((rse(GENI_CGC_CTRL) & 0x7f) != 0x7f) {
		say("bt: GENI_CGC_CTRL reads back %#x: the engine does not take writes, stopping\n", rse(GENI_CGC_CTRL));
		return false;
	}
	/*
	 * The engine's serial clock first (its clock cfg reads 0 after an earlier run of this code put it back; the 3rd bt=3 run went to
	 * CrashDump at the writes that followed the CGC one with that 0: the first two runs found it 0x11 and went through).
	 */
	say("bt: step: the serial clock (115200, root generator and divider)\n");
	if (!uart_baud(115200, 384, 15625, 2)) {
		return false;
	}
	say("bt: step: DMA_GENERAL_CFG, output control, force default\n");
	wr(SE_DMA_GENERAL_CFG, rse(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT_REG, 1);
	say("bt: step: DMA mode off, watermarks, interrupt clears\n");
	wr(SE_GENI_DMA_MODE_EN, rse(SE_GENI_DMA_MODE_EN) & ~1u);
	wr(SE_GSI_EVENT_EN, 0);
	wr(SE_GENI_RX_WATERMARK, 8);
	wr(SE_GENI_RX_RFR_WATERMARK, 14);
	/* select FIFO mode: clear the pending bits (polling needs none enabled) */
	wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	say("bt: step: packing and the UART's own registers\n");
	/* packing: 8 bits, one word per FIFO element: se_get_packing_config(8, 1, false) = (0 << 5) | (7 << 1), | 1 on the last = 0xf, cfg1 0 */
	wr(SE_GENI_TX_PACKING_CFG0, 0xf);
	wr(SE_GENI_TX_PACKING_CFG1, 0);
	wr(SE_GENI_RX_PACKING_CFG0, 0xf);
	wr(SE_GENI_RX_PACKING_CFG1, 0);
	wr(SE_UART_LOOPBACK_CFG, 0);
	wr(SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	wr(SE_UART_TX_PARITY_CFG, 0);
	wr(SE_UART_RX_TRANS_CFG, 0);
	wr(SE_UART_RX_PARITY_CFG, 0);
	wr(SE_UART_TX_WORD_LEN, 8);
	wr(SE_UART_RX_WORD_LEN, 8);
	wr(SE_UART_TX_STOP_BIT_LEN, 0);
	wr(SE_UART_RX_STALE_CNT, 0x18);
	say("bt: engine set up (the clock not yet): tx cfg %#x, word len %u, packing %#x, status %#010x\n", rse(SE_UART_TX_TRANS_CFG),
	    rse(SE_UART_TX_WORD_LEN), rse(SE_GENI_TX_PACKING_CFG0), rse(SE_GENI_STATUS));
	return true;
}

/* the engine's clock: root generator to m/n of GPLL0 even, the engine's own divider div; baud = 300 MHz * m / n / div / 32 */
static bool
uart_baud(uint32_t baud, uint32_t m, uint32_t n, uint32_t div)
{
	if (!set_rate(m, n)) {
		return false;
	}
	wr(GENI_SER_M_CLK_CFG, SER_CLK_EN | (div << CLK_DIV_SHFT));
	wr(GENI_SER_S_CLK_CFG, SER_CLK_EN | (div << CLK_DIV_SHFT));
	say("bt: %u baud: engine clock cfg %#x/%#x\n", baud, rse(GENI_SER_M_CLK_CFG), rse(GENI_SER_S_CLK_CFG));
	return true;
}

static void
uart_rx_start(void)
{
	wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_GENI_S_CMD0, (UART_START_READ << OPCODE_SHIFT) | UART_PARAM_RFR_OPEN);
}

static bool
uart_tx(const uint8_t *bytes, uint32_t n)
{
	say("bt: TX %u bytes:", n);
	for (uint32_t i = 0; i < n; i++) {
		say(" %02x", bytes[i]);
	}
	say("\n");
	wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE_UART_TX_TRANS_LEN, n);
	wr(SE_GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);
	for (uint32_t i = 0; i < n; i++) {
		int t;

		for (t = 0; t < 100 && (rse(SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MASK) >= 14; t++) {
			IOSleep(1);
		}
		wr(SE_GENI_TX_FIFOn, bytes[i]);
	}
	for (int t = 0; t < 200; t++) {
		if (rse(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE) {
			wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE);
			say("bt: TX done\n");
			return true;
		}
		IOSleep(1);
	}
	say("bt: TX did not finish in 200 ms (M irq %#010x, status %#010x, tx fifo %#x): cancelling\n", rse(SE_GENI_M_IRQ_STATUS),
	    rse(SE_GENI_STATUS), rse(SE_GENI_TX_FIFO_STATUS));
	wr(SE_GENI_M_CMD_CTRL_REG, 4);          /* M_GENI_CMD_CANCEL */
	IOSleep(10);
	wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	return false;
}

static uint8_t rxbuf[64];

/* true: the last uart_rx() saw an HCI event header (04 0e: command complete) */
static bool
rx_has_event(uint32_t got)
{
	for (uint32_t i = 0; i + 1 < got && i + 1 < sizeof(rxbuf); i++) {
		if (rxbuf[i] == 0x04 && (rxbuf[i + 1] == 0x0e || rxbuf[i + 1] == 0xff)) {
			return true;
		}
	}
	return false;
}

/* read for ms milliseconds, log every byte; returns how many */
static uint32_t
uart_rx(uint32_t ms)
{
	uint32_t got = 0;

	for (uint32_t t = 0; t < ms; t++) {
		uint32_t st = rse(SE_GENI_RX_FIFO_STATUS), wc = st & RX_FIFO_WC_MASK;

		if (wc != 0) {
			say("bt: RX (fifo status %#010x):", st);
			for (uint32_t i = 0; i < wc && i < 32; i++) {
				uint8_t b = rse(SE_GENI_RX_FIFOn) & 0xff;

				say(" %02x", b);
				if (got < sizeof(rxbuf)) {
					rxbuf[got] = b;
				}
				got++;
			}
			say("\n");
		}
		IOSleep(1);
	}
	if (got == 0) {
		say("bt: RX nothing in %u ms (status %#010x, S irq %#010x, rx fifo %#010x)\n", ms, rse(SE_GENI_STATUS), rse(SE_GENI_S_IRQ_STATUS),
		    rse(SE_GENI_RX_FIFO_STATUS));
	}
	return got;
}

static void
uart_stop(void)
{
	if (rse(SE_GENI_STATUS) & SE_GENI_STATUS_S_ACTIVE) {
		wr(SE_GENI_S_CMD_CTRL_REG, 4);  /* S_GENI_CMD_CANCEL */
		IOSleep(10);
		wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	}
	wr(GENI_SER_M_CLK_CFG, 0);
	wr(GENI_SER_S_CLK_CFG, 0);
}

static void
uart_drain(void)
{
	for (int i = 0; i < 64 && (rse(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK) != 0; i++) {
		(void)rse(SE_GENI_RX_FIFOn);
	}
}

/*
 * The controller's baud rate after power on is not known to this file (hci_qca.c starts at 115200 and changes to 3 Mbaud after the
 * firmware; the ROM may differ), so HCI_Reset is tried at each rate the clock table gives exactly (300 MHz * M/N / divider / 32) and a
 * reply that holds an HCI event (04 0e) ends the sweep; then the ROME version request is sent at that rate.
 */
static void
hci_talk(void)
{
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	static const struct { uint32_t baud, m, n, div; } rates[] = {
		{ 115200, 384, 15625, 2 }, { 3000000, 8, 25, 1 }, { 921600, 1536, 15625, 1 }, { 460800, 768, 15625, 1 }, { 2000000, 16, 75, 1 },
		{ 3200000, 128, 375, 1 }, { 230400, 384, 15625, 1 }, { 57600, 384, 15625, 4 }, { 38400, 384, 15625, 6 }, { 9600, 384, 15625, 24 },
	};
	bool found = false;

	if (!uart_setup()) {
		restore_rate();
		return;
	}
	say("bt: bt=3: HCI_Reset is sent at several baud rates, then the ROME version request at the one that answers\n");
	uart_rx_start();
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]) && !found; i++) {
		if (!uart_baud(rates[i].baud, rates[i].m, rates[i].n, rates[i].div)) {
			break;
		}
		IOSleep(5);
		uart_drain();
		if (uart_tx(reset, sizeof(reset))) {
			found = rx_has_event(uart_rx(150));
		}
		if (found) {
			say("bt: the controller answered at %u baud\n", rates[i].baud);
			if (uart_tx(version, sizeof(version))) {
				uart_rx(300);
			}
		}
	}
	if (!found) {
		say("bt: no clean answer at any rate\n");
	}
	uart_stop();
	restore_rate();
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
	gcc0 = (volatile uint32_t *)ml_io_map(GCC_PHYS, 0x1000);
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
				if (level >= 3) {
					hci_talk();
					show_engine();
				}
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
	if (level > 3) {
		say("bt: the driver does bt=1 (a look), bt=2 (power on, the engine looked at) and bt=3 (HCI_Reset and the version request) so far\n");
		level = 3;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
