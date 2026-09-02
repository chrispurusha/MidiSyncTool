/*
 * MidiSyncTool - per-channel input level meter.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

// "IS IT ARRIVING WHERE YOU THINK IT IS" is a question that has already cost two projects real time
// - EmuUtility spent a session talking confidently to the wrong MIDI destination - and it is not a
// question worth answering by reasoning. This opens every input a device has and prints what is on
// each one, which settles it in five seconds.
//
// usage: msLevels <device name> [seconds]

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msDevice.h"

#define MAX_CHANNELS    (64)

static _Atomic uint32_t gPeakBits[MAX_CHANNELS];   // float bits, so the callback needs no lock
static uint32_t         gChannels;

static void level_cb(void * user, const float * input, float * output, uint32_t frames) {
    (void)user;
    (void)output;

    if (input == NULL) {
        return;
    }

    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t c = 0; c < gChannels; c++) {
            float value = fabsf(input[(f * gChannels) + c]);
            uint32_t bits;

            memcpy(&bits, &value, sizeof(bits));

            uint32_t previous = atomic_load(&gPeakBits[c]);
            float    prior;

            memcpy(&prior, &previous, sizeof(prior));

            if (value > prior) {
                atomic_store(&gPeakBits[c], bits);
            }
        }
    }
}

int main(int argc, const char ** argv) {
    if (argc < 2) {
        printf("usage: msLevels <device name> [seconds]\n");
        return 1;
    }
    double        seconds = (argc > 2) ? atof(argv[2]) : 5.0;
    tDeviceInfo   info    = {};
    tDeviceStream stream  = {};

    if (!device_find(argv[1], true, &info)) {
        printf("no input-capable device matching \"%s\"\n", argv[1]);
        return 2;
    }
    gChannels = (info.inputChannels < MAX_CHANNELS) ? info.inputChannels : MAX_CHANNELS;

    printf("%s - %u inputs at %.0f Hz, listening for %.0f s\n",
           info.name, gChannels, info.sampleRate, seconds);

    if (!device_open(&stream, info.id, true, 0, gChannels,
                     device_buffer_frames(info.id), level_cb, NULL)) {
        printf("could not open the input stream\n");
        return 2;
    }
    device_start(&stream);
    usleep((useconds_t)(seconds * 1.0e6));
    device_stop(&stream);
    device_close(&stream);

    for (uint32_t c = 0; c < gChannels; c++) {
        uint32_t bits = atomic_load(&gPeakBits[c]);
        float    peak;

        memcpy(&peak, &bits, sizeof(peak));

        printf("  in %2u (channel %2u): %8.5f  %7.1f dBFS%s\n",
               c, c + 1, peak,
               (peak > 0.0f) ? (20.0 * log10((double)peak)) : -999.0,
               (peak > 0.01f) ? "   <- signal" : "");
    }
    return 0;
}
