# mstDriver.cpp notes

The longer comments from `mstDriver.cpp`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

A HOST THAT LIES THE WAY ABLETON LIES.

The point of this is not to be a correct VST3 host. It is to hand the plug-in a ProcessContext
that behaves the way Live 12.4.5 was MEASURED to behave on 2026-09-02 (see Docs/findings.md),
including the parts that are arguably wrong, so that clock generation can be developed and
regression-tested without a DAW and without CT sitting in front of one.

An idealised host would be worse than useless here: it would let a scheduler be written against
continuous musical time, which is exactly the assumption Live breaks at a loop wrap.

What is reproduced, and why each one matters:

```
  * systemTime is ALWAYS 0 and kSystemTimeValid is NEVER set. Live does not provide it, whatever
    the plug-in asks for through IProcessContextRequirements, so anything built on it would work
    here and fail there.
  * continousTimeSamples is always 0, same reason.
  * The block IS split at a loop boundary, and --split is what does it. This entry used to say
    the opposite, and said it on good evidence: at a 256-frame buffer Live never splits, and
    every capture behind the claim was taken at 256. At 512 it splits once every time round the
    loop - one cycle handed over as two calls at the same wall instant. Without --split the
    default is still the unsplit case, because that is what a 256-frame buffer really does.
  * AT A WRAP, projectTimeMusic and projectTimeSamples SNAP TO EXACTLY THE LOOP START for one
    block regardless of where inside that block the wrap actually fell, and the true sub-block
    phase only appears in the FOLLOWING block. This is the quirk the whole tool exists to model:
    measured at 130 BPM, a block starting at ppq 3.998222 should have ended at 4.009778, and
    instead the next two blocks reported 0.000000 and 0.009750 - the 216 samples being 256 minus
    the 39.4 the wrap fell into the block.

```
usage: mstDriver <plugin.vst3> [--bpm N] [--bars N] [--seconds N] [--rate N] [--block N] [--realtime]

## 2. `MAX_TICKS`

── The clock listener ───────────────────────────────────────────────────────────────────────────

The driver runs the plug-in FASTER THAN REALTIME by default, which is right for exercising the
wrap logic and wrong for measuring delivery - CoreMIDI will not deliver a packet stamped for a
moment that has already passed, and in fast mode most of them are. So the listener is only
meaningful with --realtime, and says so rather than reporting a misleading figure.

## 3. `gRampBpm`

THE TEMPO IN FORCE AS EACH TICK LANDED, which a ramp makes necessary. With a fixed tempo the
target interval is one number and the report can use it for every tick; under a ramp the target
moves, and measuring a moving grid against a fixed target would report the RAMP as jitter - tens
of milliseconds of it - and drown the thing being looked for.

## 4. `ECHO_PENDING`

── A DRUM MACHINE WITH A KNOWN LATENCY ──────────────────────────────────────────────────────────

--echo-ms D makes the harness impersonate a device: it hears the probe's note-on and writes a
click into the input D milliseconds later. That closes the loop through real CoreMIDI delivery
with an answer whose correct value is known in advance, which is the only way to test a
calibration without already trusting the thing being calibrated.

A drum machine could be simulated far more elaborately. It should not be: every feature added
here is one the measurement then depends on being right.

## 5. in `clock_listener()`

The WALL CLOCK on arrival, never the packet's own stamp - a virtual port passes the
sender's through untouched, and reading it back measures nothing but CoreMIDI's
ability to copy a number. That mistake produced a flattering "0.0000 ms RMS" once
already; see Docs/findings.md.

## 6. in `clock_listener()`

── One block of host duty ───────────────────────────────────────────────────────────────────────

Extracted so that the SAME code runs whether blocks are paced by a sleeping loop or by a real
audio device's callback. Two copies of the ctx-filling logic would drift apart, and the entire
value of this harness is that it behaves the way Live was measured to behave.

## 7. in `clock_listener()`

SYNTHETIC TRANSIENTS, so the detector's arithmetic can be proved without a drum machine.
A click is written into the input buffer a known number of milliseconds after every quarter
note, and the detector should report that number back.

MINUS THE LOOKAHEAD, and that is not an error. The clock stamps each tick MS_LOOKAHEAD_MS in
the future because that is genuinely when CoreMIDI will put it on the wire, and the latency is
measured from the stamp. A click injected D ms after the musical grid therefore arrives
D - lookahead after the tick it answers. With the default 10 ms lookahead, --inject-ms 20
should read 10.

## 8. in `driver_step()`

A TEMPO RAMP, linear in wall time, which is what Live's tempo automation delivers and what the
model's tempo handling had never been exercised against. A STEP change is the easy case: it
happens once and the model has seconds to recover. A ramp reports a change on EVERY BLOCK,
which is the case that found the rate term being reset before it could ever converge.

## 9. in `driver_step()`

A TRANSPORT CYCLE, so the Start/Stop/Continue edges are exercised rather than assumed:
stopped for the first tenth, playing through the middle, stopped for the last tenth. With
--from N the play begins at bar N instead of zero, which is what forces SPP + Continue rather
than Start.
THE NOTE PROBE AND THE CLOCK CANNOT BE MEASURED AT THE SAME TIME. With the transport running
the drum machine's own sequencer answers the clock, and those transients are indistinguishable
from the probe's - the first real run reported 26 hits and 29 spurious for exactly that
reason. --stopped leaves the transport alone so the probe measures the note path on its own.

## 10. in `driver_step()`

CONTIGUOUS WINDOWS, each starting exactly where the last one stopped.

Reading the clock afresh each block and taking [now, now + blockDuration) leaves a sliver
between one window's end and the next one's start, because the callback is entered a
little later each time by however much it jitters. An echo landing in that sliver was
being clamped to sample 0 - late by an arbitrary fraction of a block.

It stayed hidden until latency compensation moved the echoes onto a block boundary, and
then it read as 0.578 ms of jitter that belonged entirely to this harness. The probe grid
is 400 ms and the block 10.667 ms - exactly 37.5 blocks - so the phase between them is
fixed, and a shift of 7.5 ms was enough to park every echo on the seam.

## 11. in `driver_step()`

Written BEFORE process(), because the plug-in reads the input buffer during it.
NOT GATED ON `playing`, AND THAT MATTERS. Gating the whole block meant the input buffer was
left holding whatever the device really captured whenever the transport was stopped - so the
first and last tenth of every run fed the plug-in REAL AUDIO in the middle of a synthetic
validation. Harmless for as long as the drum machine happened to be silent, and thoroughly
misleading the moment it was not: a 22 s run injected 38 clicks on a 461.5 ms grid and the
detector saw 47 onsets, the extra nine being a Tempest playing at 500 ms. That read as a
regression in the plug-in and was entirely this harness's doing.

With --inject-ms the input is now synthetic for the WHOLE run - silence when stopped, clicks
when playing - so nothing the outside world does can reach the measurement.

## 12. in `driver_step()`

THE REPORT LAGS BY ONE BLOCK ACROSS A WRAP, which is the crux. Live reports the block after
the wrap as the loop start EXACTLY, and the block after THAT at the remainder the wrap
actually left - not at remainder-plus-a-block. Measured: 3.998222, then 0.000000, then
0.009750 with projectTimeSamples 216, where 256 - 39.4 = 216.6.

So the snap block does not advance the true position: it is the one that absorbs the
discrepancy. Advancing through it put this harness a whole block ahead of Live and would have
had a scheduler developed against it come out systematically early.

## 13. in `driver_step()`

── Real audio ───────────────────────────────────────────────────────────────────────────────────

WHY THE HARNESS NEEDED A DEVICE AT ALL. Until now it handed the plug-in silent buffers and threw
the output away, which is fine for testing clock arithmetic and useless for everything section 3
of the concept describes: the plug-in is an Fx precisely so it can hear the hardware answer its
own clock, and none of that can be developed against silence.

It also fixes a measurement problem. The sleeping loop paces blocks with usleep, whose overshoot
showed up as ~1 ms of block-period jitter and dragged the delivered clock out with it. A device
callback is the real thing at the real priority, which is what Live gives the plug-in.

## 14. in `audio_output_cb()`

── A HOST APPLICATION, minimally ────────────────────────────────────────────────────────────────

The plug-in hands its status-slot number to its controller over IConnectionPoint, and an IMessage
can only be made by the HOST - IHostApplication::createInstance is the only source of one. A
driver that passes nullptr as the initialise() context therefore never exercises that path at
all, and the first place it would be tried is the first place it must not fail.

Just enough of one to make messages and to answer its own name.

## 15. in `audio_output_cb()`

── Parameter changes ────────────────────────────────────────────────────────────────────────────

The one route by which a host tells a plug-in that a control moved, and therefore the route that
replaced the MST_* environment variables when it turned out a host launched from the Dock inherits
no shell. Exercised here so that Live is not the first place it runs.

## 16. in `main()`

THE DEVICE DECIDES THE RATE AND THE BLOCK SIZE, so it has to be found before the plug-in is
told either. Asking a device for 48000/256 and then telling the plug-in it got them, when the
device quietly kept 44100/512, is the kind of mismatch that produces a timing bug nobody can
find.

## 17. in `main()`

EITHER DIRECTION. A CoreMIDI source and destination on the same virtual port are not
obliged to share a name, and IAC does not: the destination reads "IAC Driver Bus 1"
and the source just "Bus 1". A one-directional substring test silently connects to
nothing and reports no measurement at all, which reads as "the clock never arrived".

## 18. in `main()`

A SPLIT AT THE LOOP BOUNDARY - Live's actual behaviour at a 512-frame buffer, and
the one the harness's header used to say never happens. It does; it just does not at
256, which is what every capture behind that claim was taken at.

The cycle that CONTAINS the wrap goes over as two calls at the same wall instant,
split where the wrap falls. Nothing else changes: the pair still sums to one cycle
and the next cycle is still due one cycle later, so a plug-in that handles it
correctly must report exactly the same block period as it does without --split.
That equality is the test.

## 19. in `main()`

AN ABSOLUTE DEADLINE, not a sleep of one block's length. usleep() always
overshoots, and sleeping a fixed amount each time accumulates that overshoot -
this harness ran 20% slow (wall +6.4 ms against the 5.33 ms it was aiming for),
which dragged every scheduled tick out with it and produced a 3.3 ms mean error
that looked like a defect in the plug-in and was entirely mine.

Sleeping until block N is DUE keeps the wall clock locked to musical time however
badly any individual sleep behaves.
