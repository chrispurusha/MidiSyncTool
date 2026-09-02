// Does CoreMIDI actually honour a FUTURE timestamp, and how tightly?
//
// The whole scheduling plan is: compute when each clock tick should sound, hand CoreMIDI a packet
// stamped for that host time, and let the driver deliver it precisely. That is an assumption. This
// sends 200 clock bytes at exactly 20.833 ms apart (24 PPQN at 120 BPM) down IAC, receives them on
// IAC's own source, and reports what the arrival spacing actually was.
#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>

#define N 200
static UInt64 arrival[N];
static int    got = 0;

static void read_cb(const MIDIPacketList * pl, void * a, void * b) {
    (void)a; (void)b;
    const MIDIPacket * p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        for (UInt16 j = 0; j < p->length; j++) {
            if ((p->data[j] == 0xF8) && (got < N)) {
                // THE WALL CLOCK ON RECEIPT, never the packet's own timestamp. IAC passes the
                // sender's timestamp through untouched, so reading it back measures whether
                // CoreMIDI can copy a number - which it can - and says nothing about delivery.
                arrival[got++] = AudioGetCurrentHostTime();
            }
        }
        p = MIDIPacketNext(p);
    }
}

static MIDIEndpointRef find(int dest, const char * want) {
    ItemCount n = dest ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = dest ? MIDIGetDestination(i) : MIDIGetSource(i);
        CFStringRef nm = NULL; char buf[128] = {0};
        MIDIObjectGetStringProperty(e, kMIDIPropertyName, &nm);
        if (nm) { CFStringGetCString(nm, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(nm); }
        if (strstr(buf, want)) return e;
    }
    return 0;
}

int main(void) {
    MIDIClientRef c; MIDIPortRef in, out;
    MIDIClientCreate(CFSTR("mtest"), NULL, NULL, &c);
    MIDIInputPortCreate(c, CFSTR("in"), read_cb, NULL, &in);
    MIDIOutputPortCreate(c, CFSTR("out"), &out);

    MIDIEndpointRef d = find(1, "Bus 1"), s = find(0, "Bus 1");
    if (!d || !s) { printf("no IAC Bus 1\n"); return 1; }
    MIDIPortConnectSource(in, s, NULL);

    const double stepMs = 60000.0 / (120.0 * 24.0);   // 24 PPQN at 120 BPM = 20.8333 ms
    UInt64 base = AudioGetCurrentHostTime() + AudioConvertNanosToHostTime(500000000ULL); // +500 ms

    // Two modes. SCHEDULED hands every packet over at once, each stamped for its own future
    // moment. IMMEDIATE sends each one with timestamp 0 from a sleeping loop - the shape of a
    // generator that emits at block boundaries instead of scheduling, which is the thing the whole
    // design is trying to avoid. Measuring both turns "scheduling is better" into a number.
    int immediate = (getenv("MTEST_IMMEDIATE") != NULL);

    if (immediate) {
        for (int i = 0; i < N; i++) {
            Byte b = 0xF8;
            MIDIPacketList pl;
            MIDIPacket * p = MIDIPacketListInit(&pl);
            p = MIDIPacketListAdd(&pl, sizeof(pl), p, 0, 1, &b);
            MIDISend(out, d, &pl);
            usleep((useconds_t)(stepMs * 1000.0));
        }
        printf("sent %d clocks, %.4f ms apart, IMMEDIATE from a sleeping loop\n", N, stepMs);
    } else {
        for (int i = 0; i < N; i++) {
            Byte b = 0xF8;
            MIDIPacketList pl;
            MIDIPacket * p = MIDIPacketListInit(&pl);
            UInt64 when = base + AudioConvertNanosToHostTime((UInt64)(i * stepMs * 1.0e6));
            p = MIDIPacketListAdd(&pl, sizeof(pl), p, when, 1, &b);
            MIDISend(out, d, &pl);
        }
        printf("sent %d clocks, %.4f ms apart, SCHEDULED ahead\n", N, stepMs);
    }
    usleep((useconds_t)((0.5 + N * stepMs / 1000.0 + 0.5) * 1e6));

    printf("received %d\n", got);
    if (got < 10) return 1;

    double sum = 0, sumsq = 0, worst = 0;
    for (int i = 1; i < got; i++) {
        double ms = (double)AudioConvertHostTimeToNanos(arrival[i] - arrival[i-1]) / 1e6;
        double err = ms - stepMs;
        sum += err; sumsq += err*err;
        if (fabs(err) > fabs(worst)) worst = err;
    }
    int n = got - 1;
    printf("interval error: mean %+.4f ms, RMS %.4f ms, worst %+.4f ms\n",
           sum/n, sqrt(sumsq/n), worst);
    return 0;
}
