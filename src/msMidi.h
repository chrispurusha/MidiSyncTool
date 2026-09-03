/*
 * MidiSyncTool - MIDI destination selection and scheduled sending.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_MIDI_H__
#define __MS_MIDI_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LOCAL FOR NOW, with a view to moving into SynthLib later (CT's call). SynthLib already owns the
// send primitive in synthlibMidi.c, but that one stamps every packet 0 - "deliver now" - which is
// right for a note or a CC and is exactly what this tool must not do. Rather than change shared
// code that three other projects depend on while the shape of this is still settling, the
// timestamped version lives here until it has proved itself.
//
// The measurement that decided it: 200 clocks at 24 PPQN / 120 BPM through IAC, timed on arrival.
// Scheduled ahead gave 0.019 ms RMS with no drift; sent immediately from a sleeping loop gave
// 4.157 ms RMS and a 3.9 ms MEAN error. See Docs/findings.txt.

#define MS_MIDI_MAX_DEST      (64)
#define MS_MIDI_MAX_SOURCE    (64)
#define MS_MIDI_NAME_LEN      (64)

// Rebuild the cached destination AND source lists. Talking to CoreMIDI takes its locks, so this is
// NEVER called from drawing or from the audio thread - GenBridge learned that one the hard way, with
// an enumeration inside a 30 Hz repaint contending with the opens it was driving.
void ms_midi_refresh(void);

int ms_midi_count(void);
void ms_midi_name(int index, char * out, unsigned long len);

// -1 when no destination of that name is present, which is the honest answer for a saved setup
// whose interface is unplugged.
int ms_midi_index_for_name(const char * name);

// ---- LISTENING, which is a separate list from sending -----------------------------------------
//
// A port that can be sent to and a port that can be listened to are DIFFERENT ENDPOINTS with
// different indices, even when they carry the same name. IAC Driver Bus 1 is both, and it is the
// port this exists for: point Live's Sync at it, point this at it, and Live's own generated clock
// can be measured by the same code that measures ours.
int ms_midi_source_count(void);
void ms_midi_source_name(int index, char * out, unsigned long len);
int ms_midi_source_index_for_name(const char * name);

// EVERY BYTE OF INTEREST, ON THE CoreMIDI RECEIVE THREAD. Registered rather than called directly so
// this file keeps knowing nothing about what a clock means - it moves bytes and timestamps.
typedef void (*tMsMidiListener)(uint8_t status, uint64_t hostTime, void * user);

void ms_midi_set_listener(tMsMidiListener listener, void * user);

// Connect to one source, or -1 for none. Disconnects whatever was connected before, so there is
// never more than one master feeding the estimator - two would interleave into nonsense.
bool ms_midi_listen(int index);
int ms_midi_listening(void);

// THE POINT OF THIS FILE. hostTime is a mach host-clock value - AudioGetCurrentHostTime() plus
// however far ahead the event belongs. Pass 0 only for something genuinely immediate; a clock tick
// never is.
bool ms_midi_send_at(int index, const uint8_t * data, uint32_t length, uint64_t hostTime);

// LATENCY COMPENSATION, AND THE ONLY PLACE IT BELONGS.
//
// Measuring a device's latency is half the job; the other half is sending to it that much EARLIER,
// so its sound lands where the music says it should. Applied here rather than in the clock because
// it must apply to everything scheduled - ticks, transport and probe notes alike - and because a
// compensation that reached only some of them would put transport and clock out of step.
//
// NEGATIVE MEANS EARLIER, which is the sign a user expects from "the device is 12 ms late, so take
// 12 ms off". A positive value delays, which is occasionally what a slow-responding device in the
// other direction needs.
//
// THIS IS WHAT MS_LOOKAHEAD_MS IS FOR, and the connection is worth stating because the lookahead has
// looked unjustified in every measurement so far. An event cannot be sent earlier than the moment
// its block was picked up, so the lookahead is the headroom the compensation spends: compensating
// 12 ms with 10 ms of lookahead cannot work, and the commit-margin telemetry says so plainly by
// going negative. The lookahead is not overhead - it is the budget.
void ms_midi_set_offset_ms(double offsetMs);
double ms_midi_offset_ms(void);

#ifdef __cplusplus
}
#endif

#endif // __MS_MIDI_H__
