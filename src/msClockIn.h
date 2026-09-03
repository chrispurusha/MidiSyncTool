/*
 * MidiSyncTool - what an INCOMING MIDI clock is doing: tempo, spread and transport.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_CLOCK_IN_H__
#define __MS_CLOCK_IN_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MEASURE ONLY. Nothing here steers the generated clock, and that separation is deliberate: the
// first useful thing to do with a clock input is to say what it is doing, and the second - a PLL
// that regenerates from it - is a different feature that should not be built on an estimator nobody
// has read the numbers out of yet.
//
// WHAT IT IS ACTUALLY FOR, beyond a tempo readout. The standing question this project has never
// been able to answer defensibly is "how much better is our clock than Live's own?" - because the
// only reference so far has been a drum machine's audio onsets, which carry the device, the desk
// and the A/D on top of anything the wire did. Point Live's Sync at IAC, point this at the same
// port, and both clocks can be timed against the SAME reference by the same code. The wire measures
// at about 0.013 ms here; the audio path at 0.132 ms at best. It is an order of magnitude sharper.
//
// THREAD OWNERSHIP: everything in here belongs to the CoreMIDI receive thread and to nothing else.
// The estimator's window is touched by that thread alone and published as atomics; the audio thread
// never reads it and the UI only reads the published snapshot. One owning thread per piece of
// state, everyone else posts.

// HOW MANY ARRIVALS THE FIT SPANS. At 24 PPQN and 120 BPM, 192 clocks is eight beats - four
// seconds. Long enough that the ppm figure means something (a one-sample error over four seconds is
// a few ppm) and short enough to follow a tempo change rather than average across it.
#define MS_CLOCK_IN_WINDOW    (192)

// The fewest arrivals worth fitting a line through. Below this the residual is describing the fit
// rather than the clock.
#define MS_CLOCK_IN_MIN_FIT    (16)

// AN INTERVAL THIS MANY TIMES THE EXPECTED ONE IS NOT A CLOCK PERIOD. The master stopped, was
// switched, or the port dropped out - and one of those intervals in a least-squares window drags
// the fitted rate somewhere meaningless. Counted and the window restarted, never averaged in. Same
// discipline, and the same reasoning, as MS_STATS_GAP_MS.
#define MS_CLOCK_IN_GAP_RATIO    (4.0)

typedef struct {
    double   bpm;              // from the fitted tick period; 0 until there is a fit
    double   periodMs;         // per clock, so 24 of them make a quarter note
    double   jitterRmsMs;      // residual about the fitted line - the honest spread
    double   peakDevMs;
    uint32_t fitted;           // arrivals in the current window
    uint64_t clocks;           // 0xF8 seen since the last reset
    uint64_t gaps;             // intervals rejected as not-a-period
    uint64_t starts;
    uint64_t stops;
    uint64_t continues;
    bool     running;          // Start or Continue seen, no Stop since
    uint64_t lastArrival;      // mach host time of the most recent clock, 0 if none
} tMsClockInSnapshot;

typedef struct tMsClockIn tMsClockIn;

tMsClockIn * ms_clock_in_create(void);
void ms_clock_in_destroy(tMsClockIn * in);

// Clears the window AND the counters: a source change starts a new measurement, and carrying a
// fitted rate across from a different master would be a reading of neither.
void ms_clock_in_reset(tMsClockIn * in);

// FROM THE CoreMIDI RECEIVE THREAD, one call per byte of interest. hostTime is the packet's own
// timestamp where the source gave one - which is the whole point, since it is stamped closer to the
// wire than anything this process could read for itself.
void ms_clock_in_byte(tMsClockIn * in, uint8_t status, uint64_t hostTime);

void ms_clock_in_read(const tMsClockIn * in, tMsClockInSnapshot * out);

#ifdef __cplusplus
}
#endif

#endif // __MS_CLOCK_IN_H__
