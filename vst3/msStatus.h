/*
 * MidiSyncTool - live figures the processor publishes and the editor reads.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_STATUS_H__
#define __MS_STATUS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// ONE OF THESE PER PLUG-IN INSTANCE, not one per process - the mechanism is GenBridge's and the
// reason it is not a single global is a bug that project actually had: with two instances loaded,
// every editor read whichever processor wrote last, so a panel showing one device reported figures
// from another. Two answers, one of them a lie.
//
// The processor claims a slot on construction and tells its controller which one over
// IConnectionPoint, the channel VST3 provides for exactly this. The two are separate registered
// classes precisely so a host MAY keep them apart, and nothing else bridges them.
//
// Everything here is written by the audio thread and read by the UI thread, so it is all atomic and
// none of it is a pointer. The destination name is the exception - a fixed buffer copied under no
// lock, on the grounds that the worst case is a torn string in a readout that refreshes thirty
// times a second.
#define MS_STATUS_SLOTS      (32)
#define MS_STATUS_HISTORY    (128)   // recent round trips, for the scrolling graph

typedef struct {
    atomic_bool active;
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
    _Atomic double blockPeriodRmsMs;

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

    // The breakdown. scheduleLeadMs is exact and ours; inputPathMs is the interface's A/D plus the
    // host's input buffering and is NOT knowable from inside the plug-in - it stays at zero, and is
    // shown as unknown, until an audio loopback measures it. Everything else is derived from those
    // two and the measured round trip, so the panel never invents a term it does not have.
    _Atomic double scheduleLeadMs;

    // THE HOST'S BUFFER. A hosted plug-in can report this and never set it - the host owns it. It is
    // reported because it is an exactly knowable part of the input path: the audio in a block was
    // captured at least one buffer before the block was handed over.
    _Atomic double sampleRate;
    atomic_uint    blockFrames;
    _Atomic double blockMs;
    _Atomic double inputPathMs;
    _Atomic double compensationMs;

    // WHICH MODE, as a tMsMode - and it is the panel's licence to draw a figure as live, as HELD or
    // as a dash. Not every number on the panel means something in every mode: in monitor nothing is
    // generated, so the round trip has no reference and the latency breakdown has nothing to break
    // down; in clock only nothing is listened for, so every measured figure is the last run's.
    // Showing any of those the way a live reading is shown is how a stale number gets written down
    // as a measurement an hour later.
    atomic_int     mode;
    _Atomic double monitorPeriodMs;
    _Atomic double monitorBpm;
    atomic_uint    monitorOnsets;

    // ---- THE CLOCK COMING IN, which is measured and never acted on ------------------------
    //
    // Deliberately separate from every figure above. Those describe a clock this plug-in generated
    // against a timebase it owns; these describe someone else's clock arriving on a wire, and the
    // only thing the two share is the panel they are drawn on. Mixing them would invite exactly the
    // comparison that has to be made carefully or not at all.
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
