/*
 * MidiSyncTool - live figures the processor publishes and the editor reads.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msStatus.h.md - "// notes §k" refers there.

#ifndef __MS_STATUS_H__
#define __MS_STATUS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// notes §1
#define MS_STATUS_SLOTS      (32)
#define MS_STATUS_HISTORY    (128)   // recent round trips, for the scrolling graph

typedef struct {
    atomic_bool active;

    // notes §2
    atomic_uint clearRequest;

    char        destName[128];
    atomic_int  haveDestination;

    // The saved port is named in the project but is not plugged in. Distinct from "nothing selected":
    // one is a plug-in waiting for hardware it has been told to use, the other has never been told
    // anything, and rendering them the same way is how a saved setup silently drives the wrong port.
    atomic_int     waitingForDevice;
    char           waitingName[128];

    // What the host is doing.
    _Atomic double hostBpm;
    atomic_int     playing;
    _Atomic double ppq;

    // The clock we generate, and how comfortably it is being scheduled.
    atomic_uint    ticksSent;
    _Atomic double commitMarginMeanMs;
    _Atomic double commitMarginMinMs;
    atomic_uint    lateTicks;
    // notes §3
    _Atomic double blockPeriodRmsMs;
    _Atomic double blockPeriodRecentRmsMs;
    _Atomic double blockRecentSeconds;

    // notes §4
    _Atomic double blockPeriodWorstMs;

    // HOW MANY INTERVALS WERE REJECTED AS SUSPENSIONS rather than measured - see MS_STATS_GAP_MS.
    // Shown beside the jitter figure, never swallowed: a metric that silently drops its worst
    // samples is a prettier lie than one that keeps them.
    atomic_uint    blockGaps;

    // What is LEFT of that once the timebase model has absorbed what it can - the part the output
    // clock actually inherits. Shown beside the raw figure, because either number on its own says
    // nothing about whether the absorbing works.
    _Atomic double residualRmsMs;
    atomic_uint    modelResyncs;
    _Atomic double driftPpm;
    atomic_int     driftValid;      // false while the window is still filling
    _Atomic double driftSeconds;    // how much of that window there is so far

    // What came back from the hardware.
    _Atomic double roundTripMeanMs;
    _Atomic double roundTripMinMs;
    _Atomic double roundTripMaxMs;
    _Atomic double roundTripJitterMs;
    _Atomic double roundTripPeakDevMs;
    atomic_uint    hits;
    atomic_uint    missed;
    atomic_uint    spurious;
    _Atomic float  inputPeak;

    // notes §5
    _Atomic double scheduleLeadMs;

    // notes §6
    _Atomic double sampleRate;
    atomic_uint    blockFrames;
    _Atomic double blockMs;

    // HOW OFTEN THE HOST SPLIT ONE. Corrected for, never hidden: a figure that quietly discards
    // samples has to say how many, and the count is also what says the correction is firing on real
    // splits rather than on ordinary blocks.
    atomic_uint    blockSplits;
    _Atomic double inputPathMs;
    _Atomic double compensationMs;

    // notes §7
    atomic_int     mode;
    _Atomic double monitorPeriodMs;
    _Atomic double monitorBpm;
    atomic_uint    monitorOnsets;

    // notes §8
    atomic_int     haveClockSource;
    char           clockSourceName[128];
    _Atomic double clockInBpm;
    _Atomic double clockInPeriodMs;
    _Atomic double clockInJitterMs;
    _Atomic double clockInPeakDevMs;
    atomic_uint    clockInFitted;
    atomic_uint    clockInClocks;
    atomic_uint    clockInGaps;
    atomic_int     clockInRunning;

    atomic_int     probeRunning;
    atomic_int     historyWrite;
    _Atomic float  history[MS_STATUS_HISTORY];
} tMsStatus;

int ms_status_claim(void);
void ms_status_release(int slot);
tMsStatus * ms_status(int slot);    // NULL for an out-of-range or unclaimed slot

#ifdef __cplusplus
}
#endif

#endif // __MS_STATUS_H__
