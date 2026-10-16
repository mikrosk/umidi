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

struct _NativeMidiSong {
	MIDIEvent *firstEvent;
	uint16_t ticksPerQuarterNote; /* 48 by default (independent of the tempo) */
};

typedef struct {
	/* Only one song plays at a time; the playback state lives here and not
	 * in NativeMidiSong. */
	NativeMidiSong * volatile currentSong;	/* NULL when stopped */
	MIDIEvent * volatile events;			/* next event to schedule */
	uint32_t ticksPerQuarterNote;
	int loops;

	/* 16.16 fixed point: upper 16 bits integer part, lower 16 bits fraction.
	 * Every timer interrupt executes tickFrac += tickAdd; whenever
	 * tickFrac crosses 1.0, the whole part moves into tickInt and only
	 * the fraction stays. */
	uint32_t tickAdd;
	uint32_t tickFrac;
	uint32_t tickInt;

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
UMIDI_DEF void UMidiPlay(UMidiContext* ctx, NativeMidiSong* song, int loops);
UMIDI_DEF void UMidiStop(UMidiContext* ctx);
UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx);
UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume);

/******************************************************************************/

/*
 * Two interrupt sources, one FIFO between them:
 *
 * 1. Timer B is the PRODUCER, running at a constant 960 Hz (2457600 /
 *    16 / 160), i.e. ~1 ms event timing resolution. Each interrupt adds
 *    a 16.16 fixed-point increment (MIDI ticks per timer period, derived
 *    from ticksPerQuarterNote and the current tempo) to the song
 *    position and pushes the bytes of events that became due into the
 *    FIFO. A tempo change merely recomputes the increment, so any tempo
 *    and any tick rate are representable exactly. A full FIFO pauses
 *    event processing until space frees up.
 *    Once no song plays and the FIFO is empty, the timer disables its
 *    own interrupt; playing a song enables it again.
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

#define UMIDI_TICK_NUMER		((uint32_t)((65536ULL \
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

/* Returns how many MIDI ticks elapse during one timer period, as 16.16
 * fixed point: 65536 * ticksPerQuarterNote * period_us / tempo_us. */
static __inline__ uint32_t UMidiTicksPerPeriod(const UMidiContext *ctx, uint32_t tempo) {
	uint32_t q, r;

	q = UMIDI_TICK_NUMER / tempo;
	r = UMIDI_TICK_NUMER % tempo;

	return ctx->ticksPerQuarterNote * q
		+ (ctx->ticksPerQuarterNote * r + tempo / 2) / tempo;
}

/* Producer: schedule due events into the FIFO, arm the ACIA TX interrupt */
static void __attribute__((interrupt)) UMidiTimerHandler(void) {
	UMidiContext *ctx = UMidiActiveContext;
	MIDIEvent *ev;

	if (!ctx->currentSong)
		goto transmit;

	/* advance song position */
	ctx->tickFrac += ctx->tickAdd;
	ctx->tickInt += ctx->tickFrac >> 16;
	ctx->tickFrac &= 0xffff;

	/* schedule due events */
	ev = ctx->events;
	while (ev && ev->time <= ctx->tickInt) {
		const uint8_t status = ev->status;

		if (status == 0xff) {
			/* Meta event; never sent over the wire */
			if (ev->data[0] == 0x51 && ev->extraLen == 3) {
				/* Tempo change */
				uint32_t tempo = ((uint32_t)ev->extraData[0] << 16)
							| ((uint32_t)ev->extraData[1] << 8)
							| (uint32_t)ev->extraData[2];
				if (tempo)
					ctx->tickAdd = UMidiTicksPerPeriod(ctx, tempo);
			}
			/* 0x2f (end of track) and the rest are ignored;
			 * end of song == end of the event list */
		} else {
			switch (status >> 4) {
				case MIDI_STATUS_CONTROLLER:
					if (UMidiFifoSpace(ctx) < 3)
						goto fifo_full;
					if (ev->data[0] == 0x07) {
						/* Channel volume: remember it and send it scaled
						 * by the master volume */
						ctx->lastCC7[status & 0x0f] = ev->data[1];
						UMidiFifoPush(ctx, status);
						UMidiFifoPush(ctx, 0x07);
						UMidiFifoPush(ctx, UMidiScaledVolume(ctx, status & 0x0f));
						break;
					}
					/* fall through */
				case MIDI_STATUS_NOTE_OFF:
				case MIDI_STATUS_NOTE_ON:
				case MIDI_STATUS_AFTERTOUCH:
				case MIDI_STATUS_PITCH_WHEEL:
					if (UMidiFifoSpace(ctx) < 3)
						goto fifo_full;
					UMidiFifoPush(ctx, status);
					UMidiFifoPush(ctx, ev->data[0]);
					UMidiFifoPush(ctx, ev->data[1]);
					break;

				case MIDI_STATUS_PROG_CHANGE:
				case MIDI_STATUS_PRESSURE:
					if (UMidiFifoSpace(ctx) < 2)
						goto fifo_full;
					UMidiFifoPush(ctx, status);
					UMidiFifoPush(ctx, ev->data[0]);
					break;

				default:
					/* Sysex (0xf0/0xf7): skipped */
					break;
			}
		}

		ev = ev->next;
	}
fifo_full:
	ctx->events = ev;

	if (!ev && ctx->loops) {
		if (ctx->loops > 0)
			ctx->loops--;

		ctx->events		= ctx->currentSong->firstEvent;
		ctx->tickAdd	= UMidiTicksPerPeriod(ctx, UMIDI_DEFAULT_TEMPO);
		ctx->tickFrac	= 0;
		ctx->tickInt	= 0;
	}

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
	if (ctx->fifoHead != ctx->fifoTail) {
		/* bytes queued and TX interrupt not armed: arm it. If TDRE is
		 * already set this asserts the ACIA IRQ at once (falling edge on
		 * GPIP I4), so transmission starts right after we return. */
		if (!ctx->tie) {
			ctx->tie = 1;
			UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_ON;
		}
	} else if (!ctx->currentSong || !ctx->events) {
		/* nothing to send and no song to play: disable the Timer B
		 * interrupt, UMidiPlay() enables it again */
		UMIDI_MFP_IERA &= (uint8_t)~(1 << 0);
	}

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
			 * UMidiTimerHandler() enables it again */
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

	/* Timer B through the XBIOS; its interrupt disables itself while there
	 * is nothing to do */
	Jdisint(MFP_TIMERB);
	ctx->oldTimerbVec = (uint32_t)Setexc(0x120>>2, -1);
	Xbtimer(XB_TIMERB, UMIDI_TIMER_B_CTRL, UMIDI_TIMER_B_DATA, UMidiTimerHandler);

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

UMIDI_DEF void UMidiPlay(UMidiContext* ctx, NativeMidiSong* song, int loops) {
	int i;

	if (!ctx->kbdvecs)
		return;	/* not open */

	if (ctx->currentSong)
		UMidiStop(ctx);

	ctx->events = song->firstEvent;
	ctx->ticksPerQuarterNote = song->ticksPerQuarterNote;

	ctx->tickAdd	= UMidiTicksPerPeriod(ctx, UMIDI_DEFAULT_TEMPO);
	ctx->tickFrac	= 0;
	ctx->tickInt	= 0;

	for (i = 0; i < 16; ++i)
		ctx->lastCC7[i] = UMIDI_DEFAULT_CC7;

	ctx->loops = loops;	/* number of repeats after the first pass; -1 = forever */

	/* compiler barrier: all of the song is stored before the pointer the
	 * timer interrupt checks */
	__asm__ volatile ("" : : : "memory");
	ctx->currentSong = song;
	Jenabint(MFP_TIMERB);
}

UMIDI_DEF void UMidiStop(UMidiContext* ctx) {
	if (!ctx->currentSong)
		return;

	/* drop the queue: nothing queues data any more, and the TX interrupt
	 * can't split this store; it turns itself off on the empty FIFO */
	ctx->currentSong = NULL;
	ctx->fifoTail = ctx->fifoHead;
	while (ctx->tie)
		;

	UMidiSilence();
}

UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx) {
	return ctx->currentSong
		&& (ctx->events || ctx->fifoHead != ctx->fifoTail);
}

UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume) {
	if (volume < 0)
		volume = 0;
	else if (volume > UMIDI_MAX_VOLUME)
		volume = UMIDI_MAX_VOLUME;

	ctx->volume = volume;
	ctx->volDirty = 1;
}

static UMidiContext s_ctx;
static int s_volume = MIX_MAX_VOLUME;

void native_midi_start(NativeMidiSong *song, int loops) {
	printf("%s: %d\n", __FUNCTION__, loops);

	UMidiClose(&s_ctx);
	if (UMidiOpen(&s_ctx)) {
		UMidiSetVolume(&s_ctx, s_volume);
		UMidiPlay(&s_ctx, song, loops);
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

	printf("%s\n", __FUNCTION__);

	song = (NativeMidiSong *)malloc(sizeof(*song));
	if (song) {
		song->firstEvent = CreateMIDIEventList(rw, &song->ticksPerQuarterNote);
		if (!song->firstEvent) {
			free(song);
			song = NULL;
		}
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

	if (song == ctx->currentSong)
		native_midi_stop();

	FreeMIDIEventList(song->firstEvent);
	free(song);
}

const char *native_midi_error(void) {
	return "";
}

#endif	/* __MINT__ */

#endif
