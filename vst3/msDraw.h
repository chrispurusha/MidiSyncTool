/*
 * MidiSyncTool - the editor panel's contents.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_DRAW_H__
#define __MS_DRAW_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// Drawn through SynthLib's renderer - the same render_text(), render_rectangle() and draw_button()
// the sibling applications use, so this panel cannot drift away from their look without the change
// being visible in all of them. GenBridge's editor is the model throughout.

// The logical canvas. Every coordinate is in these units and scales to whatever surface the host
// gives us, so the panel is the same shape at any window size.
#define MS_CANVAS_W    (560.0)
// TALL ENOUGH FOR WHAT IS ON IT. The content ran to about 740 units while this said 600, which put
// the graph off the bottom of the window - and the graph is now anchored to the canvas bottom rather
// than to the running y, so a row added above it can crowd the layout but can never push it off
// again. A collision is visible; a control drawn past the edge is not.
#define MS_CANVAS_H    (740.0)

// A click landed on a control the HOST must be told about, because these are VST3 parameters and
// changing one behind the host's back would leave its automation and its saved state wrong.
typedef enum {
    eMsEditNone = 0,
    eMsEditMidiDest,
    eMsEditAudioSource,
    eMsEditCompensate,
} tMsEdit;

typedef struct {
    tMsEdit which;
    double  normalized;    // already normalised for the parameter
} tMsEditRequest;

// WHAT THE DETECTOR LISTENS TO. The host owns the audio device - Live decides which interface input
// reaches the track - so the choice a plug-in can actually offer is which of the channels it was
// handed to analyse, not which device to open.
#define MS_AUDIO_SOURCES    (3)          // left, right, sum

void ms_draw_init(void);
void ms_draw_set_status_slot(int slot);
void ms_draw_frame(int pixelWidth, int pixelHeight);

// Hit test in LOGICAL units. True when something was hit; request says what the host must be told.
bool ms_draw_click(double x, double y, tMsEditRequest * request);

// Where the pointer is, in canvas coordinates, so an open drop-down can highlight under it.
void ms_draw_set_mouse(double x, double y);
bool ms_draw_menu_active(void);

// The host's current values, so the panel draws what the host believes rather than its own idea.
void ms_draw_set_values(double midiDest, double audioSource, double compensate);

#ifdef __cplusplus
}
#endif

#endif // __MS_DRAW_H__
