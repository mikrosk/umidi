/*
 * Copyright 2026 Miro Kropacek <miro.kropacek@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef UMIDI_H
#define UMIDI_H

#define UMIDI_VERSION 1

#include "SDL_config.h"

#ifdef __MINT__

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mint/osbind.h>
#include <mint/ostruct.h>

#include "../SDL_mixer.h"

#include "native_midi.h"
#include "native_midi_common.h"

/* highest volume for UMidiSetVolume() */
#define UMIDI_MAX_VOLUME 128

#define UMIDI_FIFO_SIZE			2048		/* power of two */
#define UMIDI_FIFO_MASK			(UMIDI_FIFO_SIZE - 1)

/* MIDI messages to be sent at the same time */
typedef struct {
	uint32_t time;	/* microseconds from the song start */
	uint32_t end;	/* offset just past the last byte of the block in the song data */
} UMidiBlock;

/* channel messages with their status bytes and complete system exclusive
 * messages, each shorter than UMIDI_FIFO_SIZE */
typedef struct {
	const UMidiBlock* blocks;	/* sorted by time */
	const uint8_t* data;		/* messages of all blocks */
	uint32_t numBlocks;
	uint32_t loopBlock;			/* block the repeats start at */
} UMidiSong;

typedef struct {
	/* Only one song plays at a time; the playback state lives here and not
	 * in UMidiSong. */
	const UMidiSong * volatile song;	/* NULL when stopped */
	volatile uint32_t curBlock;			/* next block to queue */
	uint32_t curPos;					/* offset of the next message to queue */
	volatile int loops;

	/* song position in microseconds and its 16-bit fraction */
	uint32_t now;
	uint32_t nowFrac;

	uint8_t fifo[UMIDI_FIFO_SIZE];
	volatile uint16_t fifoHead, fifoTail;

	uint8_t lastCC7[16];		/* channel volume as sent by the song */
	volatile uint8_t volume;	/* master volume */
	volatile int volDirty;		/* set by UMidiSetVolume(), consumed by the producer */

	volatile int tie;			/* shadow: is the TX interrupt enabled? */
	_KBDVECS *kbdvecs;

	uint32_t oldTimerbVec;
} UMidiContext;

#ifndef UMIDI_DEF
/* a translation unit doesn't have to use all of them */
#define UMIDI_DEF	static __attribute__((__unused__))
#endif	/* UMIDI_DEF */

UMIDI_DEF int UMidiOpen(UMidiContext* ctx);
UMIDI_DEF void UMidiClose(UMidiContext* ctx);
UMIDI_DEF void UMidiPlay(UMidiContext* ctx, const UMidiSong* song, int loops);
UMIDI_DEF void UMidiStop(UMidiContext* ctx);
UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx);
UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume);

/******************************************************************************/

/*
 * Two interrupt sources, one FIFO between them:
 *
 * 1. Timer B is the PRODUCER, running at a constant 960 Hz (2457600 /
 *    16 / 160), i.e. ~1 ms timing resolution. Each interrupt advances
 *    the song position by one timer period and pushes the messages of
 *    the blocks that became due into the FIFO. Block times are in
 *    microseconds, so tempo changes are already part of them. While the
 *    FIFO is full, due blocks wait and then go out in a burst; the song
 *    position keeps advancing. Once no song plays and the FIFO is empty,
 *    the timer disables its own interrupt; playing a song enables it
 *    again.
 *
 * 2. The MIDI ACIA transmit interrupt is the CONSUMER, draining the
 *    FIFO at wire speed (31250 baud / 10 bits = 3125 bytes/s), one
 *    interrupt per byte and none when idle. Both ACIAs share MFP GPIP
 *    I4 (channel 6, vector $118); the TOS handler ("midikey") loops
 *    calling kb_midisys and kb_ikbdsys until the interrupt line is
 *    released, so we hook kb_midisys via Kbdvbase() and chain the
 *    original for the receive side. The transmit interrupt (TIE) is
 *    enabled ONLY while the FIFO is non-empty: an empty transmit
 *    register asserts IRQ permanently, and since nothing would clear
 *    it, the midikey loop would hang the machine. The hook therefore
 *    always either sends a byte or turns TIE off before returning.
 *
 * Volume: a MIDI song controls 16 channel volumes (controller 7) but
 * the wire has no master volume, so UMidiSetVolume() implements one by
 * scaling. lastCC7[] holds each channel volume as the song last set
 * it: the producer updates it from controller 7 events and transmits
 * lastCC7[channel] * volume / UMIDI_MAX_VOLUME. UMidiSetVolume()
 * stores volume, sets volDirty and returns; the producer then queues
 * controller 7 messages with the recomputed values for all 16 channels.
 */

#define UMIDI_ACIA_CTRL			(*(volatile uint8_t *)0xFFFFFC04L)	/* MIDI ACIA control (write) and status (read) register */
#define UMIDI_ACIA_DATA			(*(volatile uint8_t *)0xFFFFFC06L)	/* MIDI ACIA data register */
#define UMIDI_ACIA_TDRE			(1 << 1)							/* ACIA status: transmit data register empty */

/* /16 clock, 8N1, RTS low, TX interrupt off, RX interrupt on.*/
#define UMIDI_ACIA_CTRL_TIE_OFF	0x95
/* /16 clock, 8N1, RTS low, TX interrupt on, RX interrupt on.*/
#define UMIDI_ACIA_CTRL_TIE_ON	0xB5

#define UMIDI_MFP_IERA			(*(volatile uint8_t *)0xFFFFFA07L)	/* MFP interrupt enable register A */
#define UMIDI_MFP_ISRA			(*(volatile uint8_t *)0xFFFFFA0FL)	/* MFP interrupt in-service register A */

#define UMIDI_MFP_CLOCK			2457600UL
#define UMIDI_TIMER_B_CTRL		3		/* prescaler /16 */
#define UMIDI_TIMER_B_PRESCALER	16
#define UMIDI_TIMER_B_DATA		160		/* 2457600/16/160 = 960 Hz */

/* Timer B period in microseconds, 16.16 fixed point */
#define UMIDI_TIMER_B_PERIOD	((uint32_t)((65536ULL \
									* UMIDI_TIMER_B_PRESCALER \
									* UMIDI_TIMER_B_DATA \
									* 1000000 \
									+ UMIDI_MFP_CLOCK/2) / UMIDI_MFP_CLOCK))

#define UMIDI_DEFAULT_TEMPO		500000UL	/* us per quarter note (120 bpm) */
#define UMIDI_DEFAULT_CC7		100			/* GM default channel volume */

/* the interrupt handlers and Supexec() routines take no arguments */
static UMidiContext *UMidiActiveContext;
static int UMidiHooked;	/* whether the hook is in the kb_midisys chain */

static __inline__ uint16_t UMidiDisableInterrupts(void) {
	uint16_t sr;
#ifdef __mcoldfire__
	/* ColdFire has no ori to SR; this runs in supervisor mode, so
	 * setting the S bit along with the mask is safe */
	__asm__ volatile ("move.w %%sr,%0\n\tmove.w #0x2700,%%sr" : "=d"(sr) : : "memory");
#else
	__asm__ volatile ("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory");
#endif
	return sr;
}

static __inline__ void UMidiRestoreInterrupts(uint16_t sr) {
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "memory");
}

static __inline__ uint8_t UMidiScaledVolume(const UMidiContext *ctx, uint8_t chan) {
	return (uint16_t)(ctx->lastCC7[chan] * ctx->volume) / UMIDI_MAX_VOLUME;
}

static __inline__ uint16_t UMidiFifoSpace(const UMidiContext *ctx) {
	return UMIDI_FIFO_SIZE - 1 - ((ctx->fifoHead - ctx->fifoTail) & UMIDI_FIFO_MASK);
}

static __inline__ void UMidiFifoPush(UMidiContext *ctx, uint8_t b) {
	ctx->fifo[ctx->fifoHead] = b;
	ctx->fifoHead = (ctx->fifoHead + 1) & UMIDI_FIFO_MASK;
}

/* Returns the length of a message of the song data */
static __inline__ uint16_t UMidiMessageLength(const uint8_t *msg) {
	switch (msg[0] & 0xf0) {
		case 0xc0:		/* program change */
		case 0xd0:		/* channel pressure */
			return 2;

		case 0xf0: {	/* system exclusive */
			uint16_t length = 2;

			while (msg[length - 1] != 0xf7)
				length++;
			return length;
		}

		default:
			return 3;
	}
}

/* the song played to its end and doesn't repeat */
static __inline__ int UMidiSongEnded(const UMidiContext *ctx) {
	return ctx->curBlock == ctx->song->numBlocks
		&& (!ctx->loops || ctx->song->loopBlock >= ctx->song->numBlocks);
}

/* End of a Timer B interrupt: arm the ACIA TX interrupt for queued bytes,
 * or disable the Timer B interrupt once there is nothing left to do */
static __inline__ void UMidiTransmit(UMidiContext *ctx, int idle) {
	if (ctx->fifoHead != ctx->fifoTail) {
		/* bytes queued and TX interrupt not armed: arm it. If TDRE is
		 * already set this asserts the ACIA IRQ at once (falling edge on
		 * GPIP I4), so transmission starts right after we return. */
		if (!ctx->tie) {
			ctx->tie = 1;
			UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_ON;
		}
	} else if (idle) {
		/* nothing to send and nothing to do: disable the Timer B
		 * interrupt, starting playback enables it again */
		UMIDI_MFP_IERA &= (uint8_t)~(1 << 0);
	}
}

/* Producer: queue the messages of the due blocks into the FIFO, arm the
 * ACIA TX interrupt */
static void __attribute__((interrupt)) UMidiPlayerInterrupt(void) {
	UMidiContext *ctx = UMidiActiveContext;
	const UMidiSong *song = ctx->song;
	int looped = 0;

	if (!song)
		goto transmit;

	/* advance song position */
	ctx->nowFrac += UMIDI_TIMER_B_PERIOD & 0xffff;
	ctx->now += (UMIDI_TIMER_B_PERIOD >> 16) + (ctx->nowFrac >> 16);
	ctx->nowFrac &= 0xffff;

	/* queue due blocks */
	for (;;) {
		if (ctx->curBlock == song->numBlocks) {
			/* repeats continue at the loop block, at most once per timer
			 * period: a song without any delay would loop forever otherwise */
			if (UMidiSongEnded(ctx) || looped)
				break;
			if (ctx->loops > 0)
				ctx->loops--;
			looped = 1;

			ctx->curBlock	= song->loopBlock;
			ctx->curPos		= song->loopBlock ? song->blocks[song->loopBlock - 1].end : 0;
			ctx->now		= song->blocks[song->loopBlock].time;
			ctx->nowFrac	= 0;
		}

		if (song->blocks[ctx->curBlock].time > ctx->now)
			break;

		while (ctx->curPos < song->blocks[ctx->curBlock].end) {
			const uint8_t *msg = song->data + ctx->curPos;
			const uint16_t length = UMidiMessageLength(msg);
			uint16_t i;

			if (UMidiFifoSpace(ctx) < length)
				goto fifo_full;

			if ((msg[0] & 0xf0) == 0xb0 && msg[1] == 0x07) {
				/* Channel volume: remember it and send it scaled
				 * by the master volume */
				ctx->lastCC7[msg[0] & 0x0f] = msg[2];
				UMidiFifoPush(ctx, msg[0]);
				UMidiFifoPush(ctx, 0x07);
				UMidiFifoPush(ctx, UMidiScaledVolume(ctx, msg[0] & 0x0f));
			} else {
				for (i = 0; i < length; ++i)
					UMidiFifoPush(ctx, msg[i]);
			}
			ctx->curPos += length;
		}
		ctx->curBlock++;
	}
fifo_full:

	/* Master volume change: resend the scaled channel volumes */
	if (ctx->volDirty && UMidiFifoSpace(ctx) >= 16 * 3) {
		int i;

		ctx->volDirty = 0;
		for (i = 0; i < 16; ++i) {
			UMidiFifoPush(ctx, 0xb0 | i);
			UMidiFifoPush(ctx, 0x07);
			UMidiFifoPush(ctx, UMidiScaledVolume(ctx, i));
		}
	}

transmit:
	UMidiTransmit(ctx, !ctx->song || UMidiSongEnded(ctx));

	UMIDI_MFP_ISRA = ~(1 << 0);	/* clear in service bit */
}

#define UMIDI_XBRA_MAGIC		0x58425241L		/* 'XBRA' */
#define UMIDI_XBRA_ID			0x754D4944L		/* 'uMID' */

/* XBRA header in front of a system vector routine, linking the routines
 * installed on the vector */
typedef struct {
	long magic;
	long id;
	long (*old)(void);
} UMidiXbra;

#if !defined(__GNUC__) || __GNUC__ < 8
#error "patchable_function_entry needs GCC 8 or newer"
#endif

/* kb_midisys hook: called (jsr, supervisor) by the TOS $118 handler on
 * every ACIA interrupt, in a loop until the interrupt line is released.
 * Send one byte per TDRE, disarm TIE the moment the FIFO runs dry --
 * leaving TIE armed with nothing to send would hang the midikey loop.
 * Then the previous routine gets the receive side.
 *
 * The compiler leaves room for the XBRA header right in front of the
 * routine (two bytes per NOP), UMidiHookMidisys() fills it in.
 *
 * No specification defines which registers a kbdvecs routine may
 * clobber, but both Atari TOS and EmuTOS save d0-d3/a0-a3 around these
 * calls, and a C function clobbers no more than d0-d2/a0-a1 (d2 is a
 * scratch register with -mfastcall). */
static void __attribute__((patchable_function_entry(sizeof(UMidiXbra) / 2, sizeof(UMidiXbra) / 2))) UMidiSysHandler(void) {
	UMidiContext *ctx = UMidiActiveContext;

	/* no context: closed, but a routine without an XBRA header kept the
	 * hook in the chain */
	if (ctx && ctx->tie && (UMIDI_ACIA_CTRL & UMIDI_ACIA_TDRE)) {
		if (ctx->fifoHead != ctx->fifoTail) {
			UMIDI_ACIA_DATA = ctx->fifo[ctx->fifoTail];
			ctx->fifoTail = (ctx->fifoTail + 1) & UMIDI_FIFO_MASK;
		} else {
			/* nothing to send: disable the ACIA transmit interrupt,
			 * UMidiTransmit() enables it again */
			UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;
			ctx->tie = 0;
		}
	}

	((UMidiXbra *)((long)UMidiSysHandler - (long)sizeof(UMidiXbra)))->old();
}

static UMidiXbra *UMidiSysXbra(void) {
	return (UMidiXbra *)((long)UMidiSysHandler - (long)sizeof(UMidiXbra));
}

static long UMidiHookMidisys(void) {
	UMidiContext *ctx = UMidiActiveContext;
	uint16_t sr = UMidiDisableInterrupts();
	UMidiXbra *xbra;
	long vec;
	long ret = -1;

	/* another copy of uMIDI has already hooked kb_midisys */
	for (vec = (long)ctx->kbdvecs->midisys; vec; vec = (long)xbra->old) {
		xbra = (UMidiXbra *)(vec - (long)sizeof(UMidiXbra));
		if (xbra->magic != UMIDI_XBRA_MAGIC)
			break;
		if (xbra->id == UMIDI_XBRA_ID && vec != (long)UMidiSysHandler)
			goto out;
	}

	/* still linked when the last unhook failed */
	if (!UMidiHooked) {
		xbra = UMidiSysXbra();
		xbra->magic = UMIDI_XBRA_MAGIC;
		xbra->id = UMIDI_XBRA_ID;
		xbra->old = ctx->kbdvecs->midisys;
		ctx->kbdvecs->midisys = (long (*)(void))UMidiSysHandler;
		UMidiHooked = 1;
	}
	ret = 0;

out:
	UMidiRestoreInterrupts(sr);

	return ret;
}

static long UMidiUnhookMidisys(void) {
	UMidiContext *ctx = UMidiActiveContext;
	uint16_t sr = UMidiDisableInterrupts();
	long *link;

	/* unlink the hook from the XBRA chain of kb_midisys; a routine
	 * without an XBRA header in front of it keeps it linked */
	link = (long *)&ctx->kbdvecs->midisys;
	while (*link) {
		UMidiXbra *xbra;

		if (*link == (long)UMidiSysHandler) {
			*link = (long)UMidiSysXbra()->old;
			UMidiHooked = 0;
			break;
		}
		xbra = (UMidiXbra *)(*link - (long)sizeof(UMidiXbra));
		if (xbra->magic != UMIDI_XBRA_MAGIC)
			break;
		link = (long *)&xbra->old;
	}

	UMidiRestoreInterrupts(sr);

	return 0;
}

/* Silence every channel through the XBIOS, which never writes the ACIA
 * control register. A status byte aborts a possibly half-transmitted
 * message, so the stream stays parseable. */
static void UMidiSilence(void) {
	uint8_t msg[16 * 6];
	int i;

	for (i = 0; i < 16; ++i) {
		msg[i * 6 + 0] = 0xb0 | i;
		msg[i * 6 + 1] = 0x78;	/* All Sound Off */
		msg[i * 6 + 2] = 0x00;
		msg[i * 6 + 3] = 0xb0 | i;
		msg[i * 6 + 4] = 0x7b;	/* All Notes Off */
		msg[i * 6 + 5] = 0x00;
	}
	Midiws(sizeof(msg) - 1, msg);
}

UMIDI_DEF int UMidiOpen(UMidiContext* ctx) {
	if (ctx == UMidiActiveContext)
		return 0;	/* already open */

	memset(ctx, 0, sizeof(*ctx));
	ctx->volume = UMIDI_MAX_VOLUME;

	if (UMidiActiveContext)
		return 0;	/* another context is open */

	/* kb_midisys has no XBIOS function, it's changed in supervisor mode */
	ctx->kbdvecs = Kbdvbase();
	UMidiActiveContext = ctx;
	if (Supexec(UMidiHookMidisys) != 0) {
		UMidiActiveContext = NULL;
		ctx->kbdvecs = NULL;
		return 0;
	}

	/* Timer B through the XBIOS; playing installs its interrupt */
	Jdisint(MFP_TIMERB);
	ctx->oldTimerbVec = (uint32_t)Setexc(0x120>>2, -1);

	return 1;
}

UMIDI_DEF void UMidiClose(UMidiContext* ctx) {
	if (!ctx->kbdvecs)
		return;

	UMidiStop(ctx);

	/* stop Timer B and put its previous vector back; Xbtimer() enables
	 * the interrupt, so it's disabled afterwards */
	Xbtimer(XB_TIMERB, 0, 0, ctx->oldTimerbVec);
	Jdisint(MFP_TIMERB);

	Supexec(UMidiUnhookMidisys);
	ctx->kbdvecs = NULL;
	UMidiActiveContext = NULL;
}

UMIDI_DEF void UMidiPlay(UMidiContext* ctx, const UMidiSong* song, int loops) {
	int i;

	if (!ctx->kbdvecs)
		return;	/* not open */

	if (ctx->song)
		UMidiStop(ctx);

	ctx->curBlock	= 0;
	ctx->curPos		= 0;
	ctx->now		= 0;
	ctx->nowFrac	= 0;

	for (i = 0; i < 16; ++i)
		ctx->lastCC7[i] = UMIDI_DEFAULT_CC7;

	ctx->loops = loops;	/* number of repeats after the first pass; -1 = forever */

	/* compiler barrier: all of the song is stored before the pointer the
	 * timer interrupt checks */
	__asm__ volatile ("" : : : "memory");
	ctx->song = song;
	Xbtimer(XB_TIMERB, UMIDI_TIMER_B_CTRL, UMIDI_TIMER_B_DATA, UMidiPlayerInterrupt);
}

UMIDI_DEF void UMidiStop(UMidiContext* ctx) {
	if (!ctx->song)
		return;

	/* drop the queue: nothing queues data any more, and the TX interrupt
	 * can't split this store; it turns itself off on the empty FIFO */
	ctx->song = NULL;
	ctx->fifoTail = ctx->fifoHead;
	while (ctx->tie)
		;

	UMidiSilence();
}

UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx) {
	return ctx->song
		&& (!UMidiSongEnded(ctx) || ctx->fifoHead != ctx->fifoTail);
}

UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume) {
	if (volume < 0)
		volume = 0;
	else if (volume > UMIDI_MAX_VOLUME)
		volume = UMIDI_MAX_VOLUME;

	ctx->volume = volume;
	ctx->volDirty = 1;
}

struct _NativeMidiSong {
	UMidiSong song;
};

static UMidiContext s_ctx;
static int s_volume = MIX_MAX_VOLUME;

/* Builds the blocks of a song from SDL_mixer's event list: channel
 * messages only, their tick times turned into microseconds with the tempo
 * changes applied. An empty block at the time of the last event (end of
 * track) keeps the length of the song. */
static int native_midi_convert(UMidiSong *song, const MIDIEvent *events, Uint16 division) {
	const MIDIEvent *ev;
	UMidiBlock *blocks;
	uint8_t *data;
	uint32_t numEvents = 0, numBlocks = 0, size = 0;
	uint32_t tick = 0, time = 0, rem = 0;
	uint32_t usPerTick, usRem;

	if (!division)
		return 0;

	for (ev = events; ev; ev = ev->next)
		numEvents++;

	/* at most one block per event plus the end of track, at most three
	 * bytes per event */
	blocks = (UMidiBlock *)malloc((numEvents + 1) * sizeof(*blocks));
	data = (uint8_t *)malloc(numEvents * 3 + 1);
	if (!blocks || !data) {
		free(blocks);
		free(data);
		return 0;
	}

	usPerTick = UMIDI_DEFAULT_TEMPO / division;
	usRem = UMIDI_DEFAULT_TEMPO % division;

	for (ev = events; ev; ev = ev->next) {
		const uint8_t status = ev->status;
		const uint32_t delta = ev->time - tick;
		uint32_t length;

		/* ticks to microseconds; the remainder carries over, so no
		 * rounding error accumulates, and stays below division squared */
		tick = ev->time;
		time += delta * usPerTick + delta / division * usRem;
		rem += delta % division * usRem;
		if (rem >= division) {
			time += rem / division;
			rem %= division;
		}

		if (status == 0xff) {
			/* meta event: only a tempo change matters */
			if (ev->data[0] == 0x51 && ev->extraLen == 3) {
				uint32_t tempo = ((uint32_t)ev->extraData[0] << 16)
					| ((uint32_t)ev->extraData[1] << 8)
					| (uint32_t)ev->extraData[2];
				if (tempo) {
					usPerTick = tempo / division;
					usRem = tempo % division;
				}
			}
			continue;
		}
		if (status >= 0xf0)
			continue;	/* system exclusive: skipped */

		if (numBlocks == 0 || time != blocks[numBlocks - 1].time) {
			blocks[numBlocks].time = time;
			numBlocks++;
		}

		length = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 2 : 3;
		data[size] = status;
		data[size + 1] = ev->data[0];
		if (length == 3)
			data[size + 2] = ev->data[1];
		size += length;
		blocks[numBlocks - 1].end = size;
	}

	if (numBlocks == 0 || time != blocks[numBlocks - 1].time) {
		blocks[numBlocks].time = time;
		blocks[numBlocks].end = size;
		numBlocks++;
	}

	song->blocks = blocks;
	song->data = data;
	song->numBlocks = numBlocks;
	song->loopBlock = 0;

	return 1;
}

void native_midi_start(NativeMidiSong *song, int loops) {
	printf("%s: %d\n", __FUNCTION__, loops);

	UMidiClose(&s_ctx);
	if (UMidiOpen(&s_ctx)) {
		UMidiSetVolume(&s_ctx, s_volume);
		UMidiPlay(&s_ctx, &song->song, loops);
	}
}

void native_midi_stop() {
	printf("%s\n", __FUNCTION__);

	UMidiClose(&s_ctx);
}

int native_midi_active() {
	return UMidiIsPlaying(&s_ctx);
}

void native_midi_setvolume(int volume) {
	printf("%s: %d\n", __FUNCTION__, volume);

	s_volume = volume;
	UMidiSetVolume(&s_ctx, volume);
}

int native_midi_detect() {
	return 1;	/* always available */
}

NativeMidiSong *native_midi_loadsong_RW(SDL_RWops *rw, int freerw) {
	NativeMidiSong *song;
	MIDIEvent *events;
	Uint16 division;

	printf("%s\n", __FUNCTION__);

	song = (NativeMidiSong *)malloc(sizeof(*song));
	if (song) {
		events = CreateMIDIEventList(rw, &division);
		if (!events || !native_midi_convert(&song->song, events, division)) {
			free(song);
			song = NULL;
		}
		if (events)
			FreeMIDIEventList(events);
	}

	if (freerw)
		SDL_RWclose(rw);

	return song;
}

void native_midi_freesong(NativeMidiSong *song) {
	UMidiContext *ctx = &s_ctx;

	printf("%s\n", __FUNCTION__);

	if (!song)
		return;

	if (&song->song == ctx->song)
		native_midi_stop();

	free((void *)song->song.blocks);
	free((void *)song->song.data);
	free(song);
}

const char *native_midi_error(void) {
	return "";
}

#endif	/* __MINT__ */

#endif
