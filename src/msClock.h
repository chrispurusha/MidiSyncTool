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

// ── The timebase model ───────────────────────────────────────────────────────────────────────────
//
// WHY THE HOST'S OWN BLOCK TIME IS NOT GOOD ENOUGH TO STAMP A TICK WITH.
//
// A tick used to be stamped from AudioGetCurrentHostTime() read at the top of process(). That makes
// the output clock inherit the host's callback jitter exactly: a block delivered late by d carries
// every tick in it d late. Live measured 0.232 ms RMS of block jitter, and all of it was going
// straight onto the wire.
//
// This is the failure the concept names in section 2 - "do not simply react to instantaneous
// arrival times... estimate the underlying phase and rate and independently schedule the cleanest
// possible output" - and it applies to the host's own timing just as much as to an incoming clock.
//
// So a model is kept of where musical time sits on the wall clock: an anchor, and a rate. Each block
// contributes an observation, the model is nudged towards it, and the ticks are stamped from THE
// MODEL rather than from the observation. A single late block moves the output by Kp of its error
// instead of all of it.
//
// THE RATE TERM IS NOT REDUNDANT with knowing the tempo. The tempo says how many quarter notes a
// second of AUDIO time contains; the stamps are in mach time, and the two clocks are different
// crystals. Measured on a QU-24: 19 ppm apart, which is 190 microseconds over ten seconds - small,
// steady, and exactly the kind of error a phase-only correction would chase for ever.
#define MS_MODEL_KP    (0.01)            // how much of a block's error is taken as phase correction

// KP WAS SWEPT, not chosen. Against the usleep harness - a host that genuinely delivers blocks late -
// with the delivered clock timed on arrival through IAC, as a fraction of that run's block jitter:
//
//   Kp 0.05   9.2 %        Kp 0.01   5.9 %
//   Kp 0.02   7.2 %        Kp 0.005  5.6 %        Kp 0.002  5.4 %
//
// Below 0.01 the return is small and the absolute figure stops moving at all - 0.0198 ms, which is
// the RIG'S floor rather than the plug-in's: tools/midiTimestampTest.c measured 0.019 ms RMS through
// the same port with no block pacing whatsoever. Nothing here can be shown to do better than that.
//
// A smaller Kp is only safe because the two things that would otherwise need fast tracking are
// handled explicitly: a tempo change re-slopes the line, and anything past MS_MODEL_RESYNC_MS
// re-anchors it. Left to the filter alone, both would demand a Kp large enough to converge quickly,
// and that is precisely what costs jitter.
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

    // THE LEARNED SLOPE AS A RATIO of the tempo's nominal ns-per-quarter-note, which is what makes
    // it survivable across a tempo change.
    //
    // nsPerQn on its own conflates two things: the tempo, which the host tells us exactly, and the
    // standing difference between the audio crystal and the system clock, which only the integral
    // term can discover and which takes seconds of running to converge. Re-deriving nsPerQn from the
    // new tempo at every change threw the second away along with the first - and under tempo
    // AUTOMATION, where the change fires on every block, it was thrown away before it could ever be
    // learned. Held as a ratio, the crystal correction is carried across the change untouched and
    // only the tempo part is recomputed.
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

// The residual block-timing error AFTER the model has absorbed what it can, in milliseconds RMS.
// Shown beside the host's raw block jitter, because the pair is what says whether the absorbing is
// working - one number on its own says nothing.
//
// WHAT MUST NEVER GO INTO IT, learned the hard way: a block on which the model DISCONTINUOUSLY moved.
// The measure is the step between one modelled base and the next, so a re-anchor writes the whole
// jump into it - and a sum of squares never forgets. Two paths did exactly that and made the filter
// look useless in Live while the wire was fine:
//
//   - a RESYNC, worth up to MS_MODEL_RESYNC_MS. One of those in a thousand blocks is sqrt(30^2/1000)
//     = 0.95 ms, four times Live's raw block jitter, from a single sample.
//   - the FIRST BLOCK OF A RUN. The stopped path returns before the model runs, so the retained base
//     is from before the stop and the step is the whole stopped gap - seconds of it.
//
// Both are answered by dropping the base whenever it stops being continuous, which is what
// haveBase is for. Expect the honest figure to be about MS_MODEL_KP of the raw block jitter.
double ms_clock_residual_ms(const tMsClock * clock);

// The worst single block's residual, same units. Reported beside the RMS because an RMS over
// thousands of blocks will bury one bad block, and one bad block is a bunched pair of ticks.
double ms_clock_residual_worst_ms(const tMsClock * clock);

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
