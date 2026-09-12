# midiListTest.c notes

The longer comments from `midiListTest.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Do the MIDI destination and source lists notice the world changing under them?

Both of the bugs this answers were carried over from GenBridge and fixed on 2026-09-08 (see
Docs/findings.md): the lists were whatever existed when the plug-in loaded, and rebuilding one
blanked it for every other instance in the host process for the length of a CoreMIDI walk. Neither
can be seen by watching one plug-in on a settled rig, which is why this exists - it makes the world
change on purpose, by creating and disposing VIRTUAL endpoints in this process. That is a real
kMIDIMsgSetupChanged, indistinguishable from a synth being switched on, and it needs no hardware.

```
    clang -O0 -g -std=gnu11 -Wall -Wextra -I ../src -I ../SynthLib/plugin -o midiListTest \
        midiListTest.c ../src/msMidi.c ../SynthLib/plugin/synthlibLog.c \
        -framework CoreMIDI -framework CoreAudio -framework CoreFoundation

```
Every line prints what it expects beside what it got. The run loop is spun for a second after each
change because CoreMIDI delivers the notification on it - a client created on a thread with no run
loop simply never hears, which is worth knowing if this is ever moved into another harness.
