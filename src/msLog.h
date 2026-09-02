/*
 * MidiSyncTool - logging.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#ifndef __MS_LOG_H__
#define __MS_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

// FILE-GATED, NOT ENV-GATED, and that is not a stylistic preference: a plug-in is loaded by a host
// launched from the Dock, which inherits no shell environment, so an env var set in a terminal is
// invisible to it. The presence of a file is something a host CAN see.
//
// The gate and the output are deliberately similar and deliberately different, which has already
// caught me out once in the sibling project - the GATE is /tmp/midisynctool-log (hyphen) and the
// OUTPUT is /tmp/midisynctool.log (dot).
#define MS_LOG_GATE_PATH    "/tmp/midisynctool-log"
#define MS_LOG_PATH         "/tmp/midisynctool.log"

bool ms_log_enabled(void);
void ms_log_line(const char * fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // __MS_LOG_H__
