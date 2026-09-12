# midiTimestampTest.c notes

The longer comments from `midiTimestampTest.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Does CoreMIDI actually honour a FUTURE timestamp, and how tightly?

The whole scheduling plan is: compute when each clock tick should sound, hand CoreMIDI a packet
stamped for that host time, and let the driver deliver it precisely. That is an assumption. This
sends 200 clock bytes at exactly 20.833 ms apart (24 PPQN at 120 BPM) down IAC, receives them on
IAC's own source, and reports what the arrival spacing actually was.

## 2. in `main()`

Two modes. SCHEDULED hands every packet over at once, each stamped for its own future
moment. IMMEDIATE sends each one with timestamp 0 from a sleeping loop - the shape of a
generator that emits at block boundaries instead of scheduling, which is the thing the whole
design is trying to avoid. Measuring both turns "scheduling is better" into a number.
