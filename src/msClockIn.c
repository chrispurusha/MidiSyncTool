/*
 * MidiSyncTool - what an INCOMING MIDI clock is doing: tempo, spread and transport.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "msClockIn.h"

#define MIDI_CLOCK       (0xF8)
#define MIDI_START       (0xFA)
#define MIDI_CONTINUE    (0xFB)
#define MIDI_STOP        (0xFC)

#define MS_PPQN          (24)

// Refit once a beat rather than once a clock. The window is 192 points and a least-squares pass
// over it is a few microseconds, but this runs on the CoreMIDI receive thread and there is no
// reason to spend it 24 times a beat for a readout that refreshes 30 times a second.
#define MS_REFIT_EVERY    (MS_PPQN)

struct tMsClockIn {
    // ---- owned by the CoreMIDI receive thread, and by nothing else --------------------------
    double           arrivalMs[MS_CLOCK_IN_WINDOW]; // milliseconds since the window's first clock
    uint64_t         windowStart;                   // host time of arrivalMs[0]
    int              count;
    int              sinceFit;
    uint64_t         prevArrival;
    double           lastPeriodMs;                  // the running estimate, for the gap test

    uint64_t         clocks;
    uint64_t         gaps;
    uint64_t         starts;
    uint64_t         stops;
    uint64_t         continues;
    bool             running;

    // ---- published, read by anyone --------------------------------------------------------
    _Atomic double   pubBpm;
    _Atomic double   pubPeriodMs;
    _Atomic double   pubJitterMs;
    _Atomic double   pubPeakDevMs;
    atomic_uint      pubFitted;
    _Atomic uint64_t pubClocks;
    _Atomic uint64_t pubGaps;
    _Atomic uint64_t pubStarts;
    _Atomic uint64_t pubStops;
    _Atomic uint64_t pubContinues;
    atomic_int       pubRunning;
    _Atomic uint64_t pubLastArrival;
};

static double host_to_ms(uint64_t delta) {
    return (double)AudioConvertHostTimeToNanos(delta) / 1.0e6;
}

static int compare_double(const void * a, const void * b) {
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

// THE FIT, AND THE THREE STEPS IT CANNOT SKIP.
//
// A MIDI clock looks like the easiest thing in the world to fit a line to - it is one event every
// twenty-fourth of a beat, for ever, with no rests. That is exactly why the first two steps look
// unnecessary, and they are not:
//
//   1. The period comes from the MEDIAN interval, not the mean. One dropped clock doubles an
//      interval, and a mean carries that into the rate for the whole window.
//   2. Each arrival is assigned to a SLOT, n = round(t / period), not to its ordinal position. A
//      single dropped clock otherwise shifts every arrival after it by one slot and the fit is
//      measuring the drop rather than the clock. This is the third place in this project the same
//      omission would have bitten - see monitor mode, where a rest in the pattern did it.
//   3. Phase and rate are fitted TOGETHER. The two ends of this measurement are different crystals,
//      tens of ppm apart, and a phase-only fit reports that separation as jitter growing with the
//      length of the run.
static void refit(tMsClockIn * in) {
    if (in->count < MS_CLOCK_IN_MIN_FIT) {
        return;
    }
    int    gapCount    = in->count - 1;
    double gaps[MS_CLOCK_IN_WINDOW];

    for (int i = 0; i < gapCount; i++) {
        gaps[i] = in->arrivalMs[i + 1] - in->arrivalMs[i];
    }

    qsort(gaps, (size_t)gapCount, sizeof(gaps[0]), compare_double);

    double period      = gaps[gapCount / 2];

    if (period <= 0.0) {
        return;
    }
    // Least squares of arrival time against slot number.
    double sumN = 0.0, sumT = 0.0, sumNN = 0.0, sumNT = 0.0;
    double slot[MS_CLOCK_IN_WINDOW];

    for (int i = 0; i < in->count; i++) {
        slot[i] = round(in->arrivalMs[i] / period);
        sumN   += slot[i];
        sumT   += in->arrivalMs[i];
        sumNN  += (slot[i] * slot[i]);
        sumNT  += (slot[i] * in->arrivalMs[i]);
    }

    double n           = (double)in->count;
    double denominator = (n * sumNN) - (sumN * sumN);

    if (fabs(denominator) < 1.0e-9) {
        return;
    }
    double slope       = ((n * sumNT) - (sumN * sumT)) / denominator; // ms per clock
    double intercept   = (sumT - (slope * sumN)) / n;

    if (slope <= 0.0) {
        return;
    }
    double sumSq       = 0.0;
    double peak        = 0.0;

    for (int i = 0; i < in->count; i++) {
        double residual = in->arrivalMs[i] - (intercept + (slope * slot[i]));

        sumSq += (residual * residual);

        if (fabs(residual) > fabs(peak)) {
            peak = residual;
        }
    }

    in->lastPeriodMs = slope;

    atomic_store(&in->pubPeriodMs, slope);
    atomic_store(&in->pubBpm, 60000.0 / (slope * (double)MS_PPQN));
    atomic_store(&in->pubJitterMs, sqrt(sumSq / n));
    atomic_store(&in->pubPeakDevMs, fabs(peak));
    atomic_store(&in->pubFitted, (unsigned)in->count);
}

tMsClockIn * ms_clock_in_create(void) {
    tMsClockIn * in = calloc(1, sizeof(*in));

    return in;
}

void ms_clock_in_destroy(tMsClockIn * in) {
    free(in);
}

void ms_clock_in_reset(tMsClockIn * in) {
    if (in == NULL) {
        return;
    }
    in->count        = 0;
    in->sinceFit     = 0;
    in->prevArrival  = 0;
    in->windowStart  = 0;
    in->lastPeriodMs = 0.0;
    in->clocks       = 0;
    in->gaps         = 0;
    in->starts       = 0;
    in->stops        = 0;
    in->continues    = 0;
    in->running      = false;

    atomic_store(&in->pubBpm, 0.0);
    atomic_store(&in->pubPeriodMs, 0.0);
    atomic_store(&in->pubJitterMs, 0.0);
    atomic_store(&in->pubPeakDevMs, 0.0);
    atomic_store(&in->pubFitted, 0u);
    atomic_store(&in->pubClocks, (uint64_t)0);
    atomic_store(&in->pubGaps, (uint64_t)0);
    atomic_store(&in->pubStarts, (uint64_t)0);
    atomic_store(&in->pubStops, (uint64_t)0);
    atomic_store(&in->pubContinues, (uint64_t)0);
    atomic_store(&in->pubRunning, 0);
    atomic_store(&in->pubLastArrival, (uint64_t)0);
}

// Drop the window but keep the counters: a gap ends a measurement, it does not undo the ones before
// it. The published figures are left standing until a new fit replaces them, for the same reason the
// detector keeps its figures when it goes quiet - a reading that vanishes the moment the master
// pauses is less useful than one that says what it last saw.
static void restart_window(tMsClockIn * in) {
    in->count       = 0;
    in->sinceFit    = 0;
    in->windowStart = 0;
    atomic_store(&in->pubFitted, 0u);
}

static void push_arrival(tMsClockIn * in, uint64_t hostTime) {
    if (in->count == 0) {
        in->windowStart  = hostTime;
        in->arrivalMs[0] = 0.0;
        in->count        = 1;
        return;
    }
    double ms    = host_to_ms(hostTime - in->windowStart);

    if (in->count < MS_CLOCK_IN_WINDOW) {
        in->arrivalMs[in->count] = ms;
        in->count++;
        return;
    }
    // FULL: slide by one. Re-basing on the new first arrival keeps the stored milliseconds small
    // and, more to the point, keeps the slot numbers small - a window that counted slots from the
    // first clock of the session would be doing least squares on numbers in the millions.
    double shift = in->arrivalMs[1];

    for (int i = 1; i < MS_CLOCK_IN_WINDOW; i++) {
        in->arrivalMs[i - 1] = in->arrivalMs[i] - shift;
    }

    in->arrivalMs[MS_CLOCK_IN_WINDOW - 1] = ms - shift;
    in->windowStart                      += AudioConvertNanosToHostTime((uint64_t)(shift * 1.0e6));
}

void ms_clock_in_byte(tMsClockIn * in, uint8_t status, uint64_t hostTime) {
    if (in == NULL) {
        return;
    }

    // A SOURCE MAY STAMP A PACKET 0, meaning "now". Rare for a clock and fatal to a fit if taken
    // literally - every arrival would land at the same moment - so it becomes the moment we read it,
    // which is the best this side can do and is honest about being worse.
    if (hostTime == 0) {
        hostTime = AudioGetCurrentHostTime();
    }

    switch (status) {
        case MIDI_START:
            in->starts++;
            in->running = true;
            restart_window(in);
            atomic_store(&in->pubStarts, in->starts);
            atomic_store(&in->pubRunning, 1);
            break;

        case MIDI_CONTINUE:
            in->continues++;
            in->running = true;
            restart_window(in);
            atomic_store(&in->pubContinues, in->continues);
            atomic_store(&in->pubRunning, 1);
            break;

        case MIDI_STOP:
            in->stops++;
            in->running = false;
            restart_window(in);
            atomic_store(&in->pubStops, in->stops);
            atomic_store(&in->pubRunning, 0);
            break;

        case MIDI_CLOCK:
        {
            if (in->prevArrival != 0) {
                double intervalMs = host_to_ms(hostTime - in->prevArrival);
                double expected   = (in->lastPeriodMs > 0.0) ? in->lastPeriodMs : 0.0;

                // NOT A CLOCK PERIOD. Either the master stopped without saying so, the port was
                // switched, or something was unplugged. The window goes; the counters do not.
                if ((expected > 0.0) && (intervalMs > (expected * MS_CLOCK_IN_GAP_RATIO))) {
                    in->gaps++;
                    atomic_store(&in->pubGaps, in->gaps);
                    restart_window(in);
                }
            }
            in->prevArrival = hostTime;
            in->clocks++;
            push_arrival(in, hostTime);
            in->sinceFit++;

            if (in->sinceFit >= MS_REFIT_EVERY) {
                in->sinceFit = 0;
                refit(in);
            }
            atomic_store(&in->pubClocks, in->clocks);
            atomic_store(&in->pubLastArrival, hostTime);
            break;
        }

        default:
            break;
    }
}

void ms_clock_in_read(const tMsClockIn * in, tMsClockInSnapshot * out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (in == NULL) {
        return;
    }
    out->bpm         = atomic_load(&in->pubBpm);
    out->periodMs    = atomic_load(&in->pubPeriodMs);
    out->jitterRmsMs = atomic_load(&in->pubJitterMs);
    out->peakDevMs   = atomic_load(&in->pubPeakDevMs);
    out->fitted      = atomic_load(&in->pubFitted);
    out->clocks      = atomic_load(&in->pubClocks);
    out->gaps        = atomic_load(&in->pubGaps);
    out->starts      = atomic_load(&in->pubStarts);
    out->stops       = atomic_load(&in->pubStops);
    out->continues   = atomic_load(&in->pubContinues);
    out->running     = (atomic_load(&in->pubRunning) != 0);
    out->lastArrival = atomic_load(&in->pubLastArrival);
}
