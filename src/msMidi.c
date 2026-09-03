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

static MIDIClientRef   gClient       = 0;
static MIDIPortRef     gOutPort      = 0;
static MIDIEndpointRef gDest[MS_MIDI_MAX_DEST];
static char            gName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];
static int             gCount        = 0;
static bool            gReady        = false;

// ---- the listening side -------------------------------------------------------------------------
static MIDIPortRef     gInPort       = 0;
static MIDIEndpointRef gSource[MS_MIDI_MAX_SOURCE];
static char            gSourceName[MS_MIDI_MAX_SOURCE][MS_MIDI_NAME_LEN];
static int             gSourceCount  = 0;
static int             gListening    = -1;
static tMsMidiListener gListener     = NULL;
static void *          gListenerUser = NULL;

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

// The DISPLAY name where there is one: it is what the user sees in every other application, and for
// a USB device it is the one that names the instrument rather than the interface.
static void endpoint_name(MIDIEndpointRef endpoint, char * out, const char * fallback, int index) {
    CFStringRef name = NULL;

    out[0] = '\0';

    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr) {
        name = NULL;
    }

    if (name == NULL) {
        MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name);
    }

    if (name != NULL) {
        CFStringGetCString(name, out, MS_MIDI_NAME_LEN, kCFStringEncodingUTF8);
        CFRelease(name);
    }

    if (out[0] == '\0') {
        snprintf(out, MS_MIDI_NAME_LEN, "%s %d", fallback, index);
    }
}

// SOURCES ARE ENUMERATED SEPARATELY AND THE INDICES DO NOT LINE UP with the destination list. They
// are different endpoints - a machine can have a source with no destination and the reverse - so a
// saved setup stores the NAME, exactly as the destination side already learned to.
static void refresh_sources(void) {
    ItemCount total = MIDIGetNumberOfSources();

    gSourceCount = 0;

    for (ItemCount i = 0; (i < total) && (gSourceCount < MS_MIDI_MAX_SOURCE); i++) {
        MIDIEndpointRef endpoint = MIDIGetSource(i);

        if (endpoint == 0) {
            continue;
        }
        gSource[gSourceCount] = endpoint;
        endpoint_name(endpoint, gSourceName[gSourceCount], "source", gSourceCount);
        gSourceCount++;
    }

    ms_log_line("MIDI: %d source(s)", gSourceCount);

    for (int i = 0; i < gSourceCount; i++) {
        ms_log_line("MIDI:   <%d> %s", i, gSourceName[i]);
    }
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
        gDest[gCount] = endpoint;
        endpoint_name(endpoint, gName[gCount], "destination", gCount);
        (void)name;
        gCount++;
    }

    refresh_sources();

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

// ---- listening ---------------------------------------------------------------------------------

int ms_midi_source_count(void) {
    return gSourceCount;
}

void ms_midi_source_name(int index, char * out, unsigned long len) {
    if ((out == NULL) || (len == 0)) {
        return;
    }

    if ((index < 0) || (index >= gSourceCount)) {
        snprintf(out, len, "%s", "None");
        return;
    }
    snprintf(out, len, "%s", gSourceName[index]);
}

int ms_midi_source_index_for_name(const char * name) {
    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }

    for (int i = 0; i < gSourceCount; i++) {
        if (strcmp(gSourceName[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

void ms_midi_set_listener(tMsMidiListener listener, void * user) {
    gListener     = listener;
    gListenerUser = user;
}

// ON CoreMIDI'S OWN RECEIVE THREAD, which is high priority and must not be made to wait. Nothing
// here allocates, logs or takes a lock: it walks the packets and hands each status byte on.
//
// ONLY SYSTEM REAL-TIME BYTES ARE FORWARDED (0xF8 and above). They are single bytes and are the only
// things this tool cares about - and, importantly, they are permitted to appear INSIDE another
// message, which is why each byte is examined rather than each packet's first byte. A clock landing
// in the middle of a SysEx dump is legal and would otherwise be missed.
static void read_proc(const MIDIPacketList * list, void * refCon, void * connRefCon) {
    (void)refCon;
    (void)connRefCon;

    if ((list == NULL) || (gListener == NULL)) {
        return;
    }
    const MIDIPacket * packet = &list->packet[0];

    for (UInt32 p = 0; p < list->numPackets; p++) {
        for (UInt16 b = 0; b < packet->length; b++) {
            uint8_t status = packet->data[b];

            if (status >= 0xF8) {
                gListener(status, (uint64_t)packet->timeStamp, gListenerUser);
            }
        }

        packet = MIDIPacketNext(packet);
    }
}

bool ms_midi_listen(int index) {
    ensure_client();

    if (!gReady) {
        return false;
    }

    if (  (gInPort == 0)
       && (MIDIInputPortCreate(gClient, CFSTR("MidiSyncTool In"), read_proc, NULL, &gInPort) != noErr)) {
        ms_log_line("MIDI: MIDIInputPortCreate failed");
        return false;
    }

    // DISCONNECT FIRST, ALWAYS. Two sources connected to one port interleave into a single stream
    // and the estimator would fit a line through both masters at once - a plausible-looking tempo
    // belonging to neither.
    if ((gListening >= 0) && (gListening < gSourceCount)) {
        MIDIPortDisconnectSource(gInPort, gSource[gListening]);
    }
    gListening = -1;

    if ((index < 0) || (index >= gSourceCount)) {
        ms_log_line("MIDI: listening to nothing");
        return true;
    }

    if (MIDIPortConnectSource(gInPort, gSource[index], NULL) != noErr) {
        ms_log_line("MIDI: could not connect to source <%d> %s", index, gSourceName[index]);
        return false;
    }
    gListening = index;
    ms_log_line("MIDI: listening to <%d> %s", index, gSourceName[index]);
    return true;
}

int ms_midi_listening(void) {
    return gListening;
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
