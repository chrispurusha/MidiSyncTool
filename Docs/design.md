# MIDI Sync Doctor — design notes before the build

Written 2026-09-02 from `Concept.txt`, after a session spent inside GenBridge. Nothing is built yet.
This is what to reuse, what is genuinely new, and the decisions that want making before code.

## The one thing that decides the architecture

**MIDI clock must be SCHEDULED, not emitted.**

A clock generator that calls send at the moment `process()` runs inherits every bit of the host's
block jitter — which is precisely the defect the plug-in exists to remove. At 120 BPM a 24 PPQN clock
is one message every 20.83 ms, while a 512-frame block at 48 kHz is 10.67 ms: the clock does not
divide into the block, so block-boundary emission is guaranteed to be wrong and wrong by a varying
amount.

CoreMIDI already solves this: `MIDIPacketListAdd` takes a **timestamp** in host-clock units, and the
driver delivers at that time. So the loop is: read the host's musical position, work out the wall
time of every clock tick falling in the next block or two, and hand CoreMIDI a packet list with
future timestamps. The audio thread then only has to be *approximately* on time; the timestamps carry
the precision.

**GenBridge cannot do this today.** `gb_midi_send()` → `synthlib_midi_send_to()` builds its packet
with timestamp **0**, meaning "now". That is right for a note or a CC and wrong for this. The fix is a
timestamped variant of the SynthLib send primitive — small, and it belongs down there rather than
here, because G2-Edit's own todo already carries "MIDI events are applied at block granularity, not
sample-accurate" as a defect.

Conversion is `AudioConvertNanosToHostTime()` on `ProcessContext.systemTime` plus the offset to each
tick.

## What lifts from GenBridge with little or no change

| piece | where | note |
|---|---|---|
| VST3 skeleton | `vst3/gbVst3.cpp` | two-class factory, `IPluginFactory2`, IConnectionPoint message channel, parameter plumbing with begin/perform/endEdit |
| MIDI destination selection | `vst3/gbMidi.c/.h` | enumeration, names, slot-for-name — exactly "select the MIDI output port" |
| Editor | `vst3/gbDraw.c`, `gbView.m`, `gbEditor.mm` | Metal view drawing through SynthLib, drop-down rows, scrolling menus |
| Telemetry block | `vst3/gbStatus.h` | lock-free processor → editor status, which is what every readout in the concept's UI needs |
| Latency measurement | the `measure*` machine in `gbVst3.cpp` | **the big one — see below** |
| Harnesses | `tools/vst3check`, `tools/vst3host` | including `--audio N`, `--click X,Y`, and the poll-until-open discipline |
| Release | `do-vst3`, `do-release` | unchanged |

### The measurement machine is already most of section 4

GenBridge's instrument variant **already** emits a MIDI note, listens to the returning audio for an
onset against a noise floor with a confirm count, computes the round trip, and stores the result
against a (device UID, MIDI destination) pair. That is section 4's CALIBRATE, section 3's latency
figure, and the per-device profiles, built and hardware-proven. It wants generalising from "one note"
to "a known sequence", not writing.

## What is genuinely new

1. **Host tempo and position.** `ProcessContext` via `data.processContext`. **Trap:** VST3 3.7 added
   `IProcessContextRequirements`, and a host is entitled to give you nothing unless you implement it
   and declare what you need (`kNeedTempo | kNeedProjectTimeMusic | kNeedTransportState |
   kNeedSystemTime`). This is the same shape as the `IPluginFactory2` trap G2-Edit hit — a plug-in
   that looks correct and is silently starved by the host. Implement it from the first commit.
2. **Tick scheduling** — the arithmetic above, and the decision of how far ahead to schedule.
3. **A phase/rate estimator.** The concept asks for PLL-like behaviour. GenBridge's `drift.c` is a PI
   controller on a fill measurement; the *shape* transfers (estimate, filter, correct, never chase an
   instantaneous reading) but the plant is different — phase error in beats rather than depth in
   frames. Expect to write it fresh with `drift.c` as the model.
4. **MIDI source enumeration.** `gbMidi.c` enumerates destinations only. Monitor-only mode needs
   *sources* and an input port. Small, symmetric addition.
5. **The scrolling error graph.** Nothing existing draws one.

## Traps worth deciding about before they bite

- **Two masters.** If Live is also sending its own MIDI clock to the same port, the hardware gets two
  clocks. The plug-in cannot detect this; the UI should say plainly that Live's own sync must be off
  for that port.
- **Reported latency.** If the plug-in reports latency the host shifts it, changing when `process()`
  runs relative to the audio. For a clock generator that is probably unwanted — report zero unless
  there is a reason.
- **Offline render.** A clock generator during a faster-than-realtime bounce is meaningless. Declare
  `OnlyRT` as GenBridge does, and say so in the panel rather than producing nonsense.
- **`kInfiniteTail`.** It generates regardless of input, so the same reasoning that fixed GenBridge
  applies from day one.
- **Clock while stopped.** Some gear wants clock continuously, some only while running. A choice, not
  a default.

## Decisions wanted from CT

1. **Does phase 1 need audio input at all?** Sections 1, 2, 5 and 6 do not. Registering as an Fx with
   an input bus from the start costs nothing and leaves section 3 open; registering as an instrument
   would have to change later.
2. **Clock while the transport is stopped** — always, never, or a setting?
3. **Monitor-only mode's reference:** an incoming MIDI clock on a chosen source, or the host, or
   either? This decides whether MIDI input goes in phase 1 or later.
4. **Where the timestamped-send primitive lives** — SynthLib (all four projects benefit, G2-Edit has a
   standing todo for it) or local to this project until it settles.

## House rule: C, with C++ only where VST3 forces it

`vst3/msVst3.cpp` is the COM shim and nothing else. Everything with actual behaviour in it goes in
`src/*.c` — logging already does. That is the split the sibling projects use, and it is the reason
GenBridge's bridge core could be lifted into a command line tool without a rewrite: the only C++ in
the tree is the part the VST3 interfaces make unavoidable.

## Suggested order for the first working session

1. `do-vst3` + the VST3 skeleton building and loading in `vst3check`, registering as an Fx.
2. `IProcessContextRequirements` and a panel that shows the host's tempo, position and transport
   state. Nothing sent yet — prove the reference first.
3. MIDI destination selection, reusing `gbMidi` and the drop-down row.
4. Timestamped send in SynthLib, then clock generation with Start/Stop/Continue and SPP.
5. Jitter statistics and the telemetry block.
6. Only then the estimator, and only then audio.

Steps 1–3 are mostly assembly of things that already exist and work. Step 4 is where the real thinking
starts.
