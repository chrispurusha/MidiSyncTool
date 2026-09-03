/*
 * MidiSyncTool - audio transient detection, and MIDI-to-audio latency from it.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msDetect.h"
#include "msLog.h"

// HOW LONG A SCHEDULED EVENT WAITS FOR ITS TRANSIENT before it is written off. Generous, because a
// device that is genuinely 150 ms late is a finding rather than a miss, and because a miss and a
// wrong answer are not equally bad - a miss is visible.
#define MS_DETECT_WINDOW_MS    (300.0)

// A transient cannot be followed by another within this. It is not musical spacing: it is how long
// one hit's own decay is refused a second detection. 40 ms is under a sixteenth at any tempo anyone
// calibrates at (115 ms at 130 BPM) and well over any drum's attack.
#define MS_DETECT_REFRACTORY_MS    (40.0)

// AND THE DETECTOR DOES NOT RE-ARM UNTIL THE SOUND HAS ACTUALLY GONE.
//
// A time-only refractory cannot know whether the sound that caused a detection has finished. The
// Tempest's kick had not: against a 40 ms refractory it produced one spurious onset per hit, at gaps
// of 40.0 to 46.3 ms - several at exactly the refractory, which is the giveaway that the envelope
// was still over the threshold the instant the gate lifted.
//
// Captured from the device at 48 kHz, that kick is still within 15 dB of peak 100 ms after the
// onset and ripples by 5 to 10 dB on the way down. So the re-arm is measured against THE HIT'S OWN
// PEAK rather than against a threshold the drum is dragging upwards - and against the frozen noise
// floor as well, since 12 dB alone is inside the ripple.
#define MS_DETECT_REARM_RATIO    (0.25)     // 12 dB below the hit's own peak

// The noise floor follower, and the margin a transient has to clear it by. A slow follower means a
// sustained sound raises the floor and stops retriggering; 12 dB of margin is enough to ignore
// bleed and cymbal tails without needing the signal loud.
#define MS_DETECT_FLOOR_TAU_MS    (400.0)
#define MS_DETECT_MARGIN          (4.0)     // 12 dB over the running floor
#define MS_DETECT_ABS_FLOOR       (0.0015)  // about -56 dBFS; below this it is the room, not a hit

// The envelope's decay. Attack is instantaneous by design - the whole measurement is of an onset, so
// smoothing the rise would be smoothing away the thing being measured.
#define MS_DETECT_ENV_TAU_MS    (15.0)

typedef struct {
    uint64_t stampedHostTime;
    bool     used;
} tExpected;

// HOW MANY ONSETS THE FITTED WINDOW HOLDS. A SLIDING window rather than the whole run, deliberately:
// a fit over everything since the transport started would slowly stop describing the present, and a
// tempo that moved once would bias the residual for ever afterwards. 256 quarter notes at 130 BPM is
// about two minutes, which is far more than enough to separate jitter from drift.
#define MS_DETECT_ONSETS    (256)

struct tMsDetect {
    // Audio thread only.
    int              division;
    double           envelope;
    double           floorLevel;
    double           highpassState;
    double           highpassPrevIn;
    tMsDetectSource  source;
    bool             armed;
    int              pending;        // expectations still waiting for a transient
    double           onsetPeak;      // the loudest the envelope reached since the gate closed
    uint64_t         lastOnsetTime;
    double           refractoryLeft; // seconds
    double           peak;

    tExpected        expected[MS_DETECT_EXPECTED];
    int              expectedWrite;

    // Monitor mode's sliding window of onset times, oldest-first once it has wrapped.
    uint64_t         onsets[MS_DETECT_ONSETS];
    int              onsetWrite;
    int              onsetCount;

    double           sumLatency;
    double           sumLatencySq;
    double           minLatency;
    double           maxLatency;
    uint64_t         hits;
    uint64_t         missed;
    uint64_t         spurious;

    // Published for whoever displays it.
    _Atomic double   pubMean;
    _Atomic double   pubLast;
    _Atomic double   pubMin;
    _Atomic double   pubMax;
    _Atomic double   pubJitter;
    _Atomic double   pubPeakDev;
    _Atomic double   pubInputPeak;
    _Atomic uint64_t pubHits;
    _Atomic uint64_t pubMissed;
    _Atomic uint64_t pubSpurious;
    _Atomic double   pubMonitorPeriod;
    _Atomic double   pubMonitorBpm;
    _Atomic uint64_t pubMonitorOnsets;
};

// ── Monitor mode: recovering a grid from the onsets alone ────────────────────────────────────────
//
// Three steps, and the middle one is the one that is easy to leave out.
//
//   1. Estimate the hit PERIOD as the median interval between consecutive onsets. Median rather than
//      mean because a missing hit produces an interval of twice the period, and one of those drags a
//      mean badly while leaving a median alone.
//   2. Assign each onset to a grid SLOT, n = round((t - t0) / period). Without this a pattern with
//      any rest in it is fitted against a straight 0,1,2,3... counter, the slots after the rest are
//      all off by one, and the fit reports the resulting nonsense as jitter.
//   3. Least squares for phase and rate together over (n, t), then the residual about that line.
//
// Assumes the hits are on a REGULAR grid, which is what a calibration pattern is. A deliberately
// swung or humanised pattern would have its swing reported as jitter, correctly but uselessly.

static void monitor_fit(tMsDetect * detect) {
    int      count    = detect->onsetCount;

    if (count < 8) {
        return;     // too few to find a repeat in, let alone fit one
    }
    double   times[MS_DETECT_ONSETS];
    double   gaps[MS_DETECT_ONSETS];
    int      first    = (detect->onsetCount < MS_DETECT_ONSETS)
                     ? 0 : detect->onsetWrite;                  // oldest first, once it has wrapped
    uint64_t base     = detect->onsets[first % MS_DETECT_ONSETS];

    for (int i = 0; i < count; i++) {
        uint64_t at = detect->onsets[(first + i) % MS_DETECT_ONSETS];

        times[i] = (at >= base)
                   ? ((double)AudioConvertHostTimeToNanos(at - base) / 1.0e6) : 0.0;
    }

    for (int i = 1; i < count; i++) {
        gaps[i - 1] = times[i] - times[i - 1];
    }

    int      gapCount = count - 1;

    // ── 1. HOW LONG THE PATTERN IS, in onsets ───────────────────────────────────────────────────
    //
    // THE ASSUMPTION THAT A SINGLE EVENLY-SPACED GRID DESCRIBES A DRUM PART IS FALSE, and it fails
    // in a way that looks exactly like a broken device. Measured against a Tempest playing a real
    // part, self-clocked: the gaps alternate 122.4 and 348.3 ms, whose ratio is 2.845 - not a whole
    // number, so NO uniform grid can contain both. Fitting one anyway reported 31 ms RMS of
    // "jitter" from a machine whose gaps repeat to within a millisecond.
    //
    // What is actually there is a PATTERN that repeats: k onsets, then the same k again. Each
    // position within it can sit wherever it likes - swung, or displaced because that position is a
    // different drum whose attack the detector crosses at a different moment - and none of that is
    // jitter. Jitter is how much a given position moves FROM ONE REPEAT TO THE NEXT, and that is
    // what this measures, by fitting each position its own line and pooling the residuals.
    //
    // The repeat length is the smallest k whose gap sequence matches itself k apart.
    double   meanGap  = 0.0;

    for (int i = 0; i < gapCount; i++) {
        meanGap += gaps[i];
    }

    meanGap /= (double)gapCount;

    int      repeat   = 1;

    for (int k = 1; (k <= 16) && (k <= (gapCount / 3)); k++) {
        double worstMatch = 0.0;

        for (int i = 0; (i + k) < gapCount; i++) {
            double diff = fabs(gaps[i] - gaps[i + k]);

            if (diff > worstMatch) {
                worstMatch = diff;
            }
        }

        // 5 % of the mean gap: loose enough for a device with real jitter, tight enough that a
        // genuinely different position is never mistaken for a match. Smallest k wins, so a pattern
        // that repeats every 2 is not described as repeating every 4.
        if (worstMatch < (0.05 * meanGap)) {
            repeat = k;
            break;
        }
    }

    // ── 2. EACH POSITION GETS ITS OWN LINE ──────────────────────────────────────────────────────
    //
    // Position q occurs at onsets q, q+k, q+2k... spaced by one CYCLE. Those are evenly spaced by
    // construction whatever the pattern does inside the cycle, so a straight line fits them, and the
    // residual about it is the only thing here that deserves to be called jitter.
    double   sumSq      = 0.0;
    double   worst      = 0.0;
    int      residuals  = 0;
    double   cycleSum   = 0.0;
    int      cycleTerms = 0;
    double   offsets[16];

    // THE CYCLE INDEX COMES FROM TIME, NOT FROM THE ONSET'S ORDINAL. This is the difference between
    // a fit that survives a missed hit and one that is destroyed by it.
    //
    // Counting 0, 1, 2... along the onsets assumes every hit was detected. Miss ONE - a quiet hit, a
    // threshold not quite crossed - and every onset after it is numbered one cycle short, the fit is
    // dragged through a discontinuity, and the residuals explode. Measured: the same sample and
    // pattern that reads 0.132 ms on a clean run reported 8.65 ms RMS with a 70 ms peak on a run
    // that dropped about three hits in eighty.
    //
    // Rounding elapsed time to the nearest whole cycle instead leaves a GAP in the numbering where
    // the missing hit was, which is what actually happened, and costs nothing when nothing is
    // missing. The single-grid version this replaced had that property; the rewrite lost it.
    double   cycleGuess = 0.0;

    {
        double sum = 0.0;
        int    n   = 0;

        for (int i = 0; (i + repeat) < count; i++) {
            sum += (times[i + repeat] - times[i]);
            n++;
        }

        cycleGuess = (n > 0) ? (sum / (double)n) : 0.0;
    }

    if (!(cycleGuess > 0.0)) {
        return;
    }
    uint64_t dropped    = 0;

    for (int q = 0; q < repeat; q++) {
        double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
        int    n     = 0;
        double from  = 0.0;
        double lastX = 0.0;

        for (int i = q; i < count; i += repeat) {
            if (n == 0) {
                from = times[i];
            }
            double x = floor(((times[i] - from) / cycleGuess) + 0.5);

            if ((n > 0) && (x > (lastX + 1.0))) {
                dropped += (uint64_t)(x - lastX - 1.0);
            }
            lastX  = x;

            sumX  += x;
            sumY  += times[i];
            sumXX += (x * x);
            sumXY += (x * times[i]);
            n++;
        }

        if (n < 3) {
            continue;
        }
        double dx        = ((double)n * sumXX) - (sumX * sumX);

        if (fabs(dx) < 1.0e-9) {
            continue;
        }
        double slope     = (((double)n * sumXY) - (sumX * sumY)) / dx;
        double intercept = (sumY - (slope * sumX)) / (double)n;

        cycleSum  += slope;
        cycleTerms++;
        offsets[q] = intercept;

        double fromM     = 0.0;
        int    seen      = 0;

        for (int i = q; i < count; i += repeat) {
            if (seen == 0) {
                fromM = times[i];
            }
            double m        = floor(((times[i] - fromM) / cycleGuess) + 0.5);
            double residual = times[i] - (intercept + (slope * m));

            seen++;
            sumSq += (residual * residual);
            residuals++;

            if (fabs(residual) > worst) {
                worst = fabs(residual);
            }
        }
    }

    if ((cycleTerms == 0) || (residuals < 3)) {
        return;
    }
    double cycleMs = cycleSum / (double)cycleTerms;

    // WHERE EACH HIT SITS INSIDE THE CYCLE, as a fraction. This is the swing measurement, and it is
    // the figure the jitter number deliberately throws away: two hits at 0.000 and 0.250 are
    // straight sixteenths, at 0.000 and 0.260 they are displaced by 1 % of a cycle. Whether that is
    // the sequencer swinging or the detector crossing two different drums' attacks at different
    // points, this is the number that shows it - and it CANNOT be seen in an RMS.
    {
        static int gShape = 0;

        if (((gShape++ % 40) == 0) && (cycleMs > 0.0)) {
            char line[256];
            int  at = 0;

            for (int q = 0; (q < repeat) && (at < (int)sizeof(line) - 10); q++) {
                double phase = (offsets[q] - offsets[0]) / cycleMs;

                at += snprintf(line + at, sizeof(line) - (size_t)at, "%.4f ", phase);
            }

            ms_log_line("  shape  | %d hits per %.3f ms cycle, at phases: %s",
                        repeat, cycleMs, line);
        }
    }
    atomic_store(&detect->pubJitter, sqrt(sumSq / (double)residuals));
    atomic_store(&detect->pubPeakDev, worst);
    atomic_store(&detect->pubMonitorPeriod, cycleMs);
    atomic_store(&detect->pubMonitorBpm, (cycleMs > 0.0) ? (60000.0 / cycleMs) : 0.0);
    atomic_store(&detect->pubMonitorOnsets, (uint64_t)count);
    atomic_store(&detect->pubHits, (uint64_t)count);
    atomic_store(&detect->pubMissed, dropped);
}

tMsDetect * ms_detect_create(void) {
    tMsDetect * detect = calloc(1, sizeof(tMsDetect));

    if (detect != NULL) {
        detect->division = 24;   // quarter notes, the pattern to start a calibration with
        ms_detect_reset(detect);
    }
    return detect;
}

void ms_detect_destroy(tMsDetect * detect) {
    free(detect);
}

void ms_detect_reset(tMsDetect * detect) {
    if (detect == NULL) {
        return;
    }
    int             division = detect->division;
    tMsDetectSource source   = detect->source;

    memset(detect, 0, sizeof(*detect));
    detect->source     = source;
    detect->armed      = true;
    detect->division   = (division > 0) ? division : 24;
    detect->minLatency = INFINITY;
    detect->maxLatency = -INFINITY;
}

void ms_detect_set_source(tMsDetect * detect, tMsDetectSource source) {
    if ((detect != NULL) && (detect->source != source)) {
        detect->source = source;
        ms_detect_reset(detect);
    }
}

void ms_detect_set_division(tMsDetect * detect, int ticksPerEvent) {
    if ((detect != NULL) && (ticksPerEvent > 0)) {
        detect->division = ticksPerEvent;
    }
}

int ms_detect_division(const tMsDetect * detect) {
    return (detect != NULL) ? detect->division : 0;
}

void ms_detect_tick(tMsDetect * detect, uint64_t tickIndex, uint64_t stampedHostTime) {
    if ((detect == NULL) || (detect->division <= 0) || (detect->source != eMsDetectFromClock)) {
        return;
    }

    if ((tickIndex % (uint64_t)detect->division) != 0) {
        return;
    }
    ms_detect_expect(detect, stampedHostTime);
}

void ms_detect_expect(tMsDetect * detect, uint64_t stampedHostTime) {
    if (detect == NULL) {
        return;
    }
    tExpected * slot = &detect->expected[detect->expectedWrite];

    // OVERWRITING AN UNUSED SLOT IS A MISS, and is counted as one rather than passed over. Sixty-four
    // outstanding events at a quarter note apart is half a minute of unanswered clock, so reaching
    // here at all means the audio side is finding nothing.
    if (!slot->used && (slot->stampedHostTime != 0)) {
        detect->missed++;
        detect->pending--;
    }
    slot->stampedHostTime = stampedHostTime;
    slot->used            = false;
    detect->pending++;
    detect->expectedWrite = (detect->expectedWrite + 1) % MS_DETECT_EXPECTED;
}

// The oldest unused expectation that this onset could plausibly belong to. Oldest rather than
// nearest, deliberately: a device with a latency approaching the spacing between hits would
// otherwise start matching each transient to the NEXT hit and report a latency near zero, which is
// both wrong and flattering.
static tExpected * match_expectation(tMsDetect * detect, uint64_t onsetTime, double * latencyMs) {
    tExpected * best      = NULL;
    double      bestAgeMs = 0.0;

    for (int i = 0; i < MS_DETECT_EXPECTED; i++) {
        tExpected * slot  = &detect->expected[i];

        if (slot->used || (slot->stampedHostTime == 0) || (slot->stampedHostTime > onsetTime)) {
            continue;
        }
        double      ageMs = (double)AudioConvertHostTimeToNanos(onsetTime - slot->stampedHostTime) / 1.0e6;

        if (ageMs > MS_DETECT_WINDOW_MS) {
            // Too old to be this transient's cause, and too old to be anything else's either.
            slot->used = true;
            detect->missed++;
            detect->pending--;
            continue;
        }

        if ((best == NULL) || (ageMs > bestAgeMs)) {
            best      = slot;
            bestAgeMs = ageMs;
        }
    }

    if (best != NULL) {
        *latencyMs = bestAgeMs;
    }
    return best;
}

void ms_detect_audio(tMsDetect *   detect,
                     const float * samples,
                     uint32_t      frames,
                     double        sampleRate,
                     uint64_t      blockHostTime) {
    if ((detect == NULL) || (samples == NULL) || (frames == 0) || (sampleRate <= 0.0)) {
        return;
    }

    // AGED PER BLOCK, not only when a transient happens to arrive. A device that stops answering
    // would otherwise leave its expectations outstanding for ever, and "pending" would stay high
    // enough to keep the gate open on nothing.
    for (int e = 0; e < MS_DETECT_EXPECTED; e++) {
        tExpected * slot = &detect->expected[e];

        if (slot->used || (slot->stampedHostTime == 0) || (slot->stampedHostTime > blockHostTime)) {
            continue;
        }

        if (((double)AudioConvertHostTimeToNanos(blockHostTime - slot->stampedHostTime) / 1.0e6)
            > MS_DETECT_WINDOW_MS) {
            slot->used = true;
            detect->missed++;
            detect->pending--;
        }
    }

    double dt        = 1.0 / sampleRate;
    double envDecay  = exp(-dt / (MS_DETECT_ENV_TAU_MS / 1000.0));
    double floorRise = exp(-dt / (MS_DETECT_FLOOR_TAU_MS / 1000.0));

    // A ONE-POLE HIGH PASS AT ROUGHLY 150 Hz before anything else. A room's rumble and a kick's own
    // fundamental both arrive slowly and would drag the envelope up ahead of the transient itself,
    // biasing every onset early by however long the low end takes to build. What is wanted is the
    // click, not the body.
    double hpCoeff   = 1.0 / (1.0 + (2.0 * M_PI * 150.0 * dt));

    for (uint32_t i = 0; i < frames; i++) {
        double in        = (double)samples[i];

        detect->highpassState  = hpCoeff * (detect->highpassState + in - detect->highpassPrevIn);
        detect->highpassPrevIn = in;

        double level     = fabs(detect->highpassState);
        double raw       = fabs(in);

        if (raw > detect->peak) {
            detect->peak = raw;
        }
        // Instant attack, exponential release - see MS_DETECT_ENV_TAU_MS.
        detect->envelope       = (level > detect->envelope)
                           ? level
                           : (detect->envelope * envDecay);

        // THE FLOOR IS NOT UPDATED WHILE A HIT IS SOUNDING. Letting it track the drum was half of
        // the retriggering: over a 100 ms tail the floor climbed towards the signal, the threshold
        // climbed with it, and both the detection and the re-arm decision were then being made
        // against a reference the drum was setting for itself.
        if (detect->armed) {
            detect->floorLevel = (detect->floorLevel * floorRise) + (level * (1.0 - floorRise));
        } else if (detect->envelope > detect->onsetPeak) {
            detect->onsetPeak = detect->envelope;
        }
        double threshold = detect->floorLevel * MS_DETECT_MARGIN;

        if (threshold < MS_DETECT_ABS_FLOOR) {
            threshold = MS_DETECT_ABS_FLOOR;
        }

        if (detect->refractoryLeft > 0.0) {
            detect->refractoryLeft -= dt;
            continue;
        }

        if (!detect->armed) {
            // Waiting for the previous sound to die away, not for a clock.
            //
            // BOTH CONDITIONS. Twelve dB below the hit's own peak is not enough on its own - the
            // Tempest's kick ripples by more than that while it decays. The sound has to be back at
            // the noise floor as well, which is the only statement that actually means "it has
            // finished". The floor is frozen while this waits, so it is the floor from BEFORE the
            // hit and the drum cannot raise its own bar.
            if (  (detect->envelope < (detect->onsetPeak * MS_DETECT_REARM_RATIO))
               && (detect->envelope < threshold)) {
                detect->armed = true;
            }
            continue;
        }

        // NOTHING IS DUE, SO NOTHING IS LOOKED FOR.
        //
        // This is a calibration instrument, not a general onset detector: it knows exactly when a
        // transient is expected, and outside those windows a rising edge is not a measurement of
        // anything. The Tempest's kick has a long, ripply tail that hovers around the absolute floor
        // for 300 ms - re-crossing it repeatedly - and every one of those crossings was being
        // reported as a spurious hit.
        //
        // Tightening the threshold to exclude that tail would have been the wrong repair. It would
        // have been tuned to this drum, on this desk, at this gain, and it would have thrown away
        // quiet hits from the next device measured. Asking "is one due?" costs a comparison and is
        // true for every device.
        // MONITOR MODE IS THE ONE CASE WHERE NOTHING IS EVER "DUE". There is no expectation queue,
        // so the pending test would reject every onset. What still protects it from the Tempest's
        // rippling tail is the armed/refractory machinery above, which is independent of the queue
        // and is doing the real work in all three modes.
        if (detect->source == eMsDetectMonitor) {
            if (level <= threshold) {
                continue;
            }
        } else if ((detect->pending <= 0) || (level <= threshold)) {
            continue;
        }
        // AN ONSET. Its moment is this sample's moment: the block's host time plus how far into the
        // block it fell.
        uint64_t onsetTime = blockHostTime
                             + AudioConvertNanosToHostTime((uint64_t)(((double)i * dt) * 1.0e9));
        double   latencyMs = 0.0;

        detect->refractoryLeft = MS_DETECT_REFRACTORY_MS / 1000.0;
        detect->armed          = false;
        detect->onsetPeak      = detect->envelope;

        if (detect->source == eMsDetectMonitor) {
            // REFITTED ON EVERY ONSET, which sounds expensive and is not: onsets arrive a few times
            // a second at most, and the whole fit is a sort and two passes over 256 doubles. Doing
            // it here rather than in the reader keeps every figure published by the thread that owns
            // it, which is the pattern the rest of this file follows.
            detect->onsets[detect->onsetWrite] = onsetTime;
            detect->onsetWrite                 = (detect->onsetWrite + 1) % MS_DETECT_ONSETS;

            if (detect->onsetCount < MS_DETECT_ONSETS) {
                detect->onsetCount++;
            }
            detect->lastOnsetTime              = onsetTime;
            monitor_fit(detect);
            continue;
        }
        tExpected * slot        = match_expectation(detect, onsetTime, &latencyMs);

        double      sinceLastMs = (detect->lastOnsetTime != 0)
                             ? ((double)AudioConvertHostTimeToNanos(onsetTime - detect->lastOnsetTime)
                                / 1.0e6)
                             : 0.0;

        detect->lastOnsetTime = onsetTime;

        if (slot == NULL) {
            // WORTH THE LINE. A spurious onset is either a second transient inside one sound - a
            // kick's body after its click - or a hit nobody asked for, and the gap since the last
            // onset is what tells the two apart. Guessing between them by adjusting the refractory
            // until the count looks tidy would be fitting the instrument to the answer.
            detect->spurious++;
            ms_log_line("  detector: spurious onset, %.1f ms after the previous", sinceLastMs);
            continue;
        }
        slot->used            = true;
        detect->pending--;
        detect->hits++;
        detect->sumLatency   += latencyMs;
        detect->sumLatencySq += (latencyMs * latencyMs);

        if (latencyMs < detect->minLatency) {
            detect->minLatency = latencyMs;
        }

        if (latencyMs > detect->maxLatency) {
            detect->maxLatency = latencyMs;
        }
        atomic_store(&detect->pubLast, latencyMs);
    }

    if (detect->source == eMsDetectMonitor) {
        // The latency figures stay at zero here on purpose - there is no reference to be late
        // against, and publishing a number would invite it to be read as one. Only the input peak
        // is still meaningful, and it is what says whether anything is arriving at all.
        atomic_store(&detect->pubInputPeak, detect->peak);
        return;
    }
    double mean   = (detect->hits > 0) ? (detect->sumLatency / (double)detect->hits) : 0.0;
    double meanSq = (detect->hits > 0) ? (detect->sumLatencySq / (double)detect->hits) : 0.0;
    double var    = meanSq - (mean * mean);

    atomic_store(&detect->pubMean, mean);
    atomic_store(&detect->pubJitter, (var > 0.0) ? sqrt(var) : 0.0);
    atomic_store(&detect->pubMin, isfinite(detect->minLatency) ? detect->minLatency : 0.0);
    atomic_store(&detect->pubMax, isfinite(detect->maxLatency) ? detect->maxLatency : 0.0);
    atomic_store(&detect->pubPeakDev,
                 (detect->hits > 0)
                 ? fmax(fabs(detect->maxLatency - mean), fabs(mean - detect->minLatency))
                 : 0.0);
    atomic_store(&detect->pubInputPeak, detect->peak);
    atomic_store(&detect->pubHits, detect->hits);
    atomic_store(&detect->pubMissed, detect->missed);
    atomic_store(&detect->pubSpurious, detect->spurious);
}

void ms_detect_read(const tMsDetect * detect, tMsDetectSnapshot * out) {
    if (out == NULL) {
        return;
    }

    if (detect == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->latencyMeanMs   = atomic_load(&detect->pubMean);
    out->latencyLastMs   = atomic_load(&detect->pubLast);
    out->latencyMinMs    = atomic_load(&detect->pubMin);
    out->latencyMaxMs    = atomic_load(&detect->pubMax);
    out->jitterRmsMs     = atomic_load(&detect->pubJitter);
    out->peakDeviationMs = atomic_load(&detect->pubPeakDev);
    out->inputPeak       = atomic_load(&detect->pubInputPeak);
    out->monitorPeriodMs = atomic_load(&detect->pubMonitorPeriod);
    out->monitorBpm      = atomic_load(&detect->pubMonitorBpm);
    out->monitorOnsets   = atomic_load(&detect->pubMonitorOnsets);
    out->hits            = atomic_load(&detect->pubHits);
    out->missed          = atomic_load(&detect->pubMissed);
    out->spurious        = atomic_load(&detect->pubSpurious);
}
