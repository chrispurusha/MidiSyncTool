Build a VST3 plugin for Ableton Live that provides **high-quality MIDI Clock/transport generation, timing analysis, jitter monitoring, and automatic hardware latency calibration**.

The plugin is inserted on an audio track in Live. It uses **VST3 host timing information** as the authoritative musical clock rather than relying on Live's externally generated MIDI Clock.

## Core concept

```text
Ableton Live
    │
    │ VST3 host timing / tempo / musical position
    ▼
MIDI Sync Doctor
    │
    │ conditioned MIDI Clock + transport
    ▼
Selected MIDI output port
    │
    ▼
Hardware drum machine / sequencer
    │
    ▼
Audio return
    │
    ▼
MIDI Sync Doctor timing analysis
```

## 1. MIDI Clock generator

Generate MIDI Clock (`F8`) from the VST3 host's timing information.

Also generate:

* Start
* Stop
* Continue
* Song Position Pointer where appropriate

The goal is to produce a **stable, precisely scheduled MIDI clock**, independent of jitter in an externally generated MIDI clock.

Allow selection of the MIDI output port.

## 2. Clock/jitter analysis

Timestamp generated and/or monitored MIDI Clock events and calculate:

* estimated BPM
* instantaneous phase error
* RMS jitter
* peak jitter
* long-term drift
* timing stability

If monitoring an incoming MIDI clock, **do not blindly reproduce early/late clock events**.

An unexpectedly early clock should be treated as an observation used to update the timing/jitter estimator, but should not immediately pull the generated output clock forwards.

Use a filtered phase/rate estimator, potentially PLL-like, to distinguish:

* short-term jitter
* genuine clock-rate changes
* phase changes
* long-term drift

## 3. Audio timing measurement

Because the plugin is an audio effect, it can analyse the incoming audio from the hardware device.

Compare expected musical event times against detected audio transients.

This allows measurement of:

* fixed MIDI-to-audio latency
* timing jitter
* peak timing deviation
* long-term drift

Example:

```text
Average latency     4.72 ms
Jitter RMS          0.31 ms
Peak deviation      1.18 ms
Drift               +0.04 ms/min
```

## 4. Automatic calibration

Provide a CALIBRATE function.

The plugin generates a known sequence of MIDI events, detects the resulting audio transients, and determines the hardware's actual timing characteristics.

It should be able to report:

```text
Device latency      4.72 ms
Jitter RMS          0.31 ms
Drift               +0.04 ms/min

Recommended offset  -4.72 ms
```

The user can then enable automatic compensation.

Potentially maintain per-device calibration profiles.

## 5. Monitor-only mode

The plugin should also work without controlling MIDI.

In monitor mode it simply analyses an existing MIDI/audio setup and reports:

* MIDI clock BPM
* clock jitter
* phase error
* hardware latency
* audio timing jitter
* drift
* timing quality

## 6. UI

Keep the initial UI simple and instrument-like:

```text
MIDI SYNC DOCTOR

MASTER
  Ableton             120.000 BPM

MIDI OUTPUT
  Port                Fireface MIDI 1
  Clock               120.000 BPM
  Jitter              0.03 ms
  Transport           RUNNING

MEASURED DEVICE
  Latency             4.72 ms
  Jitter RMS          0.31 ms
  Peak deviation      1.18 ms
  Drift               +0.04 ms/min

TIMING QUALITY        EXCELLENT
```

Include a scrolling timing-error graph.

## Architectural principle

Separate:

**Reference → measurement → estimation → generation → correction → telemetry**

Do not simply react to instantaneous MIDI Clock arrival times. Estimate the underlying clock phase/rate and independently schedule the cleanest possible output.

The conceptual inspiration is the existing **GenBridge** VST3 project, particularly its approach to measuring error, estimating drift, applying controlled correction, and exposing timing telemetry.

Initial prototype should focus on:

1. VST3 host tempo/position acquisition
2. MIDI output port selection
3. accurate MIDI Clock generation
4. Start/Stop/Continue handling
5. timing/jitter statistics
6. basic UI

Then add audio transient detection and automatic hardware latency calibration.
