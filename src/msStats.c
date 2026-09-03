/*
 * MidiSyncTool - timing telemetry for the generated clock.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "msClock.h"    // MS_PPQN - the ticks are the measure of musical time here
#include "msStats.h"

// THE THREADING SHAPE, which is GenBridge's and is deliberate.
//
// Everything the audio thread needs to accumulate lives in plain, non-atomic fields that ONLY the
// audio thread touches. Once a block it copies the derived figures into atomics that the UI reads.
// The UI therefore never contends with the audio thread for a lock it must not wait on, and the
// audio thread never pays for an atomic per tick.
//
// The snapshot is not a consistent set - the UI can read a mean from one block and a worst case
// from the next. For a display refreshed 30 times a second that is invisible and it costs nothing;
// a seqlock here would buy correctness nobody could observe.
struct tMsStats {
    // Audio thread only.
    double   sumMargin;
    double   sumMarginSq;
    double   minMargin;
    uint64_t tickCount;
    uint64_t lateCount;

    double   sumPeriodErrSq;
    double   worstPeriodErr;
    uint64_t blockCount;

    // THE SLIDING WINDOW behind the panel's block-jitter figure - see MS_STATS_WINDOW_SECONDS.
    // Squared errors and the moment each was measured, oldest at windowHead once it has wrapped.
    double           windowErrSq[MS_STATS_WINDOW_MAX];
    uint64_t         windowTime[MS_STATS_WINDOW_MAX];
    int              windowHead;    // where the next sample goes; also the oldest, once full
    int              windowCount;
    double           windowSumSq;
    uint64_t         windowPushes;  // only to pace the exact recompute below

    uint64_t         prevBlockTime;
    double           prevNominalMs;  // block n-1's duration - what the gap to block n must be judged against
    uint32_t         prevFrames;
    uint32_t         framesMin;      // the block sizes actually seen, because a host may vary them
    uint32_t         framesMax;
    uint64_t         sizeChanges;
    bool             havePrevBlock;
    uint64_t         gapCount;

    uint64_t         firstPlayTime; // wall clock at the first playing block since the last reset
    uint64_t         firstPlayTicks;
    double           audioSeconds;  // frames since the anchor, in seconds of the AUDIO clock
    bool             havePlayAnchor;

    int              historyWrite;
    double           history[MS_STATS_HISTORY];

    // Published for the UI.
    _Atomic double   pubMarginMean;
    _Atomic double   pubMarginMin;
    _Atomic double   pubMarginRms;
    _Atomic double   pubPeriodRms;
    _Atomic double   pubPeriodRecentRms;
    _Atomic double   pubRecentSeconds;
    _Atomic uint64_t pubRecentBlocks;
    _Atomic uint64_t pubGaps;
    _Atomic double   pubPeriodWorst;
    _Atomic uint32_t pubFramesMin;
    _Atomic uint32_t pubFramesMax;
    _Atomic uint64_t pubSizeChanges;
    _Atomic uint64_t pubBlocks;
    _Atomic double   pubDriftPpm;
    _Atomic int      pubDriftValid;
    _Atomic double   pubHostBpm;
    _Atomic double   pubMeasuredBpm;
    _Atomic double   pubWindowSeconds;
    _Atomic uint64_t pubTicks;
    _Atomic uint64_t pubLate;
    _Atomic int      pubHistoryWrite;
};

static double host_to_ms(uint64_t delta) {
    return (double)AudioConvertHostTimeToNanos(delta) / 1.0e6;
}

tMsStats * ms_stats_create(void) {
    tMsStats * stats = calloc(1, sizeof(tMsStats));

    if (stats != NULL) {
        ms_stats_reset(stats);
    }
    return stats;
}

void ms_stats_destroy(tMsStats * stats) {
    free(stats);
}

void ms_stats_reset(tMsStats * stats) {
    if (stats == NULL) {
        return;
    }
    stats->sumMargin      = 0.0;
    stats->sumMarginSq    = 0.0;
    stats->minMargin      = INFINITY;
    stats->tickCount      = 0;
    stats->lateCount      = 0;
    stats->sumPeriodErrSq = 0.0;
    stats->worstPeriodErr = 0.0;
    stats->blockCount     = 0;
    stats->gapCount       = 0;
    stats->havePrevBlock  = false;
    stats->havePlayAnchor = false;
    stats->historyWrite   = 0;
    memset(stats->history, 0, sizeof(stats->history));

    // THE BLOCK SIZE FIELDS ARE PART OF THE RESET TOO, and leaving them out was a bug rather than a
    // decision: the atomics below were being published as zero while the working fields kept their
    // old values, so the panel showed a cleared range and the very next block put the stale one
    // straight back. Every other figure here describes the run since the reset and these must as
    // well - not least so that a host whose buffer size is changed between runs stops reporting a
    // range it no longer uses.
    stats->prevFrames     = 0;
    stats->framesMin      = 0;
    stats->framesMax      = 0;
    stats->sizeChanges    = 0;

    // The window's contents are as much a part of the run as the sums are. Only the live entries
    // need clearing - windowCount is what says which those are - but the whole array is cheap here
    // and a reset is not on any hot path.
    stats->windowHead     = 0;
    stats->windowCount    = 0;
    stats->windowSumSq    = 0.0;
    stats->windowPushes   = 0;
    memset(stats->windowErrSq, 0, sizeof(stats->windowErrSq));
    memset(stats->windowTime, 0, sizeof(stats->windowTime));

    atomic_store(&stats->pubMarginMean, 0.0);
    atomic_store(&stats->pubMarginMin, 0.0);
    atomic_store(&stats->pubMarginRms, 0.0);
    atomic_store(&stats->pubPeriodRms, 0.0);
    atomic_store(&stats->pubPeriodRecentRms, 0.0);
    atomic_store(&stats->pubRecentSeconds, 0.0);
    atomic_store(&stats->pubRecentBlocks, (uint64_t)0);
    atomic_store(&stats->pubPeriodWorst, 0.0);
    atomic_store(&stats->pubFramesMin, 0u);
    atomic_store(&stats->pubFramesMax, 0u);
    atomic_store(&stats->pubSizeChanges, (uint64_t)0);
    atomic_store(&stats->pubBlocks, (uint64_t)0);
    atomic_store(&stats->pubGaps, (uint64_t)0);
    atomic_store(&stats->pubDriftPpm, 0.0);
    atomic_store(&stats->pubDriftValid, 0);
    atomic_store(&stats->pubHostBpm, 0.0);
    atomic_store(&stats->pubMeasuredBpm, 0.0);
    atomic_store(&stats->pubWindowSeconds, 0.0);
    atomic_store(&stats->pubTicks, (uint64_t)0);
    atomic_store(&stats->pubLate, (uint64_t)0);
    atomic_store(&stats->pubHistoryWrite, 0);
}

void ms_stats_tick(tMsStats * stats, uint64_t stampedHostTime, uint64_t submittedHostTime) {
    if (stats == NULL) {
        return;
    }
    // SIGNED, and that is the whole point - a stamp already in the past is the failure being looked
    // for, so it must not be lost to unsigned arithmetic.
    double marginMs = (stampedHostTime >= submittedHostTime)
                      ? host_to_ms(stampedHostTime - submittedHostTime)
                      : -host_to_ms(submittedHostTime - stampedHostTime);

    stats->sumMargin                   += marginMs;
    stats->sumMarginSq                 += (marginMs * marginMs);
    stats->tickCount++;

    if (marginMs < stats->minMargin) {
        stats->minMargin = marginMs;
    }

    if (marginMs <= 0.0) {
        stats->lateCount++;
    }
    stats->history[stats->historyWrite] = marginMs;
    stats->historyWrite                 = (stats->historyWrite + 1) % MS_STATS_HISTORY;
}

// THE SLIDING WINDOW BEHIND THE RECENT BLOCK-JITTER RMS - see MS_STATS_WINDOW_SECONDS in the header
// for why the panel's figure forgets and the worst case does not.
//
// Kept as a running sum with each leaving sample subtracted, which is O(1) per block. A running sum
// drifts, so it is recomputed exactly once every MS_STATS_WINDOW_MAX pushes: one pass over at most
// 8192 doubles, tens of seconds apart, which is nothing beside the arithmetic it keeps honest over a
// session hours long.
static int window_tail(const tMsStats * stats) {
    return ((stats->windowHead - stats->windowCount) + (2 * MS_STATS_WINDOW_MAX)) % MS_STATS_WINDOW_MAX;
}

static void window_drop_oldest(tMsStats * stats) {
    int tail = window_tail(stats);

    stats->windowSumSq -= stats->windowErrSq[tail];
    stats->windowCount--;
}

static void window_add(tMsStats * stats, double errMs, uint64_t when) {
    double sq = errMs * errMs;

    // A FULL RING OVERWRITES ITS OWN OLDEST ENTRY, so that entry has to leave the sum before it is
    // lost. Saturating means the host's buffer is small enough that MS_STATS_WINDOW_SECONDS does not
    // fit in the ring; the published span then reads shorter than the target, which is the truth and
    // is why it is published at all.
    if (stats->windowCount == MS_STATS_WINDOW_MAX) {
        window_drop_oldest(stats);
    }
    stats->windowErrSq[stats->windowHead] = sq;
    stats->windowTime[stats->windowHead]  = when;
    stats->windowSumSq                   += sq;
    stats->windowHead                     = (stats->windowHead + 1) % MS_STATS_WINDOW_MAX;
    stats->windowCount++;

    // TRIMMED BY TIME RATHER THAN BY A BLOCK COUNT, so the window is the same ten seconds whatever
    // the buffer size. ONE ENTRY ALWAYS STAYS: a window emptied by its own trim would publish a
    // zero RMS, which reads as a perfect host rather than as no measurement.
    while (  (stats->windowCount > 1)
          && (host_to_ms(when - stats->windowTime[window_tail(stats)])
              > (MS_STATS_WINDOW_SECONDS * 1000.0))) {
        window_drop_oldest(stats);
    }
    stats->windowPushes++;

    if ((stats->windowPushes % (uint64_t)MS_STATS_WINDOW_MAX) == 0) {
        double sum  = 0.0;
        int    tail = window_tail(stats);

        for (int i = 0; i < stats->windowCount; i++) {
            sum += stats->windowErrSq[(tail + i) % MS_STATS_WINDOW_MAX];
        }

        stats->windowSumSq = sum;
    }
}

void ms_stats_gap(tMsStats * stats) {
    if (stats != NULL) {
        stats->havePrevBlock = false;
    }
}

void ms_stats_block(tMsStats * stats,
                    uint64_t   blockHostTime,
                    uint32_t   blockFrames,
                    double     sampleRate,
                    double     tempo,
                    bool       playing) {
    if ((stats == NULL) || (sampleRate <= 0.0) || (blockFrames == 0)) {
        return;
    }
    double nominalMs = ((double)blockFrames / sampleRate) * 1000.0;

    // AGAINST THE PREVIOUS BLOCK'S DURATION, NOT THIS ONE'S. The wall time between callback n-1 and
    // callback n is the time the device took to consume block n-1's samples, so block n-1 is what it
    // has to be judged against.
    //
    // With a fixed block size the two are identical and the error is invisible - which is exactly
    // why it survived every offline run. A host that VARIES the block size, as Live does, then has
    // every size change reported as a timing error: a 256-frame change at 48 kHz is 5.33 ms, and it
    // turned up as a 5.209 ms worst case in Live with the clock itself perfectly steady.
    if (stats->havePrevBlock && (blockHostTime > stats->prevBlockTime)) {
        double errMs = host_to_ms(blockHostTime - stats->prevBlockTime) - stats->prevNominalMs;

        // A GAP IS NOT A LATE BLOCK. ms_stats_gap() covers the suspensions the host announces; this
        // covers the ones it does not, and backgrounding is the case in point - the process simply
        // stops being scheduled, with no setProcessing(false) to say so. Counted, never summed: one
        // of these is worth more than the whole rest of the session put together.
        if (fabs(errMs) > MS_STATS_GAP_MS) {
            stats->gapCount++;
        } else {
            stats->sumPeriodErrSq += (errMs * errMs);
            stats->blockCount++;
            window_add(stats, errMs, blockHostTime);

            if (fabs(errMs) > fabs(stats->worstPeriodErr)) {
                stats->worstPeriodErr = errMs;
            }
        }
    }

    // THE BLOCK SIZE ITSELF, because when the two figures above disagree with the wire the first
    // question is whether the host is handing over a constant block at all, and nothing else here
    // answers it.
    if (stats->havePrevBlock && (blockFrames != stats->prevFrames)) {
        stats->sizeChanges++;
    }

    if ((stats->framesMin == 0) || (blockFrames < stats->framesMin)) {
        stats->framesMin = blockFrames;
    }

    if (blockFrames > stats->framesMax) {
        stats->framesMax = blockFrames;
    }
    stats->prevFrames    = blockFrames;
    stats->prevNominalMs = nominalMs;
    stats->prevBlockTime = blockHostTime;
    stats->havePrevBlock = true;

    // DRIFT IS ONLY MEANINGFUL WHILE PLAYING, and only from an anchor taken while playing. Measuring
    // across a stop would count the stopped wall time against no musical time at all and report an
    // enormous fictional drift.
    // WHETHER THE ANCHOR WAS ALREADY SET WHEN THIS BLOCK ARRIVED, which decides whether this block's
    // frames belong inside the window. They do not: the anchor's wall time is taken at the START of
    // this block, so counting the frames it goes on to deliver puts one whole block of audio time
    // against no wall time at all.
    //
    // It showed as a drift figure decaying like 1/t - 158 ppm at 30 seconds, 138 at 34 - which is
    // the signature of a fixed offset being amortised rather than of any real drift. One 256-frame
    // block is 5.33 ms, and 5.33 ms over 30 s is 178 ppm.
    bool   hadAnchor     = stats->havePlayAnchor;

    if (!playing) {
        stats->havePlayAnchor = false;
    } else if (!stats->havePlayAnchor) {
        stats->firstPlayTime  = blockHostTime;
        stats->firstPlayTicks = stats->tickCount;
        stats->audioSeconds   = 0.0;
        stats->havePlayAnchor = true;
    }
    double windowSeconds = 0.0;
    double driftPpm      = 0.0;
    double measuredBpm   = 0.0;

    // COUNTED IN FRAMES, NOT IN TICKS.
    //
    // Ticks are integers arriving about every 19 ms, so over a short window the quantisation alone
    // dominates: one tick either way across five seconds is some 3800 ppm, and the figure jumped
    // about far too much to read. Frames are the audio clock itself and have no such step.
    //
    // WHAT IT NOW MEANS, precisely: the host's audio clock against the system clock. The blocks
    // arrive at the interface's sample rate while the timestamps this plug-in hands CoreMIDI are
    // mach time, so this is exactly the quantity that decides whether a generated clock stays with
    // the music over a long session. It is normally tens of ppm - GenBridge measures the same
    // quantity between two devices and steers a ring on it.
    if (hadAnchor && stats->havePlayAnchor) {
        stats->audioSeconds += ((double)blockFrames / sampleRate);
    }

    if (stats->havePlayAnchor && (blockHostTime > stats->firstPlayTime)) {
        double wallSeconds = host_to_ms(blockHostTime - stats->firstPlayTime) / 1000.0;

        windowSeconds = wallSeconds;

        // A LONG WINDOW OR NOTHING. At ten seconds a single block of scheduling slop is still worth
        // about 500 ppm, so a figure produced before then is measuring the last hiccup rather than
        // any drift. The panel shows the window filling instead of a number that swings.
        if (wallSeconds >= MS_STATS_DRIFT_SECONDS) {
            driftPpm = ((stats->audioSeconds / wallSeconds) - 1.0) * 1.0e6;
        }

        if ((wallSeconds > 1.0) && (stats->tickCount > stats->firstPlayTicks)) {
            measuredBpm = (((double)(stats->tickCount - stats->firstPlayTicks) / (double)MS_PPQN)
                           / wallSeconds) * 60.0;
        }
    }
    atomic_store(&stats->pubMarginMean,
                 (stats->tickCount > 0) ? (stats->sumMargin / (double)stats->tickCount) : 0.0);
    atomic_store(&stats->pubMarginMin, isfinite(stats->minMargin) ? stats->minMargin : 0.0);
    atomic_store(&stats->pubMarginRms,
                 (stats->tickCount > 0) ? sqrt(stats->sumMarginSq / (double)stats->tickCount) : 0.0);
    atomic_store(&stats->pubPeriodRms,
                 (stats->blockCount > 0) ? sqrt(stats->sumPeriodErrSq / (double)stats->blockCount) : 0.0);

    // THE WINDOW'S SPAN IS MEASURED, NOT ASSUMED. It is shorter than MS_STATS_WINDOW_SECONDS while
    // the window fills, shorter again if the ring saturated, and shorter than the wall time it
    // covers if a suspension took samples out of it - and every one of those is worth seeing, so
    // the figure comes from the entries themselves rather than from a block count and a rate.
    double recentSeconds = 0.0;

    if (stats->windowCount > 1) {
        int newest = ((stats->windowHead - 1) + MS_STATS_WINDOW_MAX) % MS_STATS_WINDOW_MAX;

        recentSeconds = host_to_ms(stats->windowTime[newest] - stats->windowTime[window_tail(stats)]) / 1000.0;
    }
    atomic_store(&stats->pubPeriodRecentRms,
                 (stats->windowCount > 0)
                 ? sqrt(fmax(stats->windowSumSq, 0.0) / (double)stats->windowCount)
                 : 0.0);
    atomic_store(&stats->pubRecentSeconds, recentSeconds);
    atomic_store(&stats->pubRecentBlocks, (uint64_t)stats->windowCount);
    atomic_store(&stats->pubPeriodWorst, stats->worstPeriodErr);
    atomic_store(&stats->pubFramesMin, stats->framesMin);
    atomic_store(&stats->pubFramesMax, stats->framesMax);
    atomic_store(&stats->pubSizeChanges, stats->sizeChanges);
    atomic_store(&stats->pubBlocks, stats->blockCount);
    atomic_store(&stats->pubGaps, stats->gapCount);
    atomic_store(&stats->pubDriftPpm, driftPpm);
    atomic_store(&stats->pubDriftValid, (windowSeconds >= MS_STATS_DRIFT_SECONDS) ? 1 : 0);
    atomic_store(&stats->pubHostBpm, tempo);
    atomic_store(&stats->pubMeasuredBpm, measuredBpm);
    atomic_store(&stats->pubWindowSeconds, windowSeconds);
    atomic_store(&stats->pubTicks, stats->tickCount);
    atomic_store(&stats->pubLate, stats->lateCount);
    atomic_store(&stats->pubHistoryWrite, stats->historyWrite);
}

void ms_stats_read(const tMsStats * stats, tMsStatsSnapshot * out) {
    if ((stats == NULL) || (out == NULL)) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return;
    }
    out->marginMeanMs           = atomic_load(&stats->pubMarginMean);
    out->marginMinMs            = atomic_load(&stats->pubMarginMin);
    out->marginRmsMs            = atomic_load(&stats->pubMarginRms);
    out->blockGaps              = atomic_load(&stats->pubGaps);
    out->blockPeriodRmsMs       = atomic_load(&stats->pubPeriodRms);
    out->blockPeriodRecentRmsMs = atomic_load(&stats->pubPeriodRecentRms);
    out->blockRecentSeconds     = atomic_load(&stats->pubRecentSeconds);
    out->blockRecentBlocks      = atomic_load(&stats->pubRecentBlocks);
    out->blockPeriodWorstMs     = atomic_load(&stats->pubPeriodWorst);
    out->blockFramesMin         = atomic_load(&stats->pubFramesMin);
    out->blockFramesMax         = atomic_load(&stats->pubFramesMax);
    out->blockSizeChanges       = atomic_load(&stats->pubSizeChanges);
    out->blocks                 = atomic_load(&stats->pubBlocks);
    out->driftPpm               = atomic_load(&stats->pubDriftPpm);
    out->driftValid             = (atomic_load(&stats->pubDriftValid) != 0);
    out->hostBpm                = atomic_load(&stats->pubHostBpm);
    out->measuredBpm            = atomic_load(&stats->pubMeasuredBpm);
    out->ticks                  = atomic_load(&stats->pubTicks);
    out->lateTicks              = atomic_load(&stats->pubLate);
    out->windowSeconds          = atomic_load(&stats->pubWindowSeconds);
}

int ms_stats_history(const tMsStats * stats, double * out, int max) {
    if ((stats == NULL) || (out == NULL) || (max <= 0)) {
        return 0;
    }
    int count = (max < MS_STATS_HISTORY) ? max : MS_STATS_HISTORY;
    int write = atomic_load(&stats->pubHistoryWrite);

    // Oldest first. The ring can be read while the audio thread is writing it, so an entry or two
    // may be a frame stale or half-updated - for a graph that is not worth a lock on the audio side.
    for (int i = 0; i < count; i++) {
        int index = ((write - count + i) + (2 * MS_STATS_HISTORY)) % MS_STATS_HISTORY;

        out[i] = stats->history[index];
    }

    return count;
}
