/*
 * MidiSyncTool - the editor panel's contents.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msDraw.h.md - "// notes §k" refers there.

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
// notes §1
#define MS_CANVAS_H    (830.0)

// The trace's own geometry. MIN is what guarantees a crowded panel collides visibly instead of
// drawing off the edge; MAX stops a short arrangement - clock-only with no source chosen and
// nothing measured - handing a third of the panel to a scrolling line.
#define MS_GRAPH_MIN_H       (70.0)
#define MS_GRAPH_MAX_H       (150.0)
#define MS_GRAPH_CAPTION_H   (14.0)
#define MS_GRAPH_MARGIN_H    (14.0)

// notes §2
typedef enum {
    eMsEditNone = 0,
    eMsEditMidiDest,
    eMsEditAudioSource,
    eMsEditCompensate,
    eMsEditMode,
    eMsEditClockSource,
    eMsEditClearStats,
} tMsEdit;

typedef struct {
    tMsEdit which;
    double  normalized;    // already normalised for the parameter
} tMsEditRequest;

// WHAT THE DETECTOR LISTENS TO. The host owns the audio device - Live decides which interface input
// reaches the track - so the choice a plug-in can actually offer is which of the channels it was
// handed to analyse, not which device to open.
#define MS_AUDIO_SOURCES    (3)          // left, right, sum

// notes §3
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
