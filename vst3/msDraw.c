/*
 * MidiSyncTool - the editor panel's contents.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

// THIS DRAWS THROUGH SYNTHLIB'S RENDERER, not through AppKit - render_text(), render_rectangle()
// and draw_button(), the same calls the three sibling applications draw with. GenBridge's gbDraw.c
// is the model and the two should stay recognisably related.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "msDraw.h"
#include "msStatus.h"

#include "synthlibDefs.h"
#include "synthlibGlobals.h"
#include "geometry.h"
#include "synthlibHost.h"
#include "synthlibTypes.h"
#include "utilsGraphics.h"
#include "contextMenu.h"
#include "msMidi.h"
#include "msStats.h"

#define FONT_PATH            "/System/Library/Fonts/Supplemental/Arial.ttf"
#define FONT_PRELOAD_SIZE    (16.0)

#define TEXT_H               (12.0)
#define STAT_VALUE_DX        (150.0)

// The three text tiers, taken from GenBridge where the contrast against RGB_BACKGROUND_GREY was
// actually measured: figures at 0.70, captions at 0.60 (~3.3:1, still reading as secondary), and
// headings brighter again.
#define MS_HEADING         {0.86, 0.86, 0.90}
#define MS_CAPTION_GREY    {0.60, 0.60, 0.63}
#define MS_FIGURE_GREY     {0.78, 0.78, 0.81}

// A FOURTH TIER, BELOW ALL THREE: A FIGURE THAT IS NOT LIVE.
//
// Not every number on this panel means something in every mode. In clock-only nothing is listened
// for, so the measured figures are the last run's; in monitor nothing is sent, so there is no
// reference and the latency breakdown has nothing to break down. Those figures are KEPT rather than
// blanked - the compensation in force was derived from them, and a value in force should have its
// provenance on screen - but a held number drawn like a live one is how it gets written down an
// hour later as this session's measurement.
//
// Both tiers drop together and keep their usual relationship (the figure still brighter than its
// caption), so an inactive block reads as a dimmed block rather than as a caption with a broken
// number beside it. The contrast is deliberately poor - about 2:1 - because "readable, but not
// current" is exactly the statement.
#define MS_HELD_CAPTION    {0.42, 0.42, 0.45}
#define MS_HELD_FIGURE     {0.50, 0.50, 0.53}

// COLOUR IS NEVER THE ONLY CUE. Two greys a tier apart is a weak signal on a poor monitor and no
// signal at all to some viewers, so every held section says so in words as well - the heading takes
// a suffix, and a line underneath names the mode that would make it live again.
#define MS_HELD_SUFFIX     "   -   HELD FROM THE LAST RUN"

static int       gStatusSlot = -1;
static bool      gFontReady;
static tCoord    gMouse;

// The host's values, pushed in before every frame.
static double    gMidiDest;
static double    gAudioSource;
static double    gCompensate;

// What a menu selection produced, picked up by ms_draw_click() on the click that made it. The action
// callback carries only an index, so the menu that is open has to be recorded alongside it - the
// "keep it in your own app-local struct" arrangement contextMenu.h describes.
static tMsEdit   gMenuFor    = eMsEditNone;
static int       gMenuChoice = -1;

// Labels must outlive the click that opens the menu: tMenuItem holds a const char *, it does not
// copy. One buffer per slot, filled when the menu is built.
#define MS_MENU_MAX      (MS_MIDI_MAX_DEST + 2)
#define MS_MENU_LABEL    (64)
#define MS_MENU_BG       {0.22, 0.22, 0.25}

static char      gMenuLabels[MS_MENU_MAX][MS_MENU_LABEL];
static tMenuItem gMenuItems[MS_MENU_MAX];

static void ms_menu_action(int index) {
    gMenuChoice = index;
}

// ---- the control rows, in canvas units --------------------------------------------------------
//
// Laid out from one origin so a change to the block moves everything together, rather than a set of
// literals per row that drift apart the first time a row is inserted - which has now happened
// twice, for the mode and for the clock input, and cost nothing either time.
#define ROW_X          (28.0)
#define ROW_VALUE_X    (178.0)
#define ROW_VALUE_W    (330.0)
#define ROW_H          (20.0)
#define ROW_GAP        (24.0)
#define ROW_ARROW_W    (24.0)
#define MS_ROWS        (5)

// A VALUE BOX IS ONLY AS WIDE AS ITS CONTENT NEEDS. A port name or the mode sentence wants the full
// width; a compensation reading is at most "100.0 ms", and giving that a 330 px box put its two
// arrows a third of the panel apart - with the right-hand one standing directly over the mode toggle
// below and reading as though it belonged to that row rather than this one.
#define ROW_STEP_W     (132.0)

static const double gRowWidth[MS_ROWS] = {ROW_VALUE_W, ROW_VALUE_W, ROW_STEP_W, ROW_VALUE_W,
                                          ROW_VALUE_W};

static double row_w(int row) {
    return ((row < 0) || (row >= MS_ROWS)) ? ROW_VALUE_W : gRowWidth[row];
}

// draw_button() DRAWS A BOX 2 * DRAW_BUTTON_MARGIN WIDER AND TALLER than the rect handed to it, from
// the same origin - see draw_button_bounds() in utilsGraphics.c. So a button given a whole row's
// rect covers the 4 px ROW_GAP leaves between rows and runs into the one underneath. Insetting here
// makes the drawn box exactly the rect the rest of the layout - and the hit test - believes in.
static tRectangle button_face(tRectangle box) {
    box.size.w -= (2.0 * DRAW_BUTTON_MARGIN);
    box.size.h -= (2.0 * DRAW_BUTTON_MARGIN);
    return box;
}

static double    gMode       = 0.0; // normalised tMsMode - see ms_mode_from_normalized()
static double    gClockSource = 0.0; // normalised MIDI SOURCE slot, 0 = none
static double    gControlTop = 0.0; // set by the frame, since it follows the sections above it

static tRectangle row_value(int row) {
    return (tRectangle){{
                            ROW_VALUE_X, gControlTop + ((double)row * ROW_GAP)
                        },
                        {
                            row_w(row), ROW_H
                        }
    };
}

static tRectangle row_prev(int row) {
    return (tRectangle){{
                            ROW_VALUE_X, gControlTop + ((double)row * ROW_GAP)
                        }, {
                            ROW_ARROW_W, ROW_H
                        }
    };
}

static tRectangle row_next(int row) {
    return (tRectangle){{
                            ROW_VALUE_X + row_w(row) - ROW_ARROW_W,
                            gControlTop + ((double)row * ROW_GAP)
                        }, {
                            ROW_ARROW_W, ROW_H
                        }
    };
}

static bool hit(tRectangle box, double x, double y) {
    return (x >= box.coord.x) && (x <= (box.coord.x + box.size.w))
           && (y >= box.coord.y) && (y <= (box.coord.y + box.size.h));
}

// SLOT 0 IS NONE. A plug-in that picks a destination on your behalf ends up driving hardware nobody
// asked it to, so the list always starts with an explicit refusal to.
static int dest_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_DEST + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_DEST) ? MS_MIDI_MAX_DEST : slot);
}

static void dest_label(int slot, char * out, unsigned long len) {
    if (slot <= 0) {
        snprintf(out, len, "none");
    } else if ((slot - 1) < ms_midi_count()) {
        ms_midi_name(slot - 1, out, len);
    } else {
        snprintf(out, len, "-");
    }
}

// SOURCES ARE A DIFFERENT LIST OF A DIFFERENT LENGTH from destinations, and the slot arithmetic has
// to use its own maximum or a saved setup lands on the wrong port.
static int source_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_SOURCE + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_SOURCE) ? MS_MIDI_MAX_SOURCE : slot);
}

static void source_label(int slot, char * out, unsigned long len) {
    if (slot <= 0) {
        snprintf(out, len, "none");
    } else if ((slot - 1) < ms_midi_source_count()) {
        ms_midi_source_name(slot - 1, out, len);
    } else {
        snprintf(out, len, "-");
    }
}

static const char * audio_source_label(double normalized) {
    static const char * names[MS_AUDIO_SOURCES] = {"left", "right", "left + right"};
    int                 index                   = (int)(normalized * (double)(MS_AUDIO_SOURCES - 1) + 0.5);

    return names[(index < 0) ? 0 : ((index >= MS_AUDIO_SOURCES) ? (MS_AUDIO_SOURCES - 1) : index)];
}

static void open_menu(tMsEdit which, int count, tRectangle anchor) {
    if (count > (MS_MENU_MAX - 1)) {
        count = MS_MENU_MAX - 1;
    }

    for (int i = 0; i < count; i++) {
        if (which == eMsEditMidiDest) {
            dest_label(i, gMenuLabels[i], sizeof(gMenuLabels[i]));
        } else if (which == eMsEditClockSource) {
            source_label(i, gMenuLabels[i], sizeof(gMenuLabels[i]));
        } else if (which == eMsEditMode) {
            snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%s", ms_mode_label((tMsMode)i));
        } else {
            snprintf(gMenuLabels[i], sizeof(gMenuLabels[i]), "%s",
                     audio_source_label((double)i / (double)(MS_AUDIO_SOURCES - 1)));
        }
        gMenuItems[i].label            = gMenuLabels[i];
        gMenuItems[i].colour           = (tRgb)MS_MENU_BG;
        gMenuItems[i].action           = ms_menu_action;
        gMenuItems[i].param            = (uint32_t)i;
        gMenuItems[i].subMenu          = NULL;
        gMenuItems[i].subMenuColumns   = 0;
        gMenuItems[i].subMenuCellWidth = 0.0;
    }

    gMenuItems[count].label  = NULL;
    gMenuItems[count].action = NULL;

    // 22 px a row. SynthLib scrolls the menu itself once it will not fit, which is what makes an
    // unbounded destination list safe - a studio's port count is not predictable.
    //
    // A CELL WIDTH OF ZERO MEANS "measure the labels", which is what the mode list needs: its
    // entries are sentences rather than names, and the fixed 240 the other two use would clip the
    // longest of them at exactly the point where it stops being a warning.
    gMenuFor                 = which;
    gMenuChoice              = -1;
    open_context_menu(below_rect(anchor), gMenuItems, 1, (which == eMsEditMode) ? 0.0 : 240.0);
}

void ms_draw_set_mouse(double x, double y) {
    gMouse.x = x;
    gMouse.y = y;
}

bool ms_draw_menu_active(void) {
    return gContextMenu.active;
}

void ms_draw_set_values(double midiDest, double audioSource, double compensate, double mode,
                        double clockSource) {
    gMidiDest    = midiDest;
    gAudioSource = audioSource;
    gCompensate  = compensate;
    gMode        = mode;
    gClockSource = clockSource;
}

static void ms_mouse_coord(tCoord * coord) {
    if (coord != NULL) {
        // contextMenu.c reaches back for the pointer position through this rather than declaring its
        // own extern - see synthlibHost.h.
        *coord = gMouse;
    }
}

void ms_draw_init(void) {
    synthlib_host_init((tSynthLibHost){.mouseCoord = ms_mouse_coord, .pointerCaptured = NULL});
    gFontReady = preload_glyph_textures(FONT_PATH, FONT_PRELOAD_SIZE);
}

void ms_draw_set_status_slot(int slot) {
    gStatusSlot = slot;
}

// The largest compensation the panel will dial in. Well past anything a drum machine has shown -
// the Tempest measured 21.9 ms round trip - and it must match the processor's own scale or the
// number shown and the number applied are different things.
#define MS_COMPENSATE_MAX    (100.0)

static void heading_at(double x, double y, const char * text, bool live) {
    set_rgb_colour(live ? (tRgb)MS_HEADING : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 13.0}}, text);
}

static void heading(double x, double y, const char * text) {
    heading_at(x, y, text, true);
}

// A LABELLED CONTROL: caption, a value box that opens a drop-down, and for a continuous value a
// pair of arrows instead. The arrows exist only where a list would be meaningless - GenBridge's rule
// that a drop-down replaces the steppers rather than sitting beside them.
//
// A ROW THAT HAS NO EFFECT IN THE CURRENT MODE IS DIMMED RATHER THAN HIDDEN, and it still works.
// The port in monitor mode and the analyse channel in clock-only are both REMEMBERED settings that
// nothing is currently acting on: removing them would lose the reading of what will be used again
// the moment the mode changes back, and leaving them at full strength says they are in force when
// they are not. Dimming says exactly the true thing - set, but not doing anything right now - and
// it stays clickable so it can be set up before switching modes.
static void control_row(int row, const char * name, const char * value, bool stepped, bool live) {
    tRectangle box   = row_value(row);
    double     textX = box.coord.x + 8.0;

    set_rgb_colour(live ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{ROW_X, box.coord.y + 5.0}, {0.0, TEXT_H}}, name);

    set_rgb_colour(live ? (tRgb){0.16, 0.16, 0.18} : (tRgb){0.13, 0.13, 0.15});
    render_rectangle(mainArea, box);

    if (stepped) {
        tRgb arrow = live ? (tRgb){0.30, 0.30, 0.33} : (tRgb){0.20, 0.20, 0.23};

        draw_button(mainArea, button_face(row_prev(row)), "<", arrow);
        draw_button(mainArea, button_face(row_next(row)), ">", arrow);
        textX = box.coord.x + ROW_ARROW_W + 10.0;
    }
    set_rgb_colour(live ? (tRgb){0.92, 0.92, 0.94} : (tRgb)MS_HELD_FIGURE);
    render_text(mainArea, (tRectangle){{textX, box.coord.y + 5.0}, {0.0, TEXT_H}}, value);
}

// THE MODE ROW: a drop-down like the others, but with the value box TINTED BY THE MODE.
//
// It was a two-state toggle, and the reason it did not simply become a plain control_row() is the
// reason it was a coloured toggle in the first place: a mode that silently changes what every figure
// below it MEANS has to be unmissable, not a word in a box the same colour as everything else. So
// the drop-down is the mechanism and the colour is retained on top of it - a click opens the list
// rather than advancing to the next state, which is what a third mode requires.
//
// NOT draw_button(), WHICH SIZES ITS LABEL OFF THE RECT'S HEIGHT - see internal_render_text(). A
// 20 px row therefore meant a 20 px font, and this row's sentence ran clean off the end of its box
// and off the panel. Every other row's value text is TEXT_H, so this is drawn the same way: the box
// painted, the label over it.
static void mode_row(int row, const char * name, tMsMode mode) {
    // AMBER FOR MONITOR, because it is the mode that stops the clock and a rig can go silent on it -
    // the loudest state deserves the loudest colour. Blue for clock-only: a real distinction from
    // the neutral measuring state, but a calm one, since it is the everyday setting.
    static const tRgb tint[eMsModeCount] = {
        {0.24, 0.24, 0.27}, {0.16, 0.26, 0.38}, {0.55, 0.36, 0.12}
    };
    tRectangle box   = row_value(row);
    int        index = (int)mode;

    if ((index < 0) || (index >= (int)eMsModeCount)) {
        index = 0;
    }
    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{ROW_X, box.coord.y + 5.0}, {0.0, TEXT_H}}, name);

    set_rgb_colour(tint[index]);
    render_rectangle(mainArea, box);

    set_rgb_colour((tRgb){0.95, 0.95, 0.97});
    render_text(mainArea, (tRectangle){{box.coord.x + 8.0, box.coord.y + 5.0}, {0.0, TEXT_H}},
                ms_mode_label(mode));
}

static void stat_at(double x, double y, const char * name, const char * value, bool live) {
    set_rgb_colour(live ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 11.0}}, name);

    set_rgb_colour(live ? (tRgb)MS_FIGURE_GREY : (tRgb)MS_HELD_FIGURE);
    render_text(mainArea, (tRectangle){{x + STAT_VALUE_DX, y}, {0.0, 11.0}}, value);
}

static void stat(double x, double y, const char * name, const char * value) {
    stat_at(x, y, name, value, true);
}

// The sentence under a held heading that names the way back to live figures. Its own call rather
// than a stat() with an empty caption, because it is an instruction and not a reading.
static void note(double x, double y, const char * text) {
    set_rgb_colour((tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 10.0}}, text);
}

// A TERM IN THE LATENCY BREAKDOWN, drawn as a proportional bar as well as a number.
//
// The bar is what makes a breakdown worth having rather than a list: it shows at a glance which
// term dominates, and an unknown term shows as an empty outline rather than as zero. Zero and
// "not measured" are completely different statements and a panel that renders them the same way is
// lying by omission.
// A HELD TERM KEEPS ITS BAR but drains the colour out of it, which is the one place a bar is better
// than a number at saying this: the proportions stay readable at a glance - which term dominated is
// exactly what a held breakdown is still good for - while the drab fill says none of it is being
// re-measured. A bar left in its live colour is the most confidently live-looking thing on the panel.
static void term(double x, double y, double width, const char * name,
                 double ms, double total, bool known, tRgb colour, bool live) {
    char   buffer[64];

    set_rgb_colour(live ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 11.0}}, name);

    if (known) {
        snprintf(buffer, sizeof(buffer), "%.2f ms", ms);
    } else {
        snprintf(buffer, sizeof(buffer), "not measured");
    }
    set_rgb_colour(live ? (tRgb)MS_FIGURE_GREY : (tRgb)MS_HELD_FIGURE);
    render_text(mainArea, (tRectangle){{x + STAT_VALUE_DX, y}, {0.0, 11.0}}, buffer);

    double barX = x + STAT_VALUE_DX + 78.0;
    double barW = width - (barX - x);

    set_rgb_colour((tRgb){0.14, 0.14, 0.16});
    render_rectangle(mainArea, (tRectangle){{barX, y}, {barW, 9.0}});

    if (known && (total > 0.0) && (ms > 0.0)) {
        double fraction = ms / total;

        if (fraction > 1.0) {
            fraction = 1.0;
        }
        // Halfway to the background rather than a fixed grey, so the terms stay distinguishable from
        // one another while every one of them recedes.
        set_rgb_colour(live ? colour
                            : (tRgb){(colour.red + 0.14) * 0.5,
                                     (colour.green + 0.14) * 0.5,
                                     (colour.blue + 0.16) * 0.5});
        render_rectangle(mainArea, (tRectangle){{barX, y}, {barW * fraction, 9.0}});
    }
}

// THE SCROLLING TIMING GRAPH the concept asks for, in DEVIATION from the mean rather than absolute
// latency. The absolute figure is a constant tens of milliseconds tall and would flatten everything
// interesting into a straight line; what a user needs to see is the wobble around it.
static void graph(double x, double y, double w, double h, tMsStatus * status, bool live) {
    set_rgb_colour((tRgb){0.12, 0.12, 0.14});
    render_rectangle(mainArea, (tRectangle){{x, y}, {w, h}});

    if (status == NULL) {
        return;
    }
    int    write = atomic_load(&status->historyWrite);
    double mean  = atomic_load(&status->roundTripMeanMs);
    double span  = atomic_load(&status->roundTripPeakDevMs);

    // A floor on the scale, so an idle or very steady run does not magnify its own noise into a
    // dramatic-looking trace. Half a millisecond is below anything worth acting on.
    if (span < 0.5) {
        span = 0.5;
    }
    // The centre line - where a perfectly steady device would sit.
    set_rgb_colour((tRgb){0.28, 0.28, 0.32});
    render_rectangle(mainArea, (tRectangle){{x, y + (h / 2.0)}, {w, 1.0}});

    // THE TRACE IS THE MOST LIVE-LOOKING THING ON THE PANEL - it is a scrolling graph, and a viewer
    // reads motion into one whether or not it is moving. It stops on its own when nothing is being
    // measured (a point is written per detection, not per block), so a held trace is a frozen one
    // and looks exactly like a device that has become perfectly steady. The colour has to say which.
    set_rgb_colour(live ? (tRgb){0.35, 0.78, 0.42} : (tRgb){0.26, 0.36, 0.28});

    for (int i = 0; i < MS_STATUS_HISTORY; i++) {
        int    index     = ((write - MS_STATUS_HISTORY + i) + (2 * MS_STATUS_HISTORY)) % MS_STATUS_HISTORY;
        float  value     = atomic_load(&status->history[index]);

        if (value == 0.0f) {
            continue;
        }
        double deviation = ((double)value - mean) / span;    // -1 .. +1 at the peak deviation

        if (deviation > 1.0) {
            deviation = 1.0;
        }

        if (deviation < -1.0) {
            deviation = -1.0;
        }
        double barX      = x + ((double)i * (w / (double)MS_STATUS_HISTORY));
        double mid       = y + (h / 2.0);
        double top       = mid - (deviation * (h / 2.0));

        render_rectangle(mainArea,
                         (tRectangle){{barX, (top < mid) ? top : mid},
                                      {w / (double)MS_STATUS_HISTORY, fabs(top - mid) + 1.0}
                         });
    }
}

// The one-word verdict the concept asks for. The thresholds are stated here rather than buried:
// under a quarter of a millisecond is better than any hardware sequencer, and over two is audible
// as sloppiness on a fast pattern.
static const char * quality(double jitterMs, unsigned hits) {
    if (hits < 4) {
        return "-";
    }

    if (jitterMs < 0.25) {
        return "EXCELLENT";
    }

    if (jitterMs < 0.75) {
        return "GOOD";
    }

    if (jitterMs < 2.0) {
        return "FAIR";
    }
    return "POOR";
}

void ms_draw_frame(int pixelWidth, int pixelHeight) {
    tMsStatus * status = ms_status(gStatusSlot);
    char        buffer[192];

    if ((pixelWidth <= 0) || (pixelHeight <= 0)) {
        return;
    }
    render_backend_set_surface(pixelWidth, pixelHeight);
    set_render_width(pixelWidth);
    set_render_height(pixelHeight);

    gGlobalGuiScale = (double)pixelWidth / MS_CANVAS_W;

    render_backend_clear((tRgb)RGB_BACKGROUND_GREY);

    if (!gFontReady) {
        render_backend_flush();
        return;
    }
    set_rgb_colour((tRgb){0.95, 0.95, 0.97});
    render_text(mainArea, (tRectangle){{20.0, 18.0}, {0.0, 20.0}}, "MidiSyncTool");

    if (status == NULL) {
        set_rgb_colour((tRgb)MS_CAPTION_GREY);
        render_text(mainArea, (tRectangle){{20.0, 50.0}, {0.0, 11.0}},
                    "no processor connected");
        render_backend_flush();
        return;
    }
    double y = 56.0;

    // ---- what the host is doing ----
    heading(20.0, y, "MASTER");
    y          += 20.0;

    snprintf(buffer, sizeof(buffer), "%.3f BPM", atomic_load(&status->hostBpm));
    stat(28.0, y, "Host tempo", buffer);
    y          += 16.0;

    stat(28.0, y, "Transport", atomic_load(&status->playing) ? "RUNNING" : "STOPPED");
    y          += 16.0;

    snprintf(buffer, sizeof(buffer), "%.3f", atomic_load(&status->ppq));
    stat(28.0, y, "Position (beats)", buffer);
    y          += 26.0;

    // ---- what this plug-in is set to do ----
    //
    // NOT "MIDI OUTPUT" any more. Two of the five rows below are inputs - the channel the detector
    // analyses, and the port an incoming clock is measured on - and a heading naming only the
    // output was already half wrong before the clock input made it plainly so.
    heading(20.0, y, "SETUP");
    y          += 20.0;

    // ---- the controls -----------------------------------------------------------------------
    //
    // THESE HAVE TO BE VST3 PARAMETERS, not environment variables. A host launched from the Dock
    // inherits no shell, so every MST_* variable this was developed against is empty inside Live -
    // the clock had no destination and generated nothing while every other figure on this panel read
    // perfectly. The same trap is recorded against $G2_VST3_PATCH in G2-Edit's plug-in notes.
    gControlTop = y - 4.0;

    // WHAT THE MODE MAKES TRUE, decided once and then simply consulted. Every "is this figure live"
    // question on the panel below is answered from these three, so a mode's meaning is stated in one
    // place instead of being re-derived, differently, at a dozen call sites.
    tMsMode mode      = ms_mode_from_normalized(gMode);
    bool    monitor   = (mode == eMsModeMonitor);
    bool    sending   = (mode != eMsModeMonitor);       // is a clock going out at all
    bool    measuring = (mode == eMsModeMeasure);       // is anything being listened for

    char label[MS_MENU_LABEL];

    if (atomic_load(&status->waitingForDevice)) {
        snprintf(label, sizeof(label), "waiting for %s", status->waitingName);
    } else {
        dest_label(dest_slot(gMidiDest), label, sizeof(label));
    }
    // The port is REMEMBERED in monitor mode but nothing goes to it, and the analyse channel is
    // remembered in clock-only but nothing is read from it. Both stay set and settable; both say so.
    control_row(0, "Port", label, false, sending);

    control_row(1, "Analyse", audio_source_label(gAudioSource), false, monitor || measuring);

    snprintf(buffer, sizeof(buffer), "%.1f ms", gCompensate * MS_COMPENSATE_MAX);

    // COMPENSATION STAYS LIVE IN CLOCK-ONLY MODE and that is the whole reason the figures behind it
    // are preserved rather than cleared: it is a phase advance applied to the generated grid, so it
    // is in force whenever a clock is going out, measured or not. In monitor nothing is sent, so
    // there is nothing to advance.
    control_row(2, "Compensation", buffer, true, sending);

    mode_row(3, "Mode", mode);

    // THE CLOCK INPUT IS LIVE IN EVERY MODE, and that is deliberate rather than an oversight. It
    // measures someone else's clock arriving on a wire; nothing about generating, measuring or
    // monitoring changes whether that clock is there or what it is doing.
    source_label(source_slot(gClockSource), label, sizeof(label));
    control_row(4, "Clock in", label, false, true);

    y           = gControlTop + ((double)MS_ROWS * ROW_GAP) + 6.0;

    if (monitor) {
        // CLOCKS SENT IS ZERO BY DESIGN HERE and saying so is better than showing the zero, which
        // reads as a fault. The grid figure replaces it: it is what the plug-in has worked out the
        // external master is doing, and a plausible tempo there is the confirmation that the
        // detector is locked onto the pattern rather than onto noise.
        double bpm = atomic_load(&status->monitorBpm);

        if (bpm > 0.0) {
            snprintf(buffer, sizeof(buffer), "%.3f ms  (%.2f BPM)",
                     atomic_load(&status->monitorPeriodMs), bpm);
        } else {
            snprintf(buffer, sizeof(buffer), "fitting...");
        }
        stat(28.0, y, "Fitted grid", buffer);
        y += 16.0;
    } else {
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)atomic_load(&status->ticksSent));
        stat(28.0, y, "Clocks sent", buffer);
        y += 16.0;
    }

    // COMMIT MARGIN, which is the plug-in's own health and the one number that says whether the
    // schedule is comfortable. While it stays positive CoreMIDI holds each tick early and delivers
    // it on its own timer; the moment it goes negative, ticks bunch.
    //
    // THESE THREE DESCRIBE A CLOCK BEING GENERATED, so in monitor mode there is none to describe.
    // Worse than stale: ms_clock_process() returns at the top with no destination and the stats are
    // reset on the way in, so they read a PERFECT 0.00 ms margin and 0 late clocks - the panel's most
    // reassuring possible reading, produced by sending nothing at all.
    snprintf(buffer, sizeof(buffer), "%.2f ms  (worst %.2f)",
             atomic_load(&status->commitMarginMeanMs), atomic_load(&status->commitMarginMinMs));
    stat_at(28.0, y, "Commit margin", sending ? buffer : "-  (no clock is being sent)", sending);
    y          += 16.0;

    unsigned late    = (unsigned)atomic_load(&status->lateTicks);

    snprintf(buffer, sizeof(buffer), "%u", late);
    set_rgb_colour(sending ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{28.0, y}, {0.0, 11.0}}, "Late clocks");
    set_rgb_colour(!sending ? (tRgb)MS_HELD_FIGURE
                   : (late > 0) ? (tRgb){0.90, 0.35, 0.25}
                                : (tRgb)MS_FIGURE_GREY);
    render_text(mainArea, (tRectangle){{28.0 + STAT_VALUE_DX, y}, {0.0, 11.0}},
                sending ? buffer : "-");
    y          += 16.0;

    // THE PAIR, always together. The raw figure is the host's; the residual is what survives the
    // timebase model and is therefore what the output clock actually inherits. Showing only the
    // first would look like a fault the plug-in was not fixing; showing only the second would hide
    // how much work is being done.
    //
    // The raw figure is the ONE clock-side number that stays live in every mode: ms_stats_block() is
    // called before the clock and unconditionally, so the host's block delivery is measured whether
    // or not anything is being sent. The residual below it is the model's, and the model does not
    // run when the clock does not.
    // THE GAP COUNT BELONGS BESIDE THE FIGURE, not in a log. A gap is an interval the metric
    // REFUSED to measure - the host stopped being scheduled, which on this machine meant Ableton
    // going to the background - and without the count the same reading means two different things:
    // a steady host, or a host that stalled twice and had it quietly discounted.
    unsigned gaps    = (unsigned)atomic_load(&status->blockGaps);

    if (gaps > 0) {
        snprintf(buffer, sizeof(buffer), "%.3f ms RMS  (%u gap%s ignored)",
                 atomic_load(&status->blockPeriodRmsMs), gaps, (gaps == 1) ? "" : "s");
    } else {
        snprintf(buffer, sizeof(buffer), "%.3f ms RMS", atomic_load(&status->blockPeriodRmsMs));
    }
    stat(28.0, y, "Host block jitter", buffer);
    y          += 16.0;

    unsigned resyncs = (unsigned)atomic_load(&status->modelResyncs);

    if (resyncs > 0) {
        snprintf(buffer, sizeof(buffer), "%.3f ms RMS  (%u resync%s)",
                 atomic_load(&status->residualRmsMs), resyncs, (resyncs == 1) ? "" : "s");
    } else {
        snprintf(buffer, sizeof(buffer), "%.3f ms RMS", atomic_load(&status->residualRmsMs));
    }
    stat_at(28.0, y, "  after filtering", sending ? buffer : "-", sending);
    y          += 26.0;

    // ---- somebody else's clock, arriving ----
    //
    // MEASURED, NEVER ACTED ON. Nothing here steers the generated clock, and the section is kept
    // visually apart from the figures above for that reason: those describe a clock this plug-in
    // made against a timebase it owns, these describe one arriving on a wire.
    //
    // THE ppm ROW IS THE POINT OF THE WHOLE SECTION. The standing question has been "how much better
    // is our clock than the host's own?", and until now the only reference was a drum machine's
    // audio onsets - which carry the device, the desk and the A/D on top of whatever the wire did.
    // Point Live's Sync at IAC, point this at the same port, and both clocks are timed by the same
    // code against the same reference, on a path that measures at 0.013 ms rather than 0.132.
    bool     haveSource = (atomic_load(&status->haveClockSource) != 0);
    double   inBpm      = atomic_load(&status->clockInBpm);
    unsigned inFitted   = (unsigned)atomic_load(&status->clockInFitted);

    heading_at(20.0, y, "CLOCK INPUT", haveSource);
    y          += 20.0;

    if (!haveSource) {
        stat_at(28.0, y, "Incoming", "-  (no source chosen)", false);
        y += 16.0;
    } else if ((inBpm <= 0.0) || (inFitted < 8)) {
        // A COUNT RATHER THAN A NUMBER while the window fills, for the same reason the drift row
        // shows the window filling: a tempo fitted through six arrivals is describing the fit.
        snprintf(buffer, sizeof(buffer), "listening - %u clock%s so far",
                 (unsigned)atomic_load(&status->clockInClocks),
                 (atomic_load(&status->clockInClocks) == 1) ? "" : "s");
        stat(28.0, y, "Incoming", buffer);
        y += 16.0;
    } else {
        snprintf(buffer, sizeof(buffer), "%.3f BPM  (%.4f ms/clock)",
                 inBpm, atomic_load(&status->clockInPeriodMs));
        stat(28.0, y, "Incoming", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms RMS  (peak %.3f, %u fitted)",
                 atomic_load(&status->clockInJitterMs), atomic_load(&status->clockInPeakDevMs),
                 inFitted);
        stat(28.0, y, "Jitter", buffer);
        y += 16.0;

        // AGAINST THE HOST'S OWN TEMPO, in ppm, because a BPM difference in the fourth decimal
        // place is unreadable and the same difference as "+18 ppm" is not. Two free-running
        // crystals is the expected answer here, not a fault - this rig's own audio clock measures
        // about -20 ppm against mach time, and a figure in that region means the two agree.
        double hostBpm = atomic_load(&status->hostBpm);

        if (hostBpm > 0.0) {
            snprintf(buffer, sizeof(buffer), "%+.1f ppm  (host %.3f BPM)",
                     ((inBpm / hostBpm) - 1.0) * 1.0e6, hostBpm);
        } else {
            snprintf(buffer, sizeof(buffer), "-  (host reports no tempo)");
        }
        stat(28.0, y, "vs host", buffer);
        y += 16.0;
    }

    if (haveSource) {
        unsigned inGaps = (unsigned)atomic_load(&status->clockInGaps);

        // GAPS, ON THE SAME PRINCIPLE AS THE HOST'S. An interval too long to be a clock period means
        // the master stopped or the port dropped out; the window restarts rather than fitting a line
        // through the hole. Saying how often that happened is what stops a steady-looking figure
        // being mistaken for a steady clock.
        snprintf(buffer, sizeof(buffer), "%s, %u clock%s",
                 (atomic_load(&status->clockInRunning) != 0) ? "RUNNING" : "stopped",
                 (unsigned)atomic_load(&status->clockInClocks),
                 (atomic_load(&status->clockInClocks) == 1) ? "" : "s");
        stat(28.0, y, "Transport", buffer);
        y += 16.0;

        if (inGaps > 0) {
            snprintf(buffer, sizeof(buffer), "%u - the window restarted that often", inGaps);
            stat(28.0, y, "Dropouts", buffer);
            y += 16.0;
        }
    }
    y          += 10.0;

    // ---- what came back ----
    //
    // HELD MEANS THESE ARE REAL FIGURES FROM A RUN THAT HAS ENDED. The detector deliberately keeps
    // them when it goes quiet - see ms_detect_set_source() - because the compensation dialled in
    // above came out of them, and a value in force with no visible provenance is worse than a dim
    // number. Three cues carry it, only one of which is colour: the heading says HELD, a line under
    // it names the way back, and every figure drops a tier.
    unsigned hits    = (unsigned)atomic_load(&status->hits);
    double   jitter  = atomic_load(&status->roundTripJitterMs);
    bool     held    = (!measuring && !monitor && (hits > 0));

    heading_at(20.0, y, held ? "MEASURED DEVICE" MS_HELD_SUFFIX : "MEASURED DEVICE", !held);
    y          += 20.0;

    if (held) {
        note(28.0, y - 4.0, "nothing is being listened for - switch Mode to Generate + measure");
        y += 12.0;
    }

    if (hits == 0) {
        // NOT "waiting for audio" IN CLOCK-ONLY MODE, which would be a lie about what the plug-in is
        // doing: it is not waiting, it is not listening.
        stat_at(28.0, y, "Round trip",
                measuring ? "waiting for audio" : "-  (not measuring in this mode)", measuring);
        y += 16.0;
    } else if (monitor) {
        // NO ROUND TRIP IN MONITOR MODE, and a dash rather than the last value it held. A stale
        // latency left on screen beside live jitter figures is exactly the sort of thing that gets
        // written down as a measurement an hour later.
        stat(28.0, y, "Round trip", "-  (no reference: listening only)");
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", jitter);
        stat(28.0, y, "Jitter RMS", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", atomic_load(&status->roundTripPeakDevMs));
        stat(28.0, y, "Peak deviation", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%u onsets fitted",
                 (unsigned)atomic_load(&status->monitorOnsets));
        stat(28.0, y, "Window", buffer);
        y += 16.0;
    } else {
        snprintf(buffer, sizeof(buffer), "%.3f ms", atomic_load(&status->roundTripMeanMs));
        stat_at(28.0, y, "Round trip", buffer, !held);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", jitter);
        stat_at(28.0, y, "Jitter RMS", buffer, !held);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", atomic_load(&status->roundTripPeakDevMs));
        stat_at(28.0, y, "Peak deviation", buffer, !held);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f / %.3f ms",
                 atomic_load(&status->roundTripMinMs), atomic_load(&status->roundTripMaxMs));
        stat_at(28.0, y, "Min / max", buffer, !held);
        y += 16.0;
    }
    // THE COUNTS ARE THE PROVENANCE - how many hits the held figures rest on - so they are kept and
    // dimmed rather than dropped. They are also the reason clock-only mode exists: while it is on,
    // no expectation is registered and "missed" cannot climb against silence.
    snprintf(buffer, sizeof(buffer), "%u hit, %u missed, %u spurious",
             hits, (unsigned)atomic_load(&status->missed), (unsigned)atomic_load(&status->spurious));
    stat_at(28.0, y, "Detections", buffer, measuring || monitor);
    y += 16.0;

    // A NUMBER OR AN HONEST WAIT, never a figure that swings. Below the window length the reading is
    // dominated by whatever the scheduler last did rather than by any drift, so the panel shows the
    // window filling instead.
    if (atomic_load(&status->driftValid)) {
        snprintf(buffer, sizeof(buffer), "%+.1f ppm", atomic_load(&status->driftPpm));
    } else {
        snprintf(buffer, sizeof(buffer), "measuring - %.0f of %.0f s",
                 atomic_load(&status->driftSeconds), (double)MS_STATS_DRIFT_SECONDS);
    }
    stat(28.0, y, "Clock drift", buffer);
    y += 26.0;

    // ---- the breakdown ----
    //
    // WHAT IS KNOWN, WHAT IS MEASURED, AND WHAT IS NEITHER. The whole value of showing a breakdown
    // rather than one number is that it says which part a user can do something about, so each term
    // has to be honest about where it came from.
    // THE WHOLE BREAKDOWN IS DEAD IN MONITOR MODE, and it used to be the panel's most confident lie.
    // Every term is a share of `lead + roundTrip`, and in monitor there is no round trip - so the
    // schedule lead came out as 100 % of the total and drew a full-width bar, over a "Total musical
    // delay" of 10 ms that nothing on the rig was actually waiting. In clock-only it is HELD, like
    // the figures it is built from.
    bool   breakdownLive = measuring;

    heading_at(20.0, y, monitor ? "LATENCY BREAKDOWN   -   NOT APPLICABLE WHILE MONITORING"
               : (hits > 0) && !measuring ? "LATENCY BREAKDOWN" MS_HELD_SUFFIX
                                          : "LATENCY BREAKDOWN", breakdownLive);
    y += 20.0;

    double lead      = atomic_load(&status->scheduleLeadMs);
    double roundTrip = atomic_load(&status->roundTripMeanMs);
    double block     = atomic_load(&status->blockMs);
    double inputPath = atomic_load(&status->inputPathMs);
    double compensat = atomic_load(&status->compensationMs);
    double total     = lead + roundTrip;
    bool   haveTrip  = (hits > 0) && !monitor;
    bool   haveInput = (inputPath > 0.0);

    // The remainder, once every term that IS known has been taken out of the measured round trip.
    double known     = block + (haveInput ? inputPath : 0.0);
    double remainder = (roundTrip > known) ? (roundTrip - known) : 0.0;

    term(28.0, y, 510.0, "Schedule lead", lead, total, !monitor, (tRgb){0.35, 0.55, 0.85},
         breakdownLive);
    y += 15.0;
    term(28.0, y, 510.0, "Host buffer", block, total, (block > 0.0) && !monitor,
         (tRgb){0.40, 0.65, 0.60}, breakdownLive);
    y += 15.0;
    term(28.0, y, 510.0, "Interface A/D", inputPath, total, haveInput && !monitor,
         (tRgb){0.55, 0.45, 0.80}, breakdownLive);
    y += 15.0;
    // WHAT IS LEFT, labelled by what is still mixed into it. While the interface term is unmeasured
    // this is the device AND the converter, and calling it "device latency" would be the exact
    // overclaim this panel exists to avoid. One buffer comes out because that much is arithmetic:
    // the audio in a block was captured at least a buffer before the block was handed over.
    term(28.0, y, 510.0, haveInput ? "Device MIDI to audio" : "Device + interface",
         remainder, total, haveTrip, (tRgb){0.85, 0.55, 0.30}, breakdownLive);
    y += 19.0;

    snprintf(buffer, sizeof(buffer), "%.2f ms", total);
    stat_at(28.0, y, "Total musical delay", haveTrip ? buffer : "-", breakdownLive);
    y += 16.0;

    // COMPENSATION IS THE ONE LIVE LINE IN A HELD BREAKDOWN, because it is still being applied: a
    // phase advance on a clock that is still going out. That is exactly why it is worth keeping the
    // figures above it on screen at all.
    snprintf(buffer, sizeof(buffer), "%.2f ms", compensat);
    stat_at(28.0, y, "Compensation", (compensat != 0.0) ? buffer : "off", sending);
    y += 16.0;

    // What is left after compensation - the number that says whether the job is done.
    snprintf(buffer, sizeof(buffer), "%+.2f ms", total - compensat);
    stat_at(28.0, y, "Residual", haveTrip ? buffer : "-", breakdownLive);

    // ---- the verdict and the trace ----
    //
    // ANCHORED TO THE BOTTOM OF THE CANVAS, not to the running y. See MS_CANVAS_H.
    double       graphY   = MS_CANVAS_H - 96.0;
    double       verdictY = graphY - 20.0;

    // THE VERDICT LOSES ITS COLOUR WHEN IT IS HELD, and that matters more than any other figure on
    // the panel: a green EXCELLENT is the one thing here that a user reads without reading, and a
    // held one would be a green light on a measurement that stopped ten minutes ago. Dimmed to the
    // held tier with the word "(held)" beside it, it can only be read as what it is.
    bool         verdictLive = measuring || monitor;

    set_rgb_colour(verdictLive ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{20.0, verdictY}, {0.0, 12.0}}, "TIMING QUALITY");

    const char * verdict  = quality(jitter, hits);
    char         verdictText[64];

    snprintf(verdictText, sizeof(verdictText), "%s%s", verdict, verdictLive ? "" : "   (held)");

    set_rgb_colour(!verdictLive ? (tRgb)MS_HELD_FIGURE
                   : (hits < 4) ? (tRgb)MS_CAPTION_GREY
                   : (jitter < 0.75) ? (tRgb){0.40, 0.85, 0.50}
                   : (jitter < 2.0) ? (tRgb){0.90, 0.75, 0.25}
                   : (tRgb){0.90, 0.35, 0.25});
    render_text(mainArea, (tRectangle){{20.0 + STAT_VALUE_DX, verdictY}, {0.0, 12.0}}, verdictText);

    graph(28.0, graphY, 504.0, 70.0, status, verdictLive);

    set_rgb_colour(verdictLive ? (tRgb)MS_CAPTION_GREY : (tRgb)MS_HELD_CAPTION);
    render_text(mainArea, (tRectangle){{28.0, graphY + 74.0}, {0.0, 10.0}},
                verdictLive ? "deviation from the mean round trip"
                            : "deviation from the mean round trip - frozen, nothing is being measured");

    // LAST, so it draws over everything - and the hover update goes here rather than in the click
    // path because the highlight has to follow the pointer while no button is down.
    update_context_menu_hover();
    render_context_menu();

    render_backend_flush();
}

// Stepping a continuous parameter. Clamped rather than wrapped: an arrow that jumps from the top of
// the range back to the bottom looks like a glitch.
static double step_value(double normalized, double stepMs) {
    double ms = (normalized * MS_COMPENSATE_MAX) + stepMs;

    if (ms < 0.0) {
        ms = 0.0;
    }

    if (ms > MS_COMPENSATE_MAX) {
        ms = MS_COMPENSATE_MAX;
    }
    return ms / MS_COMPENSATE_MAX;
}

bool ms_draw_click(double x, double y, tMsEditRequest * request) {
    request->which = eMsEditNone;

    // THE MENU GETS FIRST REFUSAL, and a click anywhere while it is open belongs to it - either
    // choosing an item or dismissing it. Letting the click fall through to the rows underneath would
    // mean dismissing the menu and working a control in the same gesture.
    if (gContextMenu.active) {
        gMenuChoice = -1;
        handle_context_menu_click((tCoord){x, y});

        if ((gMenuChoice >= 0) && (gMenuFor != eMsEditNone)) {
            request->which = gMenuFor;

            if (gMenuFor == eMsEditMidiDest) {
                request->normalized = (double)gMenuChoice / (double)MS_MIDI_MAX_DEST;
            } else if (gMenuFor == eMsEditClockSource) {
                request->normalized = (double)gMenuChoice / (double)MS_MIDI_MAX_SOURCE;
            } else if (gMenuFor == eMsEditMode) {
                request->normalized = ms_mode_normalized((tMsMode)gMenuChoice);
            } else {
                request->normalized = (double)gMenuChoice / (double)(MS_AUDIO_SOURCES - 1);
            }
            gMenuChoice    = -1;
            gMenuFor       = eMsEditNone;
            return true;
        }
        return true;    // consumed, whether it chose something or dismissed
    }

    if (hit(row_value(0), x, y)) {
        // The whole destination list plus the None at the top of it.
        open_menu(eMsEditMidiDest, ms_midi_count() + 1, row_value(0));
        return true;
    }

    if (hit(row_value(1), x, y)) {
        open_menu(eMsEditAudioSource, MS_AUDIO_SOURCES, row_value(1));
        return true;
    }

    // A MILLISECOND A CLICK, which is the resolution the measurement is good to. Finer would be
    // false precision and coarser would not reach a device's figure.
    if (hit(row_prev(2), x, y)) {
        request->which      = eMsEditCompensate;
        request->normalized = step_value(gCompensate, -1.0);
        return true;
    }

    if (hit(row_next(2), x, y)) {
        request->which      = eMsEditCompensate;
        request->normalized = step_value(gCompensate, 1.0);
        return true;
    }

    // THE MODE. A drop-down now that there are three of them: cycling through on each click was
    // right for two states and is wrong for three, because reaching the one you want may take you
    // THROUGH the one you do not - and the one in the middle stops the audio analysis while the one
    // at the end stops the clock. A rig should not have to go silent on its way to another mode.
    if (hit(row_value(3), x, y)) {
        open_menu(eMsEditMode, (int)eMsModeCount, row_value(3));
        return true;
    }

    if (hit(row_value(4), x, y)) {
        // The whole source list plus the None at the top of it.
        open_menu(eMsEditClockSource, ms_midi_source_count() + 1, row_value(4));
        return true;
    }
    return false;
}
