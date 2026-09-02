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

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

int main(int argc, const char ** argv) {
    if (argc < 2) {
        printf("usage: mstDriver <plugin.vst3> [--bpm N] [--bars N] [--seconds N]\n"
               "                  [--rate N] [--block N] [--realtime]\n"
               "\n"
               "  --bpm N       tempo (default 130, matching the measured capture)\n"
               "  --bars N      loop length in bars of 4/4; 0 disables looping (default 1)\n"
               "  --seconds N   how much musical time to run (default 10)\n"
               "  --rate N      sample rate (default 48000)\n"
               "  --block N     block size (default 256)\n"
               "  --realtime    run at one second per second; the default is as fast as it will go\n");
        return 1;
    }
    double bpm      = 130.0;
    double bars     = 1.0;
    double seconds  = 10.0;
    double rate     = 48000.0;
    int32  block    = 256;
    bool   realtime = false;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--bpm") == 0) && ((i + 1) < argc)) {
            bpm = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--bars") == 0) && ((i + 1) < argc)) {
            bars = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--seconds") == 0) && ((i + 1) < argc)) {
            seconds = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--rate") == 0) && ((i + 1) < argc)) {
            rate = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--block") == 0) && ((i + 1) < argc)) {
            block = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--realtime") == 0) {
            realtime = true;
        }
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
    component->initialize(nullptr);

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

    // THE POSITION THE HOST BELIEVES IT IS AT, in quarter notes, advanced continuously. What gets
    // REPORTED is derived from it and is deliberately not the same thing at a wrap.
    double truePosQn  = 0.0;
    bool   snapNext   = false;
    bool   snapped    = false;

    printf("driving %s\n  %.3f BPM, %.0f Hz, %d-frame blocks, loop %s\n",
           argv[1], bpm, rate, block,
           (loopEndQn > 0.0) ? "on" : "off");

    for (int64 b = 0; b < totalBlocks; b++) {
        double blockQn = (double)block / qnSamples;

        // Live's report: normally the true position, but for exactly one block after a wrap it is
        // the loop start itself, whatever the true position is.
        double reportQn = snapNext ? 0.0 : truePosQn;

        if (snapNext) {
            snapped  = true;
            snapNext = false;
        }

        ctx.state = ProcessContext::kPlaying
                    | ProcessContext::kTempoValid
                    | ProcessContext::kProjectTimeMusicValid
                    | ProcessContext::kBarPositionValid
                    | ProcessContext::kTimeSigValid;

        if (loopEndQn > 0.0) {
            ctx.state |= ProcessContext::kCycleActive;
            ctx.cycleStartMusic = 0.0;
            ctx.cycleEndMusic   = loopEndQn;
        }
        ctx.tempo               = bpm;
        ctx.timeSigNumerator    = 4;
        ctx.timeSigDenominator  = 4;
        ctx.projectTimeMusic    = reportQn;
        ctx.barPositionMusic    = floor(reportQn / 4.0) * 4.0;
        ctx.projectTimeSamples  = (TSamples)llround(reportQn * qnSamples);

        // NOT SET, and that is the whole point of this harness - see the header.
        ctx.systemTime           = 0;
        ctx.continousTimeSamples = 0;

        processor->process(data);

        // THE REPORT LAGS BY ONE BLOCK ACROSS A WRAP, which is the crux. Live reports the block
        // after the wrap as the loop start EXACTLY, and the block after THAT at the remainder the
        // wrap actually left - not at remainder-plus-a-block. Measured: 3.998222, then 0.000000,
        // then 0.009750 with projectTimeSamples 216, where 256 - 39.4 = 216.6.
        //
        // So the snap block does not advance the true position: it is the one that absorbs the
        // discrepancy. Advancing through it put this harness a whole block ahead of Live and would
        // have had a scheduler developed against it come out systematically early.
        if (snapped) {
            snapped = false;
        } else {
            truePosQn += blockQn;
        }

        if ((loopEndQn > 0.0) && (truePosQn >= loopEndQn)) {
            truePosQn -= loopEndQn;
            snapNext   = true;
        }

        if (realtime) {
            usleep((useconds_t)((double)block / rate * 1.0e6));
        }
    }
    processor->setProcessing(false);
    component->setActive(false);
    component->terminate();
    processor->release();
    component->release();

    printf("done: %lld blocks (%.1f s of musical time)\n", (long long)totalBlocks, seconds);
    return 0;
}
