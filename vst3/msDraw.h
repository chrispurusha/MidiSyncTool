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
// the graph off the bottom of the window.
//
// THE GRAPH NO LONGER HANGS OFF THIS NUMBER (2026-09-03). It was anchored to the canvas bottom so a
// row added above could never push it off the edge; what that actually did was leave the sections'
// own variation - a clock source chosen or not, a held run, the dropout and split rows, some 130
// units between them - on screen as a hole under the latency breakdown. The graph now follows the
// content and spends the remainder on its own height, so this only has to be big enough for the
// TALLEST arrangement plus a minimum trace: 758 units of content, then 26 + 20 to the graph's top
// and MS_GRAPH_MIN_H + caption + margin below it. Too tall costs a taller trace, not a gap.
#define MS_CANVAS_H    (890.0)

// The trace's own geometry. MIN is what guarantees a crowded panel collides visibly instead of
// drawing off the edge; MAX stops a short arrangement - clock-only with no source chosen and
// nothing measured - handing a third of the panel to a scrolling line.
#define MS_GRAPH_MIN_H       (70.0)
#define MS_GRAPH_MAX_H       (150.0)
#define MS_GRAPH_CAPTION_H   (14.0)
#define MS_GRAPH_MARGIN_H    (14.0)

// A click landed on a control the HOST must be told about, because these are VST3 parameters and
// changing one behind the host's back would leave its automation and its saved state wrong.
typedef enum {
    eMsEditNone = 0,
    eMsEditMidiDest,
    eMsEditAudioSource,
    eMsEditCompensate,
    eMsEditMode,
    eMsEditClockSource,
} tMsEdit;

typedef struct {
    tMsEdit which;
    double  normalized;    // already normalised for the parameter
} tMsEditRequest;

// WHAT THE DETECTOR LISTENS TO. The host owns the audio device - Live decides which interface input
// reaches the track - so the choice a plug-in can actually offer is which of the channels it was
// handed to analyse, not which device to open.
#define MS_AUDIO_SOURCES    (3)          // left, right, sum

// WHAT THE PLUG-IN IS DOING, and therefore which of the panel's figures mean anything.
//
// THE ORDER IS PART OF THE FILE FORMAT AND MUST NOT CHANGE. This replaced a two-state Monitor
// toggle on the same parameter id, which wrote exactly 0.0 or 1.0 into the saved state - so with
// Measure at 0.0 and Monitor at 1.0 every set saved by the older build still opens in the mode it
// was saved in, with no migration branch anywhere. Insert a fourth mode in the MIDDLE, never at
// either end, or that stops being true.
typedef enum {
    eMsModeMeasure = 0,    // generate the clock AND measure what comes back
    eMsModeClockOnly,      // generate the clock, listen to nothing - the everyday setting
    eMsModeMonitor,        // send nothing at all, and fit a grid to the audio
    eMsModeCount
} tMsMode;

// ONE definition of the mapping, shared by the panel and the processor. Two would be one too many:
// the saved-state compatibility above rests on the exact normalised values, and a second rounding
// rule that disagreed by half a step would break it silently.
static inline tMsMode ms_mode_from_normalized(double normalized) {
    int index = (int)((normalized * (double)(eMsModeCount - 1)) + 0.5);

    return (tMsMode)((index < 0) ? 0 : ((index >= eMsModeCount) ? (eMsModeCount - 1) : index));
}

static inline double ms_mode_normalized(tMsMode mode) {
    return (double)mode / (double)(eMsModeCount - 1);
}

static inline const char * ms_mode_label(tMsMode mode) {
    static const char * names[eMsModeCount] = {
        "Generate + measure", "Generate clock only", "MONITOR - listening, sending nothing"
    };
    int index = (int)mode;

    return names[(index < 0) ? 0 : ((index >= (int)eMsModeCount) ? ((int)eMsModeCount - 1) : index)];
}

void ms_draw_init(void);
void ms_draw_set_status_slot(int slot);
void ms_draw_frame(int pixelWidth, int pixelHeight);

// Hit test in LOGICAL units. True when something was hit; request says what the host must be told.
bool ms_draw_click(double x, double y, tMsEditRequest * request);

// Where the pointer is, in canvas coordinates, so an open drop-down can highlight under it.
void ms_draw_set_mouse(double x, double y);
bool ms_draw_menu_active(void);

// The host's current values, so the panel draws what the host believes rather than its own idea.
void ms_draw_set_values(double midiDest, double audioSource, double compensate, double mode,
                        double clockSource);

#ifdef __cplusplus
}
#endif

#endif // __MS_DRAW_H__
