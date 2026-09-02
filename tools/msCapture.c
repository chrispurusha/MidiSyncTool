/*
 * MidiSyncTool - raw input capture, for looking at what the detector is looking at.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 * Licensed under the GNU General Public License v3 - see LICENSE.
 */

// usage: msCapture <device name> <first channel> <seconds> <out.f32>
//
// Writes one channel as raw 32-bit floats. Deliberately not a WAV: nothing here reads it but a
// script, and a header is one more thing to get wrong.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msDevice.h"

static float *          gBuffer;
static _Atomic uint64_t gWritten;
static uint64_t         gCapacity;

static void capture_cb(void * user, const float * input, float * output, uint32_t frames) {
    (void)user;
    (void)output;

    uint64_t at = atomic_load(&gWritten);

    for (uint32_t f = 0; (f < frames) && (at < gCapacity); f++) {
        gBuffer[at++] = input[f * 2];   // the left of the pair that was opened
    }
    atomic_store(&gWritten, at);
}

int main(int argc, const char ** argv) {
    if (argc < 5) {
        printf("usage: msCapture <device name> <first channel> <seconds> <out.f32>\n");
        return 1;
    }
    uint32_t      first   = (uint32_t)atoi(argv[2]);
    double        seconds = atof(argv[3]);
    tDeviceInfo   info    = {};
    tDeviceStream stream  = {};

    if (!device_find(argv[1], true, &info)) {
        printf("no input-capable device matching \"%s\"\n", argv[1]);
        return 2;
    }
    gCapacity = (uint64_t)(seconds * info.sampleRate) + 65536;
    gBuffer   = calloc(gCapacity, sizeof(float));

    if (gBuffer == NULL) {
        return 2;
    }

    if (!device_open(&stream, info.id, true, first, 2,
                     device_buffer_frames(info.id), capture_cb, NULL)) {
        printf("could not open the input stream\n");
        return 2;
    }
    device_start(&stream);
    usleep((useconds_t)(seconds * 1.0e6));
    device_stop(&stream);
    device_close(&stream);

    FILE * out = fopen(argv[4], "wb");

    if (out == NULL) {
        return 2;
    }
    uint64_t written = atomic_load(&gWritten);

    fwrite(gBuffer, sizeof(float), (size_t)written, out);
    fclose(out);
    printf("%llu frames at %.0f Hz -> %s\n",
           (unsigned long long)written, info.sampleRate, argv[4]);
    return 0;
}
