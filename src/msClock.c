/*
 * MidiSyncTool - MIDI clock generation from the host's musical position.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msClock.c.md - "// notes §k" refers there.

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <string.h>

#include "msClock.h"
#include "synthlibLog.h"
#include "msMidi.h"
#include "msDetect.h"
#include "msStats.h"

#define MIDI_CLOCK       (0xF8)
#define MIDI_START       (0xFA)
#define MIDI_CONTINUE    (0xFB)
#define MIDI_STOP        (0xFC)
#define MIDI_SPP         (0xF2)

// notes §1
#define MS_CLOCKS_PER_BEAT    (6)

void ms_clock_set_compensation_ms(tMsClock * clock, double deviceMs) {
    if (clock != NULL) {
        clock->compensationMs = deviceMs;
    }
}

double ms_clock_residual_ms(const tMsClock * clock) {
    if ((clock == NULL) || (clock->modelBlocks == 0)) {
        return 0.0;
    }
    return sqrt(clock->modelErrorSumSq / (double)clock->modelBlocks);
}

double ms_clock_residual_worst_ms(const tMsClock * clock) {
    return (clock != NULL) ? clock->modelWorstMs : 0.0;
}

void ms_clock_reset_stats(tMsClock * clock) {
    if (clock == NULL) {
        return;
    }
    clock->modelErrorSumSq = 0.0;
    clock->modelWorstMs    = 0.0;
    clock->modelBlocks     = 0;
    clock->modelResyncs    = 0;
}

void ms_clock_init(tMsClock * clock) {
    memset(clock, 0, sizeof(*clock));
    clock->destination = -1;
    clock->rateRatio   = 1.0;   // NOT zero, which memset would leave it - see msClock.h
}

// notes §2
void ms_clock_process(tMsClock * clock,
                      double     ppq,
                      double     tempo,
                      bool       playing,
                      bool       cycleActive,
                      double     cycleStartPpq, // unused since the phase accumulator replaced the
                                                // range search - kept because the wrap logic
                                                // will want it again for non-bar-aligned loops
                      double   cycleEndPpq,
                      uint32_t blockFrames,
                      double   sampleRate,
                      uint64_t blockHostTime) {
    // notes §3
    if (clock->destination < 0) {
        clock->havePrev = false;
        clock->running  = false;
        return;
    }

    if ((tempo <= 0.0) || (sampleRate <= 0.0) || (blockFrames == 0)) {
        clock->havePrev = false;
        return;
    }

    if (!playing) {
        // STOP, once, on the edge. Sending it every block while stopped would be legal and would
        // also flood the wire for no purpose.
        if (clock->running) {
            uint8_t byte = MIDI_STOP;

            ms_midi_send_at(clock->destination, &byte, 1, blockHostTime);
            clock->stopsSent++;
            synthlib_log_line("transport: STOP at ppq %.4f", ppq);
        }
        // Whether clock should keep running while stopped is still an open decision (some gear
        // wants it, some only while running), so for now nothing further is sent and the position is
        // forgotten - a restart must not think it is continuing.
        clock->havePrev = false;
        clock->running  = false;
        // notes §4
        clock->haveBase = false;
        return;
    }
    // notes §5
    bool restarted = !clock->running;

    if (restarted) {
        int beat16 = (int)llround(ppq * 4.0);   // sixteenths since the start of the song

        if (beat16 <= 0) {
            uint8_t byte = MIDI_START;

            ms_midi_send_at(clock->destination, &byte, 1, blockHostTime);
            clock->startsSent++;
            synthlib_log_line("transport: START at ppq %.4f", ppq);
        } else {
            uint8_t spp[3] = {
                MIDI_SPP,
                (uint8_t)(beat16 & 0x7F),
                (uint8_t)((beat16 >> 7) & 0x7F)
            };
            uint8_t cont   = MIDI_CONTINUE;

            ms_midi_send_at(clock->destination, spp, 3, blockHostTime);
            ms_midi_send_at(clock->destination, &cont, 1, blockHostTime);
            clock->continuesSent++;
            synthlib_log_line("transport: SPP %d + CONTINUE at ppq %.4f", beat16, ppq);
        }
        // A RUN'S FIGURES SHOULD DESCRIBE THAT RUN. Carrying a mean across a stop mixes in whatever
        // the last take did, and the worst case would never improve however well the tool behaved
        // afterwards.
        ms_stats_reset(clock->stats);
        ms_detect_reset(clock->detect);

        // The tick grid restarts from wherever we resumed, so the leftover fraction from whatever
        // was playing before is meaningless and would place the first tick early.
        clock->phase      = 0.0;
        clock->ticksInRun = 0;
    }
    double blockPpq = (tempo / 60.0) * ((double)blockFrames / sampleRate);
    double tickPpq  = 1.0 / (double)MS_PPQN;

    // notes §6
    if (restarted) {
        clock->phase          = tickPpq;
        clock->advanceApplied = 0.0;
    }

    // notes §7
    if (clock->compensationMs != 0.0) {
        double advanceQn = ((clock->compensationMs + MS_LOOKAHEAD_MS) / 1000.0) * (tempo / 60.0);
        double delta     = advanceQn - clock->advanceApplied;

        clock->advanceApplied = advanceQn;

        // notes §8
        clock->phase          = clock->phase + delta;
    }
    (void)cycleStartPpq;

    // notes §9
    double observedNs     = (double)AudioConvertHostTimeToNanos(blockHostTime);
    double nominalNsPerQn = (60.0 / tempo) * 1.0e9;

    if (restarted || !clock->haveModel) {
        clock->runQn           = 0.0;
        clock->anchorQn        = 0.0;
        clock->anchorNs        = observedNs;
        // THE CRYSTAL RATIO SURVIVES A RESTART. It describes this machine's two oscillators, not
        // this run - re-deriving it from nominal would spend the first seconds of every run
        // relearning something that has not changed since the last one.
        clock->nsPerQn         = nominalNsPerQn * clock->rateRatio;
        clock->haveModel       = true;
        clock->haveBase        = false;
        clock->modelErrorSumSq = 0.0;
        clock->modelWorstMs    = 0.0;
        clock->modelBlocks     = 0;
    }

    // notes §10
    if (  clock->haveModel && (clock->lastTempo > 0.0)
       && (fabs(tempo - clock->lastTempo) > 1.0e-9)) {
        double predictedNow = clock->anchorNs + ((clock->runQn - clock->anchorQn) * clock->nsPerQn);

        clock->nsPerQn  = nominalNsPerQn * clock->rateRatio;
        clock->anchorNs = predictedNow - ((clock->runQn - clock->anchorQn) * clock->nsPerQn);
    }
    double predictedNs = clock->anchorNs + ((clock->runQn - clock->anchorQn) * clock->nsPerQn);
    double errorNs     = observedNs - predictedNs;

    // A SEEK, A TEMPO CHANGE TAKING EFFECT, OR A DROPOUT - not jitter. Filtering through one of
    // those would drag the output across seconds of wrong time while it caught up, so the model is
    // re-anchored outright and the event is counted rather than smoothed away.
    if (fabs(errorNs) > (MS_MODEL_RESYNC_MS * 1.0e6)) {
        clock->anchorQn = clock->runQn;
        clock->anchorNs = observedNs;
        // THE RATIO SURVIVES A RESYNC TOO. A dropout says the wall clock jumped; it says nothing
        // whatever about how the two crystals compare, and discarding the ratio would make every
        // glitch cost seconds of relearning.
        clock->nsPerQn  = nominalNsPerQn * clock->rateRatio;
        clock->modelResyncs++;
        predictedNs     = observedNs;
        errorNs         = 0.0;
        // notes §11
        clock->haveBase = false;
    } else {
        // A PI CORRECTION. The proportional term moves the line; the integral term tilts it, which
        // is what absorbs the standing difference between the audio crystal and the system clock
        // rather than chasing it block after block.
        double span = clock->runQn - clock->anchorQn;

        clock->anchorNs += (MS_MODEL_KP * errorNs);

        if (span > 1.0) {
            clock->nsPerQn  += (MS_MODEL_KI * errorNs / span);

            // notes §12
            clock->rateRatio = clock->nsPerQn / nominalNsPerQn;

            if (clock->rateRatio < 0.999) {
                clock->rateRatio = 0.999;
                clock->nsPerQn   = nominalNsPerQn * clock->rateRatio;
            } else if (clock->rateRatio > 1.001) {
                clock->rateRatio = 1.001;
                clock->nsPerQn   = nominalNsPerQn * clock->rateRatio;
            }
        }
        predictedNs     += (MS_MODEL_KP * errorNs);
    }

    // notes §13
    if (clock->haveBase) {
        double baseStepNs = predictedNs - clock->lastBaseNs;
        double nominalNs  = clock->lastNominalNs;
        double residualMs = (baseStepNs - nominalNs) / 1.0e6;

        clock->modelErrorSumSq += (residualMs * residualMs);
        clock->modelBlocks++;

        if (fabs(residualMs) > clock->modelWorstMs) {
            clock->modelWorstMs = fabs(residualMs);
        }
    }
    clock->lastBaseNs    = predictedNs;
    clock->lastNominalNs = ((double)blockFrames / sampleRate) * 1.0e9;
    clock->haveBase      = true;

    // THE BASE EVERY TICK IN THIS BLOCK IS STAMPED FROM. Modelled, not observed - which is the whole
    // point: a block that arrived 0.5 ms late moves this by MS_MODEL_KP of that error, not by all
    // of it.
    uint64_t baseHostTime = AudioConvertNanosToHostTime((uint64_t)predictedNs);

    if (clock->havePrev) {
        double expected = clock->lastPpq + blockPpq;

        // A WRAP, not a seek: the position went backwards AND the host says it is cycling. A seek
        // also goes backwards, but there the tick phase genuinely should restart, so only the cycle
        // case needs handling.
        if (cycleActive && (ppq < clock->lastPpq) && (expected > cycleEndPpq)) {
            clock->wrapsSeen++;
        }
    }
    // notes §14
    double distance = tickPpq - clock->phase;    // how far into this block the next tick falls

    while (distance < blockPpq) {
        // WHERE IN THE BLOCK, converted to a moment on the wall clock, plus the fixed lookahead -
        // see MS_LOOKAHEAD_MS. Handing CoreMIDI the future time is the whole reason the jitter is
        // microseconds rather than milliseconds.
        double   offsetNs = ((distance / blockPpq) * ((double)blockFrames / sampleRate) * 1.0e9)
                            + (MS_LOOKAHEAD_MS * 1.0e6);
        uint64_t when;

        // notes §15
        bool caughtUp = (offsetNs < 0.0);

        if (caughtUp) {
            when = AudioGetCurrentHostTime();       // as soon as the wire will take it
        } else {
            when = baseHostTime + AudioConvertNanosToHostTime((uint64_t)offsetNs);
        }
        uint8_t byte = MIDI_CLOCK;

        ms_midi_send_at(clock->destination, &byte, 1, when);

        // notes §16
        if (!caughtUp) {
            ms_stats_tick(clock->stats, when, AudioGetCurrentHostTime());
        }
        // COUNTED WITHIN THE RUN, not since the plug-in loaded. The detector takes every Nth tick as
        // a calibration position, and that N has to be measured from the downbeat the transport
        // started on or the grid lands between the drum machine's steps.
        ms_detect_tick(clock->detect, clock->ticksInRun, when);
        clock->ticksInRun++;
        clock->ticksSent++;
        distance += tickPpq;
    }
    clock->runQn    += blockPpq;
    clock->phase     = fmod(clock->phase + blockPpq, tickPpq);

    clock->lastPpq   = ppq;
    clock->lastTempo = tempo;
    clock->havePrev  = true;
    clock->running   = true;
}
