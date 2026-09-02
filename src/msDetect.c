/*
 * MidiSyncTool - audio transient detection, and MIDI-to-audio latency from it.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <stdatomic.h>
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
};

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
        if ((detect->pending <= 0) || (level <= threshold)) {
            continue;
        }
        // AN ONSET. Its moment is this sample's moment: the block's host time plus how far into the
        // block it fell.
        uint64_t    onsetTime   = blockHostTime
                                  + AudioConvertNanosToHostTime((uint64_t)(((double)i * dt) * 1.0e9));
        double      latencyMs   = 0.0;
        tExpected * slot        = match_expectation(detect, onsetTime, &latencyMs);

        detect->refractoryLeft = MS_DETECT_REFRACTORY_MS / 1000.0;
        detect->armed          = false;
        detect->onsetPeak      = detect->envelope;

        double      sinceLastMs = (detect->lastOnsetTime != 0)
                             ? ((double)AudioConvertHostTimeToNanos(onsetTime - detect->lastOnsetTime)
                                / 1.0e6)
                             : 0.0;

        detect->lastOnsetTime  = onsetTime;

        if (slot == NULL) {
            // WORTH THE LINE. A spurious onset is either a second transient inside one sound - a
            // kick's body after its click - or a hit nobody asked for, and the gap since the last
            // onset is what tells the two apart. Guessing between them by adjusting the refractory
            // until the count looks tidy would be fitting the instrument to the answer.
            detect->spurious++;
            ms_log_line("  detector: spurious onset, %.1f ms after the previous", sinceLastMs);
            continue;
        }
        slot->used             = true;
        detect->pending--;
        detect->hits++;
        detect->sumLatency    += latencyMs;
        detect->sumLatencySq  += (latencyMs * latencyMs);

        if (latencyMs < detect->minLatency) {
            detect->minLatency = latencyMs;
        }

        if (latencyMs > detect->maxLatency) {
            detect->maxLatency = latencyMs;
        }
        atomic_store(&detect->pubLast, latencyMs);
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
    out->hits            = atomic_load(&detect->pubHits);
    out->missed          = atomic_load(&detect->pubMissed);
    out->spurious        = atomic_load(&detect->pubSpurious);
}
