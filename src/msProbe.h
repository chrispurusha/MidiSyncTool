/*
 * MidiSyncTool - the note probe: scheduled notes, and the round trip they measure.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msProbe.h.md - "// notes §k" refers there.

#ifndef __MS_PROBE_H__
#define __MS_PROBE_H__

#include <stdbool.h>
#include <stdint.h>

#include "msDetect.h"

#ifdef __cplusplus
extern "C" {
#endif

// notes §1
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
