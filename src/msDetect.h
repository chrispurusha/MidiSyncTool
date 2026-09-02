/*
 * MidiSyncTool - audio transient detection, and MIDI-to-audio latency from it.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_DETECT_H__
#define __MS_DETECT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WHAT IS ACTUALLY BEING MEASURED, stated plainly because it is easy to overclaim.
//
// The figure this produces is the WHOLE ROUND TRIP: the moment a clock tick was stamped for, to the
// moment the resulting transient appeared in a buffer this plug-in was handed. That includes the
// drum machine's own MIDI-in to audio-out delay - the number the concept calls "device latency" and
// the one worth reporting - but also the interface's A/D, the host's input buffering, and whatever
// the host chooses to tell the plug-in about its block times.
//
// Those extra terms are CONSTANT for a given rig and buffer size, which is what makes the whole
// exercise viable: a constant is subtractable once measured. GenBridge learned to enumerate exactly
// these terms (device latency, safety offset, buffer, stream latency) and msDevice.c already
// reports them. Until they are subtracted, this is a round-trip figure and is labelled as one.
//
// THE JITTER FIGURE NEEDS NO SUCH APOLOGY. Every constant cancels in the deviation, so the spread is
// the drum machine's own timing spread plus the detector's, and the detector's is small against it.
#define MS_DETECT_EXPECTED    (64)     // outstanding scheduled events awaiting a transient

// ONSET DETECTION IS BIASED BY LEVEL, and the bias is the reason the calibration pattern has to be
// programmed a particular way. A threshold crossing on a rising edge happens later for a quieter hit
// than a louder one, because the edge takes longer to reach the threshold. Against a FIXED velocity
// the bias is a constant and cancels out of both the mean (once calibrated) and the jitter; against
// a varying one it is indistinguishable from the drum machine playing badly.
//
// Likewise a slow attack. A kick drum's onset can ramp over 10 ms, so where in that ramp the
// threshold falls moves with the threshold. A closed hat or a rim is a far better reference, and the
// difference between the two is itself worth measuring once this works.
typedef struct {
    double   latencyMeanMs;
    double   latencyLastMs;
    double   latencyMinMs;
    double   latencyMaxMs;
    double   jitterRmsMs;      // spread about the mean - the honest figure, see above
    double   peakDeviationMs;
    uint64_t hits;
    uint64_t missed;           // scheduled events that aged out with no transient
    uint64_t spurious;         // transients that matched no scheduled event
    double   inputPeak;
} tMsDetectSnapshot;

// WHICH SCHEDULED THING THE TRANSIENTS ARE ANSWERS TO. Both sources register expectations with the
// same matcher, and only one of them may be active at a time - interleaved expectations from two
// grids would match each other's transients.
typedef enum {
    eMsDetectFromClock = 0,   // the drum machine's own sequencer, following our clock
    eMsDetectFromProbe        // notes this plug-in sent directly
} tMsDetectSource;

typedef struct tMsDetect tMsDetect;

tMsDetect * ms_detect_create(void);
void ms_detect_destroy(tMsDetect * detect);
void ms_detect_reset(tMsDetect * detect);

// Switching source clears the figures, which is right - a run's numbers should describe that run.
void ms_detect_set_source(tMsDetect * detect, tMsDetectSource source);

// How many clock ticks apart the calibration hits are: 24 for quarter notes, 6 for sixteenths, 96
// for one hit a bar. Getting this wrong does not corrupt the measurement - it just reports every
// unplayed position as a miss.
void ms_detect_set_division(tMsDetect * detect, int ticksPerEvent);
int ms_detect_division(const tMsDetect * detect);

// Called by the clock for every tick, with the moment it was stamped for. Ticks that do not fall on
// the division are ignored here rather than at the call site.
void ms_detect_tick(tMsDetect * detect, uint64_t tickIndex, uint64_t stampedHostTime);

// Called by the note probe for every note it schedules. Same queue, same matcher - see the source
// enum above for why only one of the two may be feeding it.
void ms_detect_expect(tMsDetect * detect, uint64_t stampedHostTime);

// Called once per block from the audio thread with one channel of input - the left, or a sum; a
// transient is a transient on either. blockHostTime is when the block was picked up.
void ms_detect_audio(tMsDetect * detect, const float * samples, uint32_t frames, double sampleRate, uint64_t blockHostTime);

void ms_detect_read(const tMsDetect * detect, tMsDetectSnapshot * out);

#ifdef __cplusplus
}
#endif

#endif // __MS_DETECT_H__
