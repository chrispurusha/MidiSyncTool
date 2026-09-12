// Notes: Docs/code-notes/midiListTest.c.md - "// notes §k" refers there.
// notes §1
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

#include "msMidi.h"

// Names the log - touch /tmp/midisynctool-log, read /tmp/midisynctool.log. See SynthLib's plugin/synthlibLog.h.
const char gSynthLibLogName[] = "midisynctool";

static int gFailures = 0;

static void check(const char * what, int got, int expected) {
    const bool ok = (got == expected);

    printf("%-46s %4d  (expected %d) %s\n", what, got, expected, ok ? "" : "  <-- FAILED");

    if (!ok) {
        gFailures++;
    }
}

// THE SETTLING TIME IS NOT OPTIONAL. Nothing here polls, so a check made before the notification has
// been delivered would read the old list and fail for the wrong reason.
static void settle(void) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
}

// A VIRTUAL DESTINATION HAS TO BE ABLE TO RECEIVE, so it needs a read proc even though nothing is
// ever sent to it. Passing NULL is what the header forbids rather than what CoreMIDI minds.
static void swallow(const MIDIPacketList * list, void * refCon, void * connRefCon) {
    (void)list;
    (void)refCon;
    (void)connRefCon;
}

// A DESTINATION APPEARING AND GOING AWAY, which is the case that was broken: before the notify proc
// a port switched on after load was invisible for the life of the session.
static void test_destination_appears(MIDIClientRef client) {
    MIDIEndpointRef dest = 0;

    printf("\n-- a destination appears and goes away --\n");
    ms_midi_refresh();
    check("setup changed with nothing happening", (int)ms_midi_setup_changed(), 0);
    check("and a refresh is therefore not needed", (int)ms_midi_refresh_if_changed(), 0);

    int before = ms_midi_count();

    MIDIDestinationCreate(client, CFSTR("MST TEST PORT"), swallow, NULL, &dest);
    settle();
    check("setup changed after the create", (int)ms_midi_setup_changed(), 1);
    check("and the refresh happens", (int)ms_midi_refresh_if_changed(), 1);
    check("one more destination", ms_midi_count(), before + 1);
    check("the new port is findable by name",
          (ms_midi_index_for_name("MST TEST PORT") >= 0) ? 1 : 0, 1);
    check("a second refresh is not needed", (int)ms_midi_refresh_if_changed(), 0);

    MIDIEndpointDispose(dest);
    settle();
    ms_midi_refresh_if_changed();
    check("back to the count we started with", ms_midi_count(), before);
    check("and the name is gone", ms_midi_index_for_name("MST TEST PORT"), -1);
}

// A LIVE CONNECTION UNDER A MOVING LIST, which is what made the listening side hold an endpoint
// rather than an index: disposing a source EARLIER in the list shifts every index after it.
static void test_listening_follows_its_endpoint(MIDIClientRef client) {
    MIDIEndpointRef a = 0, b = 0;

    printf("\n-- a listening connection while the list moves --\n");
    MIDISourceCreate(client, CFSTR("MST TEST A"), &a);
    MIDISourceCreate(client, CFSTR("MST TEST B"), &b);
    settle();
    ms_midi_refresh();

    int indexB = ms_midi_source_index_for_name("MST TEST B");

    ms_midi_listen(indexB);
    check("listening to B", ms_midi_listening(), indexB);

    MIDIEndpointDispose(a);
    settle();
    ms_midi_refresh_if_changed();
    check("B moved down one and is still listened to", ms_midi_listening(), indexB - 1);

    char name[MS_MIDI_NAME_LEN] = {0};

    ms_midi_source_name(ms_midi_listening(), name, sizeof(name));
    printf("%-46s %s\n", "  and it is still called", name);
    check("which is B", (strcmp(name, "MST TEST B") == 0) ? 1 : 0, 1);

    MIDIEndpointDispose(b);
    settle();
    ms_midi_refresh_if_changed();
    check("B gone means listening to nothing", ms_midi_listening(), -1);
    ms_midi_listen(-1);
}

int main(void) {
    MIDIClientRef client = 0;

    // A SECOND CLIENT, deliberately: msMidi has its own and this one stands in for the rest of the
    // world. Its endpoints are as real to CoreMIDI as any interface's.
    MIDIClientCreate(CFSTR("MidiSyncTool list test"), NULL, NULL, &client);

    test_destination_appears(client);
    test_listening_follows_its_endpoint(client);

    printf("\n%s\n", (gFailures == 0) ? "all checks passed" : "SOMETHING FAILED - see above");
    return (gFailures == 0) ? 0 : 1;
}
