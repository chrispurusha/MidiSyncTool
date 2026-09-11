/*
 * MidiSyncTool - the editor's NSView.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_VIEW_H__
#define __MS_VIEW_H__

#include "msDraw.h"

#ifdef __cplusplus
extern "C" {
#endif

// A click the panel cannot act on by itself, handed back to msPlugin.c.
typedef void (*tMsEditCallback)(void * user, const tMsEditRequest * request);

// CALLED BEFORE EVERY FRAME AND EVERY CLICK, on the main thread, to put THIS editor's status slot and
// parameter values into the draw layer - which keeps both file-scope, so with two editors open
// whichever set them last would otherwise speak for both. The callback answers with
// ms_view_set_status_slot() and ms_view_set_values().
typedef void (*tMsSyncCallback)(void * user, void * view);

// Returns an NSView *, RETAINED, as a void * so C need not import AppKit - SynthLib's wrappers own it
// from there and release it after taking it out of the host's window.
void * ms_view_create(double width, double height, tMsEditCallback callback, tMsSyncCallback sync,
                      void * user);
void   ms_view_set_values(void * view, double midiDest, double audioSource, double compensate,
                          double mode, double clockSource);
void   ms_view_set_status_slot(void * view, int statusSlot);

#ifdef __cplusplus
}
#endif

#endif // __MS_VIEW_H__
