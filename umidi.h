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

#include <stdlib.h>
#include <string.h>

#include <mint/osbind.h>
#include <mint/ostruct.h>

#include "../SDL_mixer.h"

#include "native_midi.h"
#include "native_midi_common.h"

#define UMIDI_FIFO_SIZE			2048		/* power of two */
#define UMIDI_FIFO_MASK			(UMIDI_FIFO_SIZE - 1)

struct _NativeMidiSong {
	MIDIEvent *firstEvent;
	Uint16 ticksPerQuarterNote; /* 48 by default (independent of the tempo) */
};

typedef struct {
	/* Only one song plays at a time; the playback state lives here and not
	 * in NativeMidiSong. */
	NativeMidiSong * volatile currentSong;	/* NULL when stopped */
	MIDIEvent * volatile events;			/* next event to schedule */
	Uint32 ticksPerQuarterNote;
	int loops;

	/* 16.16 fixed point: upper 16 bits integer part, lower 16 bits fraction.
	 * Every timer interrupt executes tickFrac += tickAdd; whenever
	 * tickFrac crosses 1.0, the whole part moves into tickInt and only
	 * the fraction stays. */
	Uint32 tickAdd;
	Uint32 tickFrac;
	Uint32 tickInt;

	Uint8 fifo[UMIDI_FIFO_SIZE];
	volatile Uint16 fifoHead, fifoTail;

	Uint8 lastCC7[16];				/* channel volume as sent by the song */
	volatile Uint8 volume;			/* master volume */
	volatile SDL_bool volDirty;		/* set by UMidiSetVolume(), consumed by the producer */

	volatile SDL_bool tie;			/* shadow: is the TX interrupt enabled? */
	_KBDVECS *kbdvecs;

	Uint32 oldTimerbVec;
	Uint8 oldTbcr;
	SDL_bool oldTimerbEnabled, oldTimerbMasked;
} UMidiContext;

#ifndef UMIDI_DEF
#ifdef __GNUC__
/* a translation unit doesn't have to use all of them */
#define UMIDI_DEF	static __attribute__((__unused__))
#else
#define UMIDI_DEF	static
#endif
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
 *    event processing until space frees up; bytes are never dropped.
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
 * lastCC7[channel] * volume / MIX_MAX_VOLUME. UMidiSetVolume()
 * stores volume, sets volDirty and returns; the producer then queues
 * controller 7 messages with the recomputed values for all 16 channels.
 */

#define UMIDI_ACIA_CTRL			(*(volatile Uint8 *)0xFFFFFC04L)	/* MIDI ACIA control (write) and status (read) register */
#define UMIDI_ACIA_DATA			(*(volatile Uint8 *)0xFFFFFC06L)	/* MIDI ACIA data register */
#define UMIDI_ACIA_TDRE			(1 << 1)							/* ACIA status: transmit data register empty */

/* /16 clock, 8N1, RTS low, TX interrupt off, RX interrupt on.*/
#define UMIDI_ACIA_CTRL_TIE_OFF	0x95
/* /16 clock, 8N1, RTS low, TX interrupt on, RX interrupt on.*/
#define UMIDI_ACIA_CTRL_TIE_ON	0xB5

#define UMIDI_MFP_IERA			(*(volatile Uint8 *)0xFFFFFA07L)	/* MFP interrupt enable register A */
#define UMIDI_MFP_IPRA			(*(volatile Uint8 *)0xFFFFFA0BL)	/* MFP interrupt pending register A */
#define UMIDI_MFP_ISRA			(*(volatile Uint8 *)0xFFFFFA0FL)	/* MFP interrupt in-service register A */
#define UMIDI_MFP_IMRA			(*(volatile Uint8 *)0xFFFFFA13L)	/* MFP interrupt mask register A */
#define UMIDI_MFP_TBCR			(*(volatile Uint8 *)0xFFFFFA1BL)	/* MFP Timer B control register */

#define UMIDI_MFP_CLOCK			2457600UL
#define UMIDI_TIMER_B_CTRL		3		/* prescaler /16 */
#define UMIDI_TIMER_B_PRESCALER	16
#define UMIDI_TIMER_B_DATA		160		/* 2457600/16/160 = 960 Hz */

#define UMIDI_TICK_NUMER		((Uint32)((65536ULL \
									* UMIDI_TIMER_B_PRESCALER \
									* UMIDI_TIMER_B_DATA \
									* 1000000 \
									+ UMIDI_MFP_CLOCK/2) / UMIDI_MFP_CLOCK))

#define UMIDI_DEFAULT_TEMPO		500000UL	/* us per quarter note (120 bpm) */
#define UMIDI_DEFAULT_CC7		100			/* GM default channel volume */

/* the interrupt handlers and Supexec() routines take no arguments */
static UMidiContext *UMidiActiveContext;

static __inline__ Uint16 UMidiDisableInterrupts(void) {
	Uint16 sr;
#ifdef __mcoldfire__
	/* ColdFire has no ori to SR; this runs in supervisor mode, so
	 * setting the S bit along with the mask is safe */
	__asm__ volatile ("move.w %%sr,%0\n\tmove.w #0x2700,%%sr" : "=d"(sr) : : "memory");
#else
	__asm__ volatile ("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory");
#endif
	return sr;
}

static __inline__ void UMidiRestoreInterrupts(Uint16 sr) {
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "memory");
}

static __inline__ Uint8 UMidiScaledVolume(const UMidiContext *ctx, Uint8 chan) {
	return (Uint16)(ctx->lastCC7[chan] * ctx->volume) / MIX_MAX_VOLUME;
}

static __inline__ Uint16 UMidiFifoSpace(const UMidiContext *ctx) {
	return UMIDI_FIFO_SIZE - 1 - ((ctx->fifoHead - ctx->fifoTail) & UMIDI_FIFO_MASK);
}

static __inline__ void UMidiFifoPush(UMidiContext *ctx, Uint8 b) {
	ctx->fifo[ctx->fifoHead] = b;
	ctx->fifoHead = (ctx->fifoHead + 1) & UMIDI_FIFO_MASK;
}

/* Returns how many MIDI ticks elapse during one timer period, as 16.16
 * fixed point: 65536 * ticksPerQuarterNote * period_us / tempo_us. */
static __inline__ Uint32 UMidiTicksPerPeriod(const UMidiContext *ctx, Uint32 tempo) {
	Uint32 q, r;

	q = UMIDI_TICK_NUMER / tempo;
	r = UMIDI_TICK_NUMER % tempo;

	return ctx->ticksPerQuarterNote * q
		+ (ctx->ticksPerQuarterNote * r + tempo / 2) / tempo;
}

/* Producer: schedule due events into the FIFO, arm the ACIA TX interrupt */
static void __attribute__((interrupt)) UMidiTimerHandler(void) {
	UMidiContext *ctx = UMidiActiveContext;
	MIDIEvent *ev;

	/* advance song position */
	ctx->tickFrac += ctx->tickAdd;
	ctx->tickInt += ctx->tickFrac >> 16;
	ctx->tickFrac &= 0xffff;

	/* schedule due events */
	ev = ctx->events;
	while (ev && ev->time <= ctx->tickInt) {
		const Uint8 status = ev->status;

		if (status == 0xff) {
			/* Meta event; never sent over the wire */
			if (ev->data[0] == 0x51 && ev->extraLen == 3) {
				/* Tempo change */
				Uint32 tempo = ((Uint32)ev->extraData[0] << 16)
							| ((Uint32)ev->extraData[1] << 8)
							| (Uint32)ev->extraData[2];
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

		ctx->volDirty = SDL_FALSE;
		for (i = 0; i < 16; ++i) {
			UMidiFifoPush(ctx, 0xb0 | i);
			UMidiFifoPush(ctx, 0x07);
			UMidiFifoPush(ctx, UMidiScaledVolume(ctx, i));
		}
	}

	/* bytes queued and TX interrupt not armed: arm it. If TDRE is
	 * already set this asserts the ACIA IRQ at once (falling edge on
	 * GPIP I4), so transmission starts right after we return. */
	if (ctx->fifoHead != ctx->fifoTail && !ctx->tie) {
		ctx->tie = SDL_TRUE;
		UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_ON;
	}

	UMIDI_MFP_ISRA = ~(1 << 0);	/* clear in service bit */
}

/* private SDL API (src/video/ataricommon/SDL_atarixbra.c) */
typedef void (*UMidiXbraHandler)(void);
extern UMidiXbraHandler Atari_UnhookXbra(Uint32 vecnum, Uint32 app_id, UMidiXbraHandler handler);

#define UMIDI_XBRA_ID			0x4C53444DUL	/* 'LSDM' */

extern void UMidiSysHandler(void);
extern UMidiXbraHandler UMidiSysOldVec;

/* kb_midisys hook: called (jsr, supervisor) by the TOS $118 handler on
 * every ACIA interrupt, in a loop until the interrupt line is released.
 * Send one byte per TDRE, disarm TIE the moment the FIFO runs dry --
 * leaving TIE armed with nothing to send would hang the midikey loop. */
void UMidiAciaHook(void) {
	UMidiContext *ctx = UMidiActiveContext;

	if (ctx->tie && (UMIDI_ACIA_CTRL & UMIDI_ACIA_TDRE)) {
		if (ctx->fifoHead != ctx->fifoTail) {
			UMIDI_ACIA_DATA = ctx->fifo[ctx->fifoTail];
			ctx->fifoTail = (ctx->fifoTail + 1) & UMIDI_FIFO_MASK;
		} else {
			/* nothing to send: disable the ACIA transmit interrupt,
			 * UMidiTimerHandler() enables it again */
			UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;
			ctx->tie = SDL_FALSE;
		}
	}
}

#define UMIDI_STR_(x)			#x
#define UMIDI_STR(x)			UMIDI_STR_(x)
#define UMIDI_SYM(x)			UMIDI_STR(__USER_LABEL_PREFIX__) #x

/*
 * kb_midisys entry with an XBRA header: calls the C handler for the
 * transmit side, then jumps to the previous vector for the receive
 * side.
 *
 * No specification defines which registers a kbdvecs routine may
 * clobber, but both Atari TOS and EmuTOS save d0-d3/a0-a3 around these
 * calls. This handler clobbers only d0-d1/a0-a1 (C call) plus a0
 * (chain jump), so nothing has to be saved here under either reading.
 */
__asm__(
	"\n" "	.pushsection	.text"
	"\n" "	.even"
	"\n" "	.globl	" UMIDI_SYM(UMidiSysOldVec)
	"\n" "	.globl	" UMIDI_SYM(UMidiSysHandler)
	"\n" "	.ascii	\"XBRA\""
	"\n" "	.ascii	\"LSDM\""
	"\n" UMIDI_SYM(UMidiSysOldVec) ":"
	"\n" "	.long	0"
	"\n" UMIDI_SYM(UMidiSysHandler) ":"
	"\n" "	jsr		" UMIDI_SYM(UMidiAciaHook)
	"\n" "	move.l	" UMIDI_SYM(UMidiSysOldVec) ",%a0"
	"\n" "	jmp		(%a0)"
	"\n" "	.popsection"
	"\n");

static long UMidiInstallHooks(void) {
	UMidiContext *ctx = UMidiActiveContext;
	Uint16 sr = UMidiDisableInterrupts();

	ctx->oldTbcr = UMIDI_MFP_TBCR;
	ctx->oldTimerbEnabled	= (UMIDI_MFP_IERA & (1 << 0)) != 0;
	ctx->oldTimerbMasked	= (UMIDI_MFP_IMRA & (1 << 0)) != 0;

	ctx->tie = SDL_FALSE;
	UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;

	if ((UMidiXbraHandler)ctx->kbdvecs->midisys != UMidiSysHandler) {
		UMidiSysOldVec = (UMidiXbraHandler)ctx->kbdvecs->midisys;
		ctx->kbdvecs->midisys = (long (*)(void))UMidiSysHandler;
	}

	UMidiRestoreInterrupts(sr);

	return 0;
}

static void UMidiSendPolled(Uint8 b) {
	while ((UMIDI_ACIA_CTRL & UMIDI_ACIA_TDRE) == 0)
		;
	UMIDI_ACIA_DATA = b;
}

static long UMidiRemoveHooks(void) {
	UMidiContext *ctx = UMidiActiveContext;
	Uint16 sr;
	int i;

	/* order matters: hardware TIE off first, only then the shadow */
	sr = UMidiDisableInterrupts();
	UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;
	ctx->tie = SDL_FALSE;
	UMidiRestoreInterrupts(sr);

	/* Silence every channel (TIE is off, so polled sending cannot race
	 * the consumer; interrupts stay enabled, this takes ~30 ms).
	 * A status byte aborts a possibly
	 * half-transmitted message, so the stream stays parseable. */
	for (i = 0; i < 16; ++i) {
		UMidiSendPolled(0xb0 | i);
		UMidiSendPolled(0x78);	/* All Sound Off */
		UMidiSendPolled(0x00);
		UMidiSendPolled(0xb0 | i);
		UMidiSendPolled(0x7b);	/* All Notes Off */
		UMidiSendPolled(0x00);
	}

	sr = UMidiDisableInterrupts();

	Atari_UnhookXbra((Uint32)&ctx->kbdvecs->midisys, UMIDI_XBRA_ID, UMidiSysHandler);

	UMIDI_MFP_TBCR = ctx->oldTbcr;
	UMIDI_MFP_IPRA = (Uint8)~(1 << 0);	/* discard a pending tick of ours */
	if (ctx->oldTimerbEnabled)
		UMIDI_MFP_IERA |= 1 << 0;
	if (ctx->oldTimerbMasked)
		UMIDI_MFP_IMRA |= 1 << 0;

	UMidiRestoreInterrupts(sr);

	return 0;
}

UMIDI_DEF int UMidiOpen(UMidiContext* ctx) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->volume = MIX_MAX_VOLUME;

	return 1;
}

UMIDI_DEF void UMidiClose(UMidiContext* ctx) {
	if (ctx->currentSong)
		UMidiStop(ctx);
}

UMIDI_DEF void UMidiPlay(UMidiContext* ctx, NativeMidiSong* song, int loops) {
	int i;

	if (ctx->currentSong)
		UMidiStop(ctx);

	ctx->events = song->firstEvent;
	ctx->ticksPerQuarterNote = song->ticksPerQuarterNote;

	ctx->tickAdd	= UMidiTicksPerPeriod(ctx, UMIDI_DEFAULT_TEMPO);
	ctx->tickFrac	= 0;
	ctx->tickInt	= 0;
	ctx->fifoHead	= ctx->fifoTail = 0;

	for (i = 0; i < 16; ++i)
		ctx->lastCC7[i] = UMIDI_DEFAULT_CC7;

	ctx->kbdvecs = Kbdvbase();
	UMidiActiveContext = ctx;
	Supexec(UMidiInstallHooks);

	ctx->loops = loops;	/* number of repeats after the first pass; -1 = forever */

	ctx->currentSong = song;

	Jdisint(MFP_TIMERB);
	ctx->oldTimerbVec = (Uint32)Setexc(0x120>>2, -1);
	Xbtimer(XB_TIMERB, UMIDI_TIMER_B_CTRL, UMIDI_TIMER_B_DATA, UMidiTimerHandler);
}

UMIDI_DEF void UMidiStop(UMidiContext* ctx) {
	Jdisint(MFP_TIMERB);
	if (ctx->oldTimerbVec) {
		(void)Setexc(0x120>>2, ctx->oldTimerbVec);
		ctx->oldTimerbVec = 0;
	}

	if (ctx->kbdvecs)
		Supexec(UMidiRemoveHooks);

	ctx->currentSong = NULL;
}

UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx) {
	return ctx->currentSong != NULL
		&& (ctx->events || ctx->fifoHead != ctx->fifoTail);
}

UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume) {
	if (volume < 0)
		volume = 0;
	else if (volume > MIX_MAX_VOLUME)
		volume = MIX_MAX_VOLUME;

	ctx->volume = volume;
	ctx->volDirty = SDL_TRUE;
}

static UMidiContext s_ctx;
static int s_volume = MIX_MAX_VOLUME;

void native_midi_start(NativeMidiSong *song, int loops) {
	printf("%s: %d\n", __FUNCTION__, loops);

	UMidiSetVolume(&s_ctx, s_volume);
	UMidiPlay(&s_ctx, song, loops);
}

void native_midi_stop() {
	printf("%s\n", __FUNCTION__);

	UMidiStop(&s_ctx);
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
	return UMidiOpen(&s_ctx);	/* always available */
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
