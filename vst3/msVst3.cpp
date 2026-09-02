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
#include <cstring>

#include <CoreAudio/HostTime.h>

#include "msLog.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

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

class MstProcessor : public IComponent,
                     public IAudioProcessor,
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
    tresult PLUGIN_API initialize(FUnknown *) SMTG_OVERRIDE {
        ms_log_line("initialize - MidiSyncTool %s", MST_VERSION_STRING);
        return kResultOk;
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE {
        ms_log_line("terminate");
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
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) SMTG_OVERRIDE { return kResultOk; }

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
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData & data) SMTG_OVERRIDE {
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

        // WHERE THE MUSICAL POSITION SHOULD HAVE GOT TO, if nothing jumped. Anything else is a loop
        // wrap, a playhead move or a tempo change taking effect - and those are the events a clock
        // generator has to handle correctly, so they are logged the moment they happen rather than
        // whenever the heartbeat next comes round. The 2-second heartbeat was far too coarse to
        // show what a loop boundary does.
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
        double nowSeconds       = (double)blocksSeen * (double)data.numSamples / ((sampleRate > 0.0) ? sampleRate : 48000.0);
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
                     " | sysTime %lld | wall +%.2f ms | %s",
                     (unsigned long long)blocksSeen, (int)data.numSamples,
                     ctx->tempo,
                     (int)ctx->timeSigNumerator, (int)ctx->timeSigDenominator,
                     ctx->projectTimeMusic, ctx->barPositionMusic,
                     (long long)ctx->projectTimeSamples, (long long)ctx->continousTimeSamples,
                     (long long)ctx->systemTime, wallDelta,
                     flags);
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
    double             lastPpq{-1.0};
    double             lastHostMs{0.0};
    bool               warnedNoContext{false};
};

// ── Controller ───────────────────────────────────────────────────────────────────────────────────
//
// Empty of parameters for now, and present anyway - see the note on the class IDs above.

class MstController : public IEditController {
public:
    MstController(void) : refCount(1) {}
    virtual ~MstController(void) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid)
            || FUnknownPrivate::iidEqual(iid, IEditController::iid)) {
            addRef();
            *obj = static_cast<IEditController *>(this);
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
    tresult PLUGIN_API setComponentState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *) SMTG_OVERRIDE   { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) SMTG_OVERRIDE   { return kResultOk; }
    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE  { return 0; }
    tresult PLUGIN_API getParameterInfo(int32, ParameterInfo &) SMTG_OVERRIDE { return kInvalidArgument; }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) SMTG_OVERRIDE { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) SMTG_OVERRIDE { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) SMTG_OVERRIDE { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) SMTG_OVERRIDE { return 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler *) SMTG_OVERRIDE { return kResultOk; }
    IPlugView * PLUGIN_API createView(FIDString) SMTG_OVERRIDE { return nullptr; }

private:
    std::atomic<int32> refCount;
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
