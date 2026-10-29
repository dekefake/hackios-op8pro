/*
 * (A kernel-side copy of wifi/lx/src/mhi_host.c, renamed hk_mhi_*; see hk_mhi.h for what differs: no_bhie, and a power-up that stops in SBL then.)
 *
 * The MHI host (mhi_host.h): a device's boot over BHI and BHIe, its states, its rings.
 *
 * The device is told where four kinds of things are in the host's memory, which it reads and writes
 * by itself: a context per channel, per event ring and for the command ring (44 bytes each: a
 * ring's base, length, read and write pointers, and what kind of ring it is), and the rings, of
 * 16-byte elements. On a transfer or command ring the host writes elements and moves the write
 * pointer, the device reads and moves the read pointer; on an event ring it is the other way
 * round. After moving a pointer the side that moved it rings the other's doorbell (a register of
 * the window, 64 bits wide, the new pointer in it); the device's doorbell for the host is an MSI.
 *
 * A boot from the primary boot loader (EE PBL), as Linux's host does it with "fbc_download":
 *   1. BHI: the first sbl_length bytes of the image are pointed to, and a transaction number
 *      written; BHI_STATUS says when they are taken;
 *   2. the secondary boot loader runs, and says READY: the host writes where its contexts are
 *      (the MMIO setup), gives the event rings their elements and asks for M0;
 *   3. in EE SBL, BHIe: a table of the whole image's pieces, and another transaction number;
 *   4. the image runs (EE AMSS, "mission mode"): the channels whose environments that is are
 *      started with a command each, and the inbound ones given buffers.
 * Waiting is done by looking -- the registers, the event rings -- every 200 us, whether or not
 * interrupts come; with them, a waiter is done sooner.
 *
 * The device may go to M1 by itself when it has nothing to do; the host then sends it to M2
 * unless somebody wants it awake (hk_mhi_wake_get(): the wake doorbell held). Doorbells other
 * than that one are rung only in M0 (the QCA6390: "mhi,m2-no-db-access" in Android's tree).
 */
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/hk_mhi.h>

#define PAUSE_US	200
#define CHANNEL_STATE_ENABLED	1
#define BURST_DISABLED		2u
#define EVENT_RING_VALID	1

static void
say(struct hk_mhi *h, const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	h->ops->say(h->context, line);
}

static uint32_t
rd(struct hk_mhi *h, uint32_t offset)
{
	return (h->ops->read(h->context, offset));
}

static void
wr(struct hk_mhi *h, uint32_t offset, uint32_t value)
{
	h->ops->write(h->context, offset, value);
}

/* a 64-bit doorbell: high word first, the low word rings it */
/*
 * Order against the device: it is no observer of this processor's inner domain, so a "dmb ish"
 * (a C fence) does not order the rings' memory against a doorbell, nor the device's writes
 * against our reads of them (Linux: wmb() and rmb(), dsb).
 */
static inline void
device_fence(void)
{
#if defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static void
doorbell(struct hk_mhi *h, uint32_t offset, uint64_t value)
{
	device_fence();				/* (what the pointer covers is in memory first) */
	wr(h, offset + 4, (uint32_t)(value >> 32));
	wr(h, offset, (uint32_t)value);
}

static uint64_t
now_ms(struct hk_mhi *h)
{
	return (h->ops->nanoseconds(h->context) / 1000000u);
}

/* ---- contexts: little-endian words at fixed offsets ------------------------------------------------ */

static void
put32(uint8_t *at, uint32_t value)
{
	memcpy(at, &value, 4);
}

static void
put64(uint8_t *at, uint64_t value)
{
	memcpy(at, &value, 8);
}

/* (what the device writes: two loads of 32 bits, low first; its pointers stay below 4 GB here) */
static uint64_t
get64(const uint8_t *at)
{
	uint32_t low = *(const volatile uint32_t *)at, high = *(const volatile uint32_t *)(at + 4);

	return ((uint64_t)high << 32 | low);
}

#define CTX_CFG		0
#define CTX_TYPE	4
#define CTX_INDEX	8
#define CTX_RBASE	12
#define CTX_RLEN	20
#define CTX_RP		28
#define CTX_WP		36

static uint8_t *
channel_context(struct hk_mhi *h, uint32_t number)
{
	return (h->channel_contexts + (size_t)number * MHI_CONTEXT);
}

static uint8_t *
event_context(struct hk_mhi *h, unsigned ring)
{
	return (h->event_contexts + (size_t)ring * MHI_CONTEXT);
}

/* ---- rings ---------------------------------------------------------------------------------------- */

static uint8_t *
element(struct hk_mhi_ring *r, uint32_t index)
{
	return (r->base + (size_t)index * MHI_ELEMENT);
}

static uint64_t
element_address(struct hk_mhi_ring *r, uint32_t index)
{
	return (r->address + (uint64_t)index * MHI_ELEMENT);
}

static uint32_t
next(struct hk_mhi_ring *r, uint32_t index)
{
	return (index + 1 == r->elements ? 0 : index + 1);
}

/* the element the device's pointer names; ~0u if it names none of the ring's */
static uint32_t
index_of(struct hk_mhi_ring *r, uint64_t address)
{
	if (address < r->address || address >= r->address + (uint64_t)r->elements * MHI_ELEMENT ||
	    (address - r->address) % MHI_ELEMENT != 0) {
		return (~0u);
	}
	return ((uint32_t)((address - r->address) / MHI_ELEMENT));
}

static void
write_element(uint8_t *e, uint64_t pointer, uint32_t word0, uint32_t word1)
{
	put64(e, pointer);
	put32(e + 8, word0);
	put32(e + 12, word1);
}

static bool
ring_alloc(struct hk_mhi *h, struct hk_mhi_ring *r, uint32_t elements)
{
	size_t size = (size_t)elements * MHI_ELEMENT;

	/* a ring is aligned to its size, rounded up to a power of two (the device's address arithmetic) */
	size_t align = MHI_ELEMENT;

	while (align < size) {
		align <<= 1;
	}
	r->base = h->ops->dma_alloc(h->context, size, align, &r->address);
	r->elements = elements;
	r->rp = r->wp = 0;
	return (r->base != NULL);
}

/* ---- memory, once --------------------------------------------------------------------------------- */

int
hk_mhi_init(struct hk_mhi *h, const struct hk_mhi_ops *ops, void *context, const struct hk_mhi_config *config)
{
	size_t vectors;

	if (h->allocated) {
		return (0);
	}
	memset(h, 0, sizeof(*h));
	h->ops = ops;
	h->context = context;
	h->config = *config;
	if (config->channel_count > HK_MHI_CHANNELS || config->event_count == 0 || config->event_count > HK_MHI_EVENT_RINGS ||
	    config->image == NULL || config->image_length == 0 || config->segment_length == 0 || config->max_channels == 0) {
		say(h, "mhi: the controller's description is not one this host takes");
		return (EINVAL);
	}
	for (unsigned c = 0; c < config->channel_count; c++) {
		if (config->channels[c].number >= config->max_channels || config->channels[c].event_ring >= config->event_count ||
		    config->channels[c].elements < 2) {
			say(h, "mhi: channel %u is not one this host takes", config->channels[c].number);
			return (EINVAL);
		}
	}
	h->channel_contexts = ops->dma_alloc(context, (size_t)config->max_channels * MHI_CONTEXT, 64, &h->channel_contexts_address);
	h->event_contexts = ops->dma_alloc(context, (size_t)config->event_count * MHI_CONTEXT, 64, &h->event_contexts_address);
	h->command_context = ops->dma_alloc(context, MHI_CONTEXT, 64, &h->command_context_address);
	if (h->channel_contexts == NULL || h->event_contexts == NULL || h->command_context == NULL ||
	    !ring_alloc(h, &h->commands, 32)) {
		return (ENOMEM);
	}
	for (unsigned e = 0; e < config->event_count; e++) {
		if (config->events[e].offload) {
			continue;
		}
		if (!ring_alloc(h, &h->events[e], config->events[e].elements)) {
			return (ENOMEM);
		}
	}
	for (unsigned c = 0; c < config->channel_count; c++) {
		struct hk_mhi_channel *ch = &h->channels[c];

		ch->config = &h->config.channels[c];
		ch->buffer_length = config->buffer_length;
		if (!ring_alloc(h, &ch->ring, ch->config->elements)) {
			return (ENOMEM);
		}
		ch->buffers = ops->dma_alloc(context, (size_t)ch->buffer_length * ch->config->elements, 64, &ch->buffers_address);
		if (ch->buffers == NULL) {
			return (ENOMEM);
		}
	}
	/* the image where the device reads it, and the table of its pieces: BHIe takes the whole image */
	h->image_size = config->image_length;
	h->image = ops->dma_alloc(context, h->image_size, 4096, &h->image_address);
	vectors = (config->image_length + config->segment_length - 1) / config->segment_length;
	h->vectors_size = vectors * 16;
	h->vectors = ops->dma_alloc(context, h->vectors_size, 4096, &h->vectors_address);
	if (h->image == NULL || h->vectors == NULL) {
		return (ENOMEM);
	}
	memcpy(h->image, config->image, config->image_length);
	for (size_t v = 0; v < vectors; v++) {
		size_t at = v * config->segment_length, length = config->image_length - at;

		put64(h->vectors + 16 * v, h->image_address + at);
		put64(h->vectors + 16 * v + 8, length < config->segment_length ? length : config->segment_length);
	}
	h->allocated = true;
	say(h, "mhi: %u channel and %u event ring(s) set up; the image (%zu bytes, %zu pieces) is where the device reads it",
	    config->channel_count, config->event_count, config->image_length, vectors);
	return (0);
}

/* ---- the device's state -------------------------------------------------------------------------- */

static uint32_t
device_state(struct hk_mhi *h)
{
	return (rd(h, MHI_REG_MHISTATUS) >> 8 & 0xff);
}

static uint32_t
device_ee(struct hk_mhi *h)
{
	return (h->bhi != 0 ? rd(h, h->bhi + MHI_BHI_EXECENV) : ~0u);
}

static void
set_state(struct hk_mhi *h, uint32_t state)
{
	uint32_t control = rd(h, MHI_REG_MHICTRL);

	wr(h, MHI_REG_MHICTRL, (control & ~(0xffu << 8)) | state << 8);
}

static bool
in_mission(uint32_t ee)
{
	return (ee == HK_MHI_EE_AMSS || ee == HK_MHI_EE_WFW || ee == HK_MHI_EE_FP);
}

static void
ring_doorbells(struct hk_mhi *h)
{
	for (unsigned e = 0; e < h->config.event_count; e++) {
		if (h->config.events[e].offload) {
			continue;
		}
		doorbell(h, h->event_doorbells + 8 * e, get64(event_context(h, e) + CTX_WP));
	}
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		struct hk_mhi_channel *ch = &h->channels[c];

		if (ch->running) {
			doorbell(h, h->channel_doorbells + 8 * ch->config->number, element_address(&ch->ring, ch->ring.wp));
		}
	}
}

/* ---- events ---------------------------------------------------------------------------------------- */

static struct hk_mhi_channel *
channel_numbered(struct hk_mhi *h, uint32_t number)
{
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		if (h->channels[c].config->number == number) {
			return (&h->channels[c]);
		}
	}
	return (NULL);
}

static void
state_changed(struct hk_mhi *h, uint32_t state)
{
	h->state = state;
	switch (state) {
	case HK_MHI_STATE_M0:
		if (h->mission) {
			ring_doorbells(h);		/* (what was put on a ring meanwhile) */
		}
		break;
	case HK_MHI_STATE_M1:
		/* the device is idle: to M2, unless somebody holds it awake (then it comes back to M0 by itself) */
		if (h->wakes == 0) {
			set_state(h, HK_MHI_STATE_M2);
			h->state = HK_MHI_STATE_M2;
		}
		break;
	case HK_MHI_STATE_SYS_ERR:
		h->failed = true;
		break;
	default:
		break;
	}
}

/*
 * One event ring: everything the device has put there since the last look, handled, and the
 * elements given back. Datagrams are handed on without the lock (an ops->receive() that is under
 * way leaves the ring to its caller, who looks again).
 */
static void
service_ring(struct hk_mhi *h, unsigned e)
{
	struct hk_mhi_ring *r = &h->events[e];
	uint8_t *ctx = event_context(h, e);
	bool told_mission = false, told_error = false, told_rddm = false;

	if (h->event_busy[e]) {
		h->event_again[e] = true;
		return;
	}
	h->event_busy[e] = true;
	do {
		uint32_t until;

		h->event_again[e] = false;
		until = index_of(r, get64(ctx + CTX_RP));
		if (until == ~0u) {
			break;
		}
		device_fence();				/* (the elements are read after the pointer to them) */
		while (r->rp != until) {
			uint8_t *ev = element(r, r->rp);
			uint64_t pointer;
			uint32_t word0, word1, type, code, length, number;
			struct hk_mhi_channel *ch;

			memcpy(&pointer, ev, 8);
			memcpy(&word0, ev + 8, 4);
			memcpy(&word1, ev + 12, 4);
			type = word1 >> 16 & 0xff;
			code = word0 >> 24;
			length = word0 & 0xffff;
			number = word1 >> 24;
			h->serviced++;
			switch (type) {
			case MHI_TYPE_STATE_CHANGE:
				state_changed(h, code);
				told_error |= code == HK_MHI_STATE_SYS_ERR;
				break;
			case MHI_TYPE_EE:
				h->ee = code;
				if (in_mission(code) && !h->mission) {
					h->mission = told_mission = true;
				}
				h->mission_told |= in_mission(code);
				told_rddm |= code == HK_MHI_EE_RDDM;
				break;
			case MHI_TYPE_CMD_DONE: {
				uint32_t at = index_of(&h->commands, pointer), command;

				if (at == ~0u) {
					say(h, "mhi: a command's completion for no command of ours (%#llx)", (unsigned long long)pointer);
					break;
				}
				memcpy(&command, element(&h->commands, at) + 12, 4);
				if ((command >> 16 & 0xff) == 73) {
					h->sfr_status = code;           /* (the subsystem failure reason buffer's configuration, MHI_CMD_SFR_CFG) */
				} else if ((ch = channel_numbered(h, command >> 24)) != NULL) {
					ch->command_status = code;
				}
				h->commands.rp = next(&h->commands, at);
				break;
			}
			case MHI_TYPE_TX: {
				uint32_t at;

				if ((ch = channel_numbered(h, number)) == NULL || !ch->running) {
					say(h, "mhi: a transfer's completion on channel %u, which is not ours or not running", number);
					break;
				}
				if (code == MHI_CODE_DB_MODE) {
					doorbell(h, h->channel_doorbells + 8 * number, element_address(&ch->ring, ch->ring.wp));
					break;
				}
				if (code != MHI_CODE_EOT && code != MHI_CODE_EOB && code != MHI_CODE_OVERFLOW) {
					say(h, "mhi: channel %u: completion code %#x", number, code);
					break;
				}
				if ((at = index_of(&ch->ring, pointer)) == ~0u) {
					say(h, "mhi: channel %u: a completion for no element of its ring", number);
					break;
				}
				/* every element up to and with the one named is done; the last holds `length` bytes */
				while (ch->ring.rp != next(&ch->ring, at)) {
					uint32_t done = ch->ring.rp;
					uint32_t got = done == at ? length : ch->buffer_length;
					uint64_t where;
					uint8_t *buffer;

					ch->ring.rp = next(&ch->ring, done);
					if (!ch->config->inbound) {
						continue;
					}
					/* (which buffer: the element says; buffers move round the ring as they are given back) */
					memcpy(&where, element(&ch->ring, done), 8);
					if (where < ch->buffers_address || where - ch->buffers_address >=
					    (uint64_t)ch->buffer_length * ch->config->elements) {
						say(h, "mhi: channel %u: an element names no buffer of ours", number);
						continue;
					}
					buffer = ch->buffers + (where - ch->buffers_address);
					if (code == MHI_CODE_OVERFLOW && done == at) {
						say(h, "mhi: channel %u: more came than a buffer holds: dropped", number);
					} else if (h->ops->receive != NULL) {
						h->ops->unlock(h->context);
						h->ops->receive(h->context, number, buffer, got);
						h->ops->lock(h->context);
					}
					/* the buffer back on the ring */
					write_element(element(&ch->ring, ch->ring.wp), where, ch->buffer_length,
					    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
					ch->ring.wp = next(&ch->ring, ch->ring.wp);
				}
				if (ch->config->inbound) {
					put64(channel_context(h, number) + CTX_WP, element_address(&ch->ring, ch->ring.wp));
					if (h->state == HK_MHI_STATE_M0) {
						doorbell(h, h->channel_doorbells + 8 * number, element_address(&ch->ring, ch->ring.wp));
					}
				}
				break;
			}
			default:
				say(h, "mhi: event ring %u: an event of type %#x", e, type);
				break;
			}
			r->rp = next(r, r->rp);
			r->wp = next(r, r->wp);
		}
		put64(ctx + CTX_WP, element_address(r, r->wp));
		if (h->state == HK_MHI_STATE_M0 || !h->mission) {
			doorbell(h, h->event_doorbells + 8 * e, element_address(r, r->wp));
		}
	} while (h->event_again[e]);
	h->event_busy[e] = false;

	if (told_mission || told_error || told_rddm) {
		h->ops->unlock(h->context);
		if (told_mission) {
			h->ops->status(h->context, HK_MHI_MISSION);
		}
		if (told_error) {
			h->ops->status(h->context, HK_MHI_SYS_ERROR);
		}
		if (told_rddm) {
			h->ops->status(h->context, HK_MHI_RDDM);
		}
		h->ops->lock(h->context);
	}
}

/*
 * The BHI vector: what the device's state and environment are now, from its registers (which
 * say what the events say, and are looked at in case an event or its interrupt went missing).
 */
static void
service_bhi(struct hk_mhi *h)
{
	uint32_t ee = device_ee(h), state = device_state(h);
	bool error = false, rddm = false, told_mission = false;

	if (ee == ~0u) {
		return;				/* (no BHI yet, or the link is down: everything reads all ones) */
	}
	if (ee == HK_MHI_EE_RDDM && h->ee != HK_MHI_EE_RDDM) {
		h->ee = ee;
		rddm = h->powered;
	} else if (h->powered && !h->mission) {
		h->ee = ee;
		if (in_mission(ee) && h->channel_doorbells != 0) {
			h->mission = told_mission = true;
		}
	}
	if (h->powered && h->channel_doorbells != 0 && state != h->state &&
	    (state <= HK_MHI_STATE_M3 || state == HK_MHI_STATE_SYS_ERR)) {
		error = state == HK_MHI_STATE_SYS_ERR;
		state_changed(h, state);
	}
	if (error || rddm || told_mission) {
		h->ops->unlock(h->context);
		h->ops->status(h->context, told_mission ? HK_MHI_MISSION : rddm ? HK_MHI_RDDM : HK_MHI_SYS_ERROR);
		h->ops->lock(h->context);
	}
}

static void
service_all(struct hk_mhi *h)
{
	service_bhi(h);
	if (h->channel_doorbells != 0) {		/* (the rings are the device's to write once the MMIO is set up) */
		for (unsigned e = 0; e < h->config.event_count; e++) {
			if (!h->config.events[e].offload) {
				service_ring(h, e);
			}
		}
	}
}

void
hk_mhi_interrupt(struct hk_mhi *h, int vector)
{
	if (!h->allocated) {
		return;
	}
	h->ops->lock(h->context);
	if (vector < 0) {
		service_all(h);
	} else if (vector == 0) {
		service_bhi(h);
	} else {
		for (unsigned e = 0; e < h->config.event_count && h->channel_doorbells != 0; e++) {
			if (!h->config.events[e].offload && h->config.events[e].vector == (uint32_t)vector) {
				service_ring(h, e);
			}
		}
	}
	h->ops->unlock(h->context);
}

/*
 * Until `done(h)` or the timeout: looking at everything every PAUSE_US, the lock given up in
 * between. Called and returns with the lock held. 0, or ETIMEDOUT.
 */
static int
wait_for(struct hk_mhi *h, bool (*done)(struct hk_mhi *), uint32_t ms)
{
	uint64_t until = now_ms(h) + ms;

	for (;;) {
		service_all(h);
		if (done(h)) {
			return (0);
		}
		if (now_ms(h) > until) {
			return (ETIMEDOUT);
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
}

/* ---- commands ------------------------------------------------------------------------------------- */

static bool waiting_command_done(struct hk_mhi *h);
static struct hk_mhi_channel *waiting_channel;

static bool
waiting_command_done(struct hk_mhi *h)
{
	return (waiting_channel->command_status != 0 || h->failed);
}

/*
 * A command's completion waited for; the doorbells (the command ring's, the event rings') rung
 * again every 100 ms meanwhile, as one rung while the device was still on its way can be lost --
 * ringing one again with the same pointer asks nothing new.
 */
static int
command_wait(struct hk_mhi *h)
{
	uint64_t until = now_ms(h) + h->config.timeout_ms, rung = now_ms(h);

	for (;;) {
		service_all(h);
		if (waiting_command_done(h)) {
			return (0);
		}
		if (now_ms(h) > until) {
			return (ETIMEDOUT);
		}
		if (now_ms(h) - rung >= 100) {
			rung = now_ms(h);
			h->rung_again++;
			for (unsigned e = 0; e < h->config.event_count; e++) {
				if (!h->config.events[e].offload) {
					doorbell(h, h->event_doorbells + 8 * e, element_address(&h->events[e], h->events[e].wp));
				}
			}
			doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
}

static int
command(struct hk_mhi *h, struct hk_mhi_channel *ch, uint32_t type)
{
	uint32_t at = h->commands.wp;
	int error;

	if (next(&h->commands, at) == h->commands.rp) {
		return (EBUSY);
	}
	write_element(element(&h->commands, at), 0, 0, ch->config->number << 24 | type << 16);
	h->commands.wp = next(&h->commands, at);
	put64(h->command_context + CTX_WP, element_address(&h->commands, h->commands.wp));
	ch->command_status = 0;
	doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
	waiting_channel = ch;
	error = command_wait(h);
	if (error == 0 && ch->command_status != MHI_CODE_SUCCESS) {
		say(h, "mhi: channel %u: command %#x completed with %#x", ch->config->number, type, ch->command_status);
		error = EIO;
	}
	return (error);
}

/* ---- channels ------------------------------------------------------------------------------------- */

static int
channel_start(struct hk_mhi *h, struct hk_mhi_channel *ch)
{
	uint8_t *ctx = channel_context(h, ch->config->number);
	uint32_t cfg;
	int error;

	ch->ring.rp = ch->ring.wp = 0;
	memcpy(&cfg, ctx + CTX_CFG, 4);
	put32(ctx + CTX_CFG, (cfg & ~0xffffu) | (ch->config->burst != 0 ? ch->config->burst : BURST_DISABLED) << 8 | CHANNEL_STATE_ENABLED);
	put32(ctx + CTX_TYPE, ch->config->inbound ? 2 : 1);
	put32(ctx + CTX_INDEX, ch->config->event_ring);
	put64(ctx + CTX_RBASE, ch->ring.address);
	put64(ctx + CTX_RLEN, (uint64_t)ch->ring.elements * MHI_ELEMENT);
	put64(ctx + CTX_RP, ch->ring.address);
	put64(ctx + CTX_WP, ch->ring.address);
	if ((error = command(h, ch, MHI_TYPE_START_CHAN)) != 0) {
		say(h, "mhi: channel %u does not start (%d)", ch->config->number, error);
		return (error);
	}
	ch->running = true;
	if (ch->config->inbound) {
		/* all elements but one get a buffer (a full ring is one whose write pointer is one short of its read pointer) */
		for (uint32_t i = 0; i + 1 < ch->ring.elements; i++) {
			write_element(element(&ch->ring, i), ch->buffers_address + (uint64_t)i * ch->buffer_length, ch->buffer_length,
			    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
		}
		ch->ring.wp = ch->ring.elements - 1;
		put64(ctx + CTX_WP, element_address(&ch->ring, ch->ring.wp));
		doorbell(h, h->channel_doorbells + 8 * ch->config->number, element_address(&ch->ring, ch->ring.wp));
	}
	return (0);
}

/* ---- power ---------------------------------------------------------------------------------------- */

static bool
reset_done(struct hk_mhi *h)
{
	return (!(rd(h, MHI_REG_MHICTRL) & MHI_CTRL_RESET));
}

static bool
ready(struct hk_mhi *h)
{
	return (!(rd(h, MHI_REG_MHICTRL) & MHI_CTRL_RESET) && (rd(h, MHI_REG_MHISTATUS) & MHI_STATUS_READY));
}

static bool
bhi_done(struct hk_mhi *h)
{
	return ((rd(h, h->bhi + MHI_BHI_STATUS) >> 30) != 0 || h->failed);
}

static bool
bhie_done(struct hk_mhi *h)
{
	return ((rd(h, h->bhie + MHI_BHIE_TXVECSTATUS) >> 30) >= 2 || h->failed);
}

static bool
in_sbl(struct hk_mhi *h)
{
	return (h->ee == HK_MHI_EE_SBL || in_mission(h->ee) || h->failed);
}

static bool
mission(struct hk_mhi *h)
{
	return (h->mission || h->failed);
}

static bool
awake(struct hk_mhi *h)
{
	return (h->state == HK_MHI_STATE_M0 || h->failed);
}

static bool
mission_told(struct hk_mhi *h)
{
	return (h->mission_told || h->failed);
}

/* M0 as the device's register has it: h->state can still be the M0 of its boot loader's */
static bool
device_awake(struct hk_mhi *h)
{
	return (device_state(h) == HK_MHI_STATE_M0 || h->failed);
}

static uint32_t
transaction(struct hk_mhi *h)
{
	uint32_t number = 0;

	while (number == 0) {
		h->ops->random(h->context, &number, sizeof(number));
		number &= 0x3fffffff;
	}
	return (number);
}

/* The device's contexts and doorbells, given the device in READY. */
static void
setup_mmio(struct hk_mhi *h)
{
	uint32_t cfg;

	h->channel_doorbells = rd(h, MHI_REG_CHDBOFF);
	h->event_doorbells = rd(h, MHI_REG_ERDBOFF);
	/* the contexts as at the beginning: no channel enabled, the event rings full of room, the command ring empty */
	memset(h->channel_contexts, 0, (size_t)h->config.max_channels * MHI_CONTEXT);
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		uint8_t *ctx = channel_context(h, h->channels[c].config->number);

		put32(ctx + CTX_CFG, (h->channels[c].config->burst != 0 ? h->channels[c].config->burst : BURST_DISABLED) << 8);
		put32(ctx + CTX_TYPE, h->channels[c].config->inbound ? 2 : 1);
		put32(ctx + CTX_INDEX, h->channels[c].config->event_ring);
		h->channels[c].running = false;
	}
	for (unsigned e = 0; e < h->config.event_count; e++) {
		struct hk_mhi_ring *r = &h->events[e];
		uint8_t *ctx = event_context(h, e);

		if (h->config.events[e].offload) {
			memset(ctx, 0, MHI_CONTEXT);                    /* (another client's ring: left empty) */
			continue;
		}
		put32(ctx + CTX_CFG, h->config.events[e].moderation_ms << 16);
		put32(ctx + CTX_TYPE, EVENT_RING_VALID);
		put32(ctx + CTX_INDEX, h->config.events[e].vector);
		put64(ctx + CTX_RBASE, r->address);
		put64(ctx + CTX_RLEN, (uint64_t)r->elements * MHI_ELEMENT);
		put64(ctx + CTX_RP, r->address);
		r->rp = 0;
		r->wp = r->elements - 1;
		put64(ctx + CTX_WP, element_address(r, r->wp));
		h->event_busy[e] = h->event_again[e] = false;
	}
	memset(h->command_context, 0, MHI_CONTEXT);
	h->commands.rp = h->commands.wp = 0;
	put64(h->command_context + CTX_RBASE, h->commands.address);
	put64(h->command_context + CTX_RLEN, (uint64_t)h->commands.elements * MHI_ELEMENT);
	put64(h->command_context + CTX_RP, h->commands.address);
	put64(h->command_context + CTX_WP, h->commands.address);
	device_fence();

	wr(h, MHI_REG_CCABAP + 4, (uint32_t)(h->channel_contexts_address >> 32));
	wr(h, MHI_REG_CCABAP, (uint32_t)h->channel_contexts_address);
	wr(h, MHI_REG_ECABAP + 4, (uint32_t)(h->event_contexts_address >> 32));
	wr(h, MHI_REG_ECABAP, (uint32_t)h->event_contexts_address);
	wr(h, MHI_REG_CRCBAP + 4, (uint32_t)(h->command_context_address >> 32));
	wr(h, MHI_REG_CRCBAP, (uint32_t)h->command_context_address);
	wr(h, MHI_REG_CTRLBASE + 4, (uint32_t)(h->config.iova_start >> 32));
	wr(h, MHI_REG_CTRLBASE, (uint32_t)h->config.iova_start);
	wr(h, MHI_REG_CTRLLIMIT + 4, (uint32_t)(h->config.iova_stop >> 32));
	wr(h, MHI_REG_CTRLLIMIT, (uint32_t)h->config.iova_stop);
	wr(h, MHI_REG_DATABASE + 4, (uint32_t)(h->config.iova_start >> 32));
	wr(h, MHI_REG_DATABASE, (uint32_t)h->config.iova_start);
	wr(h, MHI_REG_DATALIMIT + 4, (uint32_t)(h->config.iova_stop >> 32));
	wr(h, MHI_REG_DATALIMIT, (uint32_t)h->config.iova_stop);
	cfg = rd(h, MHI_REG_MHICFG);
	wr(h, MHI_REG_MHICFG, (cfg & 0x0000ffffu) | h->config.event_count << 16 | h->config.hw_event_rings << 24);	/* event rings, the last hw_event_rings of them the hardware's (NER, NHWER) */
}

/* MHICTRL.RESET, and until the device has cleared it. 0, or ETIMEDOUT. */
static int
device_reset(struct hk_mhi *h)
{
	wr(h, MHI_REG_MHICTRL, MHI_CTRL_RESET);
	return (wait_for(h, reset_done, h->config.timeout_ms));
}

int
hk_mhi_power_up(struct hk_mhi *h)
{
	uint32_t ee, status;
	uint64_t began = now_ms(h);
	int error = 0;

	if (!h->allocated) {
		return (ENXIO);
	}
	h->ops->lock(h->context);
	h->powered = true;
	h->mission = h->failed = h->mission_told = false;
	h->wakes = 0;
	h->channel_doorbells = h->event_doorbells = 0;
	h->bhi = rd(h, MHI_REG_BHIOFF);
	h->bhie = rd(h, MHI_REG_BHIEOFF);
	if (h->bhi == 0xffffffffu || h->bhi == 0 || (h->bhie == 0 && !h->config.no_bhie)) {
		say(h, "mhi: the device has no BHI (BHIOFF %#x, BHIEOFF %#x): it does not answer", h->bhi, h->bhie);
		error = ENODEV;
		goto out;
	}
	for (uint32_t at = MHI_BHIE_RXVECADDR_LOW; at < MHI_BHIE_RXVECSTATUS; at += 4) {
		wr(h, h->bhie + at, 0);			/* (no crash dump is asked for) */
	}
	wr(h, h->bhi + MHI_BHI_INTVEC, 0);
	ee = device_ee(h);
	status = rd(h, MHI_REG_MHISTATUS);
	h->ee = ee;
	h->state = status >> 8 & 0xff;
	say(h, "mhi: power-up: EE %u, state %#x (MHISTATUS %#x)", ee, h->state, status);
	if (h->state == HK_MHI_STATE_SYS_ERR || (status & MHI_STATUS_SYSERR)) {
		if (device_reset(h) != 0) {
			say(h, "mhi: the device stays in its error state");
			error = EIO;
			goto out;
		}
		h->state = device_state(h);
		h->failed = false;
	}
	if (ee != HK_MHI_EE_PBL && ee != HK_MHI_EE_PTHRU && ee != HK_MHI_EE_EDL && !(h->config.no_bhie && ee == HK_MHI_EE_AMSS)) {
		say(h, "mhi: the device is in EE %u, not in its primary boot loader: it is not booted", ee);
		error = EIO;
		goto out;
	}
	if (ee == HK_MHI_EE_AMSS) {
		/* (no_bhie: the device is already in its mission, after its SBL: a second power-up, from MHI's reset; no boot loader to send) */
		say(h, "mhi: power-up in EE AMSS: MHICTRL %#x, MHISTATUS %#x", rd(h, MHI_REG_MHICTRL), rd(h, MHI_REG_MHISTATUS));
		goto ready_step;
	}

	/* 1. the secondary boot loader, through BHI */
	wr(h, h->bhi + MHI_BHI_STATUS, 0);
	wr(h, h->bhi + MHI_BHI_IMGADDR_HIGH, (uint32_t)(h->image_address >> 32));
	wr(h, h->bhi + MHI_BHI_IMGADDR_LOW, (uint32_t)h->image_address);
	wr(h, h->bhi + MHI_BHI_IMGSIZE, (uint32_t)(h->config.sbl_length < h->image_size ? h->config.sbl_length : h->image_size));
	device_fence();
	wr(h, h->bhi + MHI_BHI_IMGTXDB, transaction(h));
	if (wait_for(h, bhi_done, h->config.timeout_ms) != 0 || (rd(h, h->bhi + MHI_BHI_STATUS) >> 30) != 2) {
		say(h, "mhi: BHI: the boot loader was not taken (BHI_STATUS %#x, error %#x, EE %u)", rd(h, h->bhi + MHI_BHI_STATUS),
		    rd(h, h->bhi + MHI_BHI_ERRCODE), device_ee(h));
		error = EIO;
		goto out;
	}
	say(h, "mhi: BHI: the boot loader (%zu bytes) is taken, %llu ms in", h->config.sbl_length,
	    (unsigned long long)(now_ms(h) - began));

ready_step:
	/* 2. READY: the MMIO set up, the event rings given their room, M0 */
	if (wait_for(h, ready, h->config.timeout_ms) != 0) {
		say(h, "mhi: the device does not say READY (MHICTRL %#x, MHISTATUS %#x)", rd(h, MHI_REG_MHICTRL),
		    rd(h, MHI_REG_MHISTATUS));
		error = ETIMEDOUT;
		goto out;
	}
	setup_mmio(h);
	for (unsigned e = 0; e < h->config.event_count; e++) {
		if (!h->config.events[e].offload) {
			doorbell(h, h->event_doorbells + 8 * e, element_address(&h->events[e], h->events[e].wp));
		}
	}
	set_state(h, HK_MHI_STATE_M0);

	/* 3. in SBL: the whole image, through BHIe */
	if (wait_for(h, in_sbl, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device does not come to its secondary boot loader (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	if (h->config.no_bhie) {
		/* the SBL's own channels are the end of the boot: started once the device is in M0 */
		if (wait_for(h, awake, h->config.timeout_ms) != 0 || wait_for(h, device_awake, h->config.timeout_ms) != 0 || h->failed) {
			say(h, "mhi: the device is not in M0 (state %#x)", h->state);
			error = EIO;
			goto out;
		}
		for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
			if (h->channels[c].config->ee_mask & 1u << h->ee) {
				error = channel_start(h, &h->channels[c]);
			}
		}
		if (error == 0) {
			say(h, "mhi: the device is in EE %u, its channels for it are started: %llu ms", h->ee, (unsigned long long)(now_ms(h) - began));
		}
		goto out;
	}
	if (!in_mission(h->ee)) {
		uint32_t number = transaction(h);

		wr(h, h->bhie + MHI_BHIE_TXVECADDR_HIGH, (uint32_t)(h->vectors_address >> 32));
		wr(h, h->bhie + MHI_BHIE_TXVECADDR_LOW, (uint32_t)h->vectors_address);
		wr(h, h->bhie + MHI_BHIE_TXVECSIZE, (uint32_t)h->vectors_size);
		device_fence();
		wr(h, h->bhie + MHI_BHIE_TXVECDB, (rd(h, h->bhie + MHI_BHIE_TXVECDB) & ~0x3fffffffu) | number);
		if (wait_for(h, bhie_done, h->config.timeout_ms) != 0 || (rd(h, h->bhie + MHI_BHIE_TXVECSTATUS) >> 30) != 2) {
			say(h, "mhi: BHIe: the image was not taken (TXVECSTATUS %#x)", rd(h, h->bhie + MHI_BHIE_TXVECSTATUS));
			error = EIO;
			goto out;
		}
		say(h, "mhi: BHIe: the image (%zu bytes) is taken, %llu ms in", h->image_size, (unsigned long long)(now_ms(h) - began));
	}

	/* 4. the mission: the channels for it */
	if (wait_for(h, mission, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the image does not come to its mission (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	/*
	 * The register's EE turns to the mission before the image's MHI takes commands: a channel's
	 * start sent then was lost (no completion, ever). Linux waits for the device's event of the
	 * EE, and for M0, before it starts a channel; so here.
	 */
	if (wait_for(h, mission_told, 1000) != 0) {
		say(h, "mhi: no event of the device's for its mission in 1 s: its channels are started all the same");
	}
	if (wait_for(h, awake, h->config.timeout_ms) != 0 || wait_for(h, device_awake, h->config.timeout_ms) != 0 ||
	    h->failed) {
		say(h, "mhi: the device is not in M0 (state %#x)", h->state);
		error = EIO;
		goto out;
	}
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if (h->channels[c].config->ee_mask & 1u << h->ee) {
			error = channel_start(h, &h->channels[c]);
		}
	}
	if (error == 0) {
		say(h, "mhi: the device runs its image (EE %u), its channels are started: %llu ms (doorbells rung again: %u)",
		    h->ee, (unsigned long long)(now_ms(h) - began), h->rung_again);
	}
out:
	if (error != 0) {
		h->powered = false;
	}
	h->ops->unlock(h->context);
	return (error);
}

/*
 * A debugging look at a channel: the device's pointers in its context, the host's own.
 */
void
hk_mhi_channel_state(struct hk_mhi *h, uint32_t number, uint32_t *context_state, uint64_t *context_rp, uint64_t *context_wp, uint32_t *host_rp, uint32_t *host_wp, uint32_t *elements)
{
	struct hk_mhi_channel *ch = channel_numbered(h, number);
	uint8_t *ctx = channel_context(h, number);
	uint32_t word0;

	memcpy(&word0, ctx, 4);
	*context_state = word0 & 0xff;
	*context_rp = get64(ctx + CTX_RP);
	*context_wp = get64(ctx + CTX_WP);
	*host_rp = ch != NULL ? ch->ring.rp : ~0u;
	*host_wp = ch != NULL ? ch->ring.wp : ~0u;
	*elements = ch != NULL ? ch->ring.elements : 0;
}

/*
 * MHI_CMD_SFR_CFG (Android's mhi_init_sfr, sent as the mission begins): a buffer of ours where the device writes its subsystem failure reason when it crashes.
 * Command element: the buffer's address, the length less one, type 73. 0, or an errno.
 */
int
hk_mhi_sfr_config(struct hk_mhi *h, uint64_t address, uint32_t length)
{
	uint32_t at;
	uint64_t until;
	int error = 0;

	if (!h->allocated || !h->powered) {
		return (ENXIO);
	}
	h->ops->lock(h->context);
	at = h->commands.wp;
	if (next(&h->commands, at) == h->commands.rp) {
		h->ops->unlock(h->context);
		return (EBUSY);
	}
	write_element(element(&h->commands, at), address, length - 1, 73u << 16);
	h->commands.wp = next(&h->commands, at);
	put64(h->command_context + CTX_WP, element_address(&h->commands, h->commands.wp));
	h->sfr_status = 0;
	doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
	for (until = now_ms(h) + h->config.timeout_ms; h->sfr_status == 0 && !h->failed;) {
		service_all(h);
		if (h->sfr_status != 0 || now_ms(h) > until) {
			break;
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
	if (h->sfr_status != MHI_CODE_SUCCESS) {
		say(h, "mhi: the failure reason buffer's command: completion code %#x", h->sfr_status);
		error = h->sfr_status == 0 ? ETIMEDOUT : EIO;
	}
	h->ops->unlock(h->context);
	return (error);
}

/*
 * For a host with no_bhie: the device has come out of its SBL into its mission (EE AMSS: the Sahara conversation is over); its channels for that environment are started.
 * 0, or an errno.
 */
int
hk_mhi_start_mission(struct hk_mhi *h)
{
	int error = 0;

	if (!h->allocated || !h->powered) {
		return (ENXIO);
	}
	h->ops->lock(h->context);
	if (wait_for(h, mission, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device does not come to its mission (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	if (wait_for(h, mission_told, 1000) != 0) {
		say(h, "mhi: no event of the device's for its mission in 1 s: its channels are started all the same");
	}
	if (wait_for(h, awake, h->config.timeout_ms) != 0 || wait_for(h, device_awake, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device is not in M0 (state %#x)", h->state);
		error = EIO;
		goto out;
	}
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if ((h->channels[c].config->ee_mask & 1u << h->ee) && !h->channels[c].running) {
			error = channel_start(h, &h->channels[c]);
		}
	}
	if (error == 0) {
		say(h, "mhi: the device runs its image (EE %u), its channels for it are started", h->ee);
	}
out:
	h->ops->unlock(h->context);
	return (error);
}

void
hk_mhi_power_down(struct hk_mhi *h)
{
	if (!h->allocated) {
		return;
	}
	h->ops->lock(h->context);
	if (h->powered && rd(h, MHI_REG_MHICTRL) != 0xffffffffu && device_reset(h) != 0) {
		say(h, "mhi: power-down: the device does not finish its reset (MHICTRL %#x)", rd(h, MHI_REG_MHICTRL));
	}
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		h->channels[c].running = false;
	}
	h->powered = h->mission = h->mission_told = false;
	h->channel_doorbells = h->event_doorbells = 0;
	h->wakes = 0;
	h->ops->unlock(h->context);
}

/* ---- sending -------------------------------------------------------------------------------------- */

int
hk_mhi_send(struct hk_mhi *h, unsigned number, const void *data, size_t length)
{
	struct hk_mhi_channel *ch;
	uint64_t until;
	uint32_t at;
	int error;

	h->ops->lock(h->context);
	ch = channel_numbered(h, number);
	if (ch == NULL || ch->config->inbound || !ch->running || h->failed) {
		h->ops->unlock(h->context);
		return (ENOTCONN);
	}
	if (length == 0 || length > ch->buffer_length) {
		h->ops->unlock(h->context);
		return (EMSGSIZE);
	}
	for (until = now_ms(h) + h->config.timeout_ms; next(&ch->ring, ch->ring.wp) == ch->ring.rp;) {
		service_all(h);
		if (now_ms(h) > until || !ch->running) {
			h->ops->unlock(h->context);
			return (ETIMEDOUT);
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
	at = ch->ring.wp;
	memcpy(ch->buffers + (size_t)at * ch->buffer_length, data, length);
	write_element(element(&ch->ring, at), ch->buffers_address + (uint64_t)at * ch->buffer_length, (uint32_t)length,
	    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
	ch->ring.wp = next(&ch->ring, at);
	put64(channel_context(h, number) + CTX_WP, element_address(&ch->ring, ch->ring.wp));
	h->ops->unlock(h->context);
	/* (rung in M0 only: the device is woken for it, and left to sleep again) */
	if ((error = hk_mhi_wake_get(h)) == 0) {
		h->ops->lock(h->context);
		doorbell(h, h->channel_doorbells + 8 * number, element_address(&ch->ring, ch->ring.wp));
		h->ops->unlock(h->context);
	}
	hk_mhi_wake_put(h);
	return (error);
}

/* ---- wake votes, suspend ---------------------------------------------------------------------------- */

int
hk_mhi_wake_get(struct hk_mhi *h)
{
	int error = 0;

	h->ops->lock(h->context);
	if (!h->powered || h->channel_doorbells == 0) {
		h->ops->unlock(h->context);
		return (0);
	}
	if (h->wakes++ == 0) {
		doorbell(h, h->channel_doorbells + 8 * MHI_WAKE_DOORBELL, 1);
	}
	if (h->state == HK_MHI_STATE_M3) {
		;				/* (suspended: hk_mhi_resume() is what brings it back) */
	} else if (h->state != HK_MHI_STATE_M0) {
		error = wait_for(h, awake, h->config.timeout_ms);
	}
	h->ops->unlock(h->context);
	return (error);
}

void
hk_mhi_wake_put(struct hk_mhi *h)
{
	h->ops->lock(h->context);
	if (h->powered && h->channel_doorbells != 0 && h->wakes != 0 && --h->wakes == 0) {
		doorbell(h, h->channel_doorbells + 8 * MHI_WAKE_DOORBELL, 0);
	}
	h->ops->unlock(h->context);
}

static bool
suspended(struct hk_mhi *h)
{
	return (h->state == HK_MHI_STATE_M3 || h->failed);
}

int
hk_mhi_suspend(struct hk_mhi *h)
{
	int error;

	if ((error = hk_mhi_wake_get(h)) != 0) {
		hk_mhi_wake_put(h);
		return (error);
	}
	h->ops->lock(h->context);
	set_state(h, HK_MHI_STATE_M3);
	error = wait_for(h, suspended, h->config.timeout_ms);
	if (error == 0 && h->failed) {
		error = EIO;
	}
	h->ops->unlock(h->context);
	hk_mhi_wake_put(h);
	return (error);
}

int
hk_mhi_resume(struct hk_mhi *h)
{
	int error;

	h->ops->lock(h->context);
	if (!h->powered) {
		h->ops->unlock(h->context);
		return (ENXIO);
	}
	set_state(h, HK_MHI_STATE_M0);
	error = wait_for(h, awake, h->config.timeout_ms);
	if (error == 0 && h->failed) {
		error = EIO;
	}
	h->ops->unlock(h->context);
	return (error);
}

static bool
in_rddm(struct hk_mhi *h)
{
	return (device_ee(h) == HK_MHI_EE_RDDM);
}

int
hk_mhi_force_rddm(struct hk_mhi *h)
{
	int error;

	h->ops->lock(h->context);
	wr(h, MHI_REG_SOC_RESET, 1);
	error = wait_for(h, in_rddm, h->config.timeout_ms);
	h->ops->unlock(h->context);
	return (error);
}
