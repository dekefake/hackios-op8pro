/*
 * (A kernel-side copy of wifi/lx/src/mhi_host.h of the Wi-Fi work, names renamed hk_mhi_*, for the OnePlus 8 Pro's modem (bsd/dev/instantnoodle_modem.c): the
 * Wi-Fi's host and this one are separate objects. Differences: more channels, and config.no_bhie, for a device whose secondary boot loader (SBL) does not
 * take a whole image through BHIe but runs the Sahara protocol on a channel pair of its own.)
 *
 * An MHI host: what speaks Qualcomm's "Modem Host Interface" to a PCIe device -- here the
 * QCA6390 -- over its first memory window: its boot (the secondary boot loader through BHI, the
 * whole image through BHIe), its states (READY, M0, the low-power M1/M2, the suspended M3), its
 * command ring, its event rings and the transfer rings of the channels it is asked to carry.
 *
 * Written from the MHI register and ring layouts as OpenBSD's qwx(4) (ISC licence, Stefan
 * Sperling) and Linux's MHI host describe them; no code of Linux's MHI host (GPL) is used. It
 * knows nothing of any kernel: everything it needs of one -- registers, memory the device can
 * reach, time, a lock -- comes through struct hk_mhi_ops, so that it runs the same on the phone
 * (wifi/lx/src/lx_mhi.c gives it Linux's MHI interface for ath11k) and in a test on any computer
 * against a device simulated in software (tools/mhi-test).
 */
#ifndef _SYS_HK_MHI_H_
#define _SYS_HK_MHI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HK_MHI_CHANNELS	32
#define HK_MHI_EVENT_RINGS	17

/* execution environments (BHI_EXECENV, EE events) and states (MHISTATUS, state change events) */
enum { HK_MHI_EE_PBL, HK_MHI_EE_SBL, HK_MHI_EE_AMSS, HK_MHI_EE_RDDM, HK_MHI_EE_WFW, HK_MHI_EE_PTHRU,
	HK_MHI_EE_EDL, HK_MHI_EE_FP };
enum { HK_MHI_STATE_RESET, HK_MHI_STATE_READY, HK_MHI_STATE_M0, HK_MHI_STATE_M1, HK_MHI_STATE_M2,
	HK_MHI_STATE_M3, HK_MHI_STATE_M3_FAST, HK_MHI_STATE_BHI, HK_MHI_STATE_SYS_ERR = 0xff };

/* What the host tells whoever runs it (status()). */
enum hk_mhi_status { HK_MHI_MISSION, HK_MHI_SYS_ERROR, HK_MHI_RDDM };

struct hk_mhi_ops {
	uint32_t (*read)(void *context, uint32_t offset);		/* of the window */
	void	(*write)(void *context, uint32_t offset, uint32_t value);
	/* zeroed memory the device can reach, at *address for it; aligned to `align` (a power of two) */
	void	*(*dma_alloc)(void *context, size_t size, size_t align, uint64_t *address);
	void	(*dma_free)(void *context, void *memory, size_t size);
	uint64_t (*nanoseconds)(void *context);			/* never goes back */
	/* the host's lock: held by it while it changes its state, given up while it waits */
	void	(*lock)(void *context);
	void	(*unlock)(void *context);
	void	(*pause)(void *context, unsigned microseconds);		/* called without the lock */
	void	(*random)(void *context, void *bytes, size_t length);
	void	(*say)(void *context, const char *line);
	/* without the lock: */
	void	(*status)(void *context, enum hk_mhi_status what);
	void	(*receive)(void *context, unsigned channel, const void *data, size_t length);
};

struct hk_mhi_channel_config {
	uint32_t number;		/* the device's channel number */
	uint32_t elements;		/* of its transfer ring */
	uint32_t event_ring;		/* which of the event rings its completions go to */
	bool inbound;			/* from the device (buffers are posted for it) */
	uint32_t ee_mask;		/* the execution environments it runs in (bit per EE): started when the device enters one */
	uint32_t burst;			/* BRSTMODE of its context (0: the host's default, 2 disabled), 3 for a hardware channel (device tree doorbell-mode 3) */
};

struct hk_mhi_event_config {
	uint32_t elements;
	uint32_t vector;		/* the MSI vector the device raises for it */
	uint32_t moderation_ms;
	bool control;			/* state changes and command completions come here */
	bool offload;			/* another client's ring (satellite, IPA): its context is left empty and nothing is allocated or rung */
};

struct hk_mhi_config {
	uint32_t max_channels;		/* of the channel context array */
	uint32_t timeout_ms;
	uint32_t buffer_length;		/* of an inbound channel's buffers */
	unsigned channel_count, event_count;
	struct hk_mhi_channel_config channels[HK_MHI_CHANNELS];
	struct hk_mhi_event_config events[HK_MHI_EVENT_RINGS];
	const uint8_t *image;		/* the device's firmware, its secondary boot loader in front */
	size_t image_length;
	size_t sbl_length;		/* what BHI loads of it */
	size_t segment_length;		/* BHIe's pieces of it */
	uint64_t iova_start, iova_stop;	/* what the device may reach */
	unsigned hw_event_rings;		/* MHICFG's NHWER: how many of the last event rings are the hardware's */
	bool no_bhie;			/* no BHIe: the SBL's own channels are the end of the boot (the Sahara channels of an SDX55) */
};

struct hk_mhi_ring {
	uint8_t *base;
	uint64_t address;		/* the device's */
	uint32_t elements;
	uint32_t rp, wp;		/* as the host has them: element indices */
};

struct hk_mhi_channel {
	const struct hk_mhi_channel_config *config;
	struct hk_mhi_ring ring;
	uint8_t *buffers;		/* a buffer of buffer_length (inbound) or the largest send (outbound) per element */
	uint64_t buffers_address;
	uint32_t buffer_length;
	uint32_t command_status;	/* the last command's completion code; 0: none yet */
	bool running;
	bool busy;			/* its completions are being handed on */
};

struct hk_mhi {
	const struct hk_mhi_ops *ops;
	void *context;
	struct hk_mhi_config config;

	bool allocated, powered, mission, failed;
	bool mission_told;		/* the device's own event says its mission has begun (the register says it earlier) */
	unsigned rung_again;		/* doorbells rung again for a command not completed */
	volatile uint32_t ee, state;	/* as the device last said */
	volatile uint32_t sfr_status;	/* the completion code of the last MHI_CMD_SFR_CFG; 0: none yet */
	uint32_t bhi, bhie, channel_doorbells, event_doorbells;
	unsigned wakes;			/* votes for the device to stay out of M1/M2 */
	uint64_t serviced;		/* events handled, all rings */

	uint8_t *channel_contexts, *event_contexts, *command_context;
	uint64_t channel_contexts_address, event_contexts_address, command_context_address;
	struct hk_mhi_ring events[HK_MHI_EVENT_RINGS];
	bool event_busy[HK_MHI_EVENT_RINGS];
	bool event_again[HK_MHI_EVENT_RINGS];
	struct hk_mhi_ring commands;
	struct hk_mhi_channel channels[HK_MHI_CHANNELS];

	uint8_t *image;			/* the firmware where the device reads it, and BHIe's table of its pieces */
	uint64_t image_address;
	size_t image_size;
	uint8_t *vectors;
	uint64_t vectors_address;
	size_t vectors_size;
};

/* Memory for everything, once (it is kept for the next power-up). 0, or an errno. */
int	hk_mhi_init(struct hk_mhi *host, const struct hk_mhi_ops *ops, void *context, const struct hk_mhi_config *config);
/* The device booted into its mission (the channels for it started): 0, or an errno. */
int	hk_mhi_power_up(struct hk_mhi *host);
/* With no_bhie: the device is in its mission (EE AMSS) after its SBL: the channels for it started. 0, or an errno. */
int	hk_mhi_start_mission(struct hk_mhi *host);
/* A buffer for the device's failure reason string (MHI_CMD_SFR_CFG). 0, or an errno. */
int	hk_mhi_sfr_config(struct hk_mhi *host, uint64_t address, uint32_t length);
int	hk_mhi_sat_context(struct hk_mhi *host, bool event, uint32_t id, uint64_t base, uint64_t size);
int	hk_mhi_sat_command(struct hk_mhi *host, uint32_t number, bool start);
void	hk_mhi_channel_state(struct hk_mhi *host, uint32_t number, uint32_t *context_state, uint64_t *context_rp, uint64_t *context_wp, uint32_t *host_rp, uint32_t *host_wp, uint32_t *elements);
/* The device reset (MHICTRL.RESET), the channels stopped. */
void	hk_mhi_power_down(struct hk_mhi *host);
/* An interrupt: vector v of the host's (its BHI vector is 0); or nothing came, and everything is looked at (-1). */
void	hk_mhi_interrupt(struct hk_mhi *host, int vector);
/* A datagram on an outbound channel; waits for room up to the timeout. 0, or an errno. */
int	hk_mhi_send(struct hk_mhi *host, unsigned channel, const void *data, size_t length);
/* The device kept out of its low-power states while a vote stands; get waits for it to be in M0 (0, or ETIMEDOUT). */
int	hk_mhi_wake_get(struct hk_mhi *host);
void	hk_mhi_wake_put(struct hk_mhi *host);
/* M3 and back. 0, or an errno. */
int	hk_mhi_suspend(struct hk_mhi *host);
int	hk_mhi_resume(struct hk_mhi *host);
/* Ask the device for its crash dump mode (RDDM). 0, or an errno. */
int	hk_mhi_force_rddm(struct hk_mhi *host);

/* the registers (offsets in the window), for the host and its tests */
#define MHI_REG_MHICFG		0x10
#define MHI_REG_CHDBOFF		0x18
#define MHI_REG_ERDBOFF		0x20
#define MHI_REG_BHIOFF		0x28
#define MHI_REG_BHIEOFF		0x2c
#define MHI_REG_MHICTRL		0x38
#define MHI_REG_MHISTATUS	0x48
#define MHI_REG_CCABAP		0x58
#define MHI_REG_ECABAP		0x60
#define MHI_REG_CRCBAP		0x68
#define MHI_REG_CRDB		0x70
#define MHI_REG_CTRLBASE	0x80
#define MHI_REG_CTRLLIMIT	0x88
#define MHI_REG_DATABASE	0x98
#define MHI_REG_DATALIMIT	0xa0
#define MHI_REG_SOC_RESET	0xb0
#define MHI_CTRL_RESET		(1u << 1)
#define MHI_STATUS_READY	(1u << 0)
#define MHI_STATUS_SYSERR	(1u << 2)
/* BHI's, from BHIOFF; BHIe's, from BHIEOFF */
#define MHI_BHI_IMGADDR_LOW	0x08
#define MHI_BHI_IMGADDR_HIGH	0x0c
#define MHI_BHI_IMGSIZE		0x10
#define MHI_BHI_IMGTXDB		0x18
#define MHI_BHI_INTVEC		0x20
#define MHI_BHI_EXECENV		0x28
#define MHI_BHI_STATUS		0x2c
#define MHI_BHI_ERRCODE		0x30
#define MHI_BHIE_TXVECADDR_LOW	0x2c
#define MHI_BHIE_TXVECADDR_HIGH	0x30
#define MHI_BHIE_TXVECSIZE	0x34
#define MHI_BHIE_TXVECDB	0x3c
#define MHI_BHIE_TXVECSTATUS	0x44
#define MHI_BHIE_RXVECADDR_LOW	0x60
#define MHI_BHIE_RXVECSTATUS	0x78
#define MHI_WAKE_DOORBELL	127	/* the channel doorbell that holds the device awake */
/* ring elements: 16 bytes, an address and two words */
#define MHI_ELEMENT		16
#define MHI_TYPE_TRANSFER	0x02
#define MHI_TYPE_RESET_CHAN	0x10
#define MHI_TYPE_STOP_CHAN	0x11
#define MHI_TYPE_START_CHAN	0x12
#define MHI_TYPE_STATE_CHANGE	0x20
#define MHI_TYPE_CMD_DONE	0x21
#define MHI_TYPE_TX		0x22
#define MHI_TYPE_EE		0x40
#define MHI_CODE_SUCCESS	0x1
#define MHI_CODE_EOT		0x2
#define MHI_CODE_OVERFLOW	0x3
#define MHI_CODE_EOB		0x4
#define MHI_CODE_DB_MODE	0x6
#define MHI_TRE_IEOB		(1u << 8)
#define MHI_TRE_IEOT		(1u << 9)
/* the contexts: 44 bytes each, as the device reads them (little endian, packed) */
#define MHI_CONTEXT		44

#endif
