/*
 * MidiSyncTool - MIDI destination selection and scheduled sending.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */
// Notes: Docs/code-notes/msMidi.c.md - "// notes §k" refers there.

#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "synthlibLog.h"
#include "msMidi.h"

static MIDIClientRef           gClient       = 0;
static MIDIPortRef             gOutPort      = 0;
static MIDIEndpointRef         gDest[MS_MIDI_MAX_DEST];
static char                    gName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];

// notes §1
static _Atomic int             gCount        = 0;
static bool                    gReady        = false;

// SET FROM CoreMIDI'S NOTIFY PROC, CLEARED BY THE REBUILD. Nothing polls: the flag says the cached
// lists are stale and ms_midi_refresh_if_changed() acts on it from a thread that may safely take
// CoreMIDI's locks - a click, never a repaint and never the audio thread.
static _Atomic bool            gSetupChanged = false;

// Serialises rebuilds against each other - two editors open is two threads in here. Deliberately
// never taken by a send or by ms_midi_listen(), both of which can run on the audio thread; the
// atomic counts above are what make that safe.
static pthread_mutex_t         gRefreshLock  = PTHREAD_MUTEX_INITIALIZER;

// ---- the listening side -------------------------------------------------------------------------
static MIDIPortRef             gInPort       = 0;
static MIDIEndpointRef         gSource[MS_MIDI_MAX_SOURCE];
static char                    gSourceName[MS_MIDI_MAX_SOURCE][MS_MIDI_NAME_LEN];
static _Atomic int             gSourceCount  = 0;

// notes §2
static _Atomic MIDIEndpointRef gListeningEnd = 0;
static tMsMidiListener         gListener     = NULL;
static void *                  gListenerUser = NULL;

// FROM CoreMIDI'S OWN THREAD, and it does one thing. Any setup change - a synth switched on, an
// interface unplugged, a name edited in Audio MIDI Setup - means the cached lists are stale, and
// saying so is all that can safely be done from here.
static void midi_notify(const MIDINotification * message, void * refCon) {
    (void)refCon;

    if ((message != NULL) && (message->messageID == kMIDIMsgSetupChanged)) {
        atomic_store(&gSetupChanged, true);
    }
}

static void ensure_client(void) {
    if (gReady) {
        return;
    }

    // notes §3
    if (MIDIClientCreate(CFSTR("MidiSyncTool"), midi_notify, NULL, &gClient) != noErr) {
        synthlib_log_line("MIDI: MIDIClientCreate failed");
        return;
    }

    if (MIDIOutputPortCreate(gClient, CFSTR("MidiSyncTool Out"), &gOutPort) != noErr) {
        synthlib_log_line("MIDI: MIDIOutputPortCreate failed");
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

// THE SHADOWS THE LISTS ARE BUILT IN. Static rather than automatic because they are 8 KB between
// them and this can be called from a plug-in editor's click, and safe to be static because every
// use is inside gRefreshLock.
static MIDIEndpointRef gNextDest[MS_MIDI_MAX_DEST];
static char            gNextName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];
static MIDIEndpointRef gNextSource[MS_MIDI_MAX_SOURCE];
static char            gNextSourceName[MS_MIDI_MAX_SOURCE][MS_MIDI_NAME_LEN];

// notes §4
static void refresh_sources_locked(void) {
    ItemCount total = MIDIGetNumberOfSources();
    int       found = 0;

    for (ItemCount i = 0; (i < total) && (found < MS_MIDI_MAX_SOURCE); i++) {
        MIDIEndpointRef endpoint = MIDIGetSource(i);

        if (endpoint == 0) {
            continue;
        }
        gNextSource[found] = endpoint;
        endpoint_name(endpoint, gNextSourceName[found], "source", found);
        found++;
    }

    // ENTRIES FIRST, COUNT LAST - see the note on gCount. Nothing above this point was visible to a
    // reader; this is the only moment anything changes for one.
    for (int i = 0; i < found; i++) {
        gSource[i] = gNextSource[i];
        memcpy(gSourceName[i], gNextSourceName[i], MS_MIDI_NAME_LEN);
    }

    atomic_store_explicit(&gSourceCount, found, memory_order_release);

    // WORTH A LINE IN THE LOG, because it is the difference between "the master is quiet" and "the
    // master is not there any more" - two identical-looking silences. Nothing is repaired here: the
    // endpoint is what the connection is to, and ms_midi_listening() reports the absence by itself.
    if ((atomic_load(&gListeningEnd) != 0) && (ms_midi_listening() < 0)) {
        synthlib_log_line("MIDI: the source being listened to has gone away");
    }
    synthlib_log_line("MIDI: %d source(s)", found);

    for (int i = 0; i < found; i++) {
        synthlib_log_line("MIDI:   <%d> %s", i, gSourceName[i]);
    }
}

void ms_midi_refresh(void) {
    ensure_client();

    if (!gReady) {
        return;
    }
    // CLEARED BEFORE THE ENUMERATION, NEVER AFTER. A device arriving while CoreMIDI is being walked
    // may or may not be in what this call returns; clearing first means the flag it sets survives
    // and the next check looks again, where clearing afterwards would swallow it.
    atomic_store(&gSetupChanged, false);

    pthread_mutex_lock(&gRefreshLock);

    ItemCount total = MIDIGetNumberOfDestinations();
    int       found = 0;

    for (ItemCount i = 0; (i < total) && (found < MS_MIDI_MAX_DEST); i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);

        if (endpoint == 0) {
            continue;
        }
        gNextDest[found] = endpoint;
        endpoint_name(endpoint, gNextName[found], "destination", found);
        found++;
    }

    // ENTRIES FIRST, COUNT LAST, with a release. Everything above happened in a shadow, so no send
    // has been able to see a half-built list - which is the whole point of the exercise.
    for (int i = 0; i < found; i++) {
        gDest[i] = gNextDest[i];
        memcpy(gName[i], gNextName[i], MS_MIDI_NAME_LEN);
    }

    atomic_store_explicit(&gCount, found, memory_order_release);

    refresh_sources_locked();

    synthlib_log_line("MIDI: %d destination(s)", found);

    for (int i = 0; i < found; i++) {
        synthlib_log_line("MIDI:   [%d] %s", i, gName[i]);
    }

    pthread_mutex_unlock(&gRefreshLock);
}

bool ms_midi_setup_changed(void) {
    return atomic_load(&gSetupChanged);
}

bool ms_midi_refresh_if_changed(void) {
    if (!atomic_load(&gSetupChanged)) {
        return false;
    }
    ms_midi_refresh();
    return true;
}

int ms_midi_count(void) {
    return atomic_load_explicit(&gCount, memory_order_acquire);
}

void ms_midi_name(int index, char * out, unsigned long len) {
    if ((out == NULL) || (len == 0)) {
        return;
    }

    if ((index < 0) || (index >= atomic_load_explicit(&gCount, memory_order_acquire))) {
        snprintf(out, len, "%s", "None");
        return;
    }
    snprintf(out, len, "%s", gName[index]);
}

int ms_midi_index_for_name(const char * name) {
    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }
    int count = atomic_load_explicit(&gCount, memory_order_acquire);

    for (int i = 0; i < count; i++) {
        if (strcmp(gName[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

// ---- listening ---------------------------------------------------------------------------------

int ms_midi_source_count(void) {
    return atomic_load_explicit(&gSourceCount, memory_order_acquire);
}

void ms_midi_source_name(int index, char * out, unsigned long len) {
    if ((out == NULL) || (len == 0)) {
        return;
    }

    if ((index < 0) || (index >= atomic_load_explicit(&gSourceCount, memory_order_acquire))) {
        snprintf(out, len, "%s", "None");
        return;
    }
    snprintf(out, len, "%s", gSourceName[index]);
}

int ms_midi_source_index_for_name(const char * name) {
    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }
    int count = atomic_load_explicit(&gSourceCount, memory_order_acquire);

    for (int i = 0; i < count; i++) {
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

// notes §5
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
        synthlib_log_line("MIDI: MIDIInputPortCreate failed");
        return false;
    }
    // notes §6
    MIDIEndpointRef previous = atomic_exchange(&gListeningEnd, (MIDIEndpointRef)0);

    if (previous != 0) {
        MIDIPortDisconnectSource(gInPort, previous);
    }
    int             count    = atomic_load_explicit(&gSourceCount, memory_order_acquire);

    if ((index < 0) || (index >= count)) {
        synthlib_log_line("MIDI: listening to nothing");
        return true;
    }
    // READ ONCE. A rebuild on another thread can move this slot, and connecting to one endpoint
    // while remembering another is how a disconnect ends up aimed at somebody else's port.
    MIDIEndpointRef endpoint = gSource[index];

    if (MIDIPortConnectSource(gInPort, endpoint, NULL) != noErr) {
        synthlib_log_line("MIDI: could not connect to source <%d> %s", index, gSourceName[index]);
        return false;
    }
    atomic_store(&gListeningEnd, endpoint);
    synthlib_log_line("MIDI: listening to <%d> %s", index, gSourceName[index]);
    return true;
}

// notes §7
int ms_midi_listening(void) {
    MIDIEndpointRef endpoint = atomic_load(&gListeningEnd);

    if (endpoint == 0) {
        return -1;
    }
    int             count    = atomic_load_explicit(&gSourceCount, memory_order_acquire);

    for (int i = 0; i < count; i++) {
        if (gSource[i] == endpoint) {
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
    // ACQUIRE, to pair with the release in ms_midi_refresh(): an index below the count this reads is
    // guaranteed to name an entry that was written before that count was published. This is the read
    // that used to see a zero count for the length of somebody else's enumeration.
    if (  !gReady || (index < 0)
       || (index >= atomic_load_explicit(&gCount, memory_order_acquire))
       || (data == NULL) || (length == 0)) {
        return false;
    }

    // notes §8
    if (hostTime != 0) {
        double   offsetMs = atomic_load(&gOffsetMs);
        uint64_t shift    = AudioConvertNanosToHostTime((uint64_t)(fabs(offsetMs) * 1.0e6));

        if (offsetMs < 0.0) {
            // notes §9
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
