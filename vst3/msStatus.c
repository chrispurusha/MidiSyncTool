/*
 * MidiSyncTool - live figures the processor publishes and the editor reads.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <string.h>

#include "msStatus.h"

// A fixed array rather than an allocation, so a slot's address never moves and the editor can hold
// the pointer across a repaint without wondering whether the processor has been rebuilt underneath
// it. Thirty-two instances of a clock generator in one project would already be strange.
static tMsStatus  gSlots[MS_STATUS_SLOTS];
static atomic_int gTaken[MS_STATUS_SLOTS];

int ms_status_claim(void) {
    for (int i = 0; i < MS_STATUS_SLOTS; i++) {
        int expected = 0;

        if (atomic_compare_exchange_strong(&gTaken[i], &expected, 1)) {
            memset(&gSlots[i], 0, sizeof(gSlots[i]));
            return i;
        }
    }

    // Callers must cope: the panel then shows no live figures rather than someone else's.
    return -1;
}

void ms_status_release(int slot) {
    if ((slot >= 0) && (slot < MS_STATUS_SLOTS)) {
        atomic_store(&gSlots[slot].active, false);
        atomic_store(&gTaken[slot], 0);
    }
}

tMsStatus * ms_status(int slot) {
    if ((slot < 0) || (slot >= MS_STATUS_SLOTS) || (atomic_load(&gTaken[slot]) == 0)) {
        return NULL;
    }
    return &gSlots[slot];
}
