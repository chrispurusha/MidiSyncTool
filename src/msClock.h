/*
 * MidiSyncTool - MIDI clock generation from the host's musical position.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msClock.h.md - "// notes §k" refers there.

#ifndef __MS_CLOCK_H__
#define __MS_CLOCK_H__

#include <stdbool.h>
#include <stdint.h>

#include "msDetect.h"
#include "msStats.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MS_PPQN    (24)     // MIDI clock is 24 pulses per quarter note, by the standard

// notes §1
#define MS_LOOKAHEAD_MS    (10.0)

// notes §2
#define MS_MODEL_KP    (0.01)            // how much of a block's error is taken as phase correction

// notes §3
#define MS_MODEL_KI           (0.02)     // and how much is taken as rate correction
#define MS_MODEL_RESYNC_MS    (30.0)     // beyond this it is a seek or a glitch, not jitter

typedef struct {
    // OPTIONAL, and set by whoever owns the clock. The generator is the only place that knows both
    // the moment a tick was stamped for and the moment it was handed over, so it is the only place
    // the commit margin can be measured - see msStats.h. NULL simply means nobody is watching.
    tMsStats * stats;

    // Likewise optional. The generator is where a tick's intended moment is known, and the latency
    // measurement is that moment against the transient it eventually causes - so this is where the
    // detector has to be told.
    tMsDetect * detect;
    int         destination;    // index into msMidi's list; < 0 = nothing selected, generate nothing
    bool        running;        // transport state as of the last block
    double      compensationMs; // the device round trip being compensated; 0 = none
    double      advanceApplied; // how much of it is currently folded into the phase, in quarters
    // The model. anchorQn/anchorNs are a point on the line and nsPerQn is its slope; runQn is the
    // monotonic musical time the whole thing is expressed in - accumulated here rather than taken
    // from the host's reported position, which goes backwards at every loop wrap.
    bool   haveModel;
    double runQn;
    double anchorQn;
    double anchorNs;
    double nsPerQn;

    // notes §4
    double   rateRatio;
    double   lastBaseNs;            // the previous block's modelled base, for the residual below
    double   lastNominalNs;         // and ITS duration - the step spans block n-1, not block n
    bool     haveBase;
    double   modelErrorSumSq;       // how smoothly the modelled base advances - what the ticks ride
    double   modelWorstMs;          // and the worst single block of it - an RMS hides a lone outlier
    uint64_t modelBlocks;
    uint64_t modelResyncs;

    double   phase;             // musical distance since the last tick, always < 1/24 QN
    double   lastPpq;           // musical position reported for the previous block
    double   lastTempo;
    bool     havePrev;          // false until a first block has been seen
    uint64_t ticksSent;
    uint64_t ticksInRun;        // reset at every transport start - the detector's grid counts from here
    uint64_t wrapsSeen;
    uint64_t startsSent;
    uint64_t stopsSent;
    uint64_t continuesSent;
} tMsClock;

void ms_clock_init(tMsClock * clock);

// notes §5
double ms_clock_residual_ms(const tMsClock * clock);

// The worst single block's residual, same units. Reported beside the RMS because an RMS over
// thousands of blocks will bury one bad block, and one bad block is a bunched pair of ticks.
double ms_clock_residual_worst_ms(const tMsClock * clock);

// notes §6
void ms_clock_reset_stats(tMsClock * clock);

// notes §7
void ms_clock_set_compensation_ms(tMsClock * clock, double deviceMs);

// Called once per audio block, from the audio thread, with everything the host reported plus the
// wall time at which this block was picked up. Emits whatever clock ticks fall inside the block,
// each stamped for the moment it belongs at.
void ms_clock_process(tMsClock * clock, double ppq, double tempo, bool playing, bool cycleActive, double cycleStartPpq, double cycleEndPpq, uint32_t blockFrames, double sampleRate, uint64_t blockHostTime);

#ifdef __cplusplus
}
#endif

#endif // __MS_CLOCK_H__
