/*
 * MidiSyncTool - headless timing driver.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

// A HOST THAT LIES THE WAY ABLETON LIES.
//
// The point of this is not to be a correct VST3 host. It is to hand the plug-in a ProcessContext
// that behaves the way Live 12.4.5 was MEASURED to behave on 2026-09-02 (see Docs/findings.txt),
// including the parts that are arguably wrong, so that clock generation can be developed and
// regression-tested without a DAW and without CT sitting in front of one.
//
// An idealised host would be worse than useless here: it would let a scheduler be written against
// continuous musical time, which is exactly the assumption Live breaks at a loop wrap.
//
// What is reproduced, and why each one matters:
//
//   * systemTime is ALWAYS 0 and kSystemTimeValid is NEVER set. Live does not provide it, whatever
//     the plug-in asks for through IProcessContextRequirements, so anything built on it would work
//     here and fail there.
//   * continousTimeSamples is always 0, same reason.
//   * The block is NEVER split at a loop boundary. Every block is the full size, so a wrap always
//     falls inside one.
//   * AT A WRAP, projectTimeMusic and projectTimeSamples SNAP TO EXACTLY THE LOOP START for one
//     block regardless of where inside that block the wrap actually fell, and the true sub-block
//     phase only appears in the FOLLOWING block. This is the quirk the whole tool exists to model:
//     measured at 130 BPM, a block starting at ppq 3.998222 should have ended at 4.009778, and
//     instead the next two blocks reported 0.000000 and 0.009750 - the 216 samples being 256 minus
//     the 39.4 the wrap fell into the block.
//
// usage: mstDriver <plugin.vst3> [--bpm N] [--bars N] [--seconds N] [--rate N] [--block N] [--realtime]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>

#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>

extern "C" {
#include "msDevice.h"
#include "msRing.h"
}

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// ── The clock listener ───────────────────────────────────────────────────────────────────────────
//
// The driver runs the plug-in FASTER THAN REALTIME by default, which is right for exercising the
// wrap logic and wrong for measuring delivery - CoreMIDI will not deliver a packet stamped for a
// moment that has already passed, and in fast mode most of them are. So the listener is only
// meaningful with --realtime, and says so rather than reporting a misleading figure.
#define MAX_TICKS    (4096)

static UInt64 gArrival[MAX_TICKS];
static int    gTicks = 0;

// THE TEMPO IN FORCE AS EACH TICK LANDED, which a ramp makes necessary. With a fixed tempo the
// target interval is one number and the report can use it for every tick; under a ramp the target
// moves, and measuring a moving grid against a fixed target would report the RAMP as jitter - tens
// of milliseconds of it - and drown the thing being looked for.
static _Atomic double gRampBpm;
static double         gBpmAt[MAX_TICKS];

// ── A DRUM MACHINE WITH A KNOWN LATENCY ──────────────────────────────────────────────────────────
//
// --echo-ms D makes the harness impersonate a device: it hears the probe's note-on and writes a
// click into the input D milliseconds later. That closes the loop through real CoreMIDI delivery
// with an answer whose correct value is known in advance, which is the only way to test a
// calibration without already trusting the thing being calibrated.
//
// A drum machine could be simulated far more elaborately. It should not be: every feature added
// here is one the measurement then depends on being right.
#define ECHO_PENDING    (32)

static _Atomic uint64_t gEchoAt[ECHO_PENDING];
static _Atomic int      gEchoWrite;
static double           gEchoMs;

static void clock_listener(const MIDIPacketList * list, void * a, void * b) {
    (void)a;
    (void)b;
    const MIDIPacket * packet = &list->packet[0];

    for (UInt32 i = 0; i < list->numPackets; i++) {
        for (UInt16 j = 0; j < packet->length; j++) {
            // The WALL CLOCK on arrival, never the packet's own stamp - a virtual port passes the
            // sender's through untouched, and reading it back measures nothing but CoreMIDI's
            // ability to copy a number. That mistake produced a flattering "0.0000 ms RMS" once
            // already; see Docs/findings.txt.
            Byte b = packet->data[j];

            // A note-on, which only the probe sends. Its echo is scheduled from the moment it
            // ARRIVED, so whatever CoreMIDI's delivery costs is inside the measured figure exactly
            // as a real device's would be.
            if ((gEchoMs > 0.0) && ((b & 0xF0) == 0x90) && ((j + 2) < packet->length)
                && (packet->data[j + 2] > 0)) {
                int slot = atomic_fetch_add(&gEchoWrite, 1) % ECHO_PENDING;

                atomic_store(&gEchoAt[slot],
                             AudioGetCurrentHostTime()
                             + AudioConvertNanosToHostTime((UInt64)(gEchoMs * 1.0e6)));
            }

            if ((b == 0xF8) && (gTicks < MAX_TICKS)) {
                gBpmAt[gTicks]     = atomic_load(&gRampBpm);
                gArrival[gTicks++] = AudioGetCurrentHostTime();
            } else if ((b == 0xFA) || (b == 0xFB) || (b == 0xFC) || (b == 0xF2)) {
                // The transport bytes, printed as they arrive so the ORDER can be checked - an SPP
                // that lands after its Continue is useless, and only the sequence shows that.
                printf("  <- %s\n",
                       (b == 0xFA) ? "START" : ((b == 0xFB) ? "CONTINUE"
                                                : ((b == 0xFC) ? "STOP" : "SPP")));
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

// ── One block of host duty ───────────────────────────────────────────────────────────────────────
//
// Extracted so that the SAME code runs whether blocks are paced by a sleeping loop or by a real
// audio device's callback. Two copies of the ctx-filling logic would drift apart, and the entire
// value of this harness is that it behaves the way Live was measured to behave.
struct tDriverState {
    IAudioProcessor * processor;
    ProcessData *     data;
    ProcessContext *  ctx;
    double            bpm;
    double            baseBpm;    // --ramp: where the ramp starts, so bpm can be recomputed per block
    double            rampToBpm;  // and where it ends; 0 = no ramp
    double            rate;
    double            qnSamples;
    double            loopEndQn;
    double            startFrom;
    int32             block;
    int64             blocksDone;
    int64             totalBlocks;
    double            truePosQn;
    bool              snapNext;
    bool              snapped;

    // SYNTHETIC TRANSIENTS, so the detector's arithmetic can be proved without a drum machine.
    // A click is written into the input buffer a known number of milliseconds after every quarter
    // note, and the detector should report that number back.
    //
    // MINUS THE LOOKAHEAD, and that is not an error. The clock stamps each tick MS_LOOKAHEAD_MS in
    // the future because that is genuinely when CoreMIDI will put it on the wire, and the latency is
    // measured from the stamp. A click injected D ms after the musical grid therefore arrives
    // D - lookahead after the tick it answers. With the default 10 ms lookahead, --inject-ms 20
    // should read 10.
    double            injectMs;
    float **          injectInto;
    int64             samplesPlayed;
    double            clickLeft;      // samples remaining in the click currently being written
    bool              neverPlay;      // --stopped: the probe alone, with the device's sequencer idle
    UInt64            echoWindowEnd;  // where the last echo window stopped - see the note below
};

static void driver_step(tDriverState * st) {
    // A TEMPO RAMP, linear in wall time, which is what Live's tempo automation delivers and what the
    // model's tempo handling had never been exercised against. A STEP change is the easy case: it
    // happens once and the model has seconds to recover. A ramp reports a change on EVERY BLOCK,
    // which is the case that found the rate term being reset before it could ever converge.
    if (st->rampToBpm > 0.0) {
        double through = (st->totalBlocks > 1)
                         ? ((double)st->blocksDone / (double)(st->totalBlocks - 1)) : 0.0;

        st->bpm       = st->baseBpm + ((st->rampToBpm - st->baseBpm) * through);
        st->qnSamples = (st->rate * 60.0) / st->bpm;
        atomic_store(&gRampBpm, st->bpm);
    }
    double blockQn = (double)st->block / st->qnSamples;

    // Live's report: normally the true position, but for exactly one block after a wrap it is the
    // loop start itself, whatever the true position is.
    double reportQn = st->snapNext ? st->startFrom : (st->startFrom + st->truePosQn);

    if (st->snapNext) {
        st->snapped  = true;
        st->snapNext = false;
    }

    // A TRANSPORT CYCLE, so the Start/Stop/Continue edges are exercised rather than assumed:
    // stopped for the first tenth, playing through the middle, stopped for the last tenth. With
    // --from N the play begins at bar N instead of zero, which is what forces SPP + Continue rather
    // than Start.
    // THE NOTE PROBE AND THE CLOCK CANNOT BE MEASURED AT THE SAME TIME. With the transport running
    // the drum machine's own sequencer answers the clock, and those transients are indistinguishable
    // from the probe's - the first real run reported 26 hits and 29 spurious for exactly that
    // reason. --stopped leaves the transport alone so the probe measures the note path on its own.
    bool playing = !st->neverPlay
                   && (st->blocksDone > (st->totalBlocks / 10))
                   && (st->blocksDone < (st->totalBlocks - (st->totalBlocks / 10)));

    st->ctx->state = ProcessContext::kTempoValid
                     | ProcessContext::kProjectTimeMusicValid
                     | ProcessContext::kBarPositionValid
                     | ProcessContext::kTimeSigValid;

    if (playing) {
        st->ctx->state |= ProcessContext::kPlaying;

        if (st->loopEndQn > 0.0) {
            st->ctx->state |= ProcessContext::kCycleActive;
            st->ctx->cycleStartMusic = 0.0;
            st->ctx->cycleEndMusic   = st->loopEndQn;
        }
    } else {
        reportQn = st->startFrom;
    }
    st->ctx->tempo              = st->bpm;
    st->ctx->timeSigNumerator   = 4;
    st->ctx->timeSigDenominator = 4;
    st->ctx->projectTimeMusic   = reportQn;
    st->ctx->barPositionMusic   = floor(reportQn / 4.0) * 4.0;
    st->ctx->projectTimeSamples = (TSamples)llround(reportQn * st->qnSamples);

    // NOT SET, and that is the whole point of this harness - see the header.
    st->ctx->systemTime           = 0;
    st->ctx->continousTimeSamples = 0;

    // ECHOES, written BEFORE process() for the same reason as the grid clicks below.
    if ((gEchoMs > 0.0) && (st->injectInto != nullptr)) {
        UInt64 now     = AudioGetCurrentHostTime();
        double blockNs = ((double)st->data->numSamples / st->rate) * 1.0e9;

        // CONTIGUOUS WINDOWS, each starting exactly where the last one stopped.
        //
        // Reading the clock afresh each block and taking [now, now + blockDuration) leaves a sliver
        // between one window's end and the next one's start, because the callback is entered a
        // little later each time by however much it jitters. An echo landing in that sliver was
        // being clamped to sample 0 - late by an arbitrary fraction of a block.
        //
        // It stayed hidden until latency compensation moved the echoes onto a block boundary, and
        // then it read as 0.578 ms of jitter that belonged entirely to this harness. The probe grid
        // is 400 ms and the block 10.667 ms - exactly 37.5 blocks - so the phase between them is
        // fixed, and a shift of 7.5 ms was enough to park every echo on the seam.
        UInt64 windowStart = (st->echoWindowEnd != 0) ? st->echoWindowEnd : now;
        UInt64 windowEnd   = windowStart + AudioConvertNanosToHostTime((UInt64)blockNs);

        if (windowEnd < now) {
            // Fallen behind reality - a dropout, or the first block. Re-anchor rather than spend
            // the next several blocks catching up.
            windowStart = now;
            windowEnd   = now + AudioConvertNanosToHostTime((UInt64)blockNs);
        }
        st->echoWindowEnd = windowEnd;

        memset(st->injectInto[0], 0, (size_t)st->data->numSamples * sizeof(float));
        memset(st->injectInto[1], 0, (size_t)st->data->numSamples * sizeof(float));

        for (int e = 0; e < ECHO_PENDING; e++) {
            UInt64 due = atomic_load(&gEchoAt[e]);

            if ((due == 0) || (due >= windowEnd)) {
                continue;
            }
            atomic_store(&gEchoAt[e], (UInt64)0);

            int32 at = (due > windowStart)
                       ? (int32)(((double)AudioConvertHostTimeToNanos(due - windowStart) / 1.0e9) * st->rate)
                       : 0;

            if (at >= st->data->numSamples) {
                at = st->data->numSamples - 1;
            }

            for (int32 i = at; (i < st->data->numSamples) && (i < (at + (int32)(0.002 * st->rate))); i++) {
                float value = 0.8f * (1.0f - ((float)(i - at) / (0.002f * (float)st->rate)));

                st->injectInto[0][i] = value;
                st->injectInto[1][i] = value;
            }
        }
    }

    // Written BEFORE process(), because the plug-in reads the input buffer during it.
    if ((st->injectMs > 0.0) && (st->injectInto != nullptr) && playing) {
        double periodSamples = st->qnSamples;
        double offsetSamples = (st->injectMs / 1000.0) * st->rate;

        for (int32 i = 0; i < st->data->numSamples; i++) {
            double since = (double)(st->samplesPlayed + i) - offsetSamples;

            if ((since >= 0.0) && (fmod(since, periodSamples) < 1.0)) {
                st->clickLeft = 0.002 * st->rate;   // a 2 ms click, ample for a 150 Hz high pass
            }
            float value = 0.0f;

            if (st->clickLeft > 0.0) {
                value = 0.8f * (float)(st->clickLeft / (0.002 * st->rate));
                st->clickLeft -= 1.0;
            }
            st->injectInto[0][i] = value;
            st->injectInto[1][i] = value;
        }
        st->samplesPlayed += st->data->numSamples;
    }
    st->processor->process(*st->data);

    // ONE BLOCK ONLY. A host sends a parameter change when it changes; resending it every block
    // would hide a plug-in that only ever acts on the first one it sees.
    if (st->blocksDone == 0) {
        st->data->inputParameterChanges = nullptr;
    }
    st->blocksDone++;

    if (!playing) {
        return;
    }

    // THE REPORT LAGS BY ONE BLOCK ACROSS A WRAP, which is the crux. Live reports the block after
    // the wrap as the loop start EXACTLY, and the block after THAT at the remainder the wrap
    // actually left - not at remainder-plus-a-block. Measured: 3.998222, then 0.000000, then
    // 0.009750 with projectTimeSamples 216, where 256 - 39.4 = 216.6.
    //
    // So the snap block does not advance the true position: it is the one that absorbs the
    // discrepancy. Advancing through it put this harness a whole block ahead of Live and would have
    // had a scheduler developed against it come out systematically early.
    if (st->snapped) {
        st->snapped = false;
    } else {
        st->truePosQn += blockQn;
    }

    if ((st->loopEndQn > 0.0) && (st->truePosQn >= st->loopEndQn)) {
        st->truePosQn -= st->loopEndQn;
        st->snapNext   = true;
    }
}

// ── Real audio ───────────────────────────────────────────────────────────────────────────────────
//
// WHY THE HARNESS NEEDED A DEVICE AT ALL. Until now it handed the plug-in silent buffers and threw
// the output away, which is fine for testing clock arithmetic and useless for everything section 3
// of the concept describes: the plug-in is an Fx precisely so it can hear the hardware answer its
// own clock, and none of that can be developed against silence.
//
// It also fixes a measurement problem. The sleeping loop paces blocks with usleep, whose overshoot
// showed up as ~1 ms of block-period jitter and dragged the delivered clock out with it. A device
// callback is the real thing at the real priority, which is what Live gives the plug-in.
struct tAudioBridge {
    tDriverState * st;
    tRing *        capture;
    float **       inChannels;
    float **       outChannels;
    uint32_t       channels;
    volatile bool  finished;
    // WORTH PRINTING, because "no audio arrived" and "audio arrived and was silent" look identical
    // from every other number this harness reports, and the first is a routing mistake.
    volatile float inputPeak;
};

static void audio_input_cb(void * user, const float * input, float * output, uint32_t frames) {
    tAudioBridge * bridge = (tAudioBridge *)user;

    (void)output;

    for (uint32_t i = 0; i < (frames * 2); i++) {
        float level = fabsf(input[i]);

        if (level > bridge->inputPeak) {
            bridge->inputPeak = level;
        }
    }
    ring_write(bridge->capture, input, frames);
}

static void audio_output_cb(void * user, const float * input, float * output, uint32_t frames) {
    tAudioBridge * bridge = (tAudioBridge *)user;

    (void)input;

    if (bridge->st->blocksDone >= bridge->st->totalBlocks) {
        memset(output, 0, (size_t)frames * bridge->channels * sizeof(float));
        bridge->finished = true;
        return;
    }
    // DEINTERLEAVE IN, RUN, INTERLEAVE OUT. VST3 wants one buffer per channel; CoreAudio hands over
    // one interleaved buffer. There is no way round the copy and at two channels it is nothing.
    float interleaved[4096 * 2];
    uint32_t take = (frames * bridge->channels <= 4096 * 2) ? frames : (4096 * 2 / bridge->channels);

    ring_read(bridge->capture, interleaved, take);

    for (uint32_t f = 0; f < take; f++) {
        for (uint32_t c = 0; c < bridge->channels; c++) {
            bridge->inChannels[c][f] = interleaved[(f * bridge->channels) + c];
        }
    }
    bridge->st->data->numSamples = (int32)take;
    driver_step(bridge->st);

    for (uint32_t f = 0; f < take; f++) {
        for (uint32_t c = 0; c < bridge->channels; c++) {
            output[(f * bridge->channels) + c] = bridge->outChannels[c][f];
        }
    }
}

// ── A HOST APPLICATION, minimally ────────────────────────────────────────────────────────────────
//
// The plug-in hands its status-slot number to its controller over IConnectionPoint, and an IMessage
// can only be made by the HOST - IHostApplication::createInstance is the only source of one. A
// driver that passes nullptr as the initialise() context therefore never exercises that path at
// all, and the first place it would be tried is the first place it must not fail.
//
// Just enough of one to make messages and to answer its own name.
class DriverMessage : public IMessage, public IAttributeList {
public:
    DriverMessage(void) : refCount(1) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IMessage::iid)) {
            addRef();
            *obj = static_cast<IMessage *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE  { return (uint32)++refCount; }
    uint32 PLUGIN_API release(void) SMTG_OVERRIDE {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return (uint32)refCount;
    }

    FIDString PLUGIN_API getMessageID(void) SMTG_OVERRIDE { return id; }
    void PLUGIN_API setMessageID(FIDString newId) SMTG_OVERRIDE {
        strncpy(id, newId, sizeof(id) - 1);
    }
    IAttributeList * PLUGIN_API getAttributes(void) SMTG_OVERRIDE { return this; }

    tresult PLUGIN_API setInt(AttrID, int64 value) SMTG_OVERRIDE {
        intValue = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID, int64 & value) SMTG_OVERRIDE {
        value = intValue;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID, double) SMTG_OVERRIDE       { return kResultFalse; }
    tresult PLUGIN_API getFloat(AttrID, double &) SMTG_OVERRIDE     { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const TChar *) SMTG_OVERRIDE { return kResultFalse; }
    tresult PLUGIN_API getString(AttrID, TChar *, uint32) SMTG_OVERRIDE { return kResultFalse; }
    tresult PLUGIN_API setBinary(AttrID, const void *, uint32) SMTG_OVERRIDE { return kResultFalse; }
    tresult PLUGIN_API getBinary(AttrID, const void *&, uint32 &) SMTG_OVERRIDE { return kResultFalse; }

private:
    int    refCount;
    char   id[128] = {0};
    int64  intValue = 0;
};

class DriverHost : public IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid)
            || FUnknownPrivate::iidEqual(iid, IHostApplication::iid)) {
            *obj = static_cast<IHostApplication *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE  { return 1; }
    uint32 PLUGIN_API release(void) SMTG_OVERRIDE { return 1; }

    tresult PLUGIN_API getName(String128 name) SMTG_OVERRIDE {
        static const char16_t text[] = u"mstDriver";

        memcpy(name, text, sizeof(text));
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID, void ** obj) SMTG_OVERRIDE {
        if (memcmp(cid, IMessage::iid.toTUID(), sizeof(TUID)) == 0) {
            *obj = static_cast<IMessage *>(new DriverMessage());
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

static DriverHost gDriverHost;

// ── Parameter changes ────────────────────────────────────────────────────────────────────────────
//
// The one route by which a host tells a plug-in that a control moved, and therefore the route that
// replaced the MST_* environment variables when it turned out a host launched from the Dock inherits
// no shell. Exercised here so that Live is not the first place it runs.
class DriverQueue : public IParamValueQueue {
public:
    DriverQueue(ParamID idIn, ParamValue valueIn) : id(idIn), value(valueIn) {}

    tresult PLUGIN_API queryInterface(const TUID, void ** obj) SMTG_OVERRIDE {
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE  { return 1; }
    uint32 PLUGIN_API release(void) SMTG_OVERRIDE { return 1; }

    ParamID PLUGIN_API getParameterId(void) SMTG_OVERRIDE { return id; }
    int32 PLUGIN_API getPointCount(void) SMTG_OVERRIDE    { return 1; }

    tresult PLUGIN_API getPoint(int32, int32 & sampleOffset, ParamValue & v) SMTG_OVERRIDE {
        sampleOffset = 0;
        v            = value;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32, ParamValue, int32 &) SMTG_OVERRIDE { return kNotImplemented; }

private:
    ParamID    id;
    ParamValue value;
};

class DriverChanges : public IParameterChanges {
public:
    void set(ParamID id, ParamValue value) {
        queues.push_back(new DriverQueue(id, value));
    }

    void clear(void) {
        for (auto * q : queues) {
            delete q;
        }
        queues.clear();
    }

    tresult PLUGIN_API queryInterface(const TUID, void ** obj) SMTG_OVERRIDE {
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE  { return 1; }
    uint32 PLUGIN_API release(void) SMTG_OVERRIDE { return 1; }

    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE { return (int32)queues.size(); }

    IParamValueQueue * PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE {
        return ((index >= 0) && (index < (int32)queues.size())) ? queues[(size_t)index] : nullptr;
    }

    IParamValueQueue * PLUGIN_API addParameterData(const ParamID &, int32 &) SMTG_OVERRIDE {
        return nullptr;
    }

private:
    std::vector<DriverQueue *> queues;
};

int main(int argc, const char ** argv) {
    if (argc < 2) {
        printf("usage: mstDriver <plugin.vst3> [--bpm N] [--bars N] [--seconds N]\n"
               "                  [--rate N] [--block N] [--realtime]\n"
               "\n"
               "  --bpm N       tempo (default 130, matching the measured capture)\n"
               "  --ramp N      ramp the tempo linearly from --bpm to N across the run, reporting a\n"
               "                change on every block the way Live's tempo automation does\n"
               "  --vary        hand over a DIFFERENT block size each call, between a quarter of\n"
               "                --block and all of it, the way Live splits its buffer. Offline only:\n"
               "                a real device's block size is the device's to choose\n"
               "  --bars N      loop length in bars of 4/4; 0 disables looping (default 1)\n"
               "  --seconds N   how much musical time to run (default 10)\n"
               "  --rate N      sample rate (default 48000)\n"
               "  --block N     block size (default 256)\n"
               "  --realtime    run at one second per second; the default is as fast as it will go\n"
               "  --audio NAME  run the plug-in from a real CoreAudio device (implies --realtime)\n"
               "  --in-ch N     first device INPUT channel, zero based (default 0)\n"
               "  --out-ch N    first device OUTPUT channel, zero based (default 0)\n"
               "  --inject-ms D synthesise a click D ms after every quarter note, to prove the\n"
               "                transient detector without hardware (implies --realtime)\n"
               "  --echo-ms D   impersonate a drum machine with D ms of latency: answer the note\n"
               "                probe with a click D ms later (implies --realtime)\n"
               "  --stopped     never start the transport, so the note probe measures the note\n"
               "                path alone with the device's own sequencer idle\n"
               "  --param ID V  send parameter ID the normalised value V on the first block\n");
        return 1;
    }
    double bpm      = 130.0;
    double rampToBpm = 0.0;
    bool   varyBlocks = false;
    double startFrom = 0.0;
    double bars     = 1.0;
    double seconds  = 10.0;
    double rate     = 48000.0;
    int32  block    = 256;
    bool   realtime = false;
    const char * audioDevice = nullptr;
    double       injectMs    = 0.0;
    bool         neverPlay   = false;
    int          paramId     = -1;
    double       paramValue  = 0.0;
    uint32_t     inFirst     = 0;
    uint32_t     outFirst    = 0;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--bpm") == 0) && ((i + 1) < argc)) {
            bpm = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--ramp") == 0) && ((i + 1) < argc)) {
            rampToBpm = atof(argv[++i]);
            realtime  = true;   // a ramp is only meaningful against the wall clock
        } else if ((strcmp(argv[i], "--bars") == 0) && ((i + 1) < argc)) {
            bars = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--seconds") == 0) && ((i + 1) < argc)) {
            seconds = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--rate") == 0) && ((i + 1) < argc)) {
            rate = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--block") == 0) && ((i + 1) < argc)) {
            block = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--from") == 0) && ((i + 1) < argc)) {
            startFrom = atof(argv[++i]) * 4.0;   // bars of 4/4 to quarter notes
        } else if ((strcmp(argv[i], "--audio") == 0) && ((i + 1) < argc)) {
            audioDevice = argv[++i];
            realtime    = true;   // a device paces itself; nothing else would make sense
        } else if ((strcmp(argv[i], "--in-ch") == 0) && ((i + 1) < argc)) {
            inFirst = (uint32_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--out-ch") == 0) && ((i + 1) < argc)) {
            outFirst = (uint32_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--inject-ms") == 0) && ((i + 1) < argc)) {
            injectMs = atof(argv[++i]);
            realtime = true;   // the detector times against the wall clock, so it must be one
        } else if ((strcmp(argv[i], "--echo-ms") == 0) && ((i + 1) < argc)) {
            gEchoMs  = atof(argv[++i]);
            realtime = true;
        } else if ((strcmp(argv[i], "--param") == 0) && ((i + 2) < argc)) {
            paramId    = atoi(argv[++i]);
            paramValue = atof(argv[++i]);
        } else if (strcmp(argv[i], "--vary") == 0) {
            varyBlocks = true;
            realtime   = true;
        } else if (strcmp(argv[i], "--stopped") == 0) {
            neverPlay = true;
            realtime  = true;
        } else if (strcmp(argv[i], "--realtime") == 0) {
            realtime = true;
        }
    }

    // THE DEVICE DECIDES THE RATE AND THE BLOCK SIZE, so it has to be found before the plug-in is
    // told either. Asking a device for 48000/256 and then telling the plug-in it got them, when the
    // device quietly kept 44100/512, is the kind of mismatch that produces a timing bug nobody can
    // find.
    tDeviceInfo audioInfo = {};

    if (audioDevice != nullptr) {
        if (!device_find(audioDevice, true, &audioInfo)) {
            printf("no input-capable device matching \"%s\"\n", audioDevice);
            return 2;
        }

        if (device_is_running_somewhere(audioInfo.id)) {
            printf("NOTE: %s is already running for something else - rate and buffer size are\n"
                   "      device-wide, so they are being left alone.\n", audioInfo.name);
        } else {
            device_set_sample_rate_and_wait(audioInfo.id, rate);
            device_set_buffer_frames(audioInfo.id, (uint32_t)block);
        }
        rate  = device_sample_rate(audioInfo.id);
        block = (int32)device_buffer_frames(audioInfo.id);

        printf("audio: %s - %u in / %u out, %.0f Hz, %d-frame buffer\n"
               "       in from channel %u, out to channel %u (zero based)\n",
               audioInfo.name, audioInfo.inputChannels, audioInfo.outputChannels,
               rate, block, inFirst, outFirst);
    }
    char path[1024];

    snprintf(path, sizeof(path), "%s/Contents/MacOS/MidiSyncTool", argv[1]);

    void * lib = dlopen(path, RTLD_NOW);

    if (lib == nullptr) {
        printf("dlopen failed: %s\n", dlerror());
        return 2;
    }
    auto entry = (bool (*)(void *))dlsym(lib, "bundleEntry");

    if (entry != nullptr) {
        entry(nullptr);
    }
    auto getFactory = (IPluginFactory * (*)())dlsym(lib, "GetPluginFactory");

    if (getFactory == nullptr) {
        printf("no GetPluginFactory export\n");
        return 2;
    }
    IPluginFactory * factory = getFactory();
    PClassInfo       info;
    IComponent *     component = nullptr;

    for (int32 i = 0; (i < factory->countClasses()) && (component == nullptr); i++) {
        if ((factory->getClassInfo(i, &info) == kResultTrue)
            && (strcmp(info.category, kVstAudioEffectClass) == 0)) {
            factory->createInstance(info.cid, IComponent::iid, (void **)&component);
        }
    }

    if (component == nullptr) {
        printf("no audio class found\n");
        return 2;
    }
    component->initialize(&gDriverHost);

    // THE CONTROLLER, AND THE CONNECTION BETWEEN THE TWO. A host that instantiates only the
    // processor never sees the panel's figures at all, so a driver that does the same cannot tell
    // whether they would have arrived.
    IEditController * controller = nullptr;
    TUID              controllerCid;

    if (component->getControllerClassId(controllerCid) == kResultOk) {
        factory->createInstance(controllerCid, IEditController::iid, (void **)&controller);
    }

    if (controller != nullptr) {
        controller->initialize(&gDriverHost);

        IConnectionPoint * fromComponent = nullptr;
        IConnectionPoint * fromController = nullptr;

        component->queryInterface(IConnectionPoint::iid, (void **)&fromComponent);
        controller->queryInterface(IConnectionPoint::iid, (void **)&fromController);

        if ((fromComponent != nullptr) && (fromController != nullptr)) {
            fromComponent->connect(fromController);
            fromController->connect(fromComponent);
            printf("controller connected\n");
        } else {
            printf("WARNING: no IConnectionPoint - the panel would show nothing\n");
        }
        IPlugView * view = controller->createView(ViewType::kEditor);

        if (view != nullptr) {
            printf("editor view created\n");
            view->release();
        } else {
            printf("WARNING: createView returned nothing\n");
        }
    } else {
        printf("WARNING: no controller class\n");
    }

    IAudioProcessor * processor = nullptr;

    if (component->queryInterface(IAudioProcessor::iid, (void **)&processor) != kResultOk) {
        printf("no IAudioProcessor\n");
        return 2;
    }
    ProcessSetup setup = {};

    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = block;
    setup.sampleRate         = rate;
    processor->setupProcessing(setup);

    component->setActive(true);
    processor->setProcessing(true);

    // Musical geometry. A quarter note in samples, and the loop end in quarter notes.
    const double qnSamples = (rate * 60.0) / bpm;
    const double loopEndQn = (bars > 0.0) ? (bars * 4.0) : 0.0;
    const int64  totalBlocks = (int64)((seconds * rate) / (double)block);

    std::vector<float> left((size_t)block, 0.0f);
    std::vector<float> right((size_t)block, 0.0f);
    float *            inCh[2]  = { left.data(), right.data() };
    float *            outCh[2] = { left.data(), right.data() };

    AudioBusBuffers inBus  = {};
    AudioBusBuffers outBus = {};

    inBus.numChannels       = 2;
    inBus.channelBuffers32  = inCh;
    outBus.numChannels      = 2;
    outBus.channelBuffers32 = outCh;

    ProcessContext ctx = {};
    ProcessData    data = {};

    data.processMode          = kRealtime;
    data.symbolicSampleSize   = kSample32;
    data.numSamples           = block;
    data.numInputs            = 1;
    data.numOutputs           = 1;
    data.inputs               = &inBus;
    data.outputs              = &outBus;
    data.processContext       = &ctx;

    DriverChanges changes;

    if (paramId >= 0) {
        changes.set((ParamID)paramId, paramValue);
        data.inputParameterChanges = &changes;
        printf("parameter %d -> %.4f on the first block\n", paramId, paramValue);
    }

    // THE POSITION THE HOST BELIEVES IT IS AT, in quarter notes, advanced continuously. What gets
    // REPORTED is derived from it and is deliberately not the same thing at a wrap.
    UInt64       realtimeStart = 0;
    tDriverState st = {};

    st.processor   = processor;
    st.data        = &data;
    st.ctx         = &ctx;
    st.bpm         = bpm;
    st.baseBpm     = bpm;
    st.rampToBpm   = rampToBpm;
    st.rate        = rate;
    st.qnSamples   = qnSamples;
    st.loopEndQn   = loopEndQn;
    st.startFrom   = startFrom;
    st.block       = block;
    st.totalBlocks = totalBlocks;
    st.neverPlay   = neverPlay;
    st.injectMs    = injectMs;
    st.injectInto  = ((injectMs > 0.0) || (gEchoMs > 0.0)) ? inCh : nullptr;

    // Listen on whatever source the plug-in was pointed at, so its own output can be timed.
    const char *    listenTo = getenv("MST_MIDI_DEST");
    MIDIClientRef   listenClient = 0;
    MIDIPortRef     listenPort   = 0;

    if ((listenTo != NULL) && (realtime || (gEchoMs > 0.0))) {
        MIDIClientCreate(CFSTR("mstDriver"), NULL, NULL, &listenClient);
        MIDIInputPortCreate(listenClient, CFSTR("in"), clock_listener, NULL, &listenPort);

        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
            MIDIEndpointRef src  = MIDIGetSource(i);
            CFStringRef     name = NULL;
            char            buf[128] = {0};

            MIDIObjectGetStringProperty(src, kMIDIPropertyName, &name);

            if (name != NULL) {
                CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
                CFRelease(name);
            }

            // EITHER DIRECTION. A CoreMIDI source and destination on the same virtual port are not
            // obliged to share a name, and IAC does not: the destination reads "IAC Driver Bus 1"
            // and the source just "Bus 1". A one-directional substring test silently connects to
            // nothing and reports no measurement at all, which reads as "the clock never arrived".
            if ((strstr(buf, listenTo) != NULL) || (strstr(listenTo, buf) != NULL)) {
                MIDIPortConnectSource(listenPort, src, NULL);
                printf("listening for clock on '%s'\n", buf);
            }
        }
    }

    atomic_store(&gRampBpm, bpm);

    if (rampToBpm > 0.0) {
        printf("tempo ramp: %.3f -> %.3f BPM across the run\n", bpm, rampToBpm);
    }

    if (varyBlocks) {
        printf("block size: VARYING, %d..%d frames per call\n", (block > 4) ? (block / 4) : 1, block);
    }
    printf("driving %s\n  %.3f BPM, %.0f Hz, %d-frame blocks, loop %s\n",
           argv[1], bpm, rate, block,
           (loopEndQn > 0.0) ? "on" : "off");

    if (audioDevice != nullptr) {
        // TWO STREAMS ON ONE DEVICE, input into a ring and output pulling from it. They are separate
        // IOProcs and fire in the same cycle, so the ring never needs to hold much - a few blocks is
        // enough to absorb whichever of the two the HAL happens to call first.
        tRing         capture = {};
        tDeviceStream inStream = {};
        tDeviceStream outStream = {};
        tAudioBridge  bridge = {};

        ring_init(&capture, (uint32_t)block * 8, 2);
        ring_reset(&capture);

        bridge.st          = &st;
        bridge.capture     = &capture;
        bridge.inChannels  = inCh;
        bridge.outChannels = outCh;
        bridge.channels    = 2;
        bridge.finished    = false;

        if (!device_open(&inStream, audioInfo.id, true, inFirst, 2, (uint32_t)block,
                         audio_input_cb, &bridge)
            || !device_open(&outStream, audioInfo.id, false, outFirst, 2, (uint32_t)block,
                            audio_output_cb, &bridge)) {
            printf("could not open the device streams\n");
            return 2;
        }
        device_start(&inStream);
        device_start(&outStream);

        // PRIMED WITH ONE BLOCK so the very first output callback has something to read rather than
        // counting an underrun before the input stream has ever run.
        while (!bridge.finished && (st.blocksDone < totalBlocks)) {
            usleep(20000);
        }
        device_stop(&outStream);
        device_stop(&inStream);
        device_close(&outStream);
        device_close(&inStream);

        printf("input peak: %.4f (%.1f dBFS)%s\n",
               bridge.inputPeak,
               (bridge.inputPeak > 0.0f) ? (20.0 * log10((double)bridge.inputPeak)) : -999.0,
               (bridge.inputPeak <= 0.0f) ? "  <- NOTHING ARRIVED, check the routing" : "");
        printf("capture ring: %u overflow(s), %u underrun(s)\n",
               atomic_load(&capture.overflows), atomic_load(&capture.underflows));
        ring_free(&capture);
    } else {
        int64 samplesEmitted = 0;
        int64 totalSamples   = (int64)(seconds * rate);
        unsigned varySeed    = 12345u;

        for (int64 b = 0; (varyBlocks ? (samplesEmitted < totalSamples) : (b < totalBlocks)); b++) {
            // A VARYING BLOCK SIZE, which is the one Live behaviour this harness never reproduced -
            // and the one that hid an off-by-one in every per-block figure for a day. Between a
            // quarter of --block and all of it, which is the shape Live's splitting produces.
            if (varyBlocks) {
                varySeed = (varySeed * 1103515245u) + 12345u;

                int32 quarter = (st.block > 4) ? (block / 4) : 1;
                int32 size    = quarter + (int32)((varySeed >> 16) % (unsigned)(block - quarter + 1));

                st.block            = size;
                st.data->numSamples = size;
            }
            driver_step(&st);
            samplesEmitted += st.data->numSamples;

            if (realtime) {
                // AN ABSOLUTE DEADLINE, not a sleep of one block's length. usleep() always
                // overshoots, and sleeping a fixed amount each time accumulates that overshoot -
                // this harness ran 20% slow (wall +6.4 ms against the 5.33 ms it was aiming for),
                // which dragged every scheduled tick out with it and produced a 3.3 ms mean error
                // that looked like a defect in the plug-in and was entirely mine.
                //
                // Sleeping until block N is DUE keeps the wall clock locked to musical time however
                // badly any individual sleep behaves.
                realtimeStart = (realtimeStart == 0) ? AudioGetCurrentHostTime() : realtimeStart;

                double dueSamples = varyBlocks ? (double)samplesEmitted
                                               : ((double)(b + 1) * (double)block);
                UInt64 due = realtimeStart
                             + AudioConvertNanosToHostTime((UInt64)((dueSamples / rate) * 1.0e9));
                UInt64 now = AudioGetCurrentHostTime();

                if (due > now) {
                    usleep((useconds_t)(AudioConvertHostTimeToNanos(due - now) / 1000ULL));
                }
            }
        }
    }
    processor->setProcessing(false);
    component->setActive(false);
    component->terminate();
    processor->release();
    component->release();

    printf("done: %lld blocks (%.1f s of musical time)\n", (long long)totalBlocks, seconds);

    if (gTicks > 2) {
        // 24 PPQN, so a tick every quarter-note/24 - the interval the generator was aiming for.
        double target = 60000.0 / (bpm * 24.0);
        double sum = 0.0, sumsq = 0.0, worst = 0.0;

        // UNDER A RAMP THE TARGET MOVES WITH THE TICK. gBpmAt is the tempo in force as that tick
        // landed, so each interval is judged against the grid that was actually being generated.
        #define TICK_TARGET(i)    ((rampToBpm > 0.0) ? (60000.0 / (gBpmAt[i] * 24.0)) : target)

        for (int i = 1; i < gTicks; i++) {
            double ms  = (double)AudioConvertHostTimeToNanos(gArrival[i] - gArrival[i - 1]) / 1.0e6;
            double err = ms - TICK_TARGET(i);

            sum += err;
            sumsq += err * err;

            if (fabs(err) > fabs(worst)) {
                worst = err;
            }
        }
        int n = gTicks - 1;

        // WHERE the outliers fall matters more than how big they are. If they are evenly scattered
        // it is scheduling noise; if they land a fixed number of ticks apart, something structural
        // is happening at that period - and 96 ticks is exactly one bar of 4/4 at 24 PPQN.
        int lastBad = -1;

        printf("  outliers over 2 ms:\n");

        for (int i = 1; i < gTicks; i++) {
            double ms  = (double)AudioConvertHostTimeToNanos(gArrival[i] - gArrival[i - 1]) / 1.0e6;
            double err = ms - TICK_TARGET(i);

            if (fabs(err) > 2.0) {
                printf("    tick %-4d  interval %7.3f ms  err %+7.3f ms%s\n", i, ms, err,
                       (lastBad >= 0) ? "" : "");

                if (lastBad >= 0) {
                    printf("               (%d ticks since the last one)\n", i - lastBad);
                }
                lastBad = i;
            }
        }
        if (rampToBpm > 0.0) {
            printf("clock delivered: %d ticks, target %.4f -> %.4f ms (ramped)\n",
                   gTicks, 60000.0 / (bpm * 24.0), 60000.0 / (rampToBpm * 24.0));
        } else {
            printf("clock delivered: %d ticks, target %.4f ms\n", gTicks, target);
        }
        printf("  interval error: mean %+.4f ms, RMS %.4f ms, worst %+.4f ms\n",
               sum / n, sqrt(sumsq / n), worst);
    } else if ((listenTo != NULL) && !realtime) {
        printf("(no delivery measurement: needs --realtime, since a packet stamped for a moment "
               "already past is not delivered)\n");
    }

    return 0;
}
