/*
 * MidiSyncTool - timing telemetry for the generated clock.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msStats.h.md - "// notes §k" refers there.

#ifndef __MS_STATS_H__
#define __MS_STATS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// notes §1
#define MS_STATS_DRIFT_SECONDS    (30.0)

#define MS_STATS_HISTORY          (256) // recent commit margins, for the UI's scrolling graph

// notes §2
#define MS_STATS_WINDOW_SECONDS    (10.0)
#define MS_STATS_WINDOW_MAX        (8192)

// notes §3
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

// notes §4
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
