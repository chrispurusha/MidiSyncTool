# msLevels.c notes

The longer comments from `msLevels.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

"IS IT ARRIVING WHERE YOU THINK IT IS" is a question that has already cost two projects real time
- EmuUtility spent a session talking confidently to the wrong MIDI destination - and it is not a
question worth answering by reasoning. This opens every input a device has and prints what is on
each one, which settles it in five seconds.

usage: msLevels <device name> [seconds]

```
    clang -O2 -std=gnu11 -Wall -I ../SynthLib/audio -o msLevels msLevels.c \
        ../SynthLib/audio/device.c -framework CoreAudio -framework CoreFoundation

```
device.c is SynthLib's since 2026-09-09 - it used to be src/msDevice.c.
