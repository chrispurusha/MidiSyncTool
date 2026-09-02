/*
 * MidiSyncTool - MIDI clock generation from the host's musical position.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

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

// HOW FAR AHEAD OF ITSELF THE GENERATOR COMMITS, in milliseconds.
//
// A tick used to be stamped for "this block's wall time plus its offset within the block", which is
// only safe if the block is picked up exactly on time. It is not: a block that arrives late carries
// ticks whose moment has already passed, and CoreMIDI delivers those at once. Measured through IAC
// that showed as 1.667 ms RMS with a -18 ms worst case - bunching, not drift.
//
// Committing a fixed distance ahead absorbs that. The cost is real and is the reason this is not
// simply set large: a tick committed 10 ms early cannot respond to anything that happens in those
// 10 ms, so the lookahead is the floor on how quickly a tempo change reaches the wire. At 130 BPM a
// tick is 19.2 ms apart, so 10 ms leaves at most one tick already committed when the tempo moves.
//
// It is also a CONSTANT, which matters more than its size: a fixed offset is latency and can be
// reported and compensated, while a variable one is jitter and cannot.
//
// HONESTLY: 10 ms is NOT an evidence-based figure. Sweeping it 5 / 10 / 30 ms changed the measured
// jitter not at all (0.936 / 0.891 / 0.901 ms RMS), so nothing here has demonstrated that the
// lookahead buys anything. It is kept because the failure it guards against - a late block
// submitting ticks whose moment has passed, which CoreMIDI then delivers together - is real and
// costs only a known constant, and because the offline harness is a sleeping loop that may simply
// be incapable of reproducing that failure. It wants revisiting against a real host under load.
#define MS_LOOKAHEAD_MS    (10.0)

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
    double      phase;          // musical distance since the last tick, always < 1/24 QN
    double      lastPpq;        // musical position reported for the previous block
    double      lastTempo;
    bool        havePrev;       // false until a first block has been seen
    uint64_t    ticksSent;
    uint64_t    ticksInRun;     // reset at every transport start - the detector's grid counts from here
    uint64_t    wrapsSeen;
    uint64_t    startsSent;
    uint64_t    stopsSent;
    uint64_t    continuesSent;
} tMsClock;

void ms_clock_init(tMsClock * clock);

// LATENCY COMPENSATION IS A PHASE ADVANCE, NOT AN EARLIER SEND. This is the central lesson of the
// first hardware session and it is worth stating at length, because the obvious mechanism is wrong
// and fails in a way that looks like a tuning problem.
//
// The obvious mechanism is to subtract the measured latency from each event's timestamp. It cannot
// work, and not because the lookahead is too small - because of arithmetic. Let a tick's musical
// moment be m, the schedule lead be L and the device's round trip be D. The tick is stamped at
// wall(m) + L and its sound arrives at wall(m) + L + D. To land the sound on m the stamp would have
// to move back by L + D, to wall(m) - D: EARLIER THAN THE BLOCK THAT DECIDED IT. No lookahead makes
// that possible; increasing L increases the requirement by exactly as much.
//
// Measured on a Tempest at -21.9 ms of wall-clock compensation against a 10 ms lead: every tick
// clamped to the present, and jitter went from 0.169 ms RMS to 3.220 - a clean clock turned into a
// bunched one, which is the very defect this tool exists to remove.
//
// What DOES work costs nothing. The tick stream is periodic, so a grid shifted earlier by L + D is
// indistinguishable from one whose PHASE is advanced by that much - and the phase is free to move.
// Each tick then carries a musical moment L + D later than the one whose wall time it was sent at,
// its sound arrives on that moment, and no event is ever asked to exist before its own block.
//
// The one casualty is the first L + D of a run: the tick grid cannot be advanced before the
// transport has started, so the opening beat is uncompensated. Every beat after it is exact.
//
// Pass the DEVICE's round trip; the schedule lead is added here.
void ms_clock_set_compensation_ms(tMsClock * clock, double deviceMs);

// Called once per audio block, from the audio thread, with everything the host reported plus the
// wall time at which this block was picked up. Emits whatever clock ticks fall inside the block,
// each stamped for the moment it belongs at.
void ms_clock_process(tMsClock * clock, double ppq, double tempo, bool playing, bool cycleActive, double cycleStartPpq, double cycleEndPpq, uint32_t blockFrames, double sampleRate, uint64_t blockHostTime);

#ifdef __cplusplus
}
#endif

#endif // __MS_CLOCK_H__
