# msDetect.c notes

The longer comments from `msDetect.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_DETECT_REARM_RATIO`

AND THE DETECTOR DOES NOT RE-ARM UNTIL THE SOUND HAS ACTUALLY GONE.

A time-only refractory cannot know whether the sound that caused a detection has finished. The
Tempest's kick had not: against a 40 ms refractory it produced one spurious onset per hit, at gaps
of 40.0 to 46.3 ms - several at exactly the refractory, which is the giveaway that the envelope
was still over the threshold the instant the gate lifted.

Captured from the device at 48 kHz, that kick is still within 15 dB of peak 100 ms after the
onset and ripples by 5 to 10 dB on the way down. So the re-arm is measured against THE HIT'S OWN
PEAK rather than against a threshold the drum is dragging upwards - and against the frozen noise
floor as well, since 12 dB alone is inside the ripple.

## 2. `MS_DETECT_ONSETS`

HOW MANY ONSETS THE FITTED WINDOW HOLDS. A SLIDING window rather than the whole run, deliberately:
a fit over everything since the transport started would slowly stop describing the present, and a
tempo that moved once would bias the residual for ever afterwards. 256 quarter notes at 130 BPM is
about two minutes, which is far more than enough to separate jitter from drift.

## 3. `monitor_fit()`

── Monitor mode: recovering a grid from the onsets alone ────────────────────────────────────────

Three steps, and the middle one is the one that is easy to leave out.

```
  1. Estimate the hit PERIOD as the median interval between consecutive onsets. Median rather than
     mean because a missing hit produces an interval of twice the period, and one of those drags a
     mean badly while leaving a median alone.
  2. Assign each onset to a grid SLOT, n = round((t - t0) / period). Without this a pattern with
     any rest in it is fitted against a straight 0,1,2,3... counter, the slots after the rest are
     all off by one, and the fit reports the resulting nonsense as jitter.
  3. Least squares for phase and rate together over (n, t), then the residual about that line.

```
Assumes the hits are on a REGULAR grid, which is what a calibration pattern is. A deliberately
swung or humanised pattern would have its swing reported as jitter, correctly but uselessly.

## 4. in `monitor_fit()`

── 1. HOW LONG THE PATTERN IS, in onsets ───────────────────────────────────────────────────

THE ASSUMPTION THAT A SINGLE EVENLY-SPACED GRID DESCRIBES A DRUM PART IS FALSE, and it fails
in a way that looks exactly like a broken device. Measured against a Tempest playing a real
part, self-clocked: the gaps alternate 122.4 and 348.3 ms, whose ratio is 2.845 - not a whole
number, so NO uniform grid can contain both. Fitting one anyway reported 31 ms RMS of
"jitter" from a machine whose gaps repeat to within a millisecond.

What is actually there is a PATTERN that repeats: k onsets, then the same k again. Each
position within it can sit wherever it likes - swung, or displaced because that position is a
different drum whose attack the detector crosses at a different moment - and none of that is
jitter. Jitter is how much a given position moves FROM ONE REPEAT TO THE NEXT, and that is
what this measures, by fitting each position its own line and pooling the residuals.

The repeat length is the smallest k whose gap sequence matches itself k apart.

## 5. in `monitor_fit()`

── 2. EACH POSITION GETS ITS OWN LINE ──────────────────────────────────────────────────────

Position q occurs at onsets q, q+k, q+2k... spaced by one CYCLE. Those are evenly spaced by
construction whatever the pattern does inside the cycle, so a straight line fits them, and the
residual about it is the only thing here that deserves to be called jitter.

## 6. in `monitor_fit()`

THE CYCLE INDEX COMES FROM TIME, NOT FROM THE ONSET'S ORDINAL. This is the difference between
a fit that survives a missed hit and one that is destroyed by it.

Counting 0, 1, 2... along the onsets assumes every hit was detected. Miss ONE - a quiet hit, a
threshold not quite crossed - and every onset after it is numbered one cycle short, the fit is
dragged through a discontinuity, and the residuals explode. Measured: the same sample and
pattern that reads 0.132 ms on a clean run reported 8.65 ms RMS with a 70 ms peak on a run
that dropped about three hits in eighty.

Rounding elapsed time to the nearest whole cycle instead leaves a GAP in the numbering where
the missing hit was, which is what actually happened, and costs nothing when nothing is
missing. The single-grid version this replaced had that property; the rewrite lost it.

## 7. in `monitor_fit()`

WHERE EACH HIT SITS INSIDE THE CYCLE, as a fraction. This is the swing measurement, and it is
the figure the jitter number deliberately throws away: two hits at 0.000 and 0.250 are
straight sixteenths, at 0.000 and 0.260 they are displaced by 1 % of a cycle. Whether that is
the sequencer swinging or the detector crossing two different drums' attacks at different
points, this is the number that shows it - and it CANNOT be seen in an RMS.

## 8. in `ms_detect_set_source()`

GOING QUIET PRESERVES THE FIGURES; ANY OTHER CHANGE CLEARS THEM.

Every other transition starts a different measurement - a monitor reading and a latency
reading are not the same quantity and must never be averaged together - so the figures go.
eMsDetectOff starts no measurement at all, which is exactly why the last run's numbers stay:
they are what the compensation setting was derived from, and throwing them away would leave a
value in force on the panel with nothing on screen saying where it came from.

Leaving Off DOES clear, because that transition starts a run.

## 9. `match_expectation()`

The oldest unused expectation that this onset could plausibly belong to. Oldest rather than
nearest, deliberately: a device with a latency approaching the spacing between hits would
otherwise start matching each transient to the NEXT hit and report a latency near zero, which is
both wrong and flattering.

## 10. in `ms_detect_audio()`

NOTHING IS LISTENED FOR WHEN NOTHING IS BEING MEASURED, and the early return is here rather
than only at the call site because THE AGEING BELOW IS THE POINT. A detector that kept ageing
expectations against silence would count a miss per beat for the whole session - see
eMsDetectOff. The preserved figures also have to stay untouched, and every path below this
line writes to them.

## 11. in `ms_detect_audio()`

A ONE-POLE HIGH PASS AT ROUGHLY 150 Hz before anything else. A room's rumble and a kick's own
fundamental both arrive slowly and would drag the envelope up ahead of the transient itself,
biasing every onset early by however long the low end takes to build. What is wanted is the
click, not the body.

## 12. in `ms_detect_audio()`

THE FLOOR IS NOT UPDATED WHILE A HIT IS SOUNDING. Letting it track the drum was half of
the retriggering: over a 100 ms tail the floor climbed towards the signal, the threshold
climbed with it, and both the detection and the re-arm decision were then being made
against a reference the drum was setting for itself.

## 13. in `ms_detect_audio()`

Waiting for the previous sound to die away, not for a clock.

BOTH CONDITIONS. Twelve dB below the hit's own peak is not enough on its own - the
Tempest's kick ripples by more than that while it decays. The sound has to be back at
the noise floor as well, which is the only statement that actually means "it has
finished". The floor is frozen while this waits, so it is the floor from BEFORE the
hit and the drum cannot raise its own bar.

## 14. in `ms_detect_audio()`

NOTHING IS DUE, SO NOTHING IS LOOKED FOR.

This is a calibration instrument, not a general onset detector: it knows exactly when a
transient is expected, and outside those windows a rising edge is not a measurement of
anything. The Tempest's kick has a long, ripply tail that hovers around the absolute floor
for 300 ms - re-crossing it repeatedly - and every one of those crossings was being
reported as a spurious hit.

Tightening the threshold to exclude that tail would have been the wrong repair. It would
have been tuned to this drum, on this desk, at this gain, and it would have thrown away
quiet hits from the next device measured. Asking "is one due?" costs a comparison and is
true for every device.
MONITOR MODE IS THE ONE CASE WHERE NOTHING IS EVER "DUE". There is no expectation queue,
so the pending test would reject every onset. What still protects it from the Tempest's
rippling tail is the armed/refractory machinery above, which is independent of the queue
and is doing the real work in all three modes.

## 15. in `ms_detect_audio()`

REFITTED ON EVERY ONSET, which sounds expensive and is not: onsets arrive a few times
a second at most, and the whole fit is a sort and two passes over 256 doubles. Doing
it here rather than in the reader keeps every figure published by the thread that owns
it, which is the pattern the rest of this file follows.

## 16. in `ms_detect_audio()`

WORTH THE LINE. A spurious onset is either a second transient inside one sound - a
kick's body after its click - or a hit nobody asked for, and the gap since the last
onset is what tells the two apart. Guessing between them by adjusting the refractory
until the count looks tidy would be fitting the instrument to the answer.
