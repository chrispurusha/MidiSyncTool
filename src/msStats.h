/*
 * MidiSyncTool - timing telemetry for the generated clock.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_STATS_H__
#define __MS_STATS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WHAT CAN HONESTLY BE MEASURED FROM INSIDE THE PLUG-IN, and what cannot.
//
// The plug-in hands CoreMIDI a tick and a moment for it. It never sees the wire, so it CANNOT
// measure when the tick actually left - that needs a listener, and tools/midiTimestampTest.c is it.
// Claiming a wire figure from in here would be a fabrication.
//
// What it can measure is everything on its own side of that hand-off, and those are the numbers
// that decide whether the wire figure is the plug-in's fault:
//
//   COMMIT MARGIN - how far in the future each tick's moment was at the instant it was submitted.
//     While this stays positive CoreMIDI has the tick in hand early and delivers it on its own
//     timer, which is where the microsecond figures come from. The moment it goes NEGATIVE the tick
//     is already late and CoreMIDI sends it at once, several bunching together. This is the single
//     number that says whether MS_LOOKAHEAD_MS earns its place, and unlike the offline sweep it can
//     be read from inside a real host under real load.
//
//   BLOCK PERIOD ERROR - the wall time between audio callbacks against the nominal block duration.
//     The host's own steadiness, and the thing that eats the commit margin.
//
//   MUSICAL DRIFT - musical time elapsed against wall time elapsed, in ppm. Says whether the host's
//     musical position and its audio clock agree over minutes. GenBridge measures the same quantity
//     between two audio devices; here it is between the host's two notions of time.
// How long the drift window has to be before a figure is worth showing. See the note in msStats.c:
// below this the measurement is dominated by whatever the scheduler last did, not by any drift.
#define MS_STATS_DRIFT_SECONDS    (30.0)

#define MS_STATS_HISTORY          (256) // recent commit margins, for the UI's scrolling graph

typedef struct {
    double marginMeanMs;
    double marginMinMs;         // the worst case, and the one that matters
    double marginRmsMs;
    double blockPeriodRmsMs;
    double blockPeriodWorstMs;

    // WHETHER THE HOST HANDS OVER A CONSTANT BLOCK AT ALL. It is assumed everywhere else here, and
    // Live does not: a size change makes every per-block figure suspect until it is accounted for,
    // so the range and the number of changes are reported rather than left to be inferred.
    uint32_t blockFramesMin;
    uint32_t blockFramesMax;
    uint64_t blockSizeChanges;
    uint64_t blocks;            // callbacks counted, as against ticks - the two are not the same
    double   driftPpm;
    double   hostBpm;
    double   measuredBpm;       // from the tick count against the wall clock, over the whole run
    uint64_t ticks;
    uint64_t lateTicks;         // commit margin <= 0: submitted after its own moment
    double   windowSeconds;
    bool     driftValid;        // false while the window is still filling - see MS_STATS_DRIFT_SECONDS
} tMsStatsSnapshot;

// Audio-thread side. One instance, owned by the processor.
typedef struct tMsStats tMsStats;

tMsStats * ms_stats_create(void);
void ms_stats_destroy(tMsStats * stats);

// Called from the audio thread at the top of each block, with the wall time the block was picked up
// and what the host says about tempo and position.
void ms_stats_block(tMsStats * stats, uint64_t blockHostTime, uint32_t blockFrames, double sampleRate, double tempo, bool playing);

// Called from the audio thread for every tick submitted, with the moment it was stamped for.
void ms_stats_tick(tMsStats * stats, uint64_t stampedHostTime, uint64_t submittedHostTime);

// Reset everything but keep the instance - the UI's "clear" and, automatically, a transport start.
void ms_stats_reset(tMsStats * stats);

// UI-thread side. Never blocks and never touches the audio thread's working state; it reads the
// snapshot the audio thread last published.
void ms_stats_read(const tMsStats * stats, tMsStatsSnapshot * out);

// The recent commit margins in milliseconds, oldest first, for the scrolling graph. Returns how
// many were written, at most MS_STATS_HISTORY.
int ms_stats_history(const tMsStats * stats, double * out, int max);

#ifdef __cplusplus
}
#endif

#endif // __MS_STATS_H__
