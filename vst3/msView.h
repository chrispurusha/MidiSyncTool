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

// A click the host must be told about, handed back to msEditor.mm which owns the controller.
typedef void (*tMsEditCallback)(void * user, const tMsEditRequest * request);

// Returns an NSView *, as a void * so the C++ side need not import AppKit.
void * ms_view_create(double width, double height, tMsEditCallback callback, void * user,
                      int statusSlot);
void   ms_view_set_values(void * view, double midiDest, double audioSource, double compensate,
                          double mode, double clockSource);
void   ms_view_set_status_slot(void * view, int statusSlot);
void   ms_view_destroy(void * view);

#ifdef __cplusplus
}
#endif

#endif // __MS_VIEW_H__
