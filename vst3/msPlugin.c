/*
 * MidiSyncTool - a plug-in for MIDI clock generation, timing analysis and hardware latency
 * calibration.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If
 * not, see <https://www.gnu.org/licenses/>.
 */
// Notes: Docs/code-notes/msPlugin.c.md - "// notes §k" refers there.

// notes §1

#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synthlibPlugin.h"

#include "msClock.h"
#include "msClockIn.h"
#include "msDetect.h"
#include "msDraw.h"
#include "synthlibLog.h"
#include "msMidi.h"
#include "msProbe.h"
#include "msStats.h"
#include "msStatus.h"
#include "synthlibPanelView.h"

// Names the log - touch /tmp/midisynctool-log, read /tmp/midisynctool.log. See SynthLib's plugin/synthlibLog.h.
const char gSynthLibLogName[] = "midisynctool";

#ifndef MST_VERSION_STRING
#define MST_VERSION_STRING    "0.1.0"
#endif

// 0xMMMMmmbb, from the same version by do-plugin, so the Audio Unit's number and the plist's cannot
// disagree.
#ifndef MST_AU_VERSION
#define MST_AU_VERSION        (0x00000100)
#endif

// ------------------------------------------------------------------------------------------------
// Identity
// ------------------------------------------------------------------------------------------------

// THE TWO CLASS IDS msVst3.cpp DECLARED AS FUIDs, written out as the bytes they always were -
// FUID(0x7A1E5C40, ...) is 7A 1E 5C 40 ... on every platform but Windows, checked against the SDK.
// THESE MAY NEVER CHANGE: a set saved against them would reopen with an empty slot.
static const uint8_t gProcessorUid[16] = {
    0x7A, 0x1E, 0x5C, 0x40, 0x9B, 0x2D, 0x4F, 0x13, 0xA6, 0xE8, 0x0C, 0x57, 0x3D, 0x91, 0xB4, 0xE2
};
static const uint8_t gControllerUid[16] = {
    0x2C, 0x48, 0xF9, 0xA1, 0x5E, 0x7B, 0x4D, 0x06, 0x91, 0xC3, 0xA2, 0x8F, 0x6B, 0x0D, 0x57, 0xE4
};

#define FOUR_CC(a, b, c, d)    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                                ((uint32_t)(c) << 8) | (uint32_t)(d))

// The Audio Unit's codes, under the same rule. The manufacturer is the one the other two plug-ins
// register, since it is the same vendor; do-plugin writes the same codes into the plist.
#define MST_AU_TYPE            FOUR_CC('a', 'u', 'f', 'x')     // kAudioUnitType_Effect
#define MST_AU_SUBTYPE         FOUR_CC('M', 'S', 'y', 'n')
#define MST_AU_MANUFACTURER    FOUR_CC('C', 'P', 'u', 'r')
#define MST_AU_BUNDLE_ID       "com.chrispurusha.midisynctool.au"

// notes §2
enum {
    kParamMidiDest = 0,      // 0 = none, 1..n = the nth destination
    kParamCompensate,        // device latency to compensate, in milliseconds
    kParamAudioSource,       // which of the channels the host hands us to analyse
    kParamMode,              // tMsMode - generate + measure, generate only, or monitor

    // APPENDED, NEVER INSERTED. A parameter's id is what a host's automation and a saved project
    // are expressed in, so the four above keep the ids they were released with and anything new
    // goes on the end.
    kParamClockSource,       // 0 = none, 1..n = the nth MIDI SOURCE to measure
    kParamCount
};

#define MST_COMPENSATE_MAX    (100.0)    // ms; well past anything a drum machine has shown

// notes §3
static const tSynthLibParam gParams[kParamCount] = {
    { kParamMidiDest, "MIDI destination", "Port", eSynthLibUnitIndexed,
      0.0, (double)MS_MIDI_MAX_DEST, 0.0, MS_MIDI_MAX_DEST, SYNTHLIB_MIDI_NONE,
      SYNTHLIB_PARAM_NO_SAVE | SYNTHLIB_PARAM_LIST },
    { kParamCompensate, "Latency compensation", "Comp", eSynthLibUnitMilliseconds,
      0.0, MST_COMPENSATE_MAX, 0.0, 0, SYNTHLIB_MIDI_NONE,
      SYNTHLIB_PARAM_NO_SAVE },
    { kParamAudioSource, "Analyse channel", "Analyse", eSynthLibUnitIndexed,
      0.0, (double)(MS_AUDIO_SOURCES - 1), 0.0, MS_AUDIO_SOURCES - 1, SYNTHLIB_MIDI_NONE,
      SYNTHLIB_PARAM_NO_SAVE | SYNTHLIB_PARAM_LIST },
    { kParamMode, "Mode", "Mode", eSynthLibUnitIndexed,
      0.0, (double)(eMsModeCount - 1), 0.0, eMsModeCount - 1, SYNTHLIB_MIDI_NONE,
      SYNTHLIB_PARAM_NO_SAVE | SYNTHLIB_PARAM_LIST },
    { kParamClockSource, "Clock input", "Clock in", eSynthLibUnitIndexed,
      0.0, (double)MS_MIDI_MAX_SOURCE, 0.0, MS_MIDI_MAX_SOURCE, SYNTHLIB_MIDI_NONE,
      SYNTHLIB_PARAM_NO_SAVE | SYNTHLIB_PARAM_LIST },
};

// SLOT 0 IS NONE, deliberately and permanently. A plug-in that picks a destination on your behalf
// ends up driving hardware nobody asked it to - the rule GenBridge arrived at the hard way.
static int mst_dest_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_DEST + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_DEST) ? MS_MIDI_MAX_DEST : slot);
}

static double mst_dest_normalized(int slot) {
    return (double)slot / (double)MS_MIDI_MAX_DEST;
}

// The same arithmetic for the SOURCE list, which is a different list of a different length.
static int mst_source_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_SOURCE + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_SOURCE) ? MS_MIDI_MAX_SOURCE : slot);
}

static double mst_source_normalized(int slot) {
    return (double)slot / (double)MS_MIDI_MAX_SOURCE;
}

// notes §4
#define MST_STATE_V1_PARAMS   (4)
#define MST_STATE_NAME_AT     (MST_STATE_V1_PARAMS * sizeof(double))
#define MST_STATE_SOURCE_AT   (MST_STATE_NAME_AT + MS_MIDI_NAME_LEN)
#define MST_STATE_SNAME_AT    (MST_STATE_SOURCE_AT + sizeof(double))
#define MST_STATE_BYTES       (MST_STATE_SNAME_AT + MS_MIDI_NAME_LEN)

// ------------------------------------------------------------------------------------------------
// The instance
// ------------------------------------------------------------------------------------------------

typedef struct {
    const tSynthLibPluginDesc * desc;

    double          sampleRate;
    uint32_t        maxBlock;
    uint64_t        blocksSeen;
    uint32_t        lastState;
    double          lastTempo;
    double          lastLogged;

    tMsClock        clock;
    tMsStats *      stats;
    tMsDetect *     detect;
    tMsProbe        probe;
    uint64_t        lastGraphHits;

    // WHAT THE PARAMETERS ARE SET TO, for getParam() and the saved state.
    _Atomic double  params[kParamCount];
    tMsMode         mode;
    tMsClockIn *    clockIn;
    int             selectedPort;   // the port CHOSEN, before monitor mode gates it
    float           sumBuffer[8192];

    // THE STATUS SLOT, claimed at initialize() and read by the editor straight off this instance -
    // which is the whole of what the "mstSlot" message across IConnectionPoint used to be for.
    _Atomic int     statusSlot;
    tMsStatus *     status;

    double          lastPpq;
    double          lastHostMs;
    bool            warnedNoContext;

    // ---- SPLIT BLOCKS: one device cycle handed over in more than one process() call -----------
    // See the long note in ms_process(). Audio thread only; nothing here is read from the UI.
    uint64_t        cycleStartHostTime;   // wall clock at the first call of the current cycle
    uint32_t        cycleFramesSoFar;     // frames already handed over within it
    uint32_t        cycleFramesLast;      // frames the last COMPLETED cycle carried
    uint64_t        prevCallHostTime;
    uint32_t        observedMaxFrames;    // the host's real buffer, as measured not declared
    bool            haveCall;
    uint64_t        splitCalls;
    int64_t         framesSeen;           // real frames, for the heartbeat's time axis
} tMsPlugin;

static double param_of(tMsPlugin * m, int id) {
    return atomic_load(&m->params[id]);
}

// notes §5
static void ms_suspend(tMsPlugin * m) {
    ms_stats_gap(m->stats);
    m->clock.haveBase = false;
    m->clock.havePrev = false;

    // AND THE CYCLE TRACKER, for the same reason: the next call does not continue the last one's
    // device cycle.
    m->haveCall         = false;
    m->cycleFramesSoFar = 0;
}

// notes §6
static void refresh_destination(tMsPlugin * m) {
    int port = (m->mode == eMsModeMonitor) ? -1 : m->selectedPort;

    m->clock.destination = port;
    m->probe.destination = port;
}

static void set_selected_port(tMsPlugin * m, int port) {
    m->selectedPort = port;
    refresh_destination(m);
}

// ON CoreMIDI'S RECEIVE THREAD. Straight through to the estimator, which owns everything it touches
// - no lock, no allocation, and deliberately nothing that reaches into the rest of the instance.
static void clock_in_byte(uint8_t status, uint64_t hostTime, void * user) {
    tMsPlugin * m = (tMsPlugin *)user;

    if ((m != NULL) && (m->clockIn != NULL)) {
        ms_clock_in_byte(m->clockIn, status, hostTime);
    }
}

// ONE PLACE where a parameter becomes an effect, so the state restore and the host's own parameter
// changes cannot diverge.
static void apply_parameter(tMsPlugin * m, int id, double normalized) {
    if ((id < 0) || (id >= kParamCount)) {
        return;
    }
    atomic_store(&m->params[id], normalized);

    if (id == kParamMode) {
        tMsMode previous = m->mode;

        m->mode = ms_mode_from_normalized(normalized);

        // WHICH SOURCE EACH MODE IMPLIES, in one place. eMsDetectOff preserves the figures and every
        // other transition clears them - see ms_detect_set_source().
        ms_detect_set_source(m->detect, (m->mode == eMsModeMonitor)   ? eMsDetectMonitor
                                      : (m->mode == eMsModeClockOnly) ? eMsDetectOff
                                                                      : eMsDetectFromClock);

        // THE HOST-TIMING STATS ARE RESET ONLY WHEN THE CLOCK ITSELF STARTS OR STOPS, which is the
        // monitor boundary and nothing else. Block jitter, the model residual and the commit margin
        // stay live and meaningful in clock-only mode - it is still generating.
        if ((m->mode == eMsModeMonitor) != (previous == eMsModeMonitor)) {
            ms_stats_reset(m->stats);
        }
        refresh_destination(m);

        if (m->status != NULL) {
            atomic_store(&m->status->mode, (int)m->mode);
            atomic_store(&m->status->haveDestination, (m->clock.destination >= 0) ? 1 : 0);
        }
        synthlib_log_line("mode -> %s", ms_mode_label(m->mode));
    } else if (id == kParamMidiDest) {
        int slot = mst_dest_slot(normalized);

        // Slot 1 is the first destination, because slot 0 is None.
        set_selected_port(m, ((slot >= 1) && ((slot - 1) < ms_midi_count())) ? (slot - 1) : -1);

        if (m->status != NULL) {
            atomic_store(&m->status->waitingForDevice, 0);

            if (m->clock.destination >= 0) {
                ms_midi_name(m->clock.destination, m->status->destName, sizeof(m->status->destName));
                atomic_store(&m->status->haveDestination, 1);
            } else {
                atomic_store(&m->status->haveDestination, 0);
            }
        }
        synthlib_log_line("destination parameter -> slot %d (%s)", slot,
                    ((m->clock.destination >= 0) && (m->status != NULL)) ? m->status->destName : "none");
    } else if (id == kParamClockSource) {
        int slot = mst_source_slot(normalized);
        int port = ((slot >= 1) && ((slot - 1) < ms_midi_source_count())) ? (slot - 1) : -1;

        // A NEW SOURCE IS A NEW MEASUREMENT. Carrying a fitted rate across from a different master
        // would be a reading of neither, and the two could be tens of ppm apart.
        ms_clock_in_reset(m->clockIn);
        ms_midi_listen(port);

        if (m->status != NULL) {
            atomic_store(&m->status->haveClockSource, (port >= 0) ? 1 : 0);
            ms_midi_source_name(port, m->status->clockSourceName, sizeof(m->status->clockSourceName));
        }
    } else if (id == kParamCompensate) {
        ms_clock_set_compensation_ms(&m->clock, normalized * MST_COMPENSATE_MAX);
    }
    // kParamAudioSource needs nothing beyond the stored value - ms_process() reads it every block.
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

static void * ms_create(const tSynthLibPluginDesc * desc) {
    tMsPlugin * m = (tMsPlugin *)calloc(1, sizeof(tMsPlugin));

    if (m == NULL) {
        return NULL;
    }
    m->desc         = desc;
    m->sampleRate   = 48000.0;
    m->lastState    = 0xFFFFFFFFu;
    m->lastTempo    = -1.0;
    m->lastLogged   = -1.0;
    m->mode         = eMsModeMeasure;
    m->selectedPort = -1;
    m->lastPpq      = -1.0;
    atomic_init(&m->statusSlot, -1);

    for (int i = 0; i < kParamCount; i++) {
        atomic_init(&m->params[i], gParams[i].defaultNormalized);
    }
    ms_clock_init(&m->clock);
    ms_probe_init(&m->probe);
    return m;
}

static void ms_destroy(void * inst) {
    free(inst);
}

static void ms_initialize(void * inst) {
    tMsPlugin * m = (tMsPlugin *)inst;

    synthlib_log_line("initialize - MidiSyncTool %s", MST_VERSION_STRING);

    atomic_store(&m->statusSlot, ms_status_claim());
    m->status = ms_status(atomic_load(&m->statusSlot));
    ms_clock_init(&m->clock);

    // Created here rather than at construction so a host that instantiates and discards without
    // initialising costs nothing.
    m->stats   = ms_stats_create();
    m->detect  = ms_detect_create();
    m->clockIn = ms_clock_in_create();

    // THE LISTENER IS REGISTERED ONCE AND STAYS REGISTERED. What decides whether anything arrives is
    // which source is connected, not whether a callback is installed.
    ms_midi_set_listener(clock_in_byte, m);
    m->clock.stats  = m->stats;
    m->clock.detect = m->detect;

    // HOW FAR APART THE CALIBRATION HITS ARE, in clock ticks: 24 for a hit on every quarter note, 6 for
    // sixteenths, 96 for one a bar. An environment variable, like the others below, for a headless run.
    const char * division = getenv("MST_DETECT_DIV");

    if (division != NULL) {
        ms_detect_set_division(m->detect, atoi(division));
    }
    synthlib_log_line("detector: expecting a transient every %d ticks", ms_detect_division(m->detect));

    ms_probe_init(&m->probe);

    // MST_COMPENSATE_MS is the measured DEVICE round trip, compensated as a phase advance of the tick
    // grid - the mechanism that works. MST_OFFSET_MS remains as a wall-clock trim for anything with no
    // musical grid to advance (the note probe), and is bounded by the schedule lead; see msClock.h.
    const char * compensate = getenv("MST_COMPENSATE_MS");

    if (compensate != NULL) {
        ms_clock_set_compensation_ms(&m->clock, atof(compensate));
        synthlib_log_line("clock compensation: %.3f ms of device latency, advanced in phase", atof(compensate));
    }
    const char * offset = getenv("MST_OFFSET_MS");

    if (offset != NULL) {
        ms_midi_set_offset_ms(atof(offset));
        synthlib_log_line("output offset: %+.3f ms (wall clock trim)", ms_midi_offset_ms());
    }

    // ENUMERATED HERE, not from the audio thread and not from a repaint: it takes CoreMIDI's locks.
    ms_midi_refresh();

    // NO DESTINATION UNTIL ONE IS CHOSEN. MST_MIDI_DEST names one for development, so the driver and a
    // Live session can both be pointed at something without touching the panel.
    const char * wanted = getenv("MST_MIDI_DEST");

    if (wanted != NULL) {
        set_selected_port(m, ms_midi_index_for_name(wanted));

        if (m->clock.destination < 0) {
            synthlib_log_line("MST_MIDI_DEST '%s' is not present - generating nothing", wanted);
        } else {
            synthlib_log_line("clock destination: [%d] %s", m->clock.destination, wanted);
        }

        // The probe drives the same device - set_selected_port() does both.
        if ((m->status != NULL) && (m->clock.destination >= 0)) {
            ms_midi_name(m->clock.destination, m->status->destName, sizeof(m->status->destName));
            atomic_store(&m->status->haveDestination, 1);
        }
    }

    // THE NOTE PROBE, armed from the environment. MST_PROBE=note[,velocity[,channel]] - the note number
    // matters more than anything else here, because it selects which pad is being measured.
    const char * probeSpec = getenv("MST_PROBE");

    if (probeSpec != NULL) {
        int note = 36, velocity = 100, channel = 10;

        sscanf(probeSpec, "%d,%d,%d", &note, &velocity, &channel);
        m->probe.note     = note;
        m->probe.velocity = velocity;
        m->probe.channel  = channel - 1;   // as people count them

        const char * count = getenv("MST_PROBE_COUNT");

        if (count != NULL) {
            m->probe.count = atoi(count);
        }
        ms_probe_start(&m->probe, m->detect);
    }
}

static void ms_terminate(void * inst) {
    tMsPlugin * m = (tMsPlugin *)inst;

    synthlib_log_line("terminate");
    ms_status_release(atomic_load(&m->statusSlot));
    atomic_store(&m->statusSlot, -1);
    m->status = NULL;

    m->clock.stats  = NULL;
    m->clock.detect = NULL;
    ms_stats_destroy(m->stats);

    // THE LISTENER GOES FIRST, and the source with it. It runs on CoreMIDI's thread and holds a pointer
    // to this instance; leaving it connected while the estimator underneath it is freed is a
    // use-after-free waiting for the next clock byte to arrive.
    ms_midi_set_listener(NULL, NULL);
    ms_midi_listen(-1);
    ms_clock_in_destroy(m->clockIn);
    m->clockIn = NULL;

    ms_detect_destroy(m->detect);
    m->stats  = NULL;
    m->detect = NULL;
}

static void ms_prepare(void * inst, const tSynthLibSetup * setup) {
    tMsPlugin * m = (tMsPlugin *)inst;

    m->sampleRate = setup->sampleRate;
    m->maxBlock   = setup->maxFrames;
    synthlib_log_line("setupProcessing: rate %.0f, maxBlock %d, mode %s", setup->sampleRate,
                (int)setup->maxFrames, setup->offline ? "offline" : "realtime");
}

static void ms_set_active(void * inst, bool active) {
    tMsPlugin * m = (tMsPlugin *)inst;

    synthlib_log_line("setActive(%d)", active ? 1 : 0);
    m->blocksSeen = 0;
    m->lastLogged = -1.0;
    ms_suspend(m);
}

// Called when the host starts and stops feeding blocks, distinct from setActive. Logged because it
// brackets every run of blocks and makes the log readable.
static void ms_set_processing(void * inst, bool running) {
    tMsPlugin * m = (tMsPlugin *)inst;

    synthlib_log_line("setProcessing(%d)", running ? 1 : 0);
    ms_suspend(m);
}

// ------------------------------------------------------------------------------------------------
// Audio
// ------------------------------------------------------------------------------------------------

// THE TRANSPORT'S FLAGS AS ONE WORD, for noticing that any of them changed - which is what the old
// ProcessContext.state was used for, one bit per field.
static uint32_t transport_bits(const tSynthLibTransport * t) {
    return (t->playing ? 0x001u : 0u) | (t->recording ? 0x002u : 0u) | (t->cycleActive ? 0x004u : 0u) |
           (t->tempoValid ? 0x008u : 0u) | (t->musicTimeValid ? 0x010u : 0u) |
           (t->barPositionValid ? 0x020u : 0u) | (t->timeSigValid ? 0x040u : 0u) |
           (t->systemTimeValid ? 0x080u : 0u) | (t->cycleValid ? 0x100u : 0u) |
           (t->continuousTimeValid ? 0x200u : 0u);
}

static void publish_status(tMsPlugin * m, const tSynthLibTransport * t, uint32_t frames) {
    tMsStatus * status = m->status;

    if (status == NULL) {
        return;
    }
    // Cheap enough to do every block: a couple of dozen atomic stores against a UI that reads them
    // thirty times a second. Throttling it would only add a staleness nobody asked for.
    tMsStatsSnapshot  snap;
    tMsDetectSnapshot hit;

    ms_stats_read(m->stats, &snap);
    ms_detect_read(m->detect, &hit);

    atomic_store(&status->active,  true);
    atomic_store(&status->hostBpm, t->tempoValid ? t->tempo : 0.0);
    atomic_store(&status->playing, t->playing ? 1 : 0);
    atomic_store(&status->ppq,     t->projectTimeMusic);

    atomic_store(&status->ticksSent,              (unsigned)m->clock.ticksSent);
    atomic_store(&status->commitMarginMeanMs,     snap.marginMeanMs);
    atomic_store(&status->commitMarginMinMs,      snap.marginMinMs);
    atomic_store(&status->lateTicks,              (unsigned)snap.lateTicks);
    atomic_store(&status->blockPeriodRmsMs,       snap.blockPeriodRmsMs);
    atomic_store(&status->blockPeriodRecentRmsMs, snap.blockPeriodRecentRmsMs);
    atomic_store(&status->blockRecentSeconds,     snap.blockRecentSeconds);
    atomic_store(&status->blockPeriodWorstMs,     snap.blockPeriodWorstMs);
    atomic_store(&status->blockGaps,              (unsigned)snap.blockGaps);
    atomic_store(&status->residualRmsMs,          ms_clock_residual_ms(&m->clock));
    atomic_store(&status->modelResyncs,           (unsigned)m->clock.modelResyncs);
    atomic_store(&status->driftPpm,               snap.driftPpm);
    atomic_store(&status->driftValid,             snap.driftValid ? 1 : 0);
    atomic_store(&status->driftSeconds,           snap.windowSeconds);

    atomic_store(&status->roundTripMeanMs,    hit.latencyMeanMs);
    atomic_store(&status->roundTripMinMs,     hit.latencyMinMs);
    atomic_store(&status->roundTripMaxMs,     hit.latencyMaxMs);
    atomic_store(&status->roundTripJitterMs,  hit.jitterRmsMs);
    atomic_store(&status->roundTripPeakDevMs, hit.peakDeviationMs);
    atomic_store(&status->hits,               (unsigned)hit.hits);
    atomic_store(&status->missed,             (unsigned)hit.missed);
    atomic_store(&status->spurious,           (unsigned)hit.spurious);
    atomic_store(&status->inputPeak,          (float)hit.inputPeak);
    atomic_store(&status->monitorPeriodMs,    hit.monitorPeriodMs);
    atomic_store(&status->monitorBpm,         hit.monitorBpm);
    atomic_store(&status->monitorOnsets,      (unsigned)hit.monitorOnsets);

    atomic_store(&status->scheduleLeadMs, (double)MS_LOOKAHEAD_MS);

    // THE HOST'S BUFFER IS THE CYCLE, NOT THE CALL. This is read by the breakdown's "Host buffer" bar,
    // so on the two calls a loop wrap produces it was publishing a FRAGMENT - 8 frames, 0.167 ms - and
    // the whole bar chart jumped every time round the loop.
    uint32_t hostCycleFrames = (m->cycleFramesLast > 0) ? m->cycleFramesLast : frames;

    atomic_store(&status->sampleRate, m->sampleRate);
    atomic_store(&status->blockFrames, (unsigned)hostCycleFrames);
    atomic_store(&status->blockMs,
                 (m->sampleRate > 0.0) ? (((double)hostCycleFrames / m->sampleRate) * 1000.0) : 0.0);
    atomic_store(&status->blockSplits, (unsigned)m->splitCalls);
    atomic_store(&status->compensationMs, m->clock.compensationMs);
    atomic_store(&status->probeRunning,   ms_probe_running(&m->probe) ? 1 : 0);

    // THE INCOMING CLOCK, read from the estimator's published snapshot. Nothing here is computed on
    // this thread - the CoreMIDI thread owns all of it and this only forwards what it last published.
    tMsClockInSnapshot in;

    ms_clock_in_read(m->clockIn, &in);
    atomic_store(&status->clockInBpm,       in.bpm);
    atomic_store(&status->clockInPeriodMs,  in.periodMs);
    atomic_store(&status->clockInJitterMs,  in.jitterRmsMs);
    atomic_store(&status->clockInPeakDevMs, in.peakDevMs);
    atomic_store(&status->clockInFitted,    (unsigned)in.fitted);
    atomic_store(&status->clockInClocks,    (unsigned)in.clocks);
    atomic_store(&status->clockInGaps,      (unsigned)in.gaps);
    atomic_store(&status->clockInRunning,   in.running ? 1 : 0);

    // ONE POINT PER DETECTION, not one per block. The graph is of what the hardware did, so its x axis
    // is hits - a block-rate trace would be a flat line with a step in it.
    if (hit.hits != m->lastGraphHits) {
        int write = atomic_load(&status->historyWrite);

        atomic_store(&status->history[write], (float)hit.latencyLastMs);
        atomic_store(&status->historyWrite, (write + 1) % MS_STATUS_HISTORY);
        m->lastGraphHits = hit.hits;
    }
}

// THROTTLED, AND ON MOVEMENT. One line when something meaningful changes, and one every two seconds
// regardless so a steady state is still visible.
static void log_heartbeat(tMsPlugin * m, const tSynthLibTransport * t, uint32_t frames, double wallDelta) {
    uint32_t bits             = transport_bits(t);
    bool     transportChanged = (bits != m->lastState);
    bool     tempoChanged     = t->tempoValid && (fabs(t->tempo - m->lastTempo) > 0.0005);

    // REAL FRAMES, NOT CALLS TIMES THE CURRENT SIZE. With a fixed block the two agree; with a host that
    // splits, the extra calls run the heartbeat's clock fast.
    m->framesSeen += (int64_t)frames;

    double nowSeconds = (double)m->framesSeen / ((m->sampleRate > 0.0) ? m->sampleRate : 48000.0);
    bool   heartbeat  = (nowSeconds - m->lastLogged) >= 2.0;

    if (!(transportChanged || tempoChanged || heartbeat)) {
        return;
    }
    m->lastState  = bits;
    m->lastTempo  = t->tempo;
    m->lastLogged = nowSeconds;

    // EVERY VALIDITY FLAG IS PRINTED, not just the values. Which fields a host bothers to fill in is
    // the actual question here - a zero in projectTimeMusic means one thing if the valid bit is set
    // and something completely different if it is not.
    char flags[160];

    snprintf(flags, sizeof(flags), "%s%s%s%s%s%s%s%s",
             t->playing ? "PLAYING " : "", t->recording ? "REC " : "", t->cycleActive ? "CYCLE " : "",
             t->tempoValid ? "tempo " : "", t->musicTimeValid ? "ptMusic " : "",
             t->barPositionValid ? "bar " : "", t->timeSigValid ? "timeSig " : "",
             t->systemTimeValid ? "sysTime " : "");

    synthlib_log_line("blk %-6llu n=%-5d | %.4f BPM | %d/%d | ppq %.4f bar %.4f | smp %lld cont %lld"
                " | sysTime %lld | wall +%.2f ms | ticks %llu wraps %llu | %s",
                (unsigned long long)m->blocksSeen, (int)frames, t->tempo,
                (int)t->timeSigNumerator, (int)t->timeSigDenominator,
                t->projectTimeMusic, t->barPositionMusic,
                (long long)t->projectTimeSamples, (long long)t->continuousTimeSamples,
                (long long)t->systemTime, wallDelta,
                (unsigned long long)m->clock.ticksSent, (unsigned long long)m->clock.wrapsSeen, flags);

    // WHETHER THE HOST IS SPLITTING, and how often. A split is not a fault and is not corrected away
    // silently: it is the host's own behaviour, and the count is the only way to know the correction
    // is firing on real ones rather than on ordinary blocks.
    if (m->splitCalls > 0) {
        synthlib_log_line("  blocks | %llu split call(s): the host handed one device cycle over in"
                    " more than one process() call - cycle %u frames, this call %d",
                    (unsigned long long)m->splitCalls,
                    (m->cycleFramesLast > 0) ? m->cycleFramesLast : (unsigned)frames, (int)frames);
    }
    tMsDetectSnapshot hit;

    ms_detect_read(m->detect, &hit);

    if (m->mode == eMsModeMonitor) {
        // NO LATENCY LINE IN MONITOR MODE. There is no reference for a transient to be late against,
        // so the only honest figures are the fitted grid and the spread about it.
        if (hit.monitorOnsets > 0) {
            synthlib_log_line("  monitor| grid %.3f ms (%.3f BPM) | jitter RMS %.3f peak dev %.3f ms"
                        " | %llu onset(s), %llu empty slot(s) | input peak %.4f",
                        hit.monitorPeriodMs, hit.monitorBpm, hit.jitterRmsMs, hit.peakDeviationMs,
                        (unsigned long long)hit.monitorOnsets, (unsigned long long)hit.missed,
                        hit.inputPeak);
        }
    } else if (m->mode == eMsModeClockOnly) {
        // NOTHING IS BEING MEASURED, so nothing is reported. The figures still in the snapshot are the
        // last run's and are preserved on purpose, but this log is a live trace.
    } else if ((hit.hits > 0) || (hit.missed > 0) || (hit.spurious > 0)) {
        // ROUND TRIP, and labelled as such - it still contains the interface's A/D and the host's
        // input buffering. See the note at the top of msDetect.h.
        synthlib_log_line("  device | round trip mean %.3f last %.3f min %.3f max %.3f ms"
                    " | jitter RMS %.3f peak dev %.3f ms | hits %llu missed %llu spurious %llu"
                    " | input peak %.4f",
                    hit.latencyMeanMs, hit.latencyLastMs, hit.latencyMinMs, hit.latencyMaxMs,
                    hit.jitterRmsMs, hit.peakDeviationMs,
                    (unsigned long long)hit.hits, (unsigned long long)hit.missed,
                    (unsigned long long)hit.spurious, hit.inputPeak);
    }
    tMsStatsSnapshot snap;

    ms_stats_read(m->stats, &snap);

    // NOT GATED ON TICKS. The block-period and drift figures are measured whether or not a destination
    // has been chosen, and a run with no port is exactly the one someone diagnosing a silent plug-in
    // would be looking at.
    if (snap.windowSeconds <= 0.0) {
        return;
    }
    synthlib_log_line("  timing | commit margin mean %+.3f min %+.3f RMS %.3f ms | late %llu/%llu"
                " | block period RMS %.3f all-time / %.3f over %.1f s, worst %+.3f ms"
                " | %llu gap(s) | drift %+.1f ppm | BPM host %.4f measured %.4f over %.1f s",
                snap.marginMeanMs, snap.marginMinMs, snap.marginRmsMs,
                (unsigned long long)snap.lateTicks, (unsigned long long)snap.ticks,
                snap.blockPeriodRmsMs, snap.blockPeriodRecentRmsMs,
                snap.blockRecentSeconds, snap.blockPeriodWorstMs,
                (unsigned long long)snap.blockGaps, snap.driftPpm,
                snap.hostBpm, snap.measuredBpm, snap.windowSeconds);

    // THE PAIR, AND THE RESYNC COUNT, ON ONE LINE. A residual on its own says nothing: it can look
    // excellent purely because the model keeps re-anchoring. THE ALL-TIME RMS IS THE RIGHT DENOMINATOR
    // here, and deliberately not the windowed one the panel shows - see msStats.h.
    synthlib_log_line("  model  | block jitter raw %.3f ms all-time -> residual %.3f ms RMS"
                " (%.1f %% of raw) worst %.3f ms | %llu resync(s) over %llu blocks",
                snap.blockPeriodRmsMs, ms_clock_residual_ms(&m->clock),
                (snap.blockPeriodRmsMs > 0.0)
                    ? ((ms_clock_residual_ms(&m->clock) / snap.blockPeriodRmsMs) * 100.0) : 0.0,
                ms_clock_residual_worst_ms(&m->clock),
                (unsigned long long)m->clock.modelResyncs, (unsigned long long)m->clock.modelBlocks);

    // THE INCOMING CLOCK, on the same heartbeat and only once there is a source. The ppm column is the
    // one worth reading: a difference in the fourth decimal place of a BPM is not otherwise legible.
    tMsClockInSnapshot in;

    ms_clock_in_read(m->clockIn, &in);

    if (ms_midi_listening() >= 0) {
        synthlib_log_line("  clk in | %.4f BPM (%.4f ms/clock) | jitter RMS %.4f peak dev %.4f ms"
                    " | %u fitted | %s | %llu clock(s) %llu gap(s) | vs host %+.1f ppm",
                    in.bpm, in.periodMs, in.jitterRmsMs, in.peakDevMs, in.fitted,
                    in.running ? "RUNNING" : "stopped",
                    (unsigned long long)in.clocks, (unsigned long long)in.gaps,
                    ((snap.hostBpm > 0.0) && (in.bpm > 0.0)) ? (((in.bpm / snap.hostBpm) - 1.0) * 1.0e6) : 0.0);
    }

    // THE BLOCK SIZE, reported whenever the host is not handing over a constant one. Every per-block
    // figure above assumes it is, and Live does not.
    if (snap.blockFramesMin != snap.blockFramesMax) {
        synthlib_log_line("  blocks | host block size VARIES: %u..%u frames, %llu change(s)"
                    " over %llu blocks - per-block figures are judged against the"
                    " previous block's duration accordingly",
                    snap.blockFramesMin, snap.blockFramesMax,
                    (unsigned long long)snap.blockSizeChanges, (unsigned long long)snap.blocks);
    }
}

static void ms_process(void * inst, const float * const * in, uint32_t numIn, float ** out,
                       uint32_t numOut, uint32_t frames, const tSynthLibTransport * t) {
    tMsPlugin * m = (tMsPlugin *)inst;

    // PASS THE AUDIO THROUGH UNCHANGED. A plug-in on a track should not silence it just because it is
    // only looking - and a host may hand over the same buffer for both, which is nothing to copy.
    for (uint32_t ch = 0; (out != NULL) && (ch < numOut); ch++) {
        const float * source = ((in != NULL) && (ch < numIn)) ? in[ch] : NULL;

        if ((out[ch] == NULL) || (source == out[ch])) {
            continue;
        }

        if (source != NULL) {
            memcpy(out[ch], source, (size_t)frames * sizeof(float));
        } else {
            memset(out[ch], 0, (size_t)frames * sizeof(float));
        }
    }
    m->blocksSeen++;

    if (!t->valid) {
        // Worth its own line, and worth not repeating a hundred times a second: a host that hands over
        // no transport at all is the failure this whole exercise is looking for. An Audio Unit host
        // that installed no HostCallbacks is exactly this.
        if (!m->warnedNoContext) {
            m->warnedNoContext = true;
            synthlib_log_line("WARNING: the host gives no transport - no timing at all");
        }
        return;
    }

    // notes §7
    uint64_t hostNow   = AudioGetCurrentHostTime();
    double   hostNowMs = (double)AudioConvertHostTimeToNanos(hostNow) / 1.0e6;
    double   wallDelta = (m->lastHostMs > 0.0) ? (hostNowMs - m->lastHostMs) : 0.0;

    // notes §8
    if (frames > m->observedMaxFrames) {
        m->observedMaxFrames = frames;
    }
    uint32_t cycleFrames = (m->observedMaxFrames > 0) ? m->observedMaxFrames : m->maxBlock;
    double   cycleMs     = ((m->sampleRate > 0.0) && (cycleFrames > 0))
                           ? (((double)cycleFrames / m->sampleRate) * 1000.0) : 0.0;
    bool     newCycle    = true;

    if (m->haveCall && (hostNow >= m->prevCallHostTime) && (cycleMs > 0.0)) {
        double sinceMs = (double)AudioConvertHostTimeToNanos(hostNow - m->prevCallHostTime) / 1.0e6;

        newCycle = ((m->cycleFramesSoFar + frames) > cycleFrames) || (sinceMs >= (0.5 * cycleMs));
    }

    if (newCycle) {
        // THE CYCLE THAT JUST ENDED IS THE HOST'S REAL BUFFER, and it is what the latency breakdown has
        // to be told about - see publish_status().
        m->cycleFramesLast    = m->haveCall ? m->cycleFramesSoFar : frames;
        m->cycleStartHostTime = hostNow;
        m->cycleFramesSoFar   = 0;
    } else {
        m->splitCalls++;
    }
    uint64_t blockHostTime = m->cycleStartHostTime;

    if ((m->sampleRate > 0.0) && (m->cycleFramesSoFar > 0)) {
        blockHostTime += AudioConvertNanosToHostTime(
            (uint64_t)(((double)m->cycleFramesSoFar / m->sampleRate) * 1.0e9));
    }
    m->prevCallHostTime  = hostNow;
    m->cycleFramesSoFar += frames;
    m->haveCall          = true;

    // THE PANEL'S CLEAR, TAKEN HERE AND NOWHERE ELSE. The UI raises status->clearRequest and the audio
    // thread - which owns every one of these figures - acts on it, so no other thread ever writes the
    // accumulators. BEFORE ms_stats_block(), so this block is the first of the new run.
    if ((m->status != NULL) && (atomic_exchange(&m->status->clearRequest, 0u) != 0u)) {
        ms_stats_reset(m->stats);
        ms_clock_reset_stats(&m->clock);
        synthlib_log_line("figures cleared from the panel");
    }
    double tempo = t->tempoValid ? t->tempo : 0.0;

    ms_stats_block(m->stats, blockHostTime, frames, m->sampleRate, tempo, t->playing);

    // THE PROBE, which sends nothing unless a run is armed - and NOT IN A MODE THAT CANNOT HEAR THE
    // ANSWER, or it would queue expectations nothing will ever match and count each as a miss.
    if (m->mode == eMsModeMeasure) {
        ms_probe_process(&m->probe, m->detect, frames, m->sampleRate, blockHostTime);
    }

    // THE CLOCK, before any logging: the ticks in this block belong to the wall time just read.
    ms_clock_process(&m->clock, t->projectTimeMusic, tempo, t->playing, t->cycleActive,
                     t->cycleStartMusic, t->cycleEndMusic, frames, m->sampleRate, blockHostTime);

    // THE AUDIO, after the clock: this block's transients are answers to ticks scheduled in earlier
    // blocks. And NOT AT ALL IN CLOCK-ONLY MODE - the work would be done for an answer nobody asked for.
    if ((m->mode != eMsModeClockOnly) && (in != NULL) && (numIn > 0) && (in[0] != NULL)) {
        // WHICH CHANNEL, chosen by the user. A drum machine on one side of a stereo pair is ordinary,
        // and summing would let a stereo hit's own channel-to-channel delay smear the onset - so a
        // sum is offered but is not the default.
        int           source  = (int)((param_of(m, kParamAudioSource) * (double)(MS_AUDIO_SOURCES - 1)) + 0.5);
        const float * samples = in[0];

        if ((source == 1) && (numIn > 1) && (in[1] != NULL)) {
            samples = in[1];
        } else if ((source == 2) && (numIn > 1) && (in[1] != NULL)) {
            // Summed into scratch rather than in place: the input may be the OUTPUT buffer too, and
            // writing to it would alter the audio passing through a plug-in that promises not to.
            uint32_t count = (frames < (uint32_t)(sizeof(m->sumBuffer) / sizeof(m->sumBuffer[0])))
                             ? frames : (uint32_t)(sizeof(m->sumBuffer) / sizeof(m->sumBuffer[0]));

            for (uint32_t i = 0; i < count; i++) {
                m->sumBuffer[i] = 0.5f * (in[0][i] + in[1][i]);
            }
            samples = m->sumBuffer;
        }
        ms_detect_audio(m->detect, samples, frames, m->sampleRate, blockHostTime);
    }
    publish_status(m, t, frames);

    // WHERE THE MUSICAL POSITION SHOULD HAVE GOT TO, if nothing jumped. Anything else is a loop wrap, a
    // playhead move or a tempo change taking effect - logged the moment it happens.
    double expectedPpq = m->lastPpq + ((t->tempo / 60.0) * ((double)frames / m->sampleRate));
    bool   jumped      = (m->lastPpq >= 0.0) && t->playing
                         && (fabs(t->projectTimeMusic - expectedPpq) > 1.0e-6);

    if (jumped) {
        synthlib_log_line("JUMP  ppq %.6f -> %.6f (expected %.6f, delta %+.6f) | smp %lld | bar %.4f",
                    m->lastPpq, t->projectTimeMusic, expectedPpq, t->projectTimeMusic - expectedPpq,
                    (long long)t->projectTimeSamples, t->barPositionMusic);
    }
    m->lastPpq    = t->projectTimeMusic;
    m->lastHostMs = hostNowMs;

    log_heartbeat(m, t, frames, wallDelta);
}

// ------------------------------------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------------------------------------

// FROM THE HOST - inside a block on VST3, from wherever it likes on an Audio Unit - and applied at
// once, exactly as msVst3.cpp applied the last point of each queue at the top of process().
static void ms_set_param(void * inst, uint32_t id, double normalized) {
    apply_parameter((tMsPlugin *)inst, (int)id, normalized);
}

static double ms_get_param(void * inst, uint32_t id) {
    return (id < (uint32_t)kParamCount) ? param_of((tMsPlugin *)inst, (int)id) : 0.0;
}

static bool ms_param_text(const tSynthLibPluginDesc * desc, void * inst, uint32_t id, double value,
                          char * out, size_t len) {
    (void)desc;
    (void)inst;

    if (id == (uint32_t)kParamMidiDest) {
        int slot = mst_dest_slot(value);

        if (slot <= 0) {
            snprintf(out, len, "none");
        } else if ((slot - 1) < ms_midi_count()) {
            ms_midi_name(slot - 1, out, len);
        } else {
            snprintf(out, len, "-");
        }
    } else if (id == (uint32_t)kParamCompensate) {
        snprintf(out, len, "%.1f ms", value * MST_COMPENSATE_MAX);
    } else if (id == (uint32_t)kParamMode) {
        static const char * names[eMsModeCount] = { "generate + measure", "clock only", "monitor" };

        snprintf(out, len, "%s", names[(int)ms_mode_from_normalized(value)]);
    } else if (id == (uint32_t)kParamClockSource) {
        int slot = mst_source_slot(value);

        if (slot <= 0) {
            snprintf(out, len, "none");
        } else if ((slot - 1) < ms_midi_source_count()) {
            ms_midi_source_name(slot - 1, out, len);
        } else {
            snprintf(out, len, "-");
        }
    } else if (id == (uint32_t)kParamAudioSource) {
        static const char * names[MS_AUDIO_SOURCES] = { "left", "right", "left + right" };
        int index = (int)((value * (double)(MS_AUDIO_SOURCES - 1)) + 0.5);

        snprintf(out, len, "%s", names[(index < 0) ? 0 : ((index >= MS_AUDIO_SOURCES) ? (MS_AUDIO_SOURCES - 1) : index)]);
    } else {
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------

// notes §9
static size_t ms_get_state(void * inst, void * out, size_t len) {
    tMsPlugin * m = (tMsPlugin *)inst;
    uint8_t     blob[MST_STATE_BYTES];
    double      values[MST_STATE_V1_PARAMS];
    double      clockSource = param_of(m, kParamClockSource);
    char        name[MS_MIDI_NAME_LEN];
    char        sourceName[MS_MIDI_NAME_LEN];
    int         listening = ms_midi_listening();

    if (out == NULL) {
        return MST_STATE_BYTES;
    }

    for (int i = 0; i < MST_STATE_V1_PARAMS; i++) {
        values[i] = param_of(m, i);
    }
    memset(name, 0, sizeof(name));
    memset(sourceName, 0, sizeof(sourceName));

    if (m->clock.destination >= 0) {
        ms_midi_name(m->clock.destination, name, sizeof(name));
    }

    if (listening >= 0) {
        ms_midi_source_name(listening, sourceName, sizeof(sourceName));
    }
    memcpy(blob, values, sizeof(values));
    memcpy(blob + MST_STATE_NAME_AT, name, sizeof(name));
    memcpy(blob + MST_STATE_SOURCE_AT, &clockSource, sizeof(clockSource));
    memcpy(blob + MST_STATE_SNAME_AT, sourceName, sizeof(sourceName));

    size_t take = (len < MST_STATE_BYTES) ? len : MST_STATE_BYTES;

    memcpy(out, blob, take);
    return take;
}

// A FIELD, IF THE BLOB REACHES THAT FAR. A block from an older build is shorter and simply stops,
// leaving the default in place - which is exactly what the stream reads did in msVst3.cpp.
static bool read_double(const uint8_t * data, size_t len, size_t at, double * out) {
    if ((at + sizeof(double)) > len) {
        return false;
    }
    memcpy(out, data + at, sizeof(double));
    return true;
}

static bool read_name(const uint8_t * data, size_t len, size_t at, char * out) {
    memset(out, 0, MS_MIDI_NAME_LEN);

    if (at >= len) {
        return false;
    }
    size_t take = ((len - at) < (size_t)MS_MIDI_NAME_LEN) ? (len - at) : (size_t)MS_MIDI_NAME_LEN;

    memcpy(out, data + at, take);
    out[MS_MIDI_NAME_LEN - 1] = '\0';
    return (out[0] != '\0');
}

// THE PROJECT MUST REMEMBER THE PORT, or every reopened set is silent until someone notices.
static void ms_set_state(void * inst, const void * data, size_t len) {
    tMsPlugin *     m = (tMsPlugin *)inst;
    const uint8_t * p = (const uint8_t *)data;
    double          value;
    char            name[MS_MIDI_NAME_LEN];

    if ((data == NULL) || (len == 0u)) {
        return;
    }

    // A BLOCK FROM THE TWO-STATE TOGGLE BUILD IS THE SAME LENGTH and needs no migration: it wrote 0.0
    // for generate and 1.0 for monitor, which are exactly the mode's first and last steps.
    for (int i = 0; i < MST_STATE_V1_PARAMS; i++) {
        if (read_double(p, len, (size_t)i * sizeof(double), &value)) {
            apply_parameter(m, i, value);
        }
    }

    if (read_name(p, len, MST_STATE_NAME_AT, name)) {
        int index = ms_midi_index_for_name(name);

        if (index >= 0) {
            double resolved = mst_dest_normalized(index + 1);

            apply_parameter(m, kParamMidiDest, resolved);

            // AND THE HOST TOLD, which msVst3.cpp never did: the saved INDEX named whatever port sat
            // there that day, and the name has just found it somewhere else. Without this the host's
            // panel goes on naming the other one.
            synthlib_plugin_param_edited(m, kParamMidiDest, resolved);
        } else {
            // NAMED IN THE PROJECT BUT NOT PLUGGED IN, which is a different state from "nothing chosen"
            // and has to look different: one is a plug-in waiting for hardware it has been told to
            // use, the other has never been told anything.
            set_selected_port(m, -1);

            if (m->status != NULL) {
                snprintf(m->status->waitingName, sizeof(m->status->waitingName), "%s", name);
                atomic_store(&m->status->waitingForDevice, 1);
                atomic_store(&m->status->haveDestination, 0);
            }
            synthlib_log_line("saved destination '%s' is not present - generating nothing", name);
        }
    }

    // ---- ANYTHING ADDED AFTER v1 LIVES HERE, past the name -----------------------------------
    if (read_double(p, len, MST_STATE_SOURCE_AT, &value)) {
        apply_parameter(m, kParamClockSource, value);
    }

    // THE NAME WINS OVER THE INDEX, for the same reason it does on the destination side.
    if (read_name(p, len, MST_STATE_SNAME_AT, name)) {
        int    index    = ms_midi_source_index_for_name(name);
        double resolved = (index >= 0) ? mst_source_normalized(index + 1) : 0.0;

        apply_parameter(m, kParamClockSource, resolved);
        synthlib_plugin_param_edited(m, kParamClockSource, resolved);

        if (index < 0) {
            synthlib_log_line("saved clock source '%s' is not present - measuring nothing", name);
        }
    }
}

// WHAT THE SAME BYTES SAY, WITHOUT AN INSTANCE - for a VST3 controller, which the host hands the
// processor's state precisely so the panel agrees with it. THE SAME LAYOUT, read the same way: reading
// five doubles in one go would take the first eight bytes of the destination NAME as the fifth.
static uint32_t ms_state_params(const tSynthLibPluginDesc * desc, const void * data, size_t len,
                                tSynthLibParamValue * out, uint32_t capacity) {
    const uint8_t * p     = (const uint8_t *)data;
    uint32_t        count = 0;
    double          value;

    (void)desc;

    for (int i = 0; (i < MST_STATE_V1_PARAMS) && (count < capacity); i++) {
        if (read_double(p, len, (size_t)i * sizeof(double), &value)) {
            out[count].id    = (uint32_t)i;
            out[count].value = value;
            count++;
        }
    }

    if ((count < capacity) && read_double(p, len, MST_STATE_SOURCE_AT, &value)) {
        out[count].id    = kParamClockSource;
        out[count].value = value;
        count++;
    }
    return count;
}

// ------------------------------------------------------------------------------------------------
// Editor
// ------------------------------------------------------------------------------------------------

// A CLICK BECOMES A HOST PARAMETER EDIT, never a direct write - with one exception.
static void ms_on_edit(void * user, const tMsEditRequest * request) {
    tMsPlugin * m  = (tMsPlugin *)user;
    uint32_t    id = 0;

    if ((request == NULL) || (request->which == eMsEditNone)) {
        return;
    }

    // THE ONE REQUEST THAT IS NOT A PARAMETER. A clear has no value and nothing for a host to automate
    // or recall, so it goes straight to the audio thread through the shared status slot - the same
    // memory the panel already reads its figures from. See clearRequest in msStatus.h.
    if (request->which == eMsEditClearStats) {
        tMsStatus * status = (m != NULL) ? ms_status(atomic_load(&m->statusSlot)) : NULL;

        if (status != NULL) {
            atomic_fetch_add(&status->clearRequest, 1u);
        }
        return;
    }

    // EXPLICIT, not a chain of conditionals with a fall-through default: the form this replaced mapped
    // anything it did not recognise onto the compensation, so a control added later would silently
    // have dialled in latency instead of doing its own job.
    switch (request->which) {
        case eMsEditMidiDest:    id = kParamMidiDest;    break;
        case eMsEditCompensate:  id = kParamCompensate;  break;
        case eMsEditAudioSource: id = kParamAudioSource; break;
        case eMsEditMode:        id = kParamMode;        break;
        case eMsEditClockSource: id = kParamClockSource; break;
        default:                 return;
    }
    synthlib_plugin_param_edited(m, id, request->normalized);
}

// notes §10
static void ms_sync(void * user) {
    tMsPlugin * m = (tMsPlugin *)user;

    ms_draw_set_status_slot((m != NULL) ? atomic_load(&m->statusSlot) : -1);

    if (m == NULL) {
        return;
    }
    ms_draw_set_values(synthlib_plugin_param_value(m, kParamMidiDest),
                       synthlib_plugin_param_value(m, kParamAudioSource),
                       synthlib_plugin_param_value(m, kParamCompensate),
                       synthlib_plugin_param_value(m, kParamMode),
                       synthlib_plugin_param_value(m, kParamClockSource));
}

static void ms_panel_frame(void * user, int pixelWidth, int pixelHeight) {
    (void)user;
    ms_draw_frame(pixelWidth, pixelHeight);
}

static bool ms_panel_click(void * user, double x, double y) {
    tMsEditRequest request;

    if (ms_draw_click(x, y, &request) == false) {
        return false;
    }
    ms_on_edit(user, &request);
    return true;
}

static const tSynthLibPanel gPanel = {
    .canvasWidth = MS_CANVAS_W,
    .init        = ms_draw_init,
    .sync        = ms_sync,
    .frame       = ms_panel_frame,
    .click       = ms_panel_click,
    .pointer     = ms_draw_set_mouse,
    .menuActive  = ms_draw_menu_active,
};

static void * ms_create_view(const tSynthLibPluginDesc * desc, void * inst, double width, double height) {
    (void)desc;
    return synthlib_panel_view_create(&gPanel, inst, width, height);
}

// THE MACHINE'S REMEMBERED WIDTH, the starting point for an editor a project has never opened - a VST3
// project remembers its own on top of this. msVst3.cpp remembered nothing at all.
#define MST_PREFS_APP      CFSTR("com.chrispurusha.midisynctool")
#define MST_PREFS_WIDTH    CFSTR("editorWidth")

static long ms_editor_width_load(void) {
    CFPropertyListRef value = CFPreferencesCopyAppValue(MST_PREFS_WIDTH, MST_PREFS_APP);
    long              width = 0;

    if (value != NULL) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)value, kCFNumberLongType, &width);
        }
        CFRelease(value);
    }
    return width;
}

static void ms_editor_width_save(long width) {
    CFNumberRef value = CFNumberCreate(NULL, kCFNumberLongType, &width);

    if (value != NULL) {
        CFPreferencesSetAppValue(MST_PREFS_WIDTH, value, MST_PREFS_APP);
        CFRelease(value);
    }
}

// ------------------------------------------------------------------------------------------------
// The descriptor
// ------------------------------------------------------------------------------------------------

// AN EFFECT WITH AN AUDIO INPUT: the transients the hardware sends back arrive on it, and an effect
// that declares one never meets the "no valid audio input bus" rejection an instrument-shaped one does.
static const tSynthLibBus gInputs[1]  = { { "In", 2, false, true } };
static const tSynthLibBus gOutputs[1] = { { "Out", 2, false, true } };

static const tSynthLibPluginDesc gDescriptor = {
    .name               = "MidiSyncTool",
    .vendor             = "Chris Purusha",
    .url                = "https://github.com/chrispurusha/MidiSyncTool",
    .email              = "",
    .version            = MST_VERSION_STRING,

    // OnlyRT because a clock generator during a faster-than-realtime bounce is meaningless.
    .isInstrument       = false,
    .vst3SubCategory    = "Fx|NoOfflineProcess|OnlyRT|Tools",
    .inputs             = gInputs,
    .numInputs          = 1,
    .outputs            = gOutputs,
    .numOutputs         = 1,
    .wantsMidiIn        = false,

    // THE WHOLE REASON THIS PLUG-IN CAN SEE ANYTHING. A VST3 host is entitled to hand over no
    // ProcessContext unless the plug-in states it needs one; an Audio Unit installs its HostCallbacks
    // on the strength of this.
    .wantsTransport     = true,

    .vst3ProcessorUid   = gProcessorUid,
    .vst3ControllerUid  = gControllerUid,
    .auType             = MST_AU_TYPE,
    .auSubType          = MST_AU_SUBTYPE,
    .auManufacturer     = MST_AU_MANUFACTURER,
    .auVersion          = MST_AU_VERSION,
    .auBundleId         = MST_AU_BUNDLE_ID,

    .params             = gParams,
    .numParams          = kParamCount,

    .editorDefaultWidth = MS_CANVAS_W,
    .editorMinWidth     = MS_CANVAS_W * 0.75,
    .editorMaxWidth     = MS_CANVAS_W * 2.0,
    .editorAspect       = MS_CANVAS_W / MS_CANVAS_H,
    .editorWidthLoad    = ms_editor_width_load,
    .editorWidthSave    = ms_editor_width_save,

    .cb = {
        .create        = ms_create,
        .destroy       = ms_destroy,
        .initialize    = ms_initialize,
        .terminate     = ms_terminate,
        .prepare       = ms_prepare,
        .setActive     = ms_set_active,
        .setProcessing = ms_set_processing,
        .process       = ms_process,
        .setParam      = ms_set_param,
        .getParam      = ms_get_param,
        .paramText     = ms_param_text,
        .getState      = ms_get_state,
        .setState      = ms_set_state,
        .stateParams   = ms_state_params,
        .createView    = ms_create_view
    }
};

static const tSynthLibPluginSet gSet = {
    .variants = &gDescriptor,
    .count    = 1
};

const tSynthLibPluginSet * synthlib_plugin_variants(void) {
    return &gSet;
}
