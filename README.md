# uMIDI

This is a header-only library for MIDI playback through the MIDI port of real Atari hardware. It uses Timer B and the MIDI ACIA interrupt.

It consists of three `struct`s and thirteen functions:
```C
typedef struct {
	uint32_t time;	/* timer periods from the song start */
	uint32_t end;	/* offset just past the last byte of the block in the song data */
} UMidiBlock;

typedef struct {
	const UMidiBlock* blocks;	/* sorted by time */
	const uint8_t* data;		/* messages of all blocks */
	uint32_t numBlocks;
	uint32_t loopBlock;			/* block the repeats start at */
} UMidiSong;

typedef struct {
	...
} UMidiContext;

UMIDI_DEF int UMidiOpen(UMidiContext* ctx);
UMIDI_DEF void UMidiClose(UMidiContext* ctx);
UMIDI_DEF int UMidiSend(UMidiContext* ctx, const uint8_t* msg, uint16_t length);
UMIDI_DEF void UMidiPlay(UMidiContext* ctx, const UMidiSong* song, int loops);
UMIDI_DEF void UMidiStop(UMidiContext* ctx);
UMIDI_DEF int UMidiIsPlaying(const UMidiContext* ctx);
UMIDI_DEF void UMidiSetVolume(UMidiContext* ctx, int volume);
UMIDI_DEF uint32_t UMidiPeriods(uint32_t microseconds);
UMIDI_DEF void UMidiStartTicks(UMidiContext* ctx, void (*callback)(void));
UMIDI_DEF void UMidiSetTickRate(UMidiContext* ctx, uint32_t ticksPerMinute);
UMIDI_DEF void UMidiStopTicks(UMidiContext* ctx);
UMIDI_DEF void UMidiLock(UMidiContext* ctx);
UMIDI_DEF void UMidiUnlock(UMidiContext* ctx);
```
`UMIDI_DEF` expands to `static __attribute__((__unused__))`. Define `UMIDI_DEF` as empty before including the header if you prefer an externally visible copy.

`UMidiContext` is owned by the caller, its members are internal.

`UMidiOpen` returns `1` (true) on success and `0` (false) if the machine has no MIDI port or another context (also from another program) is open. `UMidiClose` must be called before the program exits.

There are two ways of playing music, only one active at a time:

- `UMidiPlay` plays a song converted in advance: blocks of complete messages (no running status, system exclusive messages shorter than `UMIDI_FIFO_SIZE`), each sent at its `time`, given in `UMIDI_TIMER_HZ` periods per second (960, or 3840 on the FireBee: switching its ACIA's transmit interrupt can abort the byte being sent, so the timer sends every byte itself and has to run faster than the MIDI port's 3125 bytes per second). `loops` is the number of repeats after the first pass, `-1` repeats forever. The song must stay in memory until it is stopped. `UMidiSetVolume` scales its channel volumes, from `0` to `UMIDI_MAX_VOLUME` (127).
- `UMidiStartTicks` calls `callback` from the timer interrupt once per MIDI tick, at the rate set by `UMidiSetTickRate` (e.g. tempo in BPM × pulses per quarter note). The callback sends its messages with `UMidiSend`, at most `UMIDI_TICK_RESERVE` (128) bytes per tick. `UMidiLock` / `UMidiUnlock` hold the callback off while the program changes data shared with it; they can be nested.

A MIDI file's times are in ticks; its division (ticks per quarter note) and tempo (microseconds per quarter note) turn them into microseconds, and `UMidiPeriods` turns these into timer periods, rounded up.

`UMidiStop` / `UMidiStopTicks` silence all channels and reset their controllers.

`UMidiSend` sends the messages in `msg`, queued behind the song or the callback's messages if one plays. It returns `0` if the context is not open or the queue is full.
