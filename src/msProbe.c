/*
 * MidiSyncTool - the note probe: scheduled notes, and the round trip they measure.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <string.h>

#include "msClock.h"     // MS_LOOKAHEAD_MS - the probe commits as far ahead as the clock does
#include "msLog.h"
#include "msMidi.h"
#include "msProbe.h"

#define MIDI_NOTE_ON     (0x90)
#define MIDI_NOTE_OFF    (0x80)

// How far ahead of the first block the first note is placed. Nothing subtle: enough that arming the
// probe from a UI thread cannot possibly land a note in the past.
#define MS_PROBE_LEAD_MS    (200.0)

void ms_probe_init(tMsProbe * probe) {
    memset(probe, 0, sizeof(*probe));
    probe->destination = -1;
    probe->channel     = 9;      // channel 10 as people count them, the usual home for drums
    probe->note        = 36;     // C1, the usual kick
    probe->velocity    = 100;
    probe->intervalMs  = MS_PROBE_DEFAULT_INTERVAL;
    probe->gateMs      = MS_PROBE_DEFAULT_GATE;
    probe->count       = MS_PROBE_DEFAULT_COUNT;
}

void ms_probe_start(tMsProbe * probe, tMsDetect * detect) {
    if ((probe == NULL) || probe->running) {
        return;
    }
    // THE DETECTOR CANNOT SERVE BOTH GRIDS AT ONCE - see tMsDetectSource. Switching the source also
    // clears the figures, which is right: a run's numbers should describe that run.
    ms_detect_set_source(detect, eMsDetectFromProbe);

    probe->sent            = 0;
    probe->nextDueHostTime = AudioGetCurrentHostTime()
                             + AudioConvertNanosToHostTime((uint64_t)(MS_PROBE_LEAD_MS * 1.0e6));
    probe->running         = true;

    ms_log_line("probe: %d notes, note %d vel %d on channel %d, every %.0f ms",
                probe->count, probe->note, probe->velocity, probe->channel + 1,
                probe->intervalMs);
}

void ms_probe_stop(tMsProbe * probe) {
    if (probe != NULL) {
        probe->running = false;
    }
}

bool ms_probe_running(const tMsProbe * probe) {
    return (probe != NULL) && probe->running;
}

void ms_probe_process(tMsProbe *  probe,
                      tMsDetect * detect,
                      uint32_t    blockFrames,
                      double      sampleRate,
                      uint64_t    blockHostTime) {
    if (  (probe == NULL) || !probe->running || (probe->destination < 0)
       || (sampleRate <= 0.0) || (blockFrames == 0)) {
        return;
    }
    // THE SAME WINDOW THE CLOCK USES, and for the same reason: a note handed to CoreMIDI with its
    // moment already past is delivered at once, which is the one failure that would put a
    // calibration figure out by a whole block and leave it looking plausible.
    uint64_t windowStart = blockHostTime
                           + AudioConvertNanosToHostTime((uint64_t)(MS_LOOKAHEAD_MS * 1.0e6));
    uint64_t windowEnd   = windowStart
                           + AudioConvertNanosToHostTime(
        (uint64_t)((((double)blockFrames / sampleRate)) * 1.0e9));

    while (probe->running && (probe->nextDueHostTime < windowEnd)) {
        if (probe->nextDueHostTime < windowStart) {
            // Only reachable if a block was missed outright. Slide rather than fire late: a note
            // whose moment has passed measures the block that was missed, not the drum machine.
            probe->nextDueHostTime = windowStart;
        }
        uint8_t  on[3]  = {
            (uint8_t)(MIDI_NOTE_ON | (probe->channel & 0x0F)),
            (uint8_t)(probe->note & 0x7F),
            (uint8_t)(probe->velocity & 0x7F)
        };
        uint8_t  off[3] = {
            (uint8_t)(MIDI_NOTE_OFF | (probe->channel & 0x0F)),
            (uint8_t)(probe->note & 0x7F),
            0
        };
        uint64_t offAt  = probe->nextDueHostTime
                          + AudioConvertNanosToHostTime((uint64_t)(probe->gateMs * 1.0e6));

        ms_midi_send_at(probe->destination, on, 3, probe->nextDueHostTime);
        ms_midi_send_at(probe->destination, off, 3, offAt);

        // THE EXPECTATION IS THE NOTE-ON'S OWN MOMENT, exactly. Nothing is inferred from a musical
        // grid here, which is what makes the note probe a better calibration than the clock-driven
        // one whatever the drum machine turns out to do.
        ms_detect_expect(detect, probe->nextDueHostTime);

        probe->sent++;
        probe->nextDueHostTime += AudioConvertNanosToHostTime(
            (uint64_t)(probe->intervalMs * 1.0e6));

        if (probe->sent >= probe->count) {
            probe->running = false;
            ms_log_line("probe: %d notes sent, waiting on the last transients", probe->sent);
        }
    }
}
