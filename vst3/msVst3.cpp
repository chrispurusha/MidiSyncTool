/*
 * MidiSyncTool - a VST3 plug-in for MIDI clock generation, timing analysis and hardware latency
 * calibration.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If
 * not, see <https://www.gnu.org/licenses/>.
 */

// STEP ONE, AND ONLY STEP ONE: does the host give us usable musical time?
//
// This plug-in generates nothing and draws nothing. It loads into Ableton on an audio track, reads
// the ProcessContext the host hands it on every block, and writes what it finds to a log. The whole
// point is to see, before writing a single line of clock generation, WHICH fields the host actually
// fills in, how they move, and whether they are steady enough to schedule against.
//
// That order is deliberate. Everything downstream - tick scheduling, the phase estimator, jitter
// figures - is built on the assumption that the host's tempo and musical position are trustworthy.
// If they are not, or if a field this design depends on is never populated, it is far cheaper to
// discover that from a log than from a clock that is subtly wrong.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <CoreAudio/HostTime.h>

#include "msClock.h"
#include "msClockIn.h"
#include "msLog.h"
#include "msDetect.h"
#include "msMidi.h"
#include "msProbe.h"
#include "msStatus.h"
#include "msDraw.h"
#include "msEditor.h"
#include "msStats.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#ifndef MST_VERSION_STRING
#define MST_VERSION_STRING    "0.0.0"
#endif

// LOGGING LIVES IN C (src/msLog.c), and so will everything else that is not COM plumbing. This
// file is the thin VST3 shim and nothing more - the same split the sibling projects use, where the
// only C++ in the tree is the part the VST3 interfaces force to be C++.

// ── Class IDs ────────────────────────────────────────────────────────────────────────────────────
//
// TWO CLASSES, processor and controller, registered separately. A single object implementing both
// is legal VST3 and is what a hand-written test host will happily accept - and Ableton will not:
// it obtains the controller by instantiating the class getControllerClassId() names, and does not
// fall back to asking the component. G2-Edit lost real time to that, and it presents as a plug-in
// that loads with no parameters and an empty panel.
static const FUID kProcessorUID(0x7A1E5C40, 0x9B2D4F13, 0xA6E80C57, 0x3D91B4E2);
static const FUID kControllerUID(0x2C48F9A1, 0x5E7B4D06, 0x91C3A28F, 0x6B0D57E4);

// ── Processor ────────────────────────────────────────────────────────────────────────────────────

// THE SLOT THE PROCESSOR PUBLISHES INTO, and the message ID that tells the controller which one.
// The two are separate registered classes precisely so a host MAY keep them apart, and
// IConnectionPoint is the only channel VST3 provides between them.
#define MST_MSG_STATUS_SLOT    "mstSlot"

// VST3 strings are UTF-16. Nothing here is ever anything but ASCII, so a widening copy is the whole
// requirement and pulling in the SDK's string classes for it would not be.
static void copy_string(Steinberg::Vst::TChar * out, const char * text) {
    int i = 0;

    while ((text[i] != '\0') && (i < 127)) {
        out[i] = (Steinberg::Vst::TChar)text[i];
        i++;
    }
    out[i] = 0;
}

// ── Parameters ───────────────────────────────────────────────────────────────────────────────────
//
// A HOST LAUNCHED FROM THE DOCK INHERITS NO SHELL ENVIRONMENT, so every MST_* variable this plug-in
// was developed against is empty inside Live: the clock had no destination and generated nothing,
// while the panel showed every other figure perfectly. G2-Edit's plug-in notes record the identical
// trap with $G2_VST3_PATCH. Anything a user must be able to set has to be a real VST3 parameter.
//
// A VST3 STEPPED PARAMETER HAS A FIXED STEP COUNT, decided at registration and cached by the host,
// so it cannot follow how many MIDI destinations the machine happens to have. GenBridge learned that
// the hard way - it scaled across the live device count in one place and the fixed count in another,
// so the same normalised value named one device in the panel and opened another. The slot count here
// is fixed at MS_MIDI_MAX_DEST and only the number of slots pointing at something real varies.
enum {
    kParamMidiDest = 0,      // 0 = none, 1..n = the nth destination
    kParamCompensate,        // device latency to compensate, in milliseconds
    kParamAudioSource,       // which of the channels the host hands us to analyse
    kParamMode,              // tMsMode - generate + measure, generate only, or monitor

    // APPENDED, NEVER INSERTED. A parameter's id is what a host's automation and a saved project
    // are expressed in, so the four above keep the ids they were released with and anything new
    // goes on the end. A shorter state block simply leaves this at "none", which is right.
    kParamClockSource,       // 0 = none, 1..n = the nth MIDI SOURCE to measure
    kParamCount
};

#define MST_COMPENSATE_MAX    (100.0)    // ms; well past anything a drum machine has shown

// HOW MANY PARAMETERS THE ORIGINAL STATE BLOCK HELD, and it must never change.
//
// The block is [N doubles][destination name]. A fifth parameter CANNOT simply extend that array: an
// old block is 4 doubles followed immediately by 64 bytes of name, so reading 5 doubles from it
// succeeds - and quietly loads the first eight characters of the port's NAME as a parameter value.
// The read would not fail, it would not be short, and the plug-in would come up listening to
// whatever "IAC Driv" happens to normalise to.
//
// So everything new goes on the END of the block, after the name, where an old file simply runs out
// and the short read leaves the default in place. See setState().
#define MST_STATE_V1_PARAMS   (4)

// SLOT 0 IS NONE, deliberately and permanently. A plug-in that picks a destination on your behalf
// ends up driving hardware nobody asked it to - the rule GenBridge arrived at the hard way.
static inline int mst_dest_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_DEST + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_DEST) ? MS_MIDI_MAX_DEST : slot);
}

static inline double mst_dest_normalized(int slot) {
    return (double)slot / (double)MS_MIDI_MAX_DEST;
}

// The same arithmetic for the SOURCE list, which is a different list of a different length. Slot 0
// is None here for a milder reason than on the destination side - listening to something uninvited
// harms nothing - but a plug-in that picked a master on your behalf would still be reporting a tempo
// nobody asked it to measure.
static inline int mst_source_slot(double normalized) {
    int slot = (int)(normalized * (double)MS_MIDI_MAX_SOURCE + 0.5);

    return (slot < 0) ? 0 : ((slot > MS_MIDI_MAX_SOURCE) ? MS_MIDI_MAX_SOURCE : slot);
}

static inline double mst_source_normalized(int slot) {
    return (double)slot / (double)MS_MIDI_MAX_SOURCE;
}

class MstProcessor : public IComponent,
                     public IAudioProcessor,
                     public IConnectionPoint,
                     public IProcessContextRequirements {
public:
    MstProcessor(void) : refCount(1) {}
    virtual ~MstProcessor(void) {}

    // ---- FUnknown ------------------------------------------------------------------------------
    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IComponent::iid)
            || FUnknownPrivate::iidEqual(iid, IPluginBase::iid)) {
            addRef();
            *obj = static_cast<IComponent *>(this);
            return kResultOk;
        }

        if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid)) {
            addRef();
            *obj = static_cast<IAudioProcessor *>(this);
            return kResultOk;
        }

        if (FUnknownPrivate::iidEqual(iid, IConnectionPoint::iid)) {
            addRef();
            *obj = static_cast<IConnectionPoint *>(this);
            return kResultOk;
        }

        if (FUnknownPrivate::iidEqual(iid, IProcessContextRequirements::iid)) {
            addRef();
            *obj = static_cast<IProcessContextRequirements *>(this);
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

        return (uint32)refCount.load();
    }

    // ---- IConnectionPoint ----------------------------------------------------------------------
    //
    // The only channel VST3 offers between the two registered classes. The slot number crosses it
    // once, on connect; the figures themselves are then read straight out of the shared structure,
    // because a message per frame per instance would be a great deal of allocation on a UI timer
    // for numbers that are only ever advisory.
    tresult PLUGIN_API connect(IConnectionPoint * other) SMTG_OVERRIDE {
        peer = other;
        send_slot();
        return kResultOk;
    }

    tresult PLUGIN_API disconnect(IConnectionPoint *) SMTG_OVERRIDE {
        peer = nullptr;
        return kResultOk;
    }

    tresult PLUGIN_API notify(IMessage *) SMTG_OVERRIDE { return kResultOk; }

    void send_slot(void) {
        if ((peer == nullptr) || (host == nullptr) || (statusSlot < 0)) {
            return;
        }
        IMessage * message = nullptr;
        TUID       messageIid;

        // createInstance takes TUIDs (raw 16-byte arrays) while the interface exposes an FUID, so it
        // has to be copied out rather than passed straight through.
        memcpy(messageIid, IMessage::iid.toTUID(), sizeof(TUID));

        if ((host->createInstance(messageIid, messageIid, (void **)&message) != kResultOk)
            || (message == nullptr)) {
            return;
        }
        message->setMessageID(MST_MSG_STATUS_SLOT);
        message->getAttributes()->setInt("value", statusSlot);
        peer->notify(message);
        message->release();
    }

    // ---- IProcessContextRequirements -----------------------------------------------------------
    //
    // THE WHOLE REASON THIS PLUG-IN CAN SEE ANYTHING. VST3 3.7 made the process context opt-in: a
    // host is entitled to hand over nothing at all unless the plug-in states what it needs. A
    // plug-in that skips this looks completely correct and is silently starved of tempo, which is
    // the same shape of trap as the IPluginFactory2 subcategory that stopped Ableton loading
    // G2-Edit's plug-in as an instrument. Asked for on the first commit, deliberately.
    uint32 PLUGIN_API getProcessContextRequirements(void) SMTG_OVERRIDE {
        return kNeedSystemTime
               | kNeedContinousTimeSamples
               | kNeedProjectTimeMusic
               | kNeedBarPositionMusic
               | kNeedTempo
               | kNeedTimeSignature
               | kNeedTransportState;
    }

    // ---- IPluginBase ---------------------------------------------------------------------------
    tresult PLUGIN_API initialize(FUnknown * context) SMTG_OVERRIDE {
        ms_log_line("initialize - MidiSyncTool %s", MST_VERSION_STRING);

        // The host application is the only thing that can make an IMessage, so it has to be kept.
        if (context != nullptr) {
            context->queryInterface(IHostApplication::iid, (void **)&host);
        }
        statusSlot = ms_status_claim();
        status     = ms_status(statusSlot);
        send_slot();
        ms_clock_init(&clock);

        // Created here rather than at construction so a host that instantiates and discards without
        // initialising costs nothing.
        stats        = ms_stats_create();
        detect       = ms_detect_create();
        clockIn      = ms_clock_in_create();

        // THE LISTENER IS REGISTERED ONCE AND STAYS REGISTERED. What decides whether anything
        // arrives is which source is connected, not whether a callback is installed - so there is
        // one place that can be wrong rather than two that have to agree.
        ms_midi_set_listener(clock_in_byte, this);
        clock.stats  = stats;
        clock.detect = detect;

        // HOW FAR APART THE CALIBRATION HITS ARE, in clock ticks: 24 for a hit on every quarter
        // note, 6 for sixteenths, 96 for one a bar. It is an environment variable until there is a
        // panel, for the same reason the destination is.
        const char * division = getenv("MST_DETECT_DIV");

        if (division != nullptr) {
            ms_detect_set_division(detect, atoi(division));
        }
        ms_log_line("detector: expecting a transient every %d ticks", ms_detect_division(detect));

        ms_probe_init(&probe);

        // THE COMPENSATION, which is the point of measuring at all. Negative sends earlier. It is an
        // environment variable until there is a panel, like everything else here.
        // MST_COMPENSATE_MS is the measured DEVICE round trip, compensated as a phase advance of
        // the tick grid - the mechanism that works. MST_OFFSET_MS remains as a wall-clock trim for
        // anything with no musical grid to advance (the note probe), and is bounded by the schedule
        // lead; see the note in msClock.h.
        const char * compensate = getenv("MST_COMPENSATE_MS");

        if (compensate != nullptr) {
            ms_clock_set_compensation_ms(&clock, atof(compensate));
            ms_log_line("clock compensation: %.3f ms of device latency, advanced in phase",
                        atof(compensate));
        }
        const char * offset = getenv("MST_OFFSET_MS");

        if (offset != nullptr) {
            ms_midi_set_offset_ms(atof(offset));
            ms_log_line("output offset: %+.3f ms (wall clock trim)", ms_midi_offset_ms());
        }

        // ENUMERATED HERE, not from the audio thread and not from a repaint: it takes CoreMIDI's
        // locks. Once at load is enough until there is a UI to change the selection from.
        ms_midi_refresh();

        // NO DESTINATION UNTIL ONE IS CHOSEN, which is the same rule GenBridge arrived at the hard
        // way - a plug-in that picks something on your behalf ends up driving hardware nobody asked
        // it to. MST_MIDI_DEST names one for development, so the driver and a Live session can both
        // be pointed at something before there is a panel to do it with.
        const char * wanted = getenv("MST_MIDI_DEST");

        if (wanted != NULL) {
            set_selected_port(ms_midi_index_for_name(wanted));

            if (clock.destination < 0) {
                ms_log_line("MST_MIDI_DEST '%s' is not present - generating nothing", wanted);
            } else {
                ms_log_line("clock destination: [%d] %s", clock.destination, wanted);
            }
            // The probe drives the same device - set_selected_port() does both. There is no case
            // for calibrating one port and clocking another.
            if ((status != nullptr) && (clock.destination >= 0)) {
                ms_midi_name(clock.destination, status->destName, sizeof(status->destName));
                atomic_store(&status->haveDestination, 1);
            }
        }

        // THE NOTE PROBE, armed from the environment for now. MST_PROBE=note[,velocity[,channel]] -
        // the note number matters more than anything else here, because it selects which pad is
        // being measured and a two-layer pad is a different measurement from a single sound.
        const char * probeSpec = getenv("MST_PROBE");

        if (probeSpec != nullptr) {
            int note = 36, velocity = 100, channel = 10;

            sscanf(probeSpec, "%d,%d,%d", &note, &velocity, &channel);
            probe.note     = note;
            probe.velocity = velocity;
            probe.channel  = channel - 1;   // as people count them

            const char * count = getenv("MST_PROBE_COUNT");

            if (count != nullptr) {
                probe.count = atoi(count);
            }
            ms_probe_start(&probe, detect);
        }

        return kResultOk;
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE {
        ms_log_line("terminate");
        ms_status_release(statusSlot);
        statusSlot = -1;
        status     = nullptr;

        if (host != nullptr) {
            host->release();
            host = nullptr;
        }
        clock.stats  = nullptr;
        clock.detect = nullptr;
        ms_stats_destroy(stats);
        // THE LISTENER GOES FIRST, and the source with it. It runs on CoreMIDI's thread and holds a
        // pointer to this object; leaving it connected while the estimator underneath it is freed is
        // a use-after-free waiting for the next clock byte to arrive.
        ms_midi_set_listener(nullptr, nullptr);
        ms_midi_listen(-1);
        ms_clock_in_destroy(clockIn);
        clockIn = nullptr;

        ms_detect_destroy(detect);
        stats  = nullptr;
        detect = nullptr;
        return kResultOk;
    }

    // ---- IComponent ----------------------------------------------------------------------------
    tresult PLUGIN_API getControllerClassId(TUID classId) SMTG_OVERRIDE {
        memcpy(classId, kControllerUID.toTUID(), sizeof(TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode(IoMode) SMTG_OVERRIDE { return kNotImplemented; }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) SMTG_OVERRIDE {
        return (type == kAudio) ? 1 : 0;
    }

    // AN EFFECT WITH AN AUDIO INPUT, even though nothing is read from it yet. Registering as an
    // effect from the start keeps the audio-transient measurement open without a later change of
    // class, and an effect that declares an input bus never meets the "no valid audio input bus"
    // rejection an instrument-shaped effect does.
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo & info) SMTG_OVERRIDE {
        if ((type != kAudio) || (index != 0)) {
            return kInvalidArgument;
        }
        memset(&info, 0, sizeof(info));
        info.mediaType    = kAudio;
        info.direction    = dir;
        info.channelCount = 2;
        info.busType      = kMain;
        info.flags        = BusInfo::kDefaultActive;

        const char16_t * name = (dir == kInput) ? u"In" : u"Out";

        for (int i = 0; (i < 3); i++) {
            info.name[i] = (char16)name[i];
        }

        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        ms_log_line("setActive(%d)", (int)state);
        blocksSeen = 0;
        lastLogged = -1.0;
        suspend();
        return kResultOk;
    }

    // NOTHING BETWEEN TWO BLOCKS IS A BLOCK PERIOD once the host has stopped feeding us. Both the
    // block-period RMS and the timebase model measure a STEP from the previous sample, so both have
    // to be told that the next one does not follow the last. Neither figure is cleared - the run's
    // history is still the run's history - only the stale timestamp behind it.
    void suspend(void) {
        ms_stats_gap(stats);
        clock.haveBase = false;
        clock.havePrev = false;
        // AND THE CYCLE TRACKER, for the same reason: the next call does not continue the last
        // one's device cycle, and treating it as a continuation would place its audio a fragment
        // into a cycle that ended before the pause.
        haveCall       = false;
        cycleFramesSoFar = 0;
    }

    // THE PROJECT MUST REMEMBER THE PORT, or every reopened set is silent until someone notices.
    // Written as the two normalised values, in order, and read back defensively: a state block from
    // an older build is shorter, and a host is entitled to hand back whatever it stored.
    tresult PLUGIN_API setState(IBStream * stream) SMTG_OVERRIDE {
        if (stream == nullptr) {
            return kResultOk;
        }
        double values[MST_STATE_V1_PARAMS] = { 0.0, 0.0, 0.0, 0.0 };
        int32  read = 0;

        stream->read(values, (int32)sizeof(values), &read);

        if (read >= (int32)sizeof(double)) {
            apply_parameter(kParamMidiDest, values[0]);
        }

        if (read >= (int32)(2 * sizeof(double))) {
            apply_parameter(kParamCompensate, values[1]);
        }

        if (read >= (int32)(3 * sizeof(double))) {
            apply_parameter(kParamAudioSource, values[2]);
        }

        // A STATE BLOCK FROM AN OLDER BUILD IS SHORTER and simply stops here, leaving the mode at
        // Generate + measure - the right default for a set saved before the mode existed.
        //
        // A BLOCK FROM THE TWO-STATE TOGGLE BUILD IS THE SAME LENGTH and needs no migration: it
        // wrote 0.0 for generate and 1.0 for monitor, which are exactly this parameter's first and
        // last steps. See the ordering note on tMsMode.
        if (read >= (int32)(4 * sizeof(double))) {
            apply_parameter(kParamMode, values[3]);
        }
        char name[MS_MIDI_NAME_LEN] = {0};

        read = 0;
        stream->read(name, (int32)sizeof(name), &read);

        if ((read > 0) && (name[0] != '\0')) {
            name[sizeof(name) - 1] = '\0';

            int index = ms_midi_index_for_name(name);

            if (index >= 0) {
                apply_parameter(kParamMidiDest, mst_dest_normalized(index + 1));
            } else {
                // NAMED IN THE PROJECT BUT NOT PLUGGED IN, which is a different state from "nothing
                // chosen" and has to look different: one is a plug-in waiting for hardware it has
                // been told to use, the other has never been told anything. Answering the first with
                // the second is how a saved setup silently starts driving the wrong port.
                set_selected_port(-1);

                if (status != nullptr) {
                    snprintf(status->waitingName, sizeof(status->waitingName), "%s", name);
                    atomic_store(&status->waitingForDevice, 1);
                    atomic_store(&status->haveDestination, 0);
                }
                ms_log_line("saved destination '%s' is not present - generating nothing", name);
            }
        }

        // ---- ANYTHING ADDED AFTER v1 LIVES HERE, past the name -------------------------------
        //
        // An older block has run out by this point, so both reads come back short and the defaults
        // stand: no clock source, which is right for a set saved before there was one.
        double clockSource = 0.0;

        read = 0;
        stream->read(&clockSource, (int32)sizeof(clockSource), &read);

        if (read >= (int32)sizeof(double)) {
            apply_parameter(kParamClockSource, clockSource);
        }
        char sourceName[MS_MIDI_NAME_LEN] = {0};

        read = 0;
        stream->read(sourceName, (int32)sizeof(sourceName), &read);

        // THE NAME WINS OVER THE INDEX, for the same reason it does on the destination side: a
        // source's index moves with whatever else is switched on that day.
        if ((read > 0) && (sourceName[0] != '\0')) {
            sourceName[sizeof(sourceName) - 1] = '\0';

            int index = ms_midi_source_index_for_name(sourceName);

            if (index >= 0) {
                apply_parameter(kParamClockSource, mst_source_normalized(index + 1));
            } else {
                apply_parameter(kParamClockSource, 0.0);
                ms_log_line("saved clock source '%s' is not present - measuring nothing", sourceName);
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream * stream) SMTG_OVERRIDE {
        if (stream == nullptr) {
            return kResultOk;
        }
        // THE FIRST FOUR ONLY. See MST_STATE_V1_PARAMS: the block's shape up to and including the
        // destination name is frozen, and everything since goes on the end.
        double values[MST_STATE_V1_PARAMS] = { paramDest, paramCompensate, paramAudioSource, paramMode };
        int32  written = 0;

        stream->write(values, (int32)sizeof(values), &written);

        // AND THE PORT'S NAME, which is what actually gets restored - the index is saved only
        // because the host's automation is expressed in it.
        //
        // A MIDI destination's index depends on what is switched on. Measured during development:
        // the Tempest was index 6 with a Kronos and a Hydrasynth powered up and index 4 without
        // them. A project storing the index alone would open a different instrument on a different
        // day, or none, and would do it silently - which is GenBridge's "a missing USB interface
        // fell through to whatever sat at slot 0, a microphone".
        char name[MS_MIDI_NAME_LEN] = {0};

        if (clock.destination >= 0) {
            ms_midi_name(clock.destination, name, sizeof(name));
        }
        stream->write(name, (int32)sizeof(name), &written);

        // ---- past the name: everything added after v1 -----------------------------------------
        stream->write(&paramClockSource, (int32)sizeof(paramClockSource), &written);

        char sourceName[MS_MIDI_NAME_LEN] = {0};
        int  listening = ms_midi_listening();

        if (listening >= 0) {
            ms_midi_source_name(listening, sourceName, sizeof(sourceName));
        }
        stream->write(sourceName, (int32)sizeof(sourceName), &written);
        return kResultOk;
    }

    // ONE PLACE where a parameter becomes an effect, so the state restore and the host's own
    // parameter changes cannot diverge.
    // THE DESTINATION IS A FUNCTION OF TWO PARAMETERS, so it gets one place to be decided in.
    // Monitor mode overrides the chosen port with "none", which is what actually makes the plug-in
    // silent: ms_clock_process returns immediately on a negative destination and sends nothing at
    // all - no ticks, no Start, no Stop. The chosen port is REMEMBERED rather than cleared, so
    // leaving monitor mode puts the rig back exactly as it was without the user re-picking it.
    // ONE OWNER FOR THE CHOSEN PORT, and it is NOT the parameter. The port can be chosen two ways -
    // the panel's parameter, or MST_MIDI_DEST at construction for a headless run - and deriving it
    // from the parameter here meant the environment's choice was silently discarded the first time
    // anything else called this. The harness set the port by name, toggled monitor mode, and the
    // clock went quiet with the panel still showing the right destination.
    void set_selected_port(int port) {
        selectedPort = port;
        refresh_destination();
    }

    void refresh_destination(void) {
        int port = (mode == eMsModeMonitor) ? -1 : selectedPort;

        clock.destination = port;
        probe.destination = port;
    }

    // ON CoreMIDI'S RECEIVE THREAD. Straight through to the estimator, which owns everything it
    // touches - no lock, no allocation, and deliberately nothing that reaches into the processor.
    static void clock_in_byte(uint8_t status, uint64_t hostTime, void * user) {
        MstProcessor * self = (MstProcessor *)user;

        if ((self != nullptr) && (self->clockIn != nullptr)) {
            ms_clock_in_byte(self->clockIn, status, hostTime);
        }
    }

    void apply_parameter(int id, double normalized) {
        if (id == kParamMode) {
            tMsMode previous = mode;

            paramMode = normalized;
            mode      = ms_mode_from_normalized(normalized);

            // WHICH SOURCE EACH MODE IMPLIES, in one place. eMsDetectOff preserves the figures and
            // every other transition clears them - see ms_detect_set_source().
            ms_detect_set_source(detect, (mode == eMsModeMonitor)   ? eMsDetectMonitor
                                       : (mode == eMsModeClockOnly) ? eMsDetectOff
                                                                    : eMsDetectFromClock);

            // THE HOST-TIMING STATS ARE RESET ONLY WHEN THE CLOCK ITSELF STARTS OR STOPS, which is
            // the monitor boundary and nothing else. Block jitter, the model residual and the commit
            // margin stay live and meaningful in clock-only mode - it is still generating - so
            // clearing them on the way in would throw away a settled run for no reason.
            if ((mode == eMsModeMonitor) != (previous == eMsModeMonitor)) {
                ms_stats_reset(stats);
            }
            refresh_destination();

            if (status != nullptr) {
                atomic_store(&status->mode, (int)mode);
                atomic_store(&status->haveDestination, (clock.destination >= 0) ? 1 : 0);
            }
            ms_log_line("mode -> %s", ms_mode_label(mode));
        } else if (id == kParamMidiDest) {
            paramDest = normalized;

            int slot = mst_dest_slot(normalized);

            // Slot 1 is the first destination, because slot 0 is None.
            set_selected_port(((slot >= 1) && ((slot - 1) < ms_midi_count())) ? (slot - 1) : -1);

            if (status != nullptr) {
                atomic_store(&status->waitingForDevice, 0);

                if (clock.destination >= 0) {
                    ms_midi_name(clock.destination, status->destName, sizeof(status->destName));
                    atomic_store(&status->haveDestination, 1);
                } else {
                    atomic_store(&status->haveDestination, 0);
                }
            }
            ms_log_line("destination parameter -> slot %d (%s)", slot,
                        (clock.destination >= 0) ? status->destName : "none");
        } else if (id == kParamClockSource) {
            paramClockSource = normalized;

            int slot = mst_source_slot(normalized);
            int port = ((slot >= 1) && ((slot - 1) < ms_midi_source_count())) ? (slot - 1) : -1;

            // A NEW SOURCE IS A NEW MEASUREMENT. Carrying a fitted rate across from a different
            // master would be a reading of neither, and the two could be tens of ppm apart.
            ms_clock_in_reset(clockIn);
            ms_midi_listen(port);

            if (status != nullptr) {
                atomic_store(&status->haveClockSource, (port >= 0) ? 1 : 0);
                ms_midi_source_name(port, status->clockSourceName, sizeof(status->clockSourceName));
            }
        } else if (id == kParamAudioSource) {
            paramAudioSource = normalized;
        } else if (id == kParamCompensate) {
            paramCompensate = normalized;
            ms_clock_set_compensation_ms(&clock, normalized * MST_COMPENSATE_MAX);
        }
    }

    // Parameter changes arrive with the block. Only the LAST point in each queue is taken: a host
    // may send a whole automation ramp for one block, and every intermediate value would reopen the
    // destination on its way past.
    void take_parameter_changes(ProcessData & data) {
        if (data.inputParameterChanges == nullptr) {
            return;
        }

        for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); i++) {
            IParamValueQueue * queue = data.inputParameterChanges->getParameterData(i);

            if (queue == nullptr) {
                continue;
            }
            int32      count = queue->getPointCount();
            int32      offset = 0;
            ParamValue value  = 0.0;

            if ((count > 0) && (queue->getPoint(count - 1, offset, value) == kResultOk)) {
                apply_parameter((int)queue->getParameterId(), value);
            }
        }
    }

    // ---- IAudioProcessor -----------------------------------------------------------------------
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32 numIns,
                                          SpeakerArrangement * outputs, int32 numOuts) SMTG_OVERRIDE {
        if ((numIns == 1) && (numOuts == 1) && (outputs[0] == SpeakerArr::kStereo)) {
            return kResultOk;
        }

        return kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement & arr) SMTG_OVERRIDE {
        if (index != 0) {
            return kInvalidArgument;
        }
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSize) SMTG_OVERRIDE {
        return (symbolicSize == kSample32) ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples(void) SMTG_OVERRIDE { return 0; }

    // Infinite, for the reason GenBridge learned the hard way: kNoTail promises that nothing comes
    // out once the input goes silent, and this plug-in will eventually produce output that has
    // nothing to do with its input. It is used on a track with nothing feeding it, which is exactly
    // when a host may stop processing a chain that has promised silence.
    uint32 PLUGIN_API getTailSamples(void) SMTG_OVERRIDE { return kInfiniteTail; }

    tresult PLUGIN_API setupProcessing(ProcessSetup & setup) SMTG_OVERRIDE {
        sampleRate  = setup.sampleRate;
        maxBlock    = setup.maxSamplesPerBlock;
        ms_log_line("setupProcessing: rate %.0f, maxBlock %d, mode %s",
                 setup.sampleRate, (int)setup.maxSamplesPerBlock,
                 (setup.processMode == kRealtime) ? "realtime"
                 : ((setup.processMode == kPrefetch) ? "prefetch" : "offline"));
        return kResultOk;
    }

    // Called when the host starts and stops feeding blocks, distinct from setActive. Logged
    // because it brackets every run of process() calls and makes the log readable.
    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE {
        ms_log_line("setProcessing(%d)", (int)state);
        suspend();
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData & data) SMTG_OVERRIDE {
        take_parameter_changes(data);

        // Pass the audio through unchanged. Nothing is analysed yet; a plug-in on a track should
        // not silence it just because it is only looking.
        if ((data.numInputs > 0) && (data.numOutputs > 0) && (data.inputs != nullptr)
            && (data.outputs != nullptr)) {
            for (int32 ch = 0; (ch < data.outputs[0].numChannels); ch++) {
                float * out = data.outputs[0].channelBuffers32[ch];
                float * in  = (ch < data.inputs[0].numChannels) ? data.inputs[0].channelBuffers32[ch] : nullptr;

                if (out == nullptr) {
                    continue;
                }

                if (in == out) {
                    continue;   // the host gave us the same buffer for both; nothing to copy
                }

                if (in != nullptr) {
                    memcpy(out, in, (size_t)data.numSamples * sizeof(float));
                } else {
                    memset(out, 0, (size_t)data.numSamples * sizeof(float));
                }
            }
        }
        blocksSeen++;

        ProcessContext * ctx = data.processContext;

        if (ctx == nullptr) {
            // Worth its own line, and worth not repeating a hundred times a second: a host that
            // hands over no context at all is the failure this whole exercise is looking for.
            if (!warnedNoContext) {
                warnedNoContext = true;
                ms_log_line("WARNING: processContext is NULL - the host is giving us no timing at all");
            }

            return kResultOk;
        }

        // THROTTLED, AND ON MOVEMENT. process() runs about a hundred times a second; logging every
        // block would produce a file nobody can read and would itself perturb the timing being
        // measured. One line when something meaningful changes, and one every two seconds
        // regardless so a steady state is still visible.
        // OUR OWN CLOCK, because the host's is not there. ProcessContext.systemTime is always 0 in
        // Live and kSystemTimeValid is never set, so the field the scheduler was going to be built
        // on does not exist. AudioGetCurrentHostTime() read here is the wall clock as the CPU sees
        // it at the moment this block is COMPUTED - which is not when its audio is heard, and the
        // difference is the output latency: unknown, but constant for a device and buffer size.
        // Measuring that constant is what makes scheduling possible at all.
        uint64_t hostNow    = AudioGetCurrentHostTime();
        double   hostNowMs  = (double)AudioConvertHostTimeToNanos(hostNow) / 1.0e6;
        double   wallDelta  = (lastHostMs > 0.0) ? (hostNowMs - lastHostMs) : 0.0;

        // ONE DEVICE CYCLE IS NOT ALWAYS ONE process() CALL, and a loop boundary is where that
        // stops being a detail.
        //
        // Live SPLITS the buffer at a loop wrap: one 512-frame cycle arrives as two calls - 500
        // frames and then 12 - microseconds apart, because both are computed inside the same audio
        // callback. Everything downstream had been written on the assumption that the wall clock
        // read at the top of process() is when THIS block's audio begins, and for the second
        // fragment that is false by the whole length of the first one.
        //
        // MEASURED, and it is the reason this exists. Live 12.4.5, 512-frame buffer, 1-bar loop at
        // 130 BPM: block sizes 8..512 with 883 changes over 53708 blocks - 2.84 per loop, which is
        // exactly one split (512 -> a -> b -> 512) every time round. Host block jitter reported a
        // worst case of -10.412 ms against a 10.667 ms cycle: the gap between the two fragments is
        // ~0, judged against the first fragment's own 10.417 ms, so the figure is the fabricated
        // difference and nothing else. Because the RMS never forgets, every wrap added another one
        // and the number climbed all session. At a 256-frame buffer Live did not split at all,
        // which is why this was never seen before and why Docs/findings.txt said it never happens.
        //
        // THE FIX IS ONE TIMESTAMP, not a special case in four metrics. A fragment's audio starts
        // where the frames already handed over this cycle end, so that is the moment it is given -
        // and the block period, the timebase model, the probe and the onset detector are all
        // correct again without knowing splitting exists. Across a cycle the corrected times still
        // sum to the real interval, so a genuinely late callback is still reported as one.
        //
        // WHAT COUNTS AS A CONTINUATION - two tests, and it takes both.
        //
        //   THE FRAMES MUST FIT. A device cycle delivers one buffer, so a call whose frames would
        //   take the running total past a whole buffer cannot belong to the cycle before it. This
        //   is the deterministic half and it carries the common case on its own.
        //
        //   AND THE CALL MUST SHARE THE CYCLE'S WALL INSTANT. Half a buffer separates the two
        //   regimes by three orders of magnitude - fragments arrive microseconds apart because
        //   they are computed inside one audio callback, cycles ten milliseconds apart because
        //   that is what the device is doing. It is the guard for the case the frames test cannot
        //   see: a genuinely SHORT cycle followed by a normal one, whose frames also happen to fit.
        //
        // THE BUFFER IS THE LARGEST SINGLE CALL SEEN, not maxSamplesPerBlock, and that distinction
        // is the one the first attempt got wrong. A host is entitled to hand over less than it
        // declared, and a host that consistently does - 256 through a 512 declaration - would have
        // put both thresholds exactly on a real cycle boundary. Measured behaviour cannot.
        //
        // Keying the timing test off the PREVIOUS CALL's duration instead was the other wrong
        // answer, and the harness caught it: where the wrap falls early in a buffer the first
        // fragment is small, so half of it is a few hundred microseconds - less than the work a
        // host does between the two halves - and the split read as two cycles again.
        if ((uint32_t)data.numSamples > observedMaxFrames) {
            observedMaxFrames = (uint32_t)data.numSamples;
        }
        uint32_t cycleFrames = (observedMaxFrames > 0) ? observedMaxFrames : (uint32_t)maxBlock;
        double   cycleMs     = ((sampleRate > 0.0) && (cycleFrames > 0))
                               ? (((double)cycleFrames / sampleRate) * 1000.0) : 0.0;
        bool     newCycle    = true;

        if (haveCall && (hostNow >= prevCallHostTime) && (cycleMs > 0.0)) {
            double sinceMs = (double)AudioConvertHostTimeToNanos(hostNow - prevCallHostTime) / 1.0e6;

            newCycle = ((cycleFramesSoFar + (uint32_t)data.numSamples) > cycleFrames)
                       || (sinceMs >= (0.5 * cycleMs));
        }

        if (newCycle) {
            // THE CYCLE THAT JUST ENDED IS THE HOST'S REAL BUFFER, and it is what the latency
            // breakdown has to be told about - see the note where blockMs is published.
            cycleFramesLast    = haveCall ? cycleFramesSoFar : (uint32_t)data.numSamples;
            cycleStartHostTime = hostNow;
            cycleFramesSoFar   = 0;
        } else {
            splitCalls++;
        }
        uint64_t blockHostTime = cycleStartHostTime;

        if ((sampleRate > 0.0) && (cycleFramesSoFar > 0)) {
            blockHostTime += AudioConvertNanosToHostTime(
                (uint64_t)(((double)cycleFramesSoFar / sampleRate) * 1.0e9));
        }
        prevCallHostTime  = hostNow;
        cycleFramesSoFar += (uint32_t)data.numSamples;
        haveCall          = true;

        // THE PANEL'S CLEAR, TAKEN HERE AND NOWHERE ELSE. The UI raises status->clearRequest and the
        // audio thread - which owns every one of these figures - is what acts on it, so no other
        // thread ever writes the accumulators. Exchanged rather than tested and cleared, so a click
        // landing between the two halves of that pair cannot be lost.
        //
        // BEFORE ms_stats_block(), so this block is the first of the new run rather than the last of
        // the old one. And the model's own figures go with it: leaving them would have the log's
        // "model |" line divide a residual accumulated since the run started by a raw figure
        // accumulated since the clear, and call the answer a percentage.
        if ((status != nullptr) && (atomic_exchange(&status->clearRequest, 0u) != 0u)) {
            ms_stats_reset(stats);
            ms_clock_reset_stats(&clock);
            ms_log_line("figures cleared from the panel");
        }

        // WHERE THE MUSICAL POSITION SHOULD HAVE GOT TO, if nothing jumped. Anything else is a loop
        // wrap, a playhead move or a tempo change taking effect - and those are the events a clock
        // generator has to handle correctly, so they are logged the moment they happen rather than
        // whenever the heartbeat next comes round. The 2-second heartbeat was far too coarse to
        // show what a loop boundary does.
        ms_stats_block(stats,
                       blockHostTime,
                       (uint32_t)data.numSamples,
                       sampleRate,
                       ((ctx->state & ProcessContext::kTempoValid) != 0) ? ctx->tempo : 0.0,
                       (ctx->state & ProcessContext::kPlaying) != 0);

        // THE PROBE, which sends nothing unless a run is armed. Ahead of the clock only because a
        // calibration run is the more time-critical of the two while it lasts.
        //
        // NOT IN A MODE THAT CANNOT HEAR THE ANSWER. ms_probe_process() registers an expectation for
        // every note it sends by calling ms_detect_expect() directly, which has no source guard of
        // its own - so a run left armed from before the mode changed would queue expectations that
        // nothing will ever match, and count each one as a miss.
        if (mode == eMsModeMeasure) {
            ms_probe_process(&probe, detect, (uint32_t)data.numSamples, sampleRate, blockHostTime);
        }

        // THE CLOCK, before any logging: the ticks in this block belong to the wall time just read,
        // and every microsecond spent deciding what to log first is a microsecond of avoidable
        // scheduling error.
        ms_clock_process(&clock,
                         ctx->projectTimeMusic,
                         ((ctx->state & ProcessContext::kTempoValid) != 0) ? ctx->tempo : 0.0,
                         (ctx->state & ProcessContext::kPlaying) != 0,
                         (ctx->state & ProcessContext::kCycleActive) != 0,
                         ctx->cycleStartMusic,
                         ctx->cycleEndMusic,
                         (uint32_t)data.numSamples,
                         sampleRate,
                         blockHostTime);

        // THE AUDIO, after the clock: this block's transients are answers to ticks scheduled in
        // earlier blocks, so the order does not matter for correctness, but the clock is the
        // time-critical half and goes first on principle.
        //
        // The LEFT channel only. A transient is a transient on either, and summing would let a
        // stereo hit's own channel-to-channel delay smear the onset.
        // AND NOT AT ALL IN CLOCK-ONLY MODE. ms_detect_audio() would return immediately anyway - the
        // invariant lives there, where the expectation ageing it protects is - but the channel choice
        // and the stereo sum above it are real work done for an answer nobody asked for.
        if ((mode != eMsModeClockOnly)
            && (data.numInputs > 0) && (data.inputs != nullptr)
            && (data.inputs[0].numChannels > 0)
            && (data.inputs[0].channelBuffers32 != nullptr)
            && (data.inputs[0].channelBuffers32[0] != nullptr)) {
            // WHICH CHANNEL, chosen by the user. A drum machine on one side of a stereo pair is
            // ordinary, and summing would let a stereo hit's own channel-to-channel delay smear the
            // onset - so a sum is offered but is not the default.
            int source = (int)((paramAudioSource * (double)(MS_AUDIO_SOURCES - 1)) + 0.5);
            const float * samples = data.inputs[0].channelBuffers32[0];

            if ((source == 1) && (data.inputs[0].numChannels > 1)
                && (data.inputs[0].channelBuffers32[1] != nullptr)) {
                samples = data.inputs[0].channelBuffers32[1];
            } else if ((source == 2) && (data.inputs[0].numChannels > 1)
                       && (data.inputs[0].channelBuffers32[1] != nullptr)) {
                // Summed into scratch rather than in place: the input buffer may be the host's own
                // and may be the OUTPUT buffer too, and writing to it would alter the audio passing
                // through a plug-in whose whole promise is that it does not.
                int32 count = (data.numSamples < (int32)(sizeof(sumBuffer) / sizeof(sumBuffer[0])))
                              ? data.numSamples
                              : (int32)(sizeof(sumBuffer) / sizeof(sumBuffer[0]));

                for (int32 i = 0; i < count; i++) {
                    sumBuffer[i] = 0.5f * (data.inputs[0].channelBuffers32[0][i]
                                           + data.inputs[0].channelBuffers32[1][i]);
                }
                samples = sumBuffer;
            }
            ms_detect_audio(detect, samples, (uint32_t)data.numSamples, sampleRate, blockHostTime);
        }
        // ---- publish, for the panel ----------------------------------------------------------
        //
        // Cheap enough to do every block: a couple of dozen atomic stores against a UI that reads
        // them thirty times a second. Throttling it would only add a staleness nobody asked for.
        if (status != nullptr) {
            tMsStatsSnapshot  snap;
            tMsDetectSnapshot hit;

            ms_stats_read(stats, &snap);
            ms_detect_read(detect, &hit);

            atomic_store(&status->active,  true);
            atomic_store(&status->hostBpm, ((ctx->state & ProcessContext::kTempoValid) != 0)
                                           ? ctx->tempo : 0.0);
            atomic_store(&status->playing, ((ctx->state & ProcessContext::kPlaying) != 0) ? 1 : 0);
            atomic_store(&status->ppq,     ctx->projectTimeMusic);

            atomic_store(&status->ticksSent,          (unsigned)clock.ticksSent);
            atomic_store(&status->commitMarginMeanMs, snap.marginMeanMs);
            atomic_store(&status->commitMarginMinMs,  snap.marginMinMs);
            atomic_store(&status->lateTicks,          (unsigned)snap.lateTicks);
            atomic_store(&status->blockPeriodRmsMs,   snap.blockPeriodRmsMs);
            atomic_store(&status->blockPeriodRecentRmsMs, snap.blockPeriodRecentRmsMs);
            atomic_store(&status->blockRecentSeconds, snap.blockRecentSeconds);
            atomic_store(&status->blockPeriodWorstMs, snap.blockPeriodWorstMs);
            atomic_store(&status->blockGaps,          (unsigned)snap.blockGaps);
            atomic_store(&status->residualRmsMs,       ms_clock_residual_ms(&clock));
            atomic_store(&status->modelResyncs,        (unsigned)clock.modelResyncs);
            atomic_store(&status->driftPpm,           snap.driftPpm);
            atomic_store(&status->driftValid,         snap.driftValid ? 1 : 0);
            atomic_store(&status->driftSeconds,       snap.windowSeconds);

            atomic_store(&status->roundTripMeanMs,    hit.latencyMeanMs);
            atomic_store(&status->roundTripMinMs,     hit.latencyMinMs);
            atomic_store(&status->roundTripMaxMs,     hit.latencyMaxMs);
            atomic_store(&status->roundTripJitterMs,  hit.jitterRmsMs);
            atomic_store(&status->roundTripPeakDevMs, hit.peakDeviationMs);
            atomic_store(&status->hits,               (unsigned)hit.hits);
            atomic_store(&status->missed,             (unsigned)hit.missed);
            atomic_store(&status->spurious,           (unsigned)hit.spurious);
            atomic_store(&status->inputPeak,          (float)hit.inputPeak);
            atomic_store(&status->monitorPeriodMs,    hit.monitorPeriodMs);
            atomic_store(&status->monitorBpm,         hit.monitorBpm);
            atomic_store(&status->monitorOnsets,      (unsigned)hit.monitorOnsets);

            atomic_store(&status->scheduleLeadMs, (double)MS_LOOKAHEAD_MS);

            // THE HOST'S BUFFER, which a plug-in can report but never set - the host owns it, and in
            // Live it is the audio preferences. It is worth reporting anyway because it is a real
            // and exactly knowable part of the input path: the audio in this block was captured at
            // least one buffer ago.
            // THE HOST'S BUFFER IS THE CYCLE, NOT THE CALL, and that distinction is the second
            // half of the split-block fault above. This is published every block and read by the
            // breakdown's "Host buffer" bar, so on the two calls a loop wrap produces it was
            // publishing a FRAGMENT - 8 frames, 0.167 ms - and the whole bar chart jumped every
            // time round the loop. Which is what "the latency bars reset at the loop start" was.
            uint32_t hostCycleFrames = (cycleFramesLast > 0) ? cycleFramesLast
                                                             : (uint32_t)data.numSamples;

            atomic_store(&status->sampleRate, sampleRate);
            atomic_store(&status->blockFrames, (unsigned)hostCycleFrames);
            atomic_store(&status->blockMs,
                         (sampleRate > 0.0) ? (((double)hostCycleFrames / sampleRate) * 1000.0) : 0.0);
            atomic_store(&status->blockSplits, (unsigned)splitCalls);
            atomic_store(&status->compensationMs, clock.compensationMs);
            atomic_store(&status->probeRunning,   ms_probe_running(&probe) ? 1 : 0);

            // THE INCOMING CLOCK, read from the estimator's published snapshot. Nothing here is
            // computed on this thread - the CoreMIDI thread owns all of it and this only forwards
            // what it last published.
            tMsClockInSnapshot in;

            ms_clock_in_read(clockIn, &in);
            atomic_store(&status->clockInBpm,       in.bpm);
            atomic_store(&status->clockInPeriodMs,  in.periodMs);
            atomic_store(&status->clockInJitterMs,  in.jitterRmsMs);
            atomic_store(&status->clockInPeakDevMs, in.peakDevMs);
            atomic_store(&status->clockInFitted,    (unsigned)in.fitted);
            atomic_store(&status->clockInClocks,    (unsigned)in.clocks);
            atomic_store(&status->clockInGaps,      (unsigned)in.gaps);
            atomic_store(&status->clockInRunning,   in.running ? 1 : 0);

            // ONE POINT PER DETECTION, not one per block. The graph is of what the hardware did, so
            // its x axis is hits - a block-rate trace would be a flat line with a step in it.
            if (hit.hits != lastGraphHits) {
                int write = atomic_load(&status->historyWrite);

                atomic_store(&status->history[write], (float)hit.latencyLastMs);
                atomic_store(&status->historyWrite, (write + 1) % MS_STATUS_HISTORY);
                lastGraphHits = hit.hits;
            }
        }
        double expectedPpq = lastPpq + ((ctx->tempo / 60.0) * ((double)data.numSamples / sampleRate));
        bool   jumped      = (lastPpq >= 0.0)
                             && ((ctx->state & ProcessContext::kPlaying) != 0)
                             && (fabs(ctx->projectTimeMusic - expectedPpq) > 1.0e-6);

        if (jumped) {
            ms_log_line("JUMP  ppq %.6f -> %.6f (expected %.6f, delta %+.6f) | smp %lld | bar %.4f",
                        lastPpq, ctx->projectTimeMusic, expectedPpq,
                        ctx->projectTimeMusic - expectedPpq,
                        (long long)ctx->projectTimeSamples, ctx->barPositionMusic);
        }
        lastPpq    = ctx->projectTimeMusic;
        lastHostMs = hostNowMs;

        bool   transportChanged = (ctx->state != lastState);
        bool   tempoChanged     = ((ctx->state & ProcessContext::kTempoValid) != 0)
                                  && (fabs(ctx->tempo - lastTempo) > 0.0005);
        // REAL FRAMES, NOT CALLS TIMES THE CURRENT SIZE. With a fixed block the two agree; with a
        // host that splits, the extra calls run the heartbeat's clock fast - 1.7 % here, harmless
        // in itself and quietly wrong in a log whose whole purpose is timing.
        framesSeen             += (int64_t)data.numSamples;
        double nowSeconds       = (double)framesSeen / ((sampleRate > 0.0) ? sampleRate : 48000.0);
        bool   heartbeat        = (nowSeconds - lastLogged) >= 2.0;

        if (transportChanged || tempoChanged || heartbeat) {
            lastState  = ctx->state;
            lastTempo  = ctx->tempo;
            lastLogged = nowSeconds;

            // EVERY VALIDITY FLAG IS PRINTED, not just the values. Which fields a host bothers to
            // fill in is the actual question here - a zero in projectTimeMusic means one thing if
            // the valid bit is set and something completely different if it is not.
            char flags[160] = {0};

            snprintf(flags, sizeof(flags), "%s%s%s%s%s%s%s%s",
                     (ctx->state & ProcessContext::kPlaying) ? "PLAYING " : "",
                     (ctx->state & ProcessContext::kRecording) ? "REC " : "",
                     (ctx->state & ProcessContext::kCycleActive) ? "CYCLE " : "",
                     (ctx->state & ProcessContext::kTempoValid) ? "tempo " : "",
                     (ctx->state & ProcessContext::kProjectTimeMusicValid) ? "ptMusic " : "",
                     (ctx->state & ProcessContext::kBarPositionValid) ? "bar " : "",
                     (ctx->state & ProcessContext::kTimeSigValid) ? "timeSig " : "",
                     (ctx->state & ProcessContext::kSystemTimeValid) ? "sysTime " : "");

            // systemTime is documented as nanoseconds, and is the field a scheduled MIDI send will
            // eventually be built on - so it is worth seeing early whether it moves the way a clock
            // should. Reported both raw and as a delta from the previous logged block.
            ms_log_line("blk %-6llu n=%-5d | %.4f BPM | %d/%d | ppq %.4f bar %.4f | smp %lld cont %lld"
                     " | sysTime %lld | wall +%.2f ms | ticks %llu wraps %llu | %s",
                     (unsigned long long)blocksSeen, (int)data.numSamples,
                     ctx->tempo,
                     (int)ctx->timeSigNumerator, (int)ctx->timeSigDenominator,
                     ctx->projectTimeMusic, ctx->barPositionMusic,
                     (long long)ctx->projectTimeSamples, (long long)ctx->continousTimeSamples,
                     (long long)ctx->systemTime, wallDelta,
                     (unsigned long long)clock.ticksSent, (unsigned long long)clock.wrapsSeen,
                     flags);

            // WHETHER THE HOST IS SPLITTING, and how often. A split is not a fault and is not
            // corrected away silently: it is the host's own behaviour, it is what a loop boundary
            // looks like from in here, and the count is the only way to know the correction above
            // is firing on real ones rather than on ordinary blocks.
            if (splitCalls > 0) {
                ms_log_line("  blocks | %llu split call(s): the host handed one device cycle over in"
                            " more than one process() call - cycle %u frames, this call %d",
                            (unsigned long long)splitCalls,
                            (cycleFramesLast > 0) ? cycleFramesLast : (unsigned)data.numSamples,
                            (int)data.numSamples);
            }

            // THE TELEMETRY, on the same heartbeat and only while ticks are actually going out.
            // Until there is a panel this log is the only way to read it, and inside a real host it
            // is the only way to find out whether MS_LOOKAHEAD_MS is doing anything - the offline
            // harness never made a tick late.
            tMsDetectSnapshot hit;

            ms_detect_read(detect, &hit);

            if (mode == eMsModeMonitor) {
                // NO LATENCY LINE IN MONITOR MODE. There is no reference for a transient to be late
                // against, so the only honest figures are the fitted grid and the spread about it.
                if (hit.monitorOnsets > 0) {
                    ms_log_line("  monitor| grid %.3f ms (%.3f BPM) | jitter RMS %.3f peak dev %.3f ms"
                                " | %llu onset(s), %llu empty slot(s) | input peak %.4f",
                                hit.monitorPeriodMs, hit.monitorBpm,
                                hit.jitterRmsMs, hit.peakDeviationMs,
                                (unsigned long long)hit.monitorOnsets,
                                (unsigned long long)hit.missed, hit.inputPeak);
                }
            } else if (mode == eMsModeClockOnly) {
                // NOTHING IS BEING MEASURED, so nothing is reported. The figures still in the
                // snapshot are the last run's and are preserved on purpose, but this log is a live
                // trace: reprinting a held number on every heartbeat is how it ends up in a
                // notebook as this session's measurement.
            } else if ((hit.hits > 0) || (hit.missed > 0) || (hit.spurious > 0)) {
                // ROUND TRIP, and labelled as such - it still contains the interface's A/D and the
                // host's input buffering. See the note at the top of msDetect.h.
                ms_log_line("  device | round trip mean %.3f last %.3f min %.3f max %.3f ms"
                            " | jitter RMS %.3f peak dev %.3f ms | hits %llu missed %llu spurious %llu"
                            " | input peak %.4f",
                            hit.latencyMeanMs, hit.latencyLastMs, hit.latencyMinMs, hit.latencyMaxMs,
                            hit.jitterRmsMs, hit.peakDeviationMs,
                            (unsigned long long)hit.hits, (unsigned long long)hit.missed,
                            (unsigned long long)hit.spurious, hit.inputPeak);
            }
            tMsStatsSnapshot snap;

            ms_stats_read(stats, &snap);

            // NOT GATED ON TICKS. The block-period and drift figures are measured whether or not a
            // destination has been chosen, and gating the whole line on ticks meant a run with no
            // port produced no telemetry at all - which is exactly the run someone diagnosing a
            // silent plug-in would be looking at.
            if (snap.windowSeconds > 0.0) {
                // BOTH BLOCK-JITTER FIGURES GO IN THE LOG, because a log is read after the fact
                // and the two answer different questions - the all-time one is what the run did,
                // the recent one is what it was doing at the moment the line was written. The panel
                // has room for one and shows the recent; a log line has room for both.
                ms_log_line("  timing | commit margin mean %+.3f min %+.3f RMS %.3f ms | late %llu/%llu"
                            " | block period RMS %.3f all-time / %.3f over %.1f s, worst %+.3f ms"
                            " | %llu gap(s) | drift %+.1f ppm | BPM host %.4f measured %.4f over %.1f s",
                            snap.marginMeanMs, snap.marginMinMs, snap.marginRmsMs,
                            (unsigned long long)snap.lateTicks, (unsigned long long)snap.ticks,
                            snap.blockPeriodRmsMs, snap.blockPeriodRecentRmsMs,
                            snap.blockRecentSeconds, snap.blockPeriodWorstMs,
                            (unsigned long long)snap.blockGaps, snap.driftPpm,
                            snap.hostBpm, snap.measuredBpm, snap.windowSeconds);

                // THE PAIR, AND THE RESYNC COUNT, ON ONE LINE. A residual on its own says nothing:
                // it can look excellent purely because the model keeps re-anchoring, and it can look
                // useless purely because one re-anchor landed in the sum. Shown against the raw
                // block jitter it came from, with the resync count and the worst single block beside
                // it, the four together cannot lie in either direction.
                //
                // Expect the residual to be roughly MS_MODEL_KP of the raw figure. Anything near the
                // raw figure means the model is not absorbing - look at the resync count first.
                // THE ALL-TIME RMS IS THE RIGHT DENOMINATOR HERE, and deliberately not the
                // windowed one the panel shows: ms_clock_residual_ms() is itself an all-time RMS
                // over the same blocks, so pairing it with a ten-second figure would put two
                // different spans either side of a division and call the answer a percentage.
                ms_log_line("  model  | block jitter raw %.3f ms all-time -> residual %.3f ms RMS"
                            " (%.1f %% of raw) worst %.3f ms | %llu resync(s) over %llu blocks",
                            snap.blockPeriodRmsMs,
                            ms_clock_residual_ms(&clock),
                            (snap.blockPeriodRmsMs > 0.0)
                                ? ((ms_clock_residual_ms(&clock) / snap.blockPeriodRmsMs) * 100.0)
                                : 0.0,
                            ms_clock_residual_worst_ms(&clock),
                            (unsigned long long)clock.modelResyncs,
                            (unsigned long long)clock.modelBlocks);

                // THE INCOMING CLOCK, on the same heartbeat and only once there is a source. The
                // ppm column is the one worth reading: it is this plug-in's generated clock and the
                // measured one expressed against the host's own tempo, which is the only form in
                // which a difference in the fourth decimal place of a BPM is legible.
                tMsClockInSnapshot in;

                ms_clock_in_read(clockIn, &in);

                if (ms_midi_listening() >= 0) {
                    ms_log_line("  clk in | %.4f BPM (%.4f ms/clock) | jitter RMS %.4f peak dev %.4f ms"
                                " | %u fitted | %s | %llu clock(s) %llu gap(s) | vs host %+.1f ppm",
                                in.bpm, in.periodMs, in.jitterRmsMs, in.peakDevMs, in.fitted,
                                in.running ? "RUNNING" : "stopped",
                                (unsigned long long)in.clocks, (unsigned long long)in.gaps,
                                ((snap.hostBpm > 0.0) && (in.bpm > 0.0))
                                    ? (((in.bpm / snap.hostBpm) - 1.0) * 1.0e6) : 0.0);
                }

                // THE BLOCK SIZE, reported whenever the host is not handing over a constant one.
                // Every per-block figure above assumes it is, and Live does not - so a varying size
                // is the first thing to know before reading any of them.
                if (snap.blockFramesMin != snap.blockFramesMax) {
                    ms_log_line("  blocks | host block size VARIES: %u..%u frames, %llu change(s)"
                                " over %llu blocks - per-block figures are judged against the"
                                " previous block's duration accordingly",
                                snap.blockFramesMin, snap.blockFramesMax,
                                (unsigned long long)snap.blockSizeChanges,
                                (unsigned long long)snap.blocks);
                }
            }
        }

        return kResultOk;
    }

private:
    std::atomic<int32> refCount;
    double             sampleRate{48000.0};
    int32              maxBlock{0};
    uint64_t           blocksSeen{0};
    uint32             lastState{0xFFFFFFFFu};
    double             lastTempo{-1.0};
    double             lastLogged{-1.0};
    tMsClock           clock{};
    tMsStats *         stats  = nullptr;
    tMsDetect *        detect = nullptr;
    tMsProbe           probe{};
    uint64_t           lastGraphHits = 0;
    double             paramDest = 0.0;
    double             paramCompensate = 0.0;
    double             paramAudioSource = 0.0;
    double             paramMode = 0.0;
    tMsMode            mode      = eMsModeMeasure;
    double             paramClockSource = 0.0;
    tMsClockIn *       clockIn    = nullptr;
    int                selectedPort = -1;   // the port CHOSEN, before monitor mode gates it
    float              sumBuffer[8192];
    int                statusSlot = -1;
    tMsStatus *        status     = nullptr;
    IConnectionPoint * peer       = nullptr;
    IHostApplication * host       = nullptr;
    double             lastPpq{-1.0};
    double             lastHostMs{0.0};
    bool               warnedNoContext{false};

    // ---- SPLIT BLOCKS: one device cycle handed over in more than one process() call -----------
    // See the long note in process(). Audio thread only; nothing here is read from the UI.
    uint64_t           cycleStartHostTime{0};   // wall clock at the first call of the current cycle
    uint32_t           cycleFramesSoFar{0};     // frames already handed over within it
    uint32_t           cycleFramesLast{0};      // frames the last COMPLETED cycle carried
    uint64_t           prevCallHostTime{0};
    uint32_t           observedMaxFrames{0};    // the host's real buffer, as measured not declared
    bool               haveCall{false};
    uint64_t           splitCalls{0};
    int64_t            framesSeen{0};           // real frames, for the heartbeat's time axis
};

// ── Controller ───────────────────────────────────────────────────────────────────────────────────
//
// Empty of parameters for now, and present anyway - see the note on the class IDs above.

class MstController : public IEditController, public IConnectionPoint {
public:
    MstController(void) : refCount(1) {}
    virtual ~MstController(void) {}

    // ---- IConnectionPoint ----------------------------------------------------------------------
    tresult PLUGIN_API connect(IConnectionPoint * other) SMTG_OVERRIDE {
        peer = other;
        return kResultOk;
    }

    tresult PLUGIN_API disconnect(IConnectionPoint *) SMTG_OVERRIDE {
        peer = nullptr;
        return kResultOk;
    }

    tresult PLUGIN_API notify(IMessage * message) SMTG_OVERRIDE {
        if ((message != nullptr) && (strcmp(message->getMessageID(), MST_MSG_STATUS_SLOT) == 0)) {
            int64 slot = -1;

            if (message->getAttributes()->getInt("value", slot) == kResultOk) {
                statusSlot = (int)slot;
                ms_log_line("controller: status slot %d", statusSlot);

                // THE SLOT MAY ARRIVE AFTER THE EDITOR IS OPEN - a host connects the two ends
                // whenever it likes, and an editor opened first would otherwise show nothing for
                // ever.
                if (editorView != nullptr) {
                    ms_editor_set_status_slot(editorView, statusSlot);
                }
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid)
            || FUnknownPrivate::iidEqual(iid, IEditController::iid)) {
            addRef();
            *obj = static_cast<IEditController *>(this);
            return kResultOk;
        }

        if (FUnknownPrivate::iidEqual(iid, IConnectionPoint::iid)) {
            addRef();
            *obj = static_cast<IConnectionPoint *>(this);
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

        return (uint32)refCount.load();
    }

    tresult PLUGIN_API initialize(FUnknown *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE        { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *) SMTG_OVERRIDE   { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) SMTG_OVERRIDE   { return kResultOk; }
    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE { return kParamCount; }

    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo & info) SMTG_OVERRIDE {
        if ((index < 0) || (index >= kParamCount)) {
            return kInvalidArgument;
        }
        memset(&info, 0, sizeof(info));
        info.id           = (ParamID)index;
        info.unitId       = 0;   // the root unit
        info.defaultNormalizedValue = 0.0;

        // A LIST PARAMETER'S STEP COUNT IS FIXED AT REGISTRATION and cached by the host, so it can
        // never follow how many MIDI ports the machine happens to have. Registering the live count
        // is what made GenBridge's panel name one device and open another.
        if (index == kParamMidiDest) {
            copy_string(info.title, "MIDI destination");
            copy_string(info.shortTitle, "Port");
            info.stepCount = MS_MIDI_MAX_DEST;
            info.flags     = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        } else if (index == kParamCompensate) {
            copy_string(info.title, "Latency compensation");
            copy_string(info.shortTitle, "Comp");
            copy_string(info.units, "ms");
            info.flags = ParameterInfo::kCanAutomate;
        } else if (index == kParamMode) {
            copy_string(info.title, "Mode");
            copy_string(info.shortTitle, "Mode");
            info.stepCount = eMsModeCount - 1;
            info.flags     = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        } else if (index == kParamClockSource) {
            copy_string(info.title, "Clock input");
            copy_string(info.shortTitle, "Clock in");
            info.stepCount = MS_MIDI_MAX_SOURCE;
            info.flags     = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        } else {
            copy_string(info.title, "Analyse channel");
            copy_string(info.shortTitle, "Analyse");
            info.stepCount = MS_AUDIO_SOURCES - 1;
            info.flags     = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        }
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 out) SMTG_OVERRIDE {
        char text[128] = {0};

        if (id == kParamMidiDest) {
            int slot = mst_dest_slot(value);

            if (slot <= 0) {
                snprintf(text, sizeof(text), "none");
            } else if ((slot - 1) < ms_midi_count()) {
                ms_midi_name(slot - 1, text, sizeof(text));
            } else {
                snprintf(text, sizeof(text), "-");
            }
        } else if (id == kParamCompensate) {
            snprintf(text, sizeof(text), "%.1f ms", value * MST_COMPENSATE_MAX);
        } else if (id == kParamMode) {
            static const char * names[eMsModeCount] = { "generate + measure", "clock only", "monitor" };

            snprintf(text, sizeof(text), "%s", names[(int)ms_mode_from_normalized(value)]);
        } else if (id == kParamClockSource) {
            int slot = mst_source_slot(value);

            if (slot <= 0) {
                snprintf(text, sizeof(text), "none");
            } else if ((slot - 1) < ms_midi_source_count()) {
                ms_midi_source_name(slot - 1, text, sizeof(text));
            } else {
                snprintf(text, sizeof(text), "-");
            }
        } else {
            static const char * names[MS_AUDIO_SOURCES] = { "left", "right", "left + right" };
            int index = (int)((value * (double)(MS_AUDIO_SOURCES - 1)) + 0.5);

            snprintf(text, sizeof(text), "%s",
                     names[(index < 0) ? 0 : ((index >= MS_AUDIO_SOURCES) ? (MS_AUDIO_SOURCES - 1) : index)]);
        }
        copy_string(out, text);
        return kResultOk;
    }

    tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) SMTG_OVERRIDE { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) SMTG_OVERRIDE {
        return (id == kParamCompensate) ? (v * MST_COMPENSATE_MAX) : v;
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) SMTG_OVERRIDE {
        return (id == kParamCompensate) ? (v / MST_COMPENSATE_MAX) : v;
    }

    ParamValue PLUGIN_API getParamNormalized(ParamID id) SMTG_OVERRIDE {
        return (id < kParamCount) ? values[id] : 0.0;
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) SMTG_OVERRIDE {
        if (id < kParamCount) {
            values[id] = value;
        }
        return kResultOk;
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler * h) SMTG_OVERRIDE {
        handler = h;
        return kResultOk;
    }

    // THE CONTROLLER IS TOLD THE PROCESSOR'S STATE SEPARATELY, through this rather than through
    // setState - a host restoring a project gives the component's own block to both halves, and a
    // controller that ignored it would show defaults over a correctly restored processor.
    tresult PLUGIN_API setComponentState(IBStream * stream) SMTG_OVERRIDE {
        if (stream == nullptr) {
            return kResultOk;
        }
        // THE SAME LAYOUT THE PROCESSOR WRITES, read the same way - see MST_STATE_V1_PARAMS. If
        // this read five doubles in one go it would take the first eight bytes of the destination
        // NAME as the fifth parameter, and the controller and the processor would then disagree
        // about what the plug-in is set to, with only the controller's version on screen.
        double stored[MST_STATE_V1_PARAMS] = { 0.0, 0.0, 0.0, 0.0 };
        int32  read = 0;

        stream->read(stored, (int32)sizeof(stored), &read);

        for (int i = 0; i < MST_STATE_V1_PARAMS; i++) {
            if (read >= (int32)((i + 1) * sizeof(double))) {
                values[i] = stored[i];
            }
        }
        char   name[MS_MIDI_NAME_LEN] = {0};
        double clockSource = 0.0;

        read = 0;
        stream->read(name, (int32)sizeof(name), &read);

        read = 0;
        stream->read(&clockSource, (int32)sizeof(clockSource), &read);

        if (read >= (int32)sizeof(double)) {
            values[kParamClockSource] = clockSource;
        }
        return kResultOk;
    }
    IPlugView * PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if ((name == nullptr) || (strcmp(name, ViewType::kEditor) != 0)) {
            return nullptr;
        }
        editorView = ms_create_editor_view(this, handler, statusSlot,
                                           MS_CANVAS_W, MS_CANVAS_H,
                                           editor_gone, editor_resized, this);
        return editorView;
    }

    // THE HOST OWNS THE VIEW, not this. createView() hands over a reference the host releases
    // whenever it closes the editor, and the object deletes itself at that point - so the pointer
    // kept here has to be cleared, or the next status update writes through a dangling one.
    static void editor_gone(void * user) {
        ((MstController *)user)->editorView = nullptr;
    }

    static void editor_resized(void *, double, double) {}


private:
    std::atomic<int32>  refCount;
    IConnectionPoint *  peer       = nullptr;
    IComponentHandler * handler    = nullptr;
    IPlugView *         editorView = nullptr;
    double              values[kParamCount] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    int                 statusSlot = -1;
};

// ── Factory ──────────────────────────────────────────────────────────────────────────────────────

class MstFactory : public IPluginFactory2 {
public:
    MstFactory(void) : refCount(1) {}
    virtual ~MstFactory(void) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory::iid)
            || FUnknownPrivate::iidEqual(iid, IPluginFactory2::iid)) {
            addRef();
            *obj = static_cast<IPluginFactory2 *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE  { return (uint32)++refCount; }
    uint32 PLUGIN_API release(void) SMTG_OVERRIDE { return (uint32)--refCount; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo * info) SMTG_OVERRIDE {
        memset(info, 0, sizeof(PFactoryInfo));
        strncpy(info->vendor, "Chris Purusha", PFactoryInfo::kNameSize - 1);
        strncpy(info->url, "https://github.com/chrispurusha/MidiSyncTool", PFactoryInfo::kURLSize - 1);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses(void) SMTG_OVERRIDE { return 2; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo * info) SMTG_OVERRIDE {
        PClassInfo2 info2;

        if (getClassInfo2(index, &info2) != kResultOk) {
            return kInvalidArgument;
        }
        memset(info, 0, sizeof(PClassInfo));
        memcpy(info->cid, info2.cid, sizeof(TUID));
        info->cardinality = info2.cardinality;
        strncpy(info->category, info2.category, PClassInfo::kCategorySize - 1);
        strncpy(info->name, info2.name, PClassInfo::kNameSize - 1);
        return kResultOk;
    }

    // The SUBCATEGORY is why IPluginFactory2 is implemented at all - the base interface reports only
    // that a class makes audio, not whether it is an instrument or an effect, and a host that cannot
    // tell assumes effect, looks for the audio input an effect must have, and refuses to load an
    // instrument that has none. Here it genuinely IS an effect, and says so.
    //
    // OnlyRT because a clock generator during a faster-than-realtime bounce is meaningless.
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 * info) SMTG_OVERRIDE {
        memset(info, 0, sizeof(PClassInfo2));

        if (index == 0) {
            memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, "MidiSyncTool", PClassInfo::kNameSize - 1);
            strncpy(info->subCategories, "Fx|NoOfflineProcess|OnlyRT|Tools", PClassInfo2::kSubCategoriesSize - 1);
        } else if (index == 1) {
            memcpy(info->cid, kControllerUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstComponentControllerClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, "MidiSyncTool Controller", PClassInfo::kNameSize - 1);
        } else {
            return kInvalidArgument;
        }
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->vendor, "Chris Purusha", PClassInfo2::kVendorSize - 1);
        strncpy(info->version, MST_VERSION_STRING, PClassInfo2::kVersionSize - 1);
        strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
        return kResultOk;
    }

    // NEITHER setHostContext NOR getClassInfoUnicode IS HERE. Both belong to IPluginFactory3,
    // which this does not implement - and writing a method an interface never declared gives you a
    // factory that compiles and is never asked.

    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void ** obj) SMTG_OVERRIDE {
        FUnknown * created = nullptr;

        if (memcmp(cid, kProcessorUID.toTUID(), sizeof(TUID)) == 0) {
            created = static_cast<IComponent *>(new MstProcessor());
        } else if (memcmp(cid, kControllerUID.toTUID(), sizeof(TUID)) == 0) {
            created = static_cast<IEditController *>(new MstController());
        } else {
            return kNoInterface;
        }
        tresult result = created->queryInterface(iid, obj);

        created->release();
        return result;
    }

private:
    std::atomic<int32> refCount;
};

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory * PLUGIN_API GetPluginFactory(void) {
    static MstFactory factory;

    factory.addRef();
    return &factory;
}

SMTG_EXPORT_SYMBOL bool bundleEntry(void *) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit(void)    { return true; }
}
