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

// ---- the three control rows, in canvas units --------------------------------------------------
//
// Laid out from one origin so a change to the block moves everything together, rather than three
// sets of literals that drift apart the first time a row is inserted.
#define ROW_X          (28.0)
#define ROW_VALUE_X    (178.0)
#define ROW_VALUE_W    (330.0)
#define ROW_H          (20.0)
#define ROW_GAP        (24.0)

static double    gControlTop = 0.0; // set by the frame, since it follows the sections above it

static tRectangle row_value(int row) {
    return (tRectangle){{
                            ROW_VALUE_X, gControlTop + ((double)row * ROW_GAP)
                        },
                        {
                            ROW_VALUE_W, ROW_H
                        }
    };
}

static tRectangle row_prev(int row) {
    return (tRectangle){{
                            ROW_VALUE_X, gControlTop + ((double)row * ROW_GAP)
                        }, {
                            24.0, ROW_H
                        }
    };
}

static tRectangle row_next(int row) {
    return (tRectangle){{
                            ROW_VALUE_X + ROW_VALUE_W - 24.0,
                            gControlTop + ((double)row * ROW_GAP)
                        }, {
                            24.0, ROW_H
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
    gMenuFor                 = which;
    gMenuChoice              = -1;
    open_context_menu(below_rect(anchor), gMenuItems, 1, 240.0);
}

void ms_draw_set_mouse(double x, double y) {
    gMouse.x = x;
    gMouse.y = y;
}

bool ms_draw_menu_active(void) {
    return gContextMenu.active;
}

void ms_draw_set_values(double midiDest, double audioSource, double compensate) {
    gMidiDest    = midiDest;
    gAudioSource = audioSource;
    gCompensate  = compensate;
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

static void heading(double x, double y, const char * text) {
    set_rgb_colour((tRgb)MS_HEADING);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 13.0}}, text);
}

// A LABELLED CONTROL: caption, a value box that opens a drop-down, and for a continuous value a
// pair of arrows instead. The arrows exist only where a list would be meaningless - GenBridge's rule
// that a drop-down replaces the steppers rather than sitting beside them.
static void control_row(int row, const char * name, const char * value, bool stepped) {
    tRectangle box   = row_value(row);
    double     textX = box.coord.x + 8.0;

    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{ROW_X, box.coord.y + 5.0}, {0.0, TEXT_H}}, name);

    set_rgb_colour((tRgb){0.16, 0.16, 0.18});
    render_rectangle(mainArea, box);

    if (stepped) {
        draw_button(mainArea, row_prev(row), "<", (tRgb){0.30, 0.30, 0.33});
        draw_button(mainArea, row_next(row), ">", (tRgb){0.30, 0.30, 0.33});
        textX = box.coord.x + 34.0;
    }
    set_rgb_colour((tRgb){0.92, 0.92, 0.94});
    render_text(mainArea, (tRectangle){{textX, box.coord.y + 5.0}, {0.0, TEXT_H}}, value);
}

static void stat(double x, double y, const char * name, const char * value) {
    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 11.0}}, name);

    set_rgb_colour((tRgb)MS_FIGURE_GREY);
    render_text(mainArea, (tRectangle){{x + STAT_VALUE_DX, y}, {0.0, 11.0}}, value);
}

// A TERM IN THE LATENCY BREAKDOWN, drawn as a proportional bar as well as a number.
//
// The bar is what makes a breakdown worth having rather than a list: it shows at a glance which
// term dominates, and an unknown term shows as an empty outline rather than as zero. Zero and
// "not measured" are completely different statements and a panel that renders them the same way is
// lying by omission.
static void term(double x, double y, double width, const char * name,
                 double ms, double total, bool known, tRgb colour) {
    char   buffer[64];

    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{x, y}, {0.0, 11.0}}, name);

    if (known) {
        snprintf(buffer, sizeof(buffer), "%.2f ms", ms);
    } else {
        snprintf(buffer, sizeof(buffer), "not measured");
    }
    set_rgb_colour((tRgb)MS_FIGURE_GREY);
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
        set_rgb_colour(colour);
        render_rectangle(mainArea, (tRectangle){{barX, y}, {barW * fraction, 9.0}});
    }
}

// THE SCROLLING TIMING GRAPH the concept asks for, in DEVIATION from the mean rather than absolute
// latency. The absolute figure is a constant tens of milliseconds tall and would flatten everything
// interesting into a straight line; what a user needs to see is the wobble around it.
static void graph(double x, double y, double w, double h, tMsStatus * status) {
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

    set_rgb_colour((tRgb){0.35, 0.78, 0.42});

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

    // ---- the clock we generate ----
    heading(20.0, y, "MIDI OUTPUT");
    y          += 20.0;

    // ---- the controls -----------------------------------------------------------------------
    //
    // THESE HAVE TO BE VST3 PARAMETERS, not environment variables. A host launched from the Dock
    // inherits no shell, so every MST_* variable this was developed against is empty inside Live -
    // the clock had no destination and generated nothing while every other figure on this panel read
    // perfectly. The same trap is recorded against $G2_VST3_PATCH in G2-Edit's plug-in notes.
    gControlTop = y - 4.0;

    char label[MS_MENU_LABEL];

    if (atomic_load(&status->waitingForDevice)) {
        snprintf(label, sizeof(label), "waiting for %s", status->waitingName);
    } else {
        dest_label(dest_slot(gMidiDest), label, sizeof(label));
    }
    control_row(0, "Port", label, false);

    control_row(1, "Analyse", audio_source_label(gAudioSource), false);

    snprintf(buffer, sizeof(buffer), "%.1f ms", gCompensate * MS_COMPENSATE_MAX);
    control_row(2, "Compensation", buffer, true);

    y           = gControlTop + (3.0 * ROW_GAP) + 6.0;

    snprintf(buffer, sizeof(buffer), "%u", (unsigned)atomic_load(&status->ticksSent));
    stat(28.0, y, "Clocks sent", buffer);
    y          += 16.0;

    // COMMIT MARGIN, which is the plug-in's own health and the one number that says whether the
    // schedule is comfortable. While it stays positive CoreMIDI holds each tick early and delivers
    // it on its own timer; the moment it goes negative, ticks bunch.
    snprintf(buffer, sizeof(buffer), "%.2f ms  (worst %.2f)",
             atomic_load(&status->commitMarginMeanMs), atomic_load(&status->commitMarginMinMs));
    stat(28.0, y, "Commit margin", buffer);
    y          += 16.0;

    unsigned late   = (unsigned)atomic_load(&status->lateTicks);

    snprintf(buffer, sizeof(buffer), "%u", late);
    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{28.0, y}, {0.0, 11.0}}, "Late clocks");
    set_rgb_colour((late > 0) ? (tRgb){0.90, 0.35, 0.25}
                                                           : (tRgb)MS_FIGURE_GREY);
    render_text(mainArea, (tRectangle){{28.0 + STAT_VALUE_DX, y}, {0.0, 11.0}}, buffer);
    y          += 16.0;

    snprintf(buffer, sizeof(buffer), "%.3f ms RMS", atomic_load(&status->blockPeriodRmsMs));
    stat(28.0, y, "Host block jitter", buffer);
    y          += 26.0;

    // ---- what came back ----
    heading(20.0, y, "MEASURED DEVICE");
    y          += 20.0;

    unsigned hits   = (unsigned)atomic_load(&status->hits);
    double   jitter = atomic_load(&status->roundTripJitterMs);

    if (hits == 0) {
        stat(28.0, y, "Round trip", "waiting for audio");
        y += 16.0;
    } else {
        snprintf(buffer, sizeof(buffer), "%.3f ms", atomic_load(&status->roundTripMeanMs));
        stat(28.0, y, "Round trip", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", jitter);
        stat(28.0, y, "Jitter RMS", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f ms", atomic_load(&status->roundTripPeakDevMs));
        stat(28.0, y, "Peak deviation", buffer);
        y += 16.0;

        snprintf(buffer, sizeof(buffer), "%.3f / %.3f ms",
                 atomic_load(&status->roundTripMinMs), atomic_load(&status->roundTripMaxMs));
        stat(28.0, y, "Min / max", buffer);
        y += 16.0;
    }
    snprintf(buffer, sizeof(buffer), "%u hit, %u missed, %u spurious",
             hits, (unsigned)atomic_load(&status->missed), (unsigned)atomic_load(&status->spurious));
    stat(28.0, y, "Detections", buffer);
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
    heading(20.0, y, "LATENCY BREAKDOWN");
    y += 20.0;

    double lead      = atomic_load(&status->scheduleLeadMs);
    double roundTrip = atomic_load(&status->roundTripMeanMs);
    double block     = atomic_load(&status->blockMs);
    double inputPath = atomic_load(&status->inputPathMs);
    double compensat = atomic_load(&status->compensationMs);
    double total     = lead + roundTrip;
    bool   haveTrip  = (hits > 0);
    bool   haveInput = (inputPath > 0.0);

    // The remainder, once every term that IS known has been taken out of the measured round trip.
    double known     = block + (haveInput ? inputPath : 0.0);
    double remainder = (roundTrip > known) ? (roundTrip - known) : 0.0;

    term(28.0, y, 510.0, "Schedule lead", lead, total, true, (tRgb){0.35, 0.55, 0.85});
    y += 15.0;
    term(28.0, y, 510.0, "Host buffer", block, total, (block > 0.0), (tRgb){0.40, 0.65, 0.60});
    y += 15.0;
    term(28.0, y, 510.0, "Interface A/D", inputPath, total, haveInput, (tRgb){0.55, 0.45, 0.80});
    y += 15.0;
    // WHAT IS LEFT, labelled by what is still mixed into it. While the interface term is unmeasured
    // this is the device AND the converter, and calling it "device latency" would be the exact
    // overclaim this panel exists to avoid. One buffer comes out because that much is arithmetic:
    // the audio in a block was captured at least a buffer before the block was handed over.
    term(28.0, y, 510.0, haveInput ? "Device MIDI to audio" : "Device + interface",
         remainder, total, haveTrip, (tRgb){0.85, 0.55, 0.30});
    y += 19.0;

    snprintf(buffer, sizeof(buffer), "%.2f ms", total);
    stat(28.0, y, "Total musical delay", haveTrip ? buffer : "-");
    y += 16.0;

    snprintf(buffer, sizeof(buffer), "%.2f ms", compensat);
    stat(28.0, y, "Compensation", (compensat != 0.0) ? buffer : "off");
    y += 16.0;

    // What is left after compensation - the number that says whether the job is done.
    snprintf(buffer, sizeof(buffer), "%+.2f ms", total - compensat);
    stat(28.0, y, "Residual", haveTrip ? buffer : "-");

    // ---- the verdict and the trace ----
    //
    // ANCHORED TO THE BOTTOM OF THE CANVAS, not to the running y. See MS_CANVAS_H.
    double       graphY   = MS_CANVAS_H - 96.0;
    double       verdictY = graphY - 20.0;

    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{20.0, verdictY}, {0.0, 12.0}}, "TIMING QUALITY");

    const char * verdict  = quality(jitter, hits);

    set_rgb_colour((hits < 4) ? (tRgb)MS_CAPTION_GREY
                   : (jitter < 0.75) ? (tRgb){0.40, 0.85, 0.50}
                   : (jitter < 2.0) ? (tRgb){0.90, 0.75, 0.25}
                   : (tRgb){0.90, 0.35, 0.25});
    render_text(mainArea, (tRectangle){{20.0 + STAT_VALUE_DX, verdictY}, {0.0, 12.0}}, verdict);

    graph(28.0, graphY, 504.0, 70.0, status);

    set_rgb_colour((tRgb)MS_CAPTION_GREY);
    render_text(mainArea, (tRectangle){{28.0, graphY + 74.0}, {0.0, 10.0}},
                "deviation from the mean round trip");

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
    return false;
}
