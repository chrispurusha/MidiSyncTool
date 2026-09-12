# msCapture.c notes

The longer comments from `msCapture.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

usage: msCapture <device name> <first channel> <seconds> <out.f32>

Writes one channel as raw 32-bit floats. Deliberately not a WAV: nothing here reads it but a
script, and a header is one more thing to get wrong.

```
    clang -O2 -std=gnu11 -Wall -I ../SynthLib/audio -o msCapture msCapture.c \
        ../SynthLib/audio/device.c -framework CoreAudio -framework CoreFoundation

```
device.c is SynthLib's since 2026-09-09 - it used to be src/msDevice.c.
