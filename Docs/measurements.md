# Measurements

Standing results for MidiSyncTool. Updated as figures are taken; the narrative of how each was
arrived at, and the traps along the way, are in [`findings.txt`](findings.txt).

**Rig throughout:** Tempest (drum machine) with audio on QU-24 inputs 15/16 (index 14), Cirklon 2
where noted, MacBook running Ableton Live. All figures 2026-09-03.

---

## 1. Clock master comparison

Four clock masters, measured at the **Tempest's audio output**. Identical throughout: the same
single sample, the same one-hit-per-beat pattern, and the same analysis (monitor mode, a phase-and-
rate line fitted per pattern position). Only the master differs.

| # | Clock master | Path to Tempest | Fitted cycle | Nominal | **Jitter RMS** | Peak dev | Tempo accuracy |
|---|---|---|---|---|---|---|---|
| 1 | Tempest's own crystal | — (internal) | 470.637 ms | 470.588 (127.5 BPM) | **0.132 ms** | 0.261 ms | **+104 ppm** |
| 2 | Cirklon, free-running | DIN | 500.000 ms | 500.000 (120.0 BPM) | **0.132 ms** | 0.315 ms | ≤10 ppm |
| 3 | This plug-in (filtered) | → Cirklon → DIN | 461.547 ms | 461.538 (130.0 BPM) | **0.133 ms** | 0.276 ms | +18 ppm |
| 4 | Live's native MIDI clock | → Cirklon → DIN | 461.551 ms | 461.538 (130.0 BPM) | **0.143 ms** | 0.318 ms | +27 ppm |

### Established

**Jitter is ~0.132 ms in every configuration.** Four different masters — a drum machine's own
crystal, a hardware sequencer, a filtered clock and a DAW's native clock — produce the same spread at
the audio output. That figure is the **Tempest's own floor**: its voice triggering, plus the QU-24's
A/D, plus the detector's 0.013 ms. Nothing upstream of it can go below that.

**Tempo accuracy is the only real separator, and it is 10x.** The Tempest's internal crystal runs
~104 ppm slow — about **19 ms of drift over a three-minute piece** — against ~10 ppm for the Cirklon.
That, and not jitter, is the practical case for an external master with this gear.

**Rows 3 and 4 read +18 and +27 ppm, and neither is an error.** Both are locked to Live's transport,
which advances with the audio clock, and this rig's audio clock is ~20 ppm off mach time — measured
independently by the drift telemetry. The 9 ppm between them is inside the noise of a 45-second
window.

### Provisional — do not cite yet

**Row 4 minus row 3 is 0.010 ms RMS**, in the plug-in's favour (~0.053 ms in quadrature). The
direction is right and two independent observers saw it — the offline driver at 0.143 ms and Live's
own plug-in instance at 0.138 ms over 134 onsets — but it is close to run-to-run scatter and rests on
one run each.

**More telling is what did not happen.** Live's block jitter measures 0.219–0.232 ms RMS. If the
chain passed clock jitter through, row 4 should have landed near sqrt(0.132² + 0.232²) ≈ **0.27 ms**.
It read 0.143. Something absorbed almost all of it, and the Cirklon — present in rows 3 and 4,
re-clocking — is the obvious candidate. **Rows 3 and 4 therefore largely measure the Cirklon's
immunity, not this plug-in's benefit.**

### Superseded

An earlier comparison gave 0.113 ms via the Cirklon against 0.182 ms for USB direct, and was reported
as a 38 % improvement. **It is withdrawn.** Both were measured against the plug-in's own tick grid
using a drum-kit pattern later shown to carry 8 ms outliers and a 4.3 ms bias between two different
drum sounds. With a clean sample the difference between masters vanishes entirely.

---

## 2. The generated clock, measured on the wire

Delivered into IAC and timed on arrival, so no drum machine, converter or detector is in the path.

| host pacing | host block jitter | model residual | **wire jitter** |
|---|---|---|---|
| QU-24 callback | 0.008 ms RMS | 0.0003 ms (4.3 %) | **0.0134 ms RMS** |
| usleep harness | 0.847 ms RMS | 0.009 ms (1.1 %) | **0.0232 ms RMS** |
| Ableton Live | 0.219 ms RMS | 0.003 ms (1.6 %) | not measured on the wire |

The residual sits at `MS_MODEL_KP` of the raw block jitter, which is the timebase model behaving
exactly as designed. `tools/midiTimestampTest.c` measures **0.019 ms RMS** through the same port with
no block pacing at all, so these figures are at the rig's own floor.

**Live's native clock has never been measured on the wire.** Every claim about how much better this
plug-in's clock is rests on that gap. See the open question below.

---

## 3. The measurement chain's own floor

Every figure above must be read against what the instrument itself contributes.

| stage | contribution |
|---|---|
| onset detector + block quantisation | **0.013 ms RMS** (validated: 10.009 ms recovered against a true 10.000) |
| plug-in → CoreMIDI → wire | 0.013 – 0.023 ms RMS |
| monitor-mode grid fit | 0.027 ms RMS against a known-perfect injected grid; recovered 461.548 ms against a true 461.5385 (**22 ppm**, which is this rig's own crystal difference) |

### The earlier jitter budget

Measured against the Tempest over USB with the plug-in as master, before the clean-sample work:

| stage | jitter RMS | share of total power |
|---|---|---|
| plug-in → CoreMIDI → wire | 0.015 – 0.023 ms | ~1.2 % |
| onset detector | 0.013 ms | ~0.5 % |
| USB + Tempest + QU-24 A/D | ~0.184 ms | **~98 %** |

Removing the generated clock's contribution entirely would move a 0.185 ms round trip to 0.1839 ms.
Section 1 reaches the same conclusion by substitution rather than subtraction: **the clock is not the
limit, and has not been for some time.**

---

## 4. Device latency

| path | mean | jitter | notes |
|---|---|---|---|
| Tempest note probe (sequencer idle) | 21.94 ms | 0.162 ms | round trip, includes A/D and input buffering |
| Tempest clock-driven | 21.94 ms | 0.169 ms | identical, so its sequencer adds nothing over its note path |
| Tempest via Live | 17.31 ms | 0.182 ms | same rig, different host |

**The jitter column here is stale and the means are not.** All three were taken with the drum-kit
pattern later shown to carry 8 ms outliers and a 4.3 ms inter-sound bias, so those jitter figures are
pessimistic and are superseded by section 1. The **means** are unaffected — a constant bias cancels
out of a mean once it is calibrated, and the outliers were too few to move it.

**The two means are not comparable and the difference is not an improvement.** The offline harness
reads ~4.5 ms longer than Live for the same rig; the suspected cause is `mstDriver`'s own capture
ring priming (one 256-frame block is 5.33 ms), unconfirmed. The practical consequence: a compensation
value seeded from a harness run would over-compensate by about a block. **Trust Live's figure** until
an audio loopback settles it.

---

## Open questions

1. **Live's native clock on the wire.** Point Live's Sync at IAC, plug-in in Monitor so it sends
   nothing, and time the arrivals with the same listener that produced 0.0134 ms. Then the same for
   this plug-in. That pair is the only defensible answer to "how much better is it".
2. **The A/B direct to the Tempest over USB**, Cirklon removed — the only configuration where the
   filtering can show its worth without a re-clocker absorbing the difference first.
3. **Repeat rows 3 and 4.** A 0.010 ms difference deserves more than one run each.
4. **Audio loopback** (QU-24 out 29 → in 15) to split the ~0.184 ms into transport, device and desk,
   and to pin the constant part of the round trip.

## How these were taken

Monitor mode (`Mode` on the panel, or `--param 3 1.0` to `tools/mstDriver`) makes the plug-in send
nothing and fit a grid to the audio instead, so a master running on its own timebase can be measured
passively. The offline driver runs a second instance purely as an observer while the rig does
whatever it is doing.

Two properties of the method are worth knowing before reading any figure from it:

- **Use one sound, at a constant velocity.** Onset detection is biased by attack time — a kick's
  envelope ramps over milliseconds where a hat's does not — so two different drums in one pattern sit
  at two different apparent phases. Measured here at **4.3 ms** between two sounds, which is 30x the
  jitter being looked for.
- **Read the peak-to-RMS ratio before the RMS.** About 2:1 is ordinary scatter; 8:1 or more means a
  handful of bad onsets are carrying the figure. That ratio diagnosed three separate faults in one
  day, two of them in this tool rather than in the hardware.
