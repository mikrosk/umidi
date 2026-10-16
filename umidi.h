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

/* Only one song plays at a time; the playback state lives here and not
 * in NativeMidiSong. */
static NativeMidiSong * volatile s_currentSong;	/* NULL when stopped */
static MIDIEvent *s_events;						/* next event to schedule */
static Uint32 s_ticksPerQuarterNote;
static int s_loops;

/* 16.16 fixed point: upper 16 bits integer part, lower 16 bits fraction.
 * Every timer interrupt executes s_tickFrac += s_tickAdd; whenever
 * s_tickFrac crosses 1.0, the whole part moves into s_tickInt and only
 * the fraction stays. */
static Uint32 s_tickAdd;
static Uint32 s_tickFrac;
static Uint32 s_tickInt;

static Uint8 s_fifo[UMIDI_FIFO_SIZE];
static volatile Uint16 s_fifoHead, s_fifoTail;

static Uint8 s_lastCC7[16];			/* channel volume as sent by the song */
static volatile Uint8 s_volume = MIX_MAX_VOLUME;	/* master volume */
static volatile SDL_bool s_volDirty;		/* set by native_midi_setvolume(), consumed by the producer */

static volatile SDL_bool s_tie;		/* shadow: is the TX interrupt enabled? */
static _KBDVECS *s_kbdvecs;

static Uint32 s_oldTimerbVec;
static Uint8 s_oldTbcr;
static SDL_bool s_oldTimerbEnabled, s_oldTimerbMasked;

#ifndef UMIDI_DEF
#ifdef __GNUC__
/* a translation unit doesn't have to use all of them */
#define UMIDI_DEF	static __attribute__((__unused__))
#else
#define UMIDI_DEF	static
#endif
#endif	/* UMIDI_DEF */

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
 * the wire has no master volume, so native_midi_setvolume() (called
 * from music.c for Mix_VolumeMusic() and for fades) implements one by
 * scaling. s_lastCC7[] holds each channel volume as the song last set
 * it: the producer updates it from controller 7 events and transmits
 * s_lastCC7[channel] * s_volume / MIX_MAX_VOLUME. native_midi_setvolume()
 * stores s_volume, sets s_volDirty and returns; the producer then queues
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

static __inline__ Uint8 UMidiScaledVolume(Uint8 chan) {
	return (Uint16)(s_lastCC7[chan] * s_volume) / MIX_MAX_VOLUME;
}

static __inline__ Uint16 UMidiFifoSpace(void) {
	return UMIDI_FIFO_SIZE - 1 - ((s_fifoHead - s_fifoTail) & UMIDI_FIFO_MASK);
}

static __inline__ void UMidiFifoPush(Uint8 b) {
	s_fifo[s_fifoHead] = b;
	s_fifoHead = (s_fifoHead + 1) & UMIDI_FIFO_MASK;
}

/* Returns how many MIDI ticks elapse during one timer period, as 16.16
 * fixed point: 65536 * ticksPerQuarterNote * period_us / tempo_us. */
static __inline__ Uint32 UMidiTicksPerPeriod(Uint32 tempo) {
	Uint32 q, r;

	q = UMIDI_TICK_NUMER / tempo;
	r = UMIDI_TICK_NUMER % tempo;

	return s_ticksPerQuarterNote * q
		+ (s_ticksPerQuarterNote * r + tempo / 2) / tempo;
}

/* Producer: schedule due events into the FIFO, arm the ACIA TX interrupt */
static void __attribute__((interrupt)) UMidiTimerHandler(void) {
	MIDIEvent *ev;

	/* advance song position */
	s_tickFrac += s_tickAdd;
	s_tickInt += s_tickFrac >> 16;
	s_tickFrac &= 0xffff;

	/* schedule due events */
	ev = s_events;
	while (ev && ev->time <= s_tickInt) {
		const Uint8 status = ev->status;

		if (status == 0xff) {
			/* Meta event; never sent over the wire */
			if (ev->data[0] == 0x51 && ev->extraLen == 3) {
				/* Tempo change */
				Uint32 tempo = ((Uint32)ev->extraData[0] << 16)
							| ((Uint32)ev->extraData[1] << 8)
							| (Uint32)ev->extraData[2];
				if (tempo)
					s_tickAdd = UMidiTicksPerPeriod(tempo);
			}
			/* 0x2f (end of track) and the rest are ignored;
			 * end of song == end of the event list */
		} else {
			switch (status >> 4) {
				case MIDI_STATUS_CONTROLLER:
					if (UMidiFifoSpace() < 3)
						goto fifo_full;
					if (ev->data[0] == 0x07) {
						/* Channel volume: remember it and send it scaled
						 * by the master volume */
						s_lastCC7[status & 0x0f] = ev->data[1];
						UMidiFifoPush(status);
						UMidiFifoPush(0x07);
						UMidiFifoPush(UMidiScaledVolume(status & 0x0f));
						break;
					}
					/* fall through */
				case MIDI_STATUS_NOTE_OFF:
				case MIDI_STATUS_NOTE_ON:
				case MIDI_STATUS_AFTERTOUCH:
				case MIDI_STATUS_PITCH_WHEEL:
					if (UMidiFifoSpace() < 3)
						goto fifo_full;
					UMidiFifoPush(status);
					UMidiFifoPush(ev->data[0]);
					UMidiFifoPush(ev->data[1]);
					break;

				case MIDI_STATUS_PROG_CHANGE:
				case MIDI_STATUS_PRESSURE:
					if (UMidiFifoSpace() < 2)
						goto fifo_full;
					UMidiFifoPush(status);
					UMidiFifoPush(ev->data[0]);
					break;

				default:
					/* Sysex (0xf0/0xf7): skipped */
					break;
			}
		}

		ev = ev->next;
	}
fifo_full:
	s_events = ev;

	if (!ev && s_loops) {
		if (s_loops > 0)
			s_loops--;

		s_events	= s_currentSong->firstEvent;
		s_tickAdd	= UMidiTicksPerPeriod(UMIDI_DEFAULT_TEMPO);
		s_tickFrac	= 0;
		s_tickInt	= 0;
	}

	/* Master volume change: resend the scaled channel volumes */
	if (s_volDirty && UMidiFifoSpace() >= 16 * 3) {
		int i;

		s_volDirty = SDL_FALSE;
		for (i = 0; i < 16; ++i) {
			UMidiFifoPush(0xb0 | i);
			UMidiFifoPush(0x07);
			UMidiFifoPush(UMidiScaledVolume(i));
		}
	}

	/* bytes queued and TX interrupt not armed: arm it. If TDRE is
	 * already set this asserts the ACIA IRQ at once (falling edge on
	 * GPIP I4), so transmission starts right after we return. */
	if (s_fifoHead != s_fifoTail && !s_tie) {
		s_tie = SDL_TRUE;
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
	if (s_tie && (UMIDI_ACIA_CTRL & UMIDI_ACIA_TDRE)) {
		if (s_fifoHead != s_fifoTail) {
			UMIDI_ACIA_DATA = s_fifo[s_fifoTail];
			s_fifoTail = (s_fifoTail + 1) & UMIDI_FIFO_MASK;
		} else {
			/* nothing to send: disable the ACIA transmit interrupt,
			 * UMidiTimerHandler() enables it again */
			UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;
			s_tie = SDL_FALSE;
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
	Uint16 sr = UMidiDisableInterrupts();

	s_oldTbcr = UMIDI_MFP_TBCR;
	s_oldTimerbEnabled	= (UMIDI_MFP_IERA & (1 << 0)) != 0;
	s_oldTimerbMasked	= (UMIDI_MFP_IMRA & (1 << 0)) != 0;

	s_tie = SDL_FALSE;
	UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;

	if ((UMidiXbraHandler)s_kbdvecs->midisys != UMidiSysHandler) {
		UMidiSysOldVec = (UMidiXbraHandler)s_kbdvecs->midisys;
		s_kbdvecs->midisys = (long (*)(void))UMidiSysHandler;
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
	Uint16 sr;
	int i;

	/* order matters: hardware TIE off first, only then the shadow */
	sr = UMidiDisableInterrupts();
	UMIDI_ACIA_CTRL = UMIDI_ACIA_CTRL_TIE_OFF;
	s_tie = SDL_FALSE;
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

	Atari_UnhookXbra((Uint32)&s_kbdvecs->midisys, UMIDI_XBRA_ID, UMidiSysHandler);

	UMIDI_MFP_TBCR = s_oldTbcr;
	UMIDI_MFP_IPRA = (Uint8)~(1 << 0);	/* discard a pending tick of ours */
	if (s_oldTimerbEnabled)
		UMIDI_MFP_IERA |= 1 << 0;
	if (s_oldTimerbMasked)
		UMIDI_MFP_IMRA |= 1 << 0;

	UMidiRestoreInterrupts(sr);

	return 0;
}

void native_midi_start(NativeMidiSong *song, int loops) {
	int i;

	printf("%s: %d\n", __FUNCTION__, loops);

	if (s_currentSong)
		native_midi_stop();

	s_events = song->firstEvent;
	s_ticksPerQuarterNote = song->ticksPerQuarterNote;

	s_tickAdd		= UMidiTicksPerPeriod(UMIDI_DEFAULT_TEMPO);
	s_tickFrac		= 0;
	s_tickInt		= 0;
	s_fifoHead		= s_fifoTail = 0;

	for (i = 0; i < 16; ++i)
		s_lastCC7[i] = UMIDI_DEFAULT_CC7;
	s_volDirty = SDL_TRUE;		/* apply the master volume from the start */

	s_kbdvecs = Kbdvbase();
	Supexec(UMidiInstallHooks);

	s_loops = loops;	/* number of repeats after the first pass; -1 = forever */

	s_currentSong = song;

	Jdisint(MFP_TIMERB);
	s_oldTimerbVec = (Uint32)Setexc(0x120>>2, -1);
	Xbtimer(XB_TIMERB, UMIDI_TIMER_B_CTRL, UMIDI_TIMER_B_DATA, UMidiTimerHandler);
}

void native_midi_stop() {
	printf("%s\n", __FUNCTION__);

	Jdisint(MFP_TIMERB);
	if (s_oldTimerbVec) {
		(void)Setexc(0x120>>2, s_oldTimerbVec);
		s_oldTimerbVec = 0;
	}

	if (s_kbdvecs)
		Supexec(UMidiRemoveHooks);

	s_currentSong = NULL;
}

int native_midi_active() {
	return s_currentSong != NULL
		&& (s_events || s_fifoHead != s_fifoTail);
}

void native_midi_setvolume(int volume) {
	printf("%s: %d\n", __FUNCTION__, volume);

	if (volume < 0)
		volume = 0;
	else if (volume > MIX_MAX_VOLUME)
		volume = MIX_MAX_VOLUME;

	s_volume = volume;
	s_volDirty = SDL_TRUE;
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
	printf("%s\n", __FUNCTION__);

	if (!song)
		return;

	if (song == s_currentSong)
		native_midi_stop();

	FreeMIDIEventList(song->firstEvent);
	free(song);
}

const char *native_midi_error(void) {
	return "";
}

#endif	/* __MINT__ */

#endif
