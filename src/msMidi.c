/*
 * MidiSyncTool - MIDI destination selection and scheduled sending.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "msLog.h"
#include "msMidi.h"

static MIDIClientRef           gClient       = 0;
static MIDIPortRef             gOutPort      = 0;
static MIDIEndpointRef         gDest[MS_MIDI_MAX_DEST];
static char                    gName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];

// THE COUNTS ARE PUBLISHED LAST, AND ATOMIC, because these tables are process-global and a host
// loads every plug-in into one process. A SECOND MidiSyncTool arriving in a running session used to
// stop the FIRST one's clock: refresh() set gCount = 0 and then spent milliseconds inside CoreMIDI,
// and for that whole window every send in the process saw "index >= gCount" and returned false -
// ticks dropped, on an instance that was not the one enumerating and had no idea why.
//
// The lists are built into shadows and copied in, and the COUNT goes last with a release, so a
// reader indexing below the count it loaded sees entries written before it. A slot can still name a
// different endpoint after a rebuild, which is why a destination is saved and restored by NAME.
// GenBridge arrived at exactly this shape on 2026-09-08; see gbMidi.c.
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

// WHAT IS BEING LISTENED TO IS AN ENDPOINT, NOT AN INDEX, and that is the whole of the state.
//
// Before the lists could be rebuilt at runtime an index was as good as an endpoint. Now a rebuild
// can move a source under a live connection, so a stored index would go stale in two ways that both
// matter: a disconnect aimed at the old slot would disconnect somebody else's port and leave ours
// connected, and getState() would write down the wrong name to restore. Holding the endpoint and
// working the index out from it when anybody asks removes both, and removes the write that the
// audio thread and a rebuilding editor would otherwise be racing over.
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

    // A NOTIFY PROC, so the lists do not have to be polled and do not have to be right forever.
    // Passing NULL here meant the destination and source menus were whatever existed at load: a
    // synth switched on afterwards never appeared in either, and nothing looked again. CoreMIDI
    // delivers this on the run loop of the thread that created the client, so a client created
    // without one simply gets no notifications and behaves as it did before - which is why the
    // first enumeration still happens explicitly at load.
    if (MIDIClientCreate(CFSTR("MidiSyncTool"), midi_notify, NULL, &gClient) != noErr) {
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

// THE SHADOWS THE LISTS ARE BUILT IN. Static rather than automatic because they are 8 KB between
// them and this can be called from a plug-in editor's click, and safe to be static because every
// use is inside gRefreshLock.
static MIDIEndpointRef gNextDest[MS_MIDI_MAX_DEST];
static char            gNextName[MS_MIDI_MAX_DEST][MS_MIDI_NAME_LEN];
static MIDIEndpointRef gNextSource[MS_MIDI_MAX_SOURCE];
static char            gNextSourceName[MS_MIDI_MAX_SOURCE][MS_MIDI_NAME_LEN];

// SOURCES ARE ENUMERATED SEPARATELY AND THE INDICES DO NOT LINE UP with the destination list. They
// are different endpoints - a machine can have a source with no destination and the reverse - so a
// saved setup stores the NAME, exactly as the destination side already learned to.
//
// CALLED WITH gRefreshLock HELD.
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
        ms_log_line("MIDI: the source being listened to has gone away");
    }
    ms_log_line("MIDI: %d source(s)", found);

    for (int i = 0; i < found; i++) {
        ms_log_line("MIDI:   <%d> %s", i, gSourceName[i]);
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

    ms_log_line("MIDI: %d destination(s)", found);

    for (int i = 0; i < found; i++) {
        ms_log_line("MIDI:   [%d] %s", i, gName[i]);
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
    //
    // BY ENDPOINT, because the list can be rebuilt under a live connection - see gListeningEnd. A
    // disconnect from an endpoint that has gone away simply fails, which is the right outcome.
    MIDIEndpointRef previous = atomic_exchange(&gListeningEnd, (MIDIEndpointRef)0);

    if (previous != 0) {
        MIDIPortDisconnectSource(gInPort, previous);
    }
    int             count    = atomic_load_explicit(&gSourceCount, memory_order_acquire);

    if ((index < 0) || (index >= count)) {
        ms_log_line("MIDI: listening to nothing");
        return true;
    }
    // READ ONCE. A rebuild on another thread can move this slot, and connecting to one endpoint
    // while remembering another is how a disconnect ends up aimed at somebody else's port.
    MIDIEndpointRef endpoint = gSource[index];

    if (MIDIPortConnectSource(gInPort, endpoint, NULL) != noErr) {
        ms_log_line("MIDI: could not connect to source <%d> %s", index, gSourceName[index]);
        return false;
    }
    atomic_store(&gListeningEnd, endpoint);
    ms_log_line("MIDI: listening to <%d> %s", index, gSourceName[index]);
    return true;
}

// WORKED OUT, NOT REMEMBERED. The answer is "where in the list is the endpoint I am connected to",
// and a list that can be rebuilt at any moment is exactly why that question has to be asked again
// each time rather than answered from a stored index. An endpoint that has been switched off is in
// no list, so this says -1 - which is the truth: nothing is arriving.
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
