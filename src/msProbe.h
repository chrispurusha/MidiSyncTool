/*
 * MidiSyncTool - the note probe: scheduled notes, and the round trip they measure.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_PROBE_H__
#define __MS_PROBE_H__

#include <stdbool.h>
#include <stdint.h>

#include "msDetect.h"

#ifdef __cplusplus
extern "C" {
#endif

// TWO DIFFERENT LATENCIES, AND THE DIFFERENCE BETWEEN THEM IS ITSELF THE MEASUREMENT.
//
// Driving the drum machine's own sequencer from our clock and timing the result measures the whole
// path: clock on the wire, the machine's sequencer deciding a step has arrived, its voice sounding.
// Sending it a NOTE and timing that measures only the last part - note in, sound out.
//
// Subtract one from the other and what is left is what the sequencer itself contributes, which is
// the part a user can do nothing about and the part most worth knowing. A machine with a 3 ms note
// latency and a 12 ms clock-driven latency has a 9 ms sequencer, and no amount of tightening the
// clock will improve it.
//
// The note probe is also the better CALIBRATION, and by some distance:
//
//   * It needs no pattern programmed, so there is nothing for the user to get wrong - no swing left
//     on, no velocity variation, no pad that turned out to be a two-layer sound.
//   * It works with the transport stopped, so calibration is a button rather than a session.
//   * The moment of each note is chosen by this code rather than inferred from a musical grid, so
//     the expectation is exact by construction.
//   * Velocity can be swept, which is the only way to find out how much the onset detector's own
//     level bias is worth on this particular sound.
#define MS_PROBE_DEFAULT_COUNT       (32)
#define MS_PROBE_DEFAULT_INTERVAL    (400.0)    // ms between notes; well clear of any drum's decay
#define MS_PROBE_DEFAULT_GATE        (60.0)     // ms a note is held

typedef struct {
    int      destination;
    int      channel;        // 0-15
    int      note;
    int      velocity;
    double   intervalMs;
    double   gateMs;
    int      count;          // how many notes a run sends

    // Run state, audio thread only.
    bool     running;
    int      sent;
    uint64_t nextDueHostTime;
} tMsProbe;

void ms_probe_init(tMsProbe * probe);

// Arms a run. The first note is scheduled a little way ahead so nothing is ever asked of CoreMIDI
// in the past. Safe to call from a UI thread only while the probe is idle.
void ms_probe_start(tMsProbe * probe, tMsDetect * detect);
void ms_probe_stop(tMsProbe * probe);

// Called once per audio block. Emits whatever notes fall inside it and registers each one with the
// detector as the moment a transient is expected.
void ms_probe_process(tMsProbe * probe, tMsDetect * detect, uint32_t blockFrames, double sampleRate, uint64_t blockHostTime);

bool ms_probe_running(const tMsProbe * probe);

#ifdef __cplusplus
}
#endif

#endif // __MS_PROBE_H__
