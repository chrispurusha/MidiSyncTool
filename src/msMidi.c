/*
 * MidiSyncTool - MIDI destination selection and scheduled sending.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "msLog.h"
#include "msMidi.h"

static MIDIClientRef   gClient  = 0;
static MIDIPortRef     gOutPort = 0;
static MIDIEndpointRef gDest[MS_MIDI_MAX_DEST];
static char            gName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];
static int             gCount   = 0;
static bool            gReady   = false;

static void ensure_client(void) {
    if (gReady) {
        return;
    }

    if (MIDIClientCreate(CFSTR("MidiSyncTool"), NULL, NULL, &gClient) != noErr) {
        ms_log_line("MIDI: MIDIClientCreate failed");
        return;
    }

    if (MIDIOutputPortCreate(gClient, CFSTR("MidiSyncTool Out"), &gOutPort) != noErr) {
        ms_log_line("MIDI: MIDIOutputPortCreate failed");
        return;
    }
    gReady = true;
}

void ms_midi_refresh(void) {
    ensure_client();

    if (!gReady) {
        return;
    }
    ItemCount total = MIDIGetNumberOfDestinations();

    gCount = 0;

    for (ItemCount i = 0; (i < total) && (gCount < MS_MIDI_MAX_DEST); i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);
        CFStringRef     name     = NULL;

        if (endpoint == 0) {
            continue;
        }
        gDest[gCount]    = endpoint;
        gName[gCount][0] = '\0';

        // The DISPLAY name where there is one: it is what the user sees in every other application,
        // and for a USB device it is the one that names the instrument rather than the interface.
        if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr) {
            name = NULL;
        }

        if (name == NULL) {
            MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name);
        }

        if (name != NULL) {
            CFStringGetCString(name, gName[gCount], MS_MIDI_NAME_LEN, kCFStringEncodingUTF8);
            CFRelease(name);
        }

        if (gName[gCount][0] == '\0') {
            snprintf(gName[gCount], MS_MIDI_NAME_LEN, "destination %d", gCount);
        }
        gCount++;
    }

    ms_log_line("MIDI: %d destination(s)", gCount);

    for (int i = 0; i < gCount; i++) {
        ms_log_line("MIDI:   [%d] %s", i, gName[i]);
    }
}

int ms_midi_count(void) {
    return gCount;
}

void ms_midi_name(int index, char * out, unsigned long len) {
    if ((out == NULL) || (len == 0)) {
        return;
    }

    if ((index < 0) || (index >= gCount)) {
        snprintf(out, len, "%s", "None");
        return;
    }
    snprintf(out, len, "%s", gName[index]);
}

int ms_midi_index_for_name(const char * name) {
    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }

    for (int i = 0; i < gCount; i++) {
        if (strcmp(gName[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

static _Atomic double gOffsetMs = 0.0;

void ms_midi_set_offset_ms(double offsetMs) {
    atomic_store(&gOffsetMs, offsetMs);
}

double ms_midi_offset_ms(void) {
    return atomic_load(&gOffsetMs);
}

bool ms_midi_send_at(int index, const uint8_t * data, uint32_t length, uint64_t hostTime) {
    if (!gReady || (index < 0) || (index >= gCount) || (data == NULL) || (length == 0)) {
        return false;
    }

    // A clock tick is one byte and a transport message three; the largest thing this tool sends is
    // a Song Position Pointer at three. 64 bytes of packet list is ample and keeps the frame small
    // enough to sit on the audio thread's stack without thought.
    // THE COMPENSATION, applied to every scheduled event and to nothing immediate. A hostTime of 0
    // means "now" by contract, and shifting "now" would be meaningless.
    if (hostTime != 0) {
        double   offsetMs = atomic_load(&gOffsetMs);
        uint64_t shift    = AudioConvertNanosToHostTime((uint64_t)(fabs(offsetMs) * 1.0e6));

        if (offsetMs < 0.0) {
            // CLAMPED AT THE PRESENT, because an event stamped in the past is delivered immediately
            // and several of them arrive together - the exact bunching this tool exists to remove.
            // The commit-margin telemetry is what reports that the budget has been overspent; this
            // only stops it turning into a burst.
            uint64_t now = AudioGetCurrentHostTime();

            hostTime = ((hostTime > shift) && ((hostTime - shift) > now)) ? (hostTime - shift) : now;
        } else if (offsetMs > 0.0) {
            hostTime += shift;
        }
    }
    uint8_t          storage[64];
    MIDIPacketList * list   = (MIDIPacketList *)storage;
    MIDIPacket *     packet = MIDIPacketListInit(list);

    packet = MIDIPacketListAdd(list, sizeof(storage), packet, (MIDITimeStamp)hostTime, length, data);

    if (packet == NULL) {
        return false;
    }
    return MIDISend(gOutPort, gDest[index], list) == noErr;
}
