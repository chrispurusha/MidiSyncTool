# MIDI Sync Doctor

A VST3 plug-in for generating, measuring and correcting MIDI clock and hardware timing.

It takes the **host's** musical timing as the reference rather than an externally generated MIDI
clock, schedules a clean clock of its own to a chosen MIDI output, and measures what the hardware
at the other end actually does with it — latency, jitter and drift — by listening to the audio
coming back.

**Status: nothing is built yet.** The concept is in [`Docs/Concept.txt`](Docs/Concept.txt) and the
design thinking, what is being reused, and the open decisions are in
[`Docs/design.md`](Docs/design.md).

## Why it exists

MIDI clock out of a DAW is usually jittery, and the jitter is not the same as the hardware's own
latency — the two get blamed for each other. This separates them: it generates the cleanest clock
it can, then measures the device's response independently so the two numbers can be read apart.

## Relationship to the sibling projects

This is the fourth in a family that shares [SynthLib](https://github.com/chrispurusha/SynthLib):

- **G2-Edit** — editor for the Nord G2
- **SynthEdit** — generic multi-synth editor
- **EmuUtility** — E-mu EOS sampler utility
- **GenBridge** — bridges any CoreAudio device into a DAW

It borrows heavily from GenBridge, which already solves several of the same problems — VST3
plumbing, MIDI port selection, lock-free telemetry to the editor, and a hardware latency
measurement that emits MIDI and detects the returning audio onset. See `Docs/design.md` for what
lifts directly and what has to be written.

## Licence

GPLv3 — see [`LICENSE`](LICENSE). Third-party components and their redistribution obligations are
recorded in [`THIRD_PARTY.md`](THIRD_PARTY.md).
