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
 *   bt=4   ... bt=3's HCI at 115200 real, then the firmware downloaded as btqca.c does it: the version, the patch (848 segments of 243
 *          bytes, the 8 ms between them because the engine ignores CTS; only the last one acknowledged, download mode 3), 10 ms, the NVM
 *          (24 segments, each acknowledged), then -- as btqca.c does, and it matters -- HCI_Reset AT THE DOWNLOAD'S RATE (it makes the
 *          controller run the patch and take the NVM's baud rate, 3.2 Mbaud: tag 17's byte 1 is 0x11, the code of 3200000), and only then
 *          the host to 3.2 Mbaud; Qualcomm's in-band sleep (hci_qca.c: 0xfd wake-up, 0xfc acknowledgement) answered. VERIFIED on the
 *          phone (2026-10-27): the version after reads patch 0x4509 (before: 0x0d2b), HCI_Reset and Read_Local_Version answer (HCI 5.2,
 *          manufacturer 0x1d = Qualcomm). Without the reset at the old rate the controller waited at 115200 while the host spoke 3.2 M
 *          and answered a wake-up with a firmware crash report (04 ff fd 01 08 ...); wrong-rate bytes make it flood. The firmware is the
 *          phone's own and is never in this repository: the kernel build (op8pro-data/docker/rebuild-bt.sh) turns op8pro-data/bt-fw/
 *          htbtfw20.tlv and htnv20.bin into a generated header, instantnoodle_btfw.h, that only the build's copy of this directory has;
 *          without it bt=4 says so and stops. Boot-args for experiments: bt_nvm=1 (set the NVM's baud byte 2 to 0), bt_skip=1|2|3 (no
 *          patch / no NVM), bt_cts=1 (the engine honours the controller's RTS).
 *   bt=5   ... bt=4, and then the controller stays: a worker thread owns the engine and /dev/hk-bt (one H4 packet a read, one a write)
 *          takes HCI to and from the controller, in-band sleep handled; a self-test (reset, versions, buffer sizes, a hundred long events)
 *          runs through the same calls the node makes. Nothing is put back at the end: the controller stays powered for the run.
 *   bt=6   ... bt=5, and the kernel control "com.apple.uart.bluetooth" registered: it is how BlueTool's UART transport reaches a controller;
 *          while BlueTool is connected Broadcom's vendor commands are answered here and the rest goes to the Qualcomm controller (see ctl_*).
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
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/systm.h>
#include <sys/uio.h>
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
	/*
	 * A command left running by the previous run (its receive command: status 0x00041000 at the start of the fourth run, the engine's
	 * clock config 0): cancelled, then aborted if it stays -- with the clock on, which is why this comes after it.
	 */
	for (int tries = 0; tries < 2 && (rse(SE_GENI_STATUS) & (SE_GENI_STATUS_M_ACTIVE | SE_GENI_STATUS_S_ACTIVE)); tries++) {
		say("bt: a command is active in the engine (status %#010x): %s\n", rse(SE_GENI_STATUS), tries == 0 ? "cancelled" : "aborted");
		wr(SE_GENI_M_CMD_CTRL_REG, tries == 0 ? 4 : 2);
		wr(SE_GENI_S_CMD_CTRL_REG, tries == 0 ? 4 : 2);
		IOSleep(20);
		wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
		wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	}
	if (rse(SE_GENI_STATUS) & (SE_GENI_STATUS_M_ACTIVE | SE_GENI_STATUS_S_ACTIVE)) {
		say("bt: the command stays active (status %#010x): stopping\n", rse(SE_GENI_STATUS));
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
	{
		uint32_t cts = level >= 5;      /* (bt=5, the transport: the controller's flow control is honoured) */

		(void)PE_parse_boot_argn("bt_cts", &cts, sizeof(cts));
		wr(SE_UART_TX_TRANS_CFG, cts != 0 ? 0 : UART_CTS_MASK);     /* bt_cts=1: the controller's RTS (our CTS pin) holds our transmit back */
	}
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

/* ---- bt=4 ---- */

#if __has_include("instantnoodle_btfw.h")
#include "instantnoodle_btfw.h"         /* generated by the kernel build from the phone's own firmware files: btfw_patch[], btfw_nvm[] */
#define HAVE_BTFW 1
#endif

#define MAX_SEG                 243     /* btqca.h MAX_SIZE_PER_TLV_SEGMENT */
static uint8_t evbuf[300];              /* the last event's bytes: 04 evt len params... */

static bool send_raw(const uint8_t *pkt, uint32_t n);

/* one HCI event from the controller within ms milliseconds: its length in evbuf (0: none); everything before the 04 is dropped */
static uint32_t
rx_event(uint32_t ms)
{
	uint32_t have = 0;

	for (uint32_t t = 0; t < ms; t++) {
		uint32_t wc = rse(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;

		while (wc-- != 0) {
			uint8_t b = rse(SE_GENI_RX_FIFOn) & 0xff;

			if (have == 0 && b != 0x04) {
				if (b == 0xfd) {        /* the controller's in-band wake-up: answered */
					static const uint8_t ack = 0xfc;

					send_raw(&ack, 1);
				}
				continue;
			}
			if (have < sizeof(evbuf)) {
				evbuf[have++] = b;
			}
		}
		if (have >= 3 && have >= 3u + evbuf[2]) {
			return have;
		}
		IOSleep(1);
	}
	return have;
}

static void
show_event(const char *what, uint32_t n)
{
	say("bt: %s: event of %u bytes%s\n", what, n, n == 0 ? " (none)" : "");
	for (uint32_t i = 0; i < n && i < 300; i += 32) {
		say("bt:   %03x:", i);
		for (uint32_t k = i; k < i + 32 && k < n; k++) {
			say(" %02x", evbuf[k]);
		}
		say("\n");
	}
}

/* bytes onto the wire, quietly (a TX stall is reported) */
static bool
send_raw(const uint8_t *pkt, uint32_t n)
{
	wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SE_UART_TX_TRANS_LEN, n);
	wr(SE_GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);
	for (uint32_t i = 0; i < n; i++) {
		for (int t = 0; t < 3000 && (rse(SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MASK) >= 14; t++) {
			IOSleep(1);
		}
		wr(SE_GENI_TX_FIFOn, pkt[i]);
	}
	for (int t = 0; t < 3000; t++) {
		if (rse(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE) {
			wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE);
			return true;
		}
		IOSleep(1);
	}
	say("bt: TX stalled (M irq %#010x, status %#010x, tx fifo %#x, CTS pin %u): cancelling\n", rse(SE_GENI_M_IRQ_STATUS), rse(SE_GENI_STATUS),
	    rse(SE_GENI_TX_FIFO_STATUS), instantnoodle_gpio_level(PIN_CTS) ? 1u : 0u);
	wr(SE_GENI_M_CMD_CTRL_REG, 4);
	IOSleep(10);
	wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	return false;
}

static bool ibs_on;
static bool kept;               /* bt=5: the controller, the engine and its clocks stay as they are at the end of the thread */

/*
 * In-band sleep, the host's side (hci_qca.c qca_enqueue/HCI_IBS_TX_WAKING): before data goes to a controller that may be asleep, the host
 * sends 0xfd (WAKE_IND) and waits for 0xfc (WAKE_ACK) -- up to 300 ms here, asking again after 100 -- and only then the packet. A 0xfd
 * from the controller meanwhile is answered with 0xfc.
 */
static bool
ibs_wake(void)
{
	static const uint8_t wake = 0xfd, ack = 0xfc;

	for (int ask = 0; ask < 3; ask++) {
		send_raw(&wake, 1);
		for (int t = 0; t < 100; t++) {
			uint32_t wc = rse(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;

			while (wc-- != 0) {
				uint8_t b = rse(SE_GENI_RX_FIFOn) & 0xff;

				if (b == 0xfc) {
					return true;
				}
				if (b == 0xfd) {
					send_raw(&ack, 1);
					return true;
				}
			}
			IOSleep(1);
		}
	}
	say("bt: the controller does not answer a wake-up (0xfd) with 0xfc\n");
	return false;
}

/* a command packet 01 opcode len params, quietly */
static bool
send_cmd(uint16_t opcode, const uint8_t *params, uint32_t len)
{
	uint8_t pkt[4 + 255];

	pkt[0] = 0x01;
	pkt[1] = opcode & 0xff;
	pkt[2] = opcode >> 8;
	pkt[3] = len;
	memcpy(pkt + 4, params, len);
	if (ibs_on) {
		ibs_wake();
	}
	return send_raw(pkt, 4 + len);
}

/* the ROME version (btqca.c: 0xfc00 0x19; this controller's answer is a command complete: status, 19, length, product, patch, ROM, SoC) */
static bool
read_version(const char *when)
{
	static const uint8_t req = 0x19;
	uint32_t n;

	uart_drain();
	if (!send_cmd(0xfc00, &req, 1)) {
		return false;
	}
	n = rx_event(500);
	if (n < 3 + 4 + 2 + 12 || evbuf[1] != 0x0e || evbuf[7] != 0x19) {
		show_event("the version request's answer is not what was seen before", n);
		return false;
	}
	say("bt: %s: product %#x, patch %#x, ROM %#x, SoC %#010x\n", when, evbuf[9] | evbuf[10] << 8, evbuf[13] | evbuf[14] << 8,
	    evbuf[15] | evbuf[16] << 8, (uint32_t)evbuf[17] | evbuf[18] << 8 | (uint32_t)evbuf[19] << 16 | (uint32_t)evbuf[20] << 24);
	return true;
}

static bool
hci_simple(uint16_t opcode, const char *what)
{
	uint32_t n;

	uart_drain();
	if (!send_cmd(opcode, NULL, 0)) {
		return false;
	}
	n = rx_event(1000);
	show_event(what, n);
	if (n >= 3 && evbuf[1] == 0xff) {
		/* a vendor event instead of the answer: what follows it, as the events come */
		for (int more = 0; more < 3; more++) {
			uint32_t m = rx_event(300);

			show_event("the next event", m);
			if (m == 0) {
				break;
			}
		}
	}
	return n >= 7 && evbuf[1] == 0x0e && evbuf[6] == 0;
}

#ifdef HAVE_BTFW
/*
 * btqca.c's qca_download_firmware: the file whole in segments of 243 bytes, each as 0xfc00 [0x1e, length, data]. A patch file's
 * download mode (byte 14: 3 here) says the controller acknowledges only the last segment; the NVM file's every segment is acknowledged.
 */
static bool
download(const char *what, const uint8_t *file, uint32_t size, bool acked_each, uint32_t pause_ms)
{
	uint32_t sent = 0, segments = 0, anomalies = 0;

	say("bt: next: %s, %u bytes, in segments of %u (%s)\n", what, size, MAX_SEG, acked_each ? "each acknowledged" : "only the last acknowledged");
	while (sent < size) {
		uint8_t seg[MAX_SEG + 2];
		uint32_t n = size - sent < MAX_SEG ? size - sent : MAX_SEG;
		bool last = sent + n == size;

		seg[0] = 0x1e;
		seg[1] = n;
		memcpy(seg + 2, file + sent, n);
		if (!send_cmd(0xfc00, seg, n + 2)) {
			say("bt: %s: the transmit stopped at byte %u (segment %u)\n", what, sent, segments);
			return false;
		}
		sent += n;
		segments++;
		if (acked_each || last) {
			uint32_t e = rx_event(last ? 3000 : 1000);

			if (e == 0) {
				say("bt: %s: no answer to segment %u (byte %u of %u)\n", what, segments, sent, size);
				return false;
			}
			if (segments <= 2 || last || anomalies < 3) {
				/* the first, the last, and a few more for the log */
				if (segments <= 2 || last) {
					show_event(last ? "the last segment's answer" : "an early segment's answer", e);
				}
			}
			/*
			 * A command complete's status (04 0e 05 01 00 fc STATUS 1e: the 1e is the request type echoed, seen on the last patch
			 * segment's answer) nonzero is a failure; a vendor event is only logged
			 */
			if (evbuf[1] == 0x0e && e >= 7 && evbuf[6] != 0) {
				anomalies++;
				show_event("a segment's answer with a nonzero end", e);
				if (anomalies > 3) {
					return false;
				}
			}
		} else if (pause_ms != 0) {
			IOSleep(pause_ms);
		}
	}
	say("bt: %s: %u segments sent, %u anomalies\n", what, segments, anomalies);
	return anomalies == 0;
}

/* listen for ms, answering every 0xfd with 0xfc (hci_qca.c IBS); one summary line: counts and the first other bytes */
static void
listen_ack(uint32_t ms, const char *when)
{
	static const uint8_t ack = 0xfc;
	uint32_t fds = 0, others = 0;
	uint8_t first[24];

	for (uint32_t t = 0; t < ms; t++) {
		uint32_t wc = rse(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK, fd_now = 0;

		while (wc-- != 0) {
			uint8_t b = rse(SE_GENI_RX_FIFOn) & 0xff;

			if (b == 0xfd) {
				fd_now++;
			} else {
				if (others < sizeof(first)) {
					first[others] = b;
				}
				others++;
			}
		}
		if (fd_now != 0) {
			fds += fd_now;
			send_raw(&ack, 1);
		}
		IOSleep(1);
	}
	say("bt: %s (%u ms): %u x 0xfd answered, %u other bytes:", when, ms, fds, others);
	for (uint32_t k = 0; k < others && k < sizeof(first); k++) {
		say(" %02x", first[k]);
	}
	say("; CTS %u, sw-ctrl %u\n", instantnoodle_gpio_level(PIN_CTS) ? 1u : 0u, instantnoodle_gpio_level(PIN_SW_CTRL) ? 1u : 0u);
}

static uint8_t nvm_copy[sizeof(btfw_nvm)];

static bool
firmware(void)
{
	/* the patch's download mode (byte 14 of the file): 3 = only the last segment is acknowledged (btqca.c qca_tlv_check_data) */
	bool patch_acks = btfw_patch[14] == 0 || btfw_patch[14] == 2;
	uint32_t skip = 0, idx = 0, length = (btfw_nvm[1] | btfw_nvm[2] << 8 | (uint32_t)btfw_nvm[3] << 16);

	say("bt: bt=4: the firmware: patch %u bytes (type %u, download mode %u, product %#x, ROM build %#x, patch version %#x), NVM %u bytes (type %u)\n",
	    (unsigned)sizeof(btfw_patch), btfw_patch[0], btfw_patch[14], btfw_patch[16] | btfw_patch[17] << 8, btfw_patch[18] | btfw_patch[19] << 8,
	    btfw_patch[20] | btfw_patch[21] << 8, (unsigned)sizeof(btfw_nvm), btfw_nvm[0]);
	/* the NVM: tag 17 (HCI transport) byte 2 is the baud rate code: 0 = 115200 (btqca.c sets the operating speed there); its sleep bits stay as the file has them */
	memcpy(nvm_copy, btfw_nvm, sizeof(nvm_copy));
	while (idx < length && 4 + idx + 12 <= sizeof(nvm_copy)) {
		uint8_t *tag = nvm_copy + 4 + idx;
		uint32_t id = tag[0] | tag[1] << 8, tlen = tag[2] | tag[3] << 8;

		if (id == 17) {
			uint32_t edit = 0;

			(void)PE_parse_boot_argn("bt_nvm", &edit, sizeof(edit));
			say("bt: NVM tag 17 (HCI transport): length %u, data %02x %02x %02x; %s\n", tlen, tag[12], tag[13], tag[14], edit != 0 ?
			    "byte 2 (btqca.c's baud code) set to 0 (bt_nvm=1)" : "left as the file has it (bt_nvm=1 sets byte 2 to 0, the old ROME code for 115200)");
			if (edit != 0) {
				tag[14] = 0;
			}
		}
		idx += 12 + tlen;
	}
	uart_drain();
	{
		say("bt: CTS pin (the controller's RTS) reads %u; the engine ignores it, so the unacknowledged patch segments are 8 ms apart\n",
		    instantnoodle_gpio_level(PIN_CTS) ? 1u : 0u);
	}
	if (!read_version("before the firmware")) {
		return false;
	}
	(void)PE_parse_boot_argn("bt_skip", &skip, sizeof(skip));
	{
		if (skip & 1) {
			say("bt: bt_skip=%u: the patch is not downloaded\n", skip);
		} else if (!download("the patch", btfw_patch, sizeof(btfw_patch), patch_acks, 8)) {
			return false;
		}
		IOSleep(10);
		if (skip & 2) {
			say("bt: bt_skip=%u: the NVM is not downloaded\n", skip);
		} else if (!download("the NVM", nvm_copy, sizeof(nvm_copy), true, 0)) {
			return false;
		}
	}
	/*
	 * btqca.c's order: the HCI_Reset right after the NVM, at the rate the download used (it is what makes the controller run the patch
	 * and take the NVM's baud rate, 3.2 Mbaud: tag 17's byte 1 is 0x11, the code of 3200000); only then does the host change its rate.
	 * Earlier runs of this file skipped that reset and talked at 3.2 Mbaud to a controller still waiting at 115200: wake-up bytes and the
	 * 8 s of silence, then a firmware crash report (04 ff fd 01 08 ...). Wake-ups (0xfd) are answered with 0xfc throughout.
	 */
	uart_drain();
	{
		static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };

		say("bt: next: HCI_Reset at the download's rate, as btqca.c does after the NVM\n");
		send_raw(reset, sizeof(reset));
		listen_ack(600, "after the reset, at the download's rate");
	}
	if (!(skip & 2)) {
		say("bt: next: the host to 3.2 Mbaud\n");
		if (!uart_baud(3200000, 64, 375, 1)) {
			return false;
		}
		uart_drain();
		listen_ack(3000, "at 3.2 Mbaud");
	}
	ibs_on = true;
	if (!hci_simple(0x0c03, "HCI_Reset")) {
		return false;
	}
	IOSleep(100);
	read_version("after the firmware");
	hci_simple(0x1001, "HCI_Read_Local_Version_Information");
	hci_simple(0x1009, "HCI_Read_BD_ADDR (the address is the controller's own: not recorded)");
	return true;
}
#endif

/* ---- bt=5: the controller as a packet device (/dev/hk-bt) ------------------------------------------------ */

#ifdef HAVE_BTFW
/*
 * What the rest of iOS is to get from this file: HCI packets in the H4 framing (a type byte, then the packet: 01 command, 02 ACL data
 * from and to the host, 03 SCO data, 04 event), whole, one per read() and one per write() of /dev/hk-bt. Underneath, one worker
 * thread owns the engine: it polls the receive FIFO, cuts the byte stream into packets (the in-band sleep bytes 0xfc/0xfd/0xfe are
 * the controller's own signalling and are taken out between packets only, as hci_qca.c does), answers a wake-up (0xfd) with 0xfc,
 * and sends what the host wrote -- after a wake-up of its own (0xfd, its 0xfc) the first time. The host never sends 0xfe (sleep), so the
 * controller's receiver stays awake. The engine's receive side runs with the controller's flow control (RFR follows the FIFO's
 * watermark: no "RFR open"), because a poll every millisecond cannot keep up with 3.2 Mbaud by itself (16 bytes fill the FIFO in 50 us).
 *
 * The packets between the worker and the readers are in two rings of 16-bit length + bytes; one producer, one consumer each.
 */
#define RX_RING                 (32 * 1024)
#define TX_RING                 (16 * 1024)
#define MAX_PACKET              1200
static uint8_t rx_ring[RX_RING], tx_ring[TX_RING];
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;    /* head: written by the producer, tail: by the consumer */
static bool awake, got_wake_ack, node_open;
static uint32_t stat_packets, stat_bad, stat_tx, stat_wakes;

static inline uint32_t
ld(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void
st(volatile uint32_t *p, uint32_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static bool
ring_put(uint8_t *ring, uint32_t size, volatile uint32_t *head, volatile uint32_t *tail, const uint8_t *data, uint32_t len)
{
	uint32_t h = *head, t = ld(tail), used = (h - t + size) % size;

	if (used + len + 2 >= size) {
		return false;
	}
	ring[h] = len & 0xff;
	ring[(h + 1) % size] = len >> 8;
	for (uint32_t i = 0; i < len; i++) {
		ring[(h + 2 + i) % size] = data[i];
	}
	st(head, (h + 2 + len) % size);
	return true;
}

/* the next packet's length (0: none), and, with buf, its bytes taken */
static uint32_t
ring_get(uint8_t *ring, uint32_t size, volatile uint32_t *head, volatile uint32_t *tail, uint8_t *buf, uint32_t cap)
{
	uint32_t t = *tail, h = ld(head), len;

	if (t == h) {
		return 0;
	}
	len = ring[t] | ring[(t + 1) % size] << 8;
	if (buf == NULL) {
		return len;
	}
	for (uint32_t i = 0; i < len && i < cap; i++) {
		buf[i] = ring[(t + 2 + i) % size];
	}
	st(tail, (t + 2 + len) % size);
	return len < cap ? len : cap;
}

/*
 * bt=6: the kernel control "com.apple.uart.bluetooth" that BlueTool's UART transport connects to (see ctl_* below). While BlueTool is
 * connected the controller's packets go to its socket instead of the ring; the fake answers to Broadcom's commands (fk_ring) are
 * queued by the socket's send callback and delivered by the worker, because the socket may not be re-entered from its own callback.
 */
static kern_ctl_ref ctl_ref;
static volatile uint32_t ctl_unit_no;
static volatile bool ctl_connected;
static uint8_t fk_ring[4096];
static volatile uint32_t fk_head, fk_tail;
static uint32_t stat_ctl_drop;

static bool ring_put(uint8_t *ring, uint32_t size, volatile uint32_t *head, volatile uint32_t *tail, const uint8_t *data, uint32_t len);

/* a packet from the controller (or a fake one) to whoever has the transport: BlueTool's socket, else the /dev/hk-bt ring */
static bool
deliver(const uint8_t *pkt, uint32_t len)
{
	if (ctl_connected) {
		if (ctl_enqueuedata(ctl_ref, ctl_unit_no, (void *)(uintptr_t)pkt, len, 0) != 0) {
			stat_ctl_drop++;
			return false;
		}
		return true;
	}
	return ring_put(rx_ring, RX_RING, &rx_head, &rx_tail, pkt, len);
}

/* the receive state machine: bytes in, whole packets out */
static struct {
	uint8_t pkt[MAX_PACKET + 8];
	uint32_t have, need;            /* need: bytes still to come for the header, then the body */
	int kind;                       /* 0 between packets, 4 event, 2 ACL, 3 SCO */
	bool in_body;
} rxs;

static void
rx_byte(uint8_t b)
{
	if (rxs.kind == 0) {
		static const uint8_t ack = 0xfc;

		switch (b) {
		case 0xfc:
			got_wake_ack = true;
			return;
		case 0xfd:                      /* the controller wants to send: answered */
			send_raw(&ack, 1);
			stat_wakes++;
			return;
		case 0xfe:                      /* its sleep indication: nothing to answer */
			return;
		case 0x04: rxs.need = 2; break;
		case 0x02: rxs.need = 4; break;
		case 0x03: rxs.need = 3; break;
		default:
			stat_bad++;
			return;
		}
		rxs.kind = b;
		rxs.pkt[0] = b;
		rxs.have = 1;
		rxs.in_body = false;
		return;
	}
	rxs.pkt[rxs.have++] = b;
	if (--rxs.need != 0) {
		return;
	}
	if (!rxs.in_body) {
		uint32_t body = rxs.kind == 4 ? rxs.pkt[2] : rxs.kind == 2 ? (rxs.pkt[3] | rxs.pkt[4] << 8) : rxs.pkt[3];

		if (rxs.have + body > MAX_PACKET) {
			stat_bad++;
			rxs.kind = 0;
			return;
		}
		rxs.in_body = true;
		rxs.need = body;
		if (body != 0) {
			return;
		}
	}
	if (deliver(rxs.pkt, rxs.have)) {
		stat_packets++;
	} else {
		stat_bad++;
	}
	rxs.kind = 0;
}

static void
rx_poll(void)
{
	uint32_t wc = rse(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;

	while (wc-- != 0) {
		rx_byte(rse(SE_GENI_RX_FIFOn) & 0xff);
	}
}

static void
tx_one(void)
{
	static uint8_t pkt[MAX_PACKET + 8];
	uint32_t len = ring_get(tx_ring, TX_RING, &tx_head, &tx_tail, pkt, sizeof(pkt));

	if (len == 0) {
		return;
	}
	if (!awake) {
		static const uint8_t wake = 0xfd;

		for (int ask = 0; ask < 3 && !got_wake_ack; ask++) {
			send_raw(&wake, 1);
			for (int t = 0; t < 100 && !got_wake_ack; t++) {
				rx_poll();
				IOSleep(1);
			}
		}
		if (!got_wake_ack) {
			say("bt: the controller does not answer a wake-up: the packet goes out anyway\n");
		}
		awake = true;
	}
	if (send_raw(pkt, len)) {
		stat_tx++;
	}
}

static void
worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t before = stat_packets + stat_tx;

		rx_poll();
		if (ring_get(tx_ring, TX_RING, &tx_head, &tx_tail, NULL, 0) != 0) {
			tx_one();
		}
		if (ring_get(fk_ring, sizeof(fk_ring), &fk_head, &fk_tail, NULL, 0) != 0) {
			static uint8_t fake[300];
			uint32_t n = ring_get(fk_ring, sizeof(fk_ring), &fk_head, &fk_tail, fake, sizeof(fake));

			if (deliver(fake, n)) {
				stat_packets++;
			}
		}
		if (stat_packets + stat_tx == before) {
			IOSleep(1);
		}
	}
}

/* the host's side (here and in /dev/hk-bt): false: the ring is full or the packet is not a command or ACL/SCO data */
static bool
hkbt_write_packet(const uint8_t *pkt, uint32_t len)
{
	uint32_t want;

	if (len < 4 || len > MAX_PACKET) {
		return false;
	}
	switch (pkt[0]) {
	case 0x01: want = 4 + pkt[3]; break;
	case 0x02: want = 5 + (pkt[3] | pkt[4] << 8); break;
	case 0x03: want = 4 + pkt[3]; break;
	default: return false;
	}
	return want == len && ring_put(tx_ring, TX_RING, &tx_head, &tx_tail, pkt, len);
}

/* a packet within ms milliseconds: its length (0: none), bytes in buf (up to cap) */
static uint32_t
hkbt_read_packet(uint8_t *buf, uint32_t cap, uint32_t ms)
{
	for (uint32_t t = 0; t <= ms; t++) {
		uint32_t n = ring_get(rx_ring, RX_RING, &rx_head, &rx_tail, buf, cap);

		if (n != 0) {
			return n;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return 0;
}

static int
bt_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	static uint8_t scrap[MAX_PACKET + 8];

	if (node_open) {
		return EBUSY;
	}
	node_open = true;
	while (hkbt_read_packet(scrap, sizeof(scrap), 0) != 0) {       /* (what came before this open is not for it) */
	}
	return 0;
}

static int
bt_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	node_open = false;
	return 0;
}

/* one packet a read; the answer is 0 bytes when none came in two seconds (the caller asks again) */
static int
bt_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint8_t pkt[MAX_PACKET + 8];
	uint32_t n = hkbt_read_packet(pkt, sizeof(pkt), 2000);

	if (n == 0) {
		return 0;
	}
	return uiomove((caddr_t)pkt, (int)MIN((user_ssize_t)n, uio_resid(uio)), uio);
}

static int
bt_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t pkt[MAX_PACKET + 8];
	user_ssize_t n = uio_resid(uio);
	int error;

	if (n < 4 || n > MAX_PACKET) {
		return EINVAL;
	}
	if ((error = uiomove((caddr_t)pkt, (int)n, uio)) != 0) {
		return error;
	}
	return hkbt_write_packet(pkt, (uint32_t)n) ? 0 : EINVAL;
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_open, .d_close = bt_close, .d_read = bt_read, .d_write = bt_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- bt=6: the kernel control BlueTool's UART transport connects to ----------------------------------------- */

/*
 * BlueTool (iOS 16.5, disassembled: logs/bluetool-transport-analysis.md) opens its UART transport as a Darwin kernel-control socket
 * (socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL), CTLIOCGINFO on the name "com.apple.uart.bluetooth", connect) and speaks H4 on it, with a
 * private socket option (0x40484813) for the line settings. Apple's UART driver registers that control; this registers it over the same
 * rings as /dev/hk-bt, and while BlueTool is connected its H4 stream is cut into packets, Broadcom's own vendor commands (OGF 0x3f) are
 * answered here without touching the Qualcomm controller (Update_UART_Baud_Rate 0xfc18, Download_Minidriver 0xfc2e, Write_RAM 0xfc4c,
 * Launch_RAM 0xfc4e, Write_BD_ADDR 0xfc01: "success"; any other vendor opcode: "unknown command", so BlueTool shows what else it wants),
 * and every command that goes by is logged (the first 300 lines), everything else goes to the controller.
 */
#define CTL_NAME                "com.apple.uart.bluetooth"
static struct {
	uint8_t pkt[MAX_PACKET + 8];
	uint32_t have, need;
	int kind;                       /* 0 between packets, 1 command, 2 ACL, 3 SCO */
	bool in_body;
} txs;
static uint32_t ctl_logged, stat_fake, stat_ctl_tx;

static void
ctl_log(const char *dir, const uint8_t *p, uint32_t n)
{
	if (ctl_logged++ >= 300) {
		return;
	}
	say("bt: ctl %s %u bytes:", dir, n);
	for (uint32_t i = 0; i < n && i < 20; i++) {
		say(" %02x", p[i]);
	}
	say("%s\n", n > 20 ? " ..." : "");
}

static void
fake_reply(uint16_t opcode, uint8_t status)
{
	uint8_t ev[7] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)(opcode & 0xff), (uint8_t)(opcode >> 8), status };

	stat_fake++;
	(void)ring_put(fk_ring, sizeof(fk_ring), &fk_head, &fk_tail, ev, sizeof(ev));
}

/* one whole H4 packet from BlueTool */
static void
ctl_packet(const uint8_t *p, uint32_t n)
{
	stat_ctl_tx++;
	if (p[0] == 1) {
		uint16_t op = (uint16_t)(p[1] | p[2] << 8);

		ctl_log("->", p, n);
		if ((op >> 10) == 0x3f) {               /* the vendor group: Broadcom's, not this controller's */
			switch (op) {
			case 0xfc18: case 0xfc2e: case 0xfc4c: case 0xfc4e: case 0xfc01:
				fake_reply(op, 0);
				break;
			default:
				fake_reply(op, 1);
				break;
			}
			return;
		}
	}
	if (!hkbt_write_packet(p, n)) {
		say("bt: ctl: the controller's ring refuses a packet of %u bytes (type %u)\n", n, p[0]);
	}
}

static void
ctl_tx_byte(uint8_t b)
{
	if (txs.kind == 0) {
		if (b < 1 || b > 3) {
			if (ctl_logged++ < 300) {
				say("bt: ctl: byte %02x outside a packet from BlueTool, dropped\n", b);
			}
			return;
		}
		txs.kind = b;
		txs.need = b == 2 ? 4 : 3;
		txs.pkt[0] = b;
		txs.have = 1;
		txs.in_body = false;
		return;
	}
	txs.pkt[txs.have++] = b;
	if (--txs.need != 0) {
		return;
	}
	if (!txs.in_body) {
		uint32_t body = txs.kind == 2 ? (txs.pkt[3] | txs.pkt[4] << 8) : txs.pkt[3];

		if (txs.have + body > MAX_PACKET) {
			txs.kind = 0;
			return;
		}
		txs.in_body = true;
		txs.need = body;
		if (body != 0) {
			return;
		}
	}
	ctl_packet(txs.pkt, txs.have);
	txs.kind = 0;
}

static errno_t
ctl_connect_cb(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	if (ctl_connected) {
		return EBUSY;
	}
	bzero(&txs, sizeof(txs));
	ctl_unit_no = sac->sc_unit;
	ctl_connected = true;
	say("bt: ctl: BlueTool connected to %s (unit %u)\n", CTL_NAME, sac->sc_unit);
	return 0;
}

static errno_t
ctl_disconnect_cb(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo)
{
	ctl_connected = false;
	say("bt: ctl: BlueTool disconnected (%u packets from it, %u fake answers, %u events it could not take)\n", stat_ctl_tx, stat_fake, stat_ctl_drop);
	return 0;
}

static errno_t
ctl_send_cb(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	for (mbuf_t c = m; c != NULL; c = mbuf_next(c)) {
		const uint8_t *d = mbuf_data(c);
		size_t l = mbuf_len(c);

		for (size_t i = 0; i < l; i++) {
			ctl_tx_byte(d[i]);
		}
	}
	mbuf_freem(m);
	return 0;
}

/* the line settings BlueTool asks for (opcode 0x40484813 and any other): accepted and logged; what it reads back is zeros */
static errno_t
ctl_setopt_cb(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	say("bt: ctl: setsockopt %#x, %zu bytes:", opt, len);
	for (size_t i = 0; i < len && i < 24; i++) {
		say(" %02x", ((uint8_t *)data)[i]);
	}
	say("\n");
	return 0;
}

static errno_t
ctl_getopt_cb(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	say("bt: ctl: getsockopt %#x, room for %zu bytes\n", opt, *len);
	if (data != NULL) {
		bzero(data, *len);
	}
	return 0;
}

static void
ctl_start(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, CTL_NAME, sizeof(reg.ctl_name));
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = ctl_connect_cb;
	reg.ctl_disconnect = ctl_disconnect_cb;
	reg.ctl_send = ctl_send_cb;
	reg.ctl_setopt = ctl_setopt_cb;
	reg.ctl_getopt = ctl_getopt_cb;
	error = ctl_register(&reg, &ctl_ref);
	say("bt: bt=6: the kernel control %s %s (%d)\n", CTL_NAME, error == 0 ? "is registered: BlueTool's UART transport can connect" : "cannot be registered",
	    error);
}

/* One request through the same path /dev/hk-bt takes: sent, the answer (an event) waited for and shown. */
static bool
selftest(const char *what, const uint8_t *cmd, uint32_t len, uint32_t expect_event_len)
{
	static uint8_t ev[MAX_PACKET + 8];
	uint32_t n;

	if (!hkbt_write_packet(cmd, len)) {
		say("bt: %s: the packet is refused\n", what);
		return false;
	}
	n = hkbt_read_packet(ev, sizeof(ev), 1500);
	if (n == 0) {
		say("bt: %s: no answer\n", what);
		return false;
	}
	say("bt: %s: %u bytes:", what, n);
	for (uint32_t i = 0; i < n && i < 20; i++) {
		say(" %02x", ev[i]);
	}
	say("%s\n", n > 20 ? " ..." : "");
	return n >= 7 && ev[0] == 4 && ev[1] == 0x0e && (expect_event_len == 0 || n == expect_event_len) && ev[6] == 0;
}

static void
transport_run(void)
{
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 }, version[] = { 0x01, 0x01, 0x10, 0x00 }, bufsize[] = { 0x01, 0x05, 0x10, 0x00 },
	    lebuf[] = { 0x01, 0x02, 0x20, 0x00 }, commands[] = { 0x01, 0x02, 0x10, 0x00 }, features[] = { 0x01, 0x03, 0x10, 0x00 };
	thread_t thread;
	unsigned good = 0, tries = 0, ok = 0, bad_before;
	int major;

	say("bt: bt=5: the controller as a packet device: the worker starts, the engine's receiver without 'RFR open' (the controller's flow control)\n");
	/* the receive command again, now with the watermark deciding RFR (the S command is cancelled, then started without UART_PARAM_RFR_OPEN) */
	wr(SE_GENI_S_CMD_CTRL_REG, 4);
	IOSleep(10);
	wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	uart_drain();
	wr(SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
	awake = false;
	got_wake_ack = false;
	if (kernel_thread_start(worker, NULL, &thread) != KERN_SUCCESS) {
		say("bt: the worker thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
	/* the self-test, before iOS has the node (two readers of one ring take each other's answers): the same calls it will make */
	tries++; good += selftest("HCI_Reset", reset, sizeof(reset), 7);
	tries++; good += selftest("Read_Local_Version", version, sizeof(version), 15);
	tries++; good += selftest("Read_Buffer_Size", bufsize, sizeof(bufsize), 14);
	tries++; good += selftest("LE_Read_Buffer_Size", lebuf, sizeof(lebuf), 10);
	tries++; good += selftest("Read_Local_Supported_Features", features, sizeof(features), 15);
	tries++; good += selftest("Read_Local_Supported_Commands (a long event)", commands, sizeof(commands), 71);
	/* a hundred long events back to back: the flow control and the polling are tested by the length every one has to have */
	bad_before = stat_bad;
	for (unsigned i = 0; i < 100; i++) {
		static uint8_t ev[MAX_PACKET + 8];

		if (hkbt_write_packet(commands, sizeof(commands)) && hkbt_read_packet(ev, sizeof(ev), 1500) == 71 && ev[0] == 4 && ev[1] == 0x0e) {
			ok++;
		}
	}
	say("bt: 100 x Read_Local_Supported_Commands: %u whole 71-byte events, %u bytes taken out of the stream as not a packet's\n", ok,
	    stat_bad - bad_before);
	tries++; good += ok == 100;
	say("bt: bt=5: %u of %u tests good; %u packets received, %u sent, %u wake-ups answered, %u bad bytes\n", good, tries, stat_packets, stat_tx,
	    stat_wakes, stat_bad);
	major = cdevsw_add(-1, &bt_cdevsw);
	if (major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hk-bt") != NULL) {
		say("bt: /dev/hk-bt: one HCI packet (H4: a type byte, then the packet) a read and a write\n");
	} else {
		say("bt: /dev/hk-bt cannot be made\n");
	}
	if (level >= 6) {
		ctl_start();
	}
}
#endif

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
	uart_rx_start();
	if (level >= 4) {
#ifdef HAVE_BTFW
		if (uart_baud(115200, 384, 15625, 4)) {         /* (the sweep's "57600" entry: 115200 real, see above) */
			IOSleep(5);
			if (firmware() && level >= 5) {
				kept = true;
				transport_run();
				return;                 /* (the engine, the clocks and the rate stay as they are: the worker thread owns them now) */
			}
		}
#else
		say("bt: bt=4: this build has no firmware header (instantnoodle_btfw.h): nothing downloaded\n");
#endif
		uart_stop();
		restore_rate();
		return;
	}
	say("bt: bt=3: HCI_Reset is sent at several baud rates, then the ROME version request at the one that answers\n");
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
			if (!kept) {
				power_down();
			}
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
	if (level > 6) {
		say("bt: the driver does bt=1 (a look), bt=2 (power on), bt=3 (HCI_Reset and the version request), bt=4 (firmware), bt=5 (packet device), "
		    "bt=6 (and BlueTool's kernel control) so far\n");
		level = 6;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
