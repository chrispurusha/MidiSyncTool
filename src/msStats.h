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

// HOW LONG "RECENT" IS FOR THE BLOCK JITTER RMS, and why there are two of that figure at all.
//
// The all-time RMS is sqrt(sum / N) since the last reset, so a single bad block of size E reads
// E/sqrt(N) for ever after. That is not a small effect: one 40 ms block at 256 frames / 48 kHz
// still reads 0.377 ms RMS a minute later and takes THREE AND A HALF MINUTES to fall back under
// 0.2 ms, on a host that has been perfect throughout. Watching the panel after a hiccup therefore
// means watching a number crawl rather than reading what the host is doing now.
//
// So the panel's figure is taken over a sliding window instead - the same event reads 0.924 ms
// while it is in the window and 0.000 the moment it leaves, ten seconds later.
//
// NOTHING IS LOST BY FORGETTING, AND ONLY BECAUSE OF WHAT SITS BESIDE IT. blockPeriodWorstMs stays
// ALL-TIME, so an event that has aged out of the window is still on the panel; the window answers
// "is the host steady now" and the worst case answers "did anything go wrong at any point". The
// all-time RMS is kept too, for the log's model-residual ratio, which would otherwise be comparing
// a windowed figure against an all-time one - see the "model |" line in msVst3.cpp.
//
// TRIMMED BY TIME, NOT BY A BLOCK COUNT, so that the window is the same ten seconds whatever the
// host's buffer size - and so a suspension inside it removes samples rather than stretching the
// span. MS_STATS_WINDOW_MAX caps the memory: it is enough for ten seconds at 64 frames / 48 kHz,
// and at settings finer than that the window simply becomes shorter, which the published span says
// out loud rather than hiding.
#define MS_STATS_WINDOW_SECONDS    (10.0)
#define MS_STATS_WINDOW_MAX        (8192)

// A BLOCK PERIOD ERROR THIS LARGE IS NOT A MEASUREMENT, IT IS A GAP.
//
// The RMS below is an all-time sum that never forgets, so a single interval that is not really a
// block period poisons the figure for the rest of the session. And they happen: the host suspends
// the plug-in, the audio device is released, or - the one that was actually hit - Ableton goes to
// the BACKGROUND and stops being scheduled while another application is brought to the front.
//
// Measured cost of exactly one of them: a host delivering blocks with ZERO error for five minutes,
// backgrounded for thirty seconds, then perfect for five minutes more, reports 89.44 ms RMS. It
// reads as a catastrophically bad host and it is entirely the metric's fault.
//
// 250 ms is some three hundred times any error a real callback has produced here (Live's worst was
// 5.2 ms, and that was a block SIZE change rather than lateness) and far below any suspension. The
// count is published rather than swallowed - the project's own rule that a metric which drops
// samples must say how many, or it is just a prettier lie. See also ms_stats_gap().
#define MS_STATS_GAP_MS    (250.0)

typedef struct {
    double marginMeanMs;
    double marginMinMs;         // the worst case, and the one that matters
    double marginRmsMs;

    // TWO BLOCK-JITTER FIGURES THAT MEAN DIFFERENT THINGS - see MS_STATS_WINDOW_SECONDS. The recent
    // one is what the panel shows and the all-time one is what the log's residual ratio needs. The
    // worst case is all-time in both cases and is the reason forgetting is safe.
    double   blockPeriodRmsMs;       // all-time since the last reset
    double   blockPeriodRecentRmsMs; // over the last MS_STATS_WINDOW_SECONDS
    double   blockRecentSeconds;     // what that window actually spans, which is less while it fills
    uint64_t blockRecentBlocks;      // how many intervals are in it, so a reader knows the sample size
    double   blockPeriodWorstMs;

    // WHETHER THE HOST HANDS OVER A CONSTANT BLOCK AT ALL. It is assumed everywhere else here, and
    // Live does not: a size change makes every per-block figure suspect until it is accounted for,
    // so the range and the number of changes are reported rather than left to be inferred.
    uint32_t blockFramesMin;
    uint32_t blockFramesMax;
    uint64_t blockSizeChanges;
    uint64_t blocks;            // callbacks counted, as against ticks - the two are not the same
    uint64_t blockGaps;         // intervals rejected as suspensions - see MS_STATS_GAP_MS
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

// THE HOST IS ABOUT TO STOP FEEDING BLOCKS, or has just started again. Drops only the previous
// block's timestamp, so the interval ACROSS the pause is never mistaken for a callback period; every
// figure accumulated so far is kept, which is the point - a suspension is not a reason to throw away
// a run's history, only a reason not to measure the hole it leaves.
//
// This is the same bug the timebase model's `haveBase` had, in a second place: a flag set in one
// place and cleared in none. Whenever a metric is a step between two samples, ask what clears it.
void ms_stats_gap(tMsStats * stats);

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
