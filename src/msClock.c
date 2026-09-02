/*
 * MidiSyncTool - MIDI clock generation from the host's musical position.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreAudio/HostTime.h>
#include <math.h>
#include <string.h>

#include "msClock.h"
#include "msLog.h"
#include "msMidi.h"
#include "msDetect.h"
#include "msStats.h"

#define MIDI_CLOCK       (0xF8)
#define MIDI_START       (0xFA)
#define MIDI_CONTINUE    (0xFB)
#define MIDI_STOP        (0xFC)
#define MIDI_SPP         (0xF2)

// A MIDI beat is a SIXTEENTH note, not a quarter and not a bar - six MIDI clocks. Song Position
// Pointer counts those, 14 bits, LSB first. It is the only way to tell a device where "continue"
// means, and a device that receives Continue without one resumes from wherever it last was, which
// is rarely where the DAW is.
#define MS_CLOCKS_PER_BEAT    (6)

void ms_clock_set_compensation_ms(tMsClock * clock, double deviceMs) {
    if (clock != NULL) {
        clock->compensationMs = deviceMs;
    }
}

void ms_clock_init(tMsClock * clock) {
    memset(clock, 0, sizeof(*clock));
    clock->destination = -1;
}

// WHERE THE HOST SAYS WE ARE IS NOT ALWAYS WHERE WE ARE, and that is the whole difficulty.
//
// Live reports musical position at the start of each block, and it never splits a block at a loop
// boundary - so a wrap always falls INSIDE a block. Worse, the block after a wrap reads EXACTLY the
// loop start whatever the true position, and the true sub-block phase only turns up in the block
// after that. Measured at 130 BPM: 3.998222, then 0.000000, then 0.009750 - where a continuous
// reading would have given 4.009778 and 4.021334.
//
// So a generator that trusts the reported position across a wrap places every following tick up to
// a block early - 5.3 ms at 130 BPM, and systematic rather than random, which is exactly the defect
// this tool exists to find in other people's equipment.
//
// The handling: detect that the position went backwards while cycling, work out from the PREVIOUS
// block where the wrap really fell, and carry on counting ticks from there rather than from what
// the host claims.
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
    if ((clock->destination < 0) || (tempo <= 0.0) || (sampleRate <= 0.0) || (blockFrames == 0)) {
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
            ms_log_line("transport: STOP at ppq %.4f", ppq);
        }
        // Whether clock should keep running while stopped is still an open decision (some gear
        // wants it, some only while running), so for now nothing further is sent and the position is
        // forgotten - a restart must not think it is continuing.
        clock->havePrev = false;
        clock->running  = false;
        return;
    }
    // START OR CONTINUE, on the edge into playing. The distinction is not cosmetic: Start means "from
    // the beginning" and a device that receives it jumps to bar 1 whatever the DAW's playhead says.
    // Anywhere else has to be Continue, and Continue is meaningless without a Song Position Pointer
    // first to say where.
    bool restarted = !clock->running;

    if (restarted) {
        int beat16 = (int)llround(ppq * 4.0);   // sixteenths since the start of the song

        if (beat16 <= 0) {
            uint8_t byte = MIDI_START;

            ms_midi_send_at(clock->destination, &byte, 1, blockHostTime);
            clock->startsSent++;
            ms_log_line("transport: START at ppq %.4f", ppq);
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
            ms_log_line("transport: SPP %d + CONTINUE at ppq %.4f", beat16, ppq);
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

    // A TICK ON THE DOWNBEAT ITSELF, at the same instant as the Start, rather than one twenty-fourth
    // of a beat afterwards.
    //
    // With phase left at zero the first tick falls a whole tick after the transport message - 19.2 ms
    // at 130 BPM. That is defensible by the standard, and it is also an ambiguity the latency
    // measurement cannot afford: gear differs on whether step 1 fires on the Start byte or on the
    // first clock after it, and the two readings would differ by exactly that 19.2 ms with nothing
    // to say which was right. Placing the first tick ON the start moment collapses the two answers
    // into one.
    if (restarted) {
        clock->phase          = tickPpq;
        clock->advanceApplied = 0.0;
    }

    // THE COMPENSATION, folded into the phase - see the long note in msClock.h for why it cannot be
    // a subtraction from the timestamp.
    //
    // Tracked incrementally rather than reapplied, because the advance is a fixed number of
    // MILLISECONDS and therefore a changing number of quarter notes: a tempo change has to move the
    // phase by the difference, and reapplying the whole thing every block would move it by the whole
    // thing every block.
    if (clock->compensationMs != 0.0) {
        double advanceQn = ((clock->compensationMs + MS_LOOKAHEAD_MS) / 1000.0) * (tempo / 60.0);
        double delta     = advanceQn - clock->advanceApplied;

        clock->advanceApplied = advanceQn;

        // WRAPPED INTO ONE TICK, which loses nothing: a periodic stream shifted by a whole number of
        // tick periods is the same stream. Keeping phase inside [0, tickPpq) is also what guarantees
        // the in-block offset below can never come out negative.
        clock->phase          = fmod(clock->phase + delta, tickPpq);

        if (clock->phase < 0.0) {
            clock->phase += tickPpq;
        }
    }
    (void)cycleStartPpq;

    if (clock->havePrev) {
        double expected = clock->lastPpq + blockPpq;

        // A WRAP, not a seek: the position went backwards AND the host says it is cycling. A seek
        // also goes backwards, but there the tick phase genuinely should restart, so only the cycle
        // case needs handling.
        if (cycleActive && (ppq < clock->lastPpq) && (expected > cycleEndPpq)) {
            clock->wrapsSeen++;
        }
    }
    // A PHASE ACCUMULATOR, NOT A SEARCH FOR TICK POSITIONS INSIDE [start, end).
    //
    // The obvious implementation - find every multiple of 1/24 that falls in this block's musical
    // span - has a fault that only shows at a loop boundary, and it showed here as an outlier every
    // 96 ticks: exactly one bar of 4/4 at 24 PPQN. The tick at the loop END and the tick at the loop
    // START are the SAME musical moment, so a range search emits both and the pair arrives about
    // 2 ms apart instead of 19.2. Measured before this change: RMS 1.71 ms, worst -18 ms, with the
    // outliers landing 96, 96, 96 ticks apart - which is what said it was structural rather than
    // noise.
    //
    // Accumulating phase cannot duplicate a boundary tick, because it never revisits one. It simply
    // carries however much of a tick was left over into the next block, wrap or no wrap.
    //
    // THE ASSUMPTION, worth stating: the loop length is a whole number of ticks. That holds for any
    // bar- or beat-aligned loop, which is every loop anyone sets in practice. A loop of some
    // arbitrary fractional length would put the grid slightly out after each wrap.
    double distance = tickPpq - clock->phase;    // how far into this block the next tick falls

    while (distance < blockPpq) {
        // WHERE IN THE BLOCK, converted to a moment on the wall clock, plus the fixed lookahead -
        // see MS_LOOKAHEAD_MS. Handing CoreMIDI the future time is the whole reason the jitter is
        // microseconds rather than milliseconds.
        double   offsetNs = ((distance / blockPpq) * ((double)blockFrames / sampleRate) * 1.0e9)
                            + (MS_LOOKAHEAD_MS * 1.0e6);
        uint64_t when     = blockHostTime + AudioConvertNanosToHostTime((uint64_t)offsetNs);
        uint8_t  byte     = MIDI_CLOCK;

        ms_midi_send_at(clock->destination, &byte, 1, when);

        // READ AFTER the send, not before: what is wanted is how much future was left once the tick
        // was actually in CoreMIDI's hands, and the send itself takes time.
        ms_stats_tick(clock->stats, when, AudioGetCurrentHostTime());
        // COUNTED WITHIN THE RUN, not since the plug-in loaded. The detector takes every Nth tick as
        // a calibration position, and that N has to be measured from the downbeat the transport
        // started on or the grid lands between the drum machine's steps.
        ms_detect_tick(clock->detect, clock->ticksInRun, when);
        clock->ticksInRun++;
        clock->ticksSent++;
        distance += tickPpq;
    }
    clock->phase     = fmod(clock->phase + blockPpq, tickPpq);

    clock->lastPpq   = ppq;
    clock->lastTempo = tempo;
    clock->havePrev  = true;
    clock->running   = true;
}
