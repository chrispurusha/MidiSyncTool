/*
 * MidiSyncTool - MIDI destination selection and scheduled sending.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msMidi.h.md - "// notes §k" refers there.

#ifndef __MS_MIDI_H__
#define __MS_MIDI_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// notes §1

#define MS_MIDI_MAX_DEST      (64)
#define MS_MIDI_MAX_SOURCE    (64)
#define MS_MIDI_NAME_LEN      (64)

// notes §2
void ms_midi_refresh(void);

// HAS THE MIDI SETUP CHANGED SINCE THE LAST REBUILD? A synth switched on after load used to be
// invisible for the life of the session: the client was created with no notify proc and nothing
// polled. CoreMIDI now says so, and this reports it.
bool ms_midi_setup_changed(void);

// Rebuild only if it has, and say whether it did. This is the call a UI makes - from a CLICK, which
// is a thread that may take CoreMIDI's locks, and never from a repaint.
bool ms_midi_refresh_if_changed(void);

int ms_midi_count(void);
void ms_midi_name(int index, char * out, unsigned long len);

// -1 when no destination of that name is present, which is the honest answer for a saved setup
// whose interface is unplugged.
int ms_midi_index_for_name(const char * name);

// notes §3
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

// notes §4
void ms_midi_set_offset_ms(double offsetMs);
double ms_midi_offset_ms(void);

#ifdef __cplusplus
}
#endif

#endif // __MS_MIDI_H__
