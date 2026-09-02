/*
 * MidiSyncTool - logging.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "msLog.h"

bool ms_log_enabled(void) {
    static int cached = -1;

    if (cached < 0) {
        cached = (access(MS_LOG_GATE_PATH, F_OK) == 0) ? 1 : 0;
    }

    return cached == 1;
}

void ms_log_line(const char * fmt, ...) {
    if (!ms_log_enabled()) {
        return;
    }
    FILE * file = fopen(MS_LOG_PATH, "a");

    if (file == NULL) {
        return;
    }
    va_list args;

    va_start(args, fmt);
    fprintf(file, "[mst %d] ", (int)getpid());
    vfprintf(file, fmt, args);
    fputc('\n', file);
    va_end(args);
    fclose(file);
}
