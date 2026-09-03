# MIDI Sync Doctor

A VST3 plug-in for generating, measuring and correcting MIDI clock and hardware timing.

It takes the **host's** musical timing as the reference rather than an externally generated MIDI
clock, schedules a clean clock of its own to a chosen MIDI output, and measures what the hardware
at the other end actually does with it — latency, jitter and drift — by listening to the audio
coming back.

**Status: working, and taking real measurements.** It generates and schedules a clock, corrects for
the host's own block jitter, measures a device's latency and jitter from the returning audio, and has
a **monitor mode** that sends nothing at all and recovers the grid from the audio instead — so gear
already running on someone else's clock can be measured too.

Results so far are in [`Docs/measurements.md`](Docs/measurements.md), including a four-way comparison
of clock masters. The short version: on this rig the drum machine's own timing floor swamps every
difference between clock sources, and what actually separates the masters is **tempo accuracy**, not
jitter.

The concept is in [`Docs/Concept.txt`](Docs/Concept.txt), the design thinking and open decisions in
[`Docs/design.md`](Docs/design.md), and the full history — every measurement and every trap that cost
real time — in [`Docs/findings.txt`](Docs/findings.txt).

## Why it exists

MIDI clock out of a DAW is usually jittery, and the jitter is not the same as the hardware's own
latency — the two get blamed for each other. This separates them: it generates the cleanest clock
it can, then measures the device's response independently so the two numbers can be read apart.

## Releases

```
./do-release beta
```

Builds a universal (arm64 + x86_64) plug-in, ad-hoc signs it, and writes a `.dmg` containing it and
a `Read Me First.txt` to the Desktop, ready to attach to a GitHub release. It never installs over
the copy the machine is running, and it never creates or pushes the tag — it prints the `git tag`
commands to run once the `.dmg` has been checked.

Versions are semver git tags of the form `V<major>.<minor>.<patch>-beta.<n>`. The `.dmg` is built
before the tag exists, so the script computes the next version from the newest tag: `beta` is the
everyday one, with `patch`, `minor`, `major` and `final` for the rest. An explicit version
(`./do-release 0.9.0-rc1`) is used as given — which is how the first release had to be named, since
the repository had no tags at all.

There is no paid Apple Developer membership behind this project, so releases cannot be notarized.
A host will silently refuse to list the plug-in until the quarantine flag is cleared, and those
instructions ship inside the `.dmg`.

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
