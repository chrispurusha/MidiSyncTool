# msStats.h notes

The longer comments from `msStats.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_STATS_DRIFT_SECONDS`

WHAT CAN HONESTLY BE MEASURED FROM INSIDE THE PLUG-IN, and what cannot.

The plug-in hands CoreMIDI a tick and a moment for it. It never sees the wire, so it CANNOT
measure when the tick actually left - that needs a listener, and tools/midiTimestampTest.c is it.
Claiming a wire figure from in here would be a fabrication.

What it can measure is everything on its own side of that hand-off, and those are the numbers
that decide whether the wire figure is the plug-in's fault:

```
  COMMIT MARGIN - how far in the future each tick's moment was at the instant it was submitted.
    While this stays positive CoreMIDI has the tick in hand early and delivers it on its own
    timer, which is where the microsecond figures come from. The moment it goes NEGATIVE the tick
    is already late and CoreMIDI sends it at once, several bunching together. This is the single
    number that says whether MS_LOOKAHEAD_MS earns its place, and unlike the offline sweep it can
    be read from inside a real host under real load.

  BLOCK PERIOD ERROR - the wall time between audio callbacks against the nominal block duration.
    The host's own steadiness, and the thing that eats the commit margin.

  MUSICAL DRIFT - musical time elapsed against wall time elapsed, in ppm. Says whether the host's
    musical position and its audio clock agree over minutes. GenBridge measures the same quantity
    between two audio devices; here it is between the host's two notions of time.
```
How long the drift window has to be before a figure is worth showing. See the note in msStats.c:
below this the measurement is dominated by whatever the scheduler last did, not by any drift.

## 2. `MS_STATS_WINDOW_SECONDS`

HOW LONG "RECENT" IS FOR THE BLOCK JITTER RMS, and why there are two of that figure at all.

The all-time RMS is sqrt(sum / N) since the last reset, so a single bad block of size E reads
E/sqrt(N) for ever after. That is not a small effect: one 40 ms block at 256 frames / 48 kHz
still reads 0.377 ms RMS a minute later and takes THREE AND A HALF MINUTES to fall back under
0.2 ms, on a host that has been perfect throughout. Watching the panel after a hiccup therefore
means watching a number crawl rather than reading what the host is doing now.

So the panel's figure is taken over a sliding window instead - the same event reads 0.924 ms
while it is in the window and 0.000 the moment it leaves, ten seconds later.

NOTHING IS LOST BY FORGETTING, AND ONLY BECAUSE OF WHAT SITS BESIDE IT. blockPeriodWorstMs stays
ALL-TIME, so an event that has aged out of the window is still on the panel; the window answers
"is the host steady now" and the worst case answers "did anything go wrong at any point". The
all-time RMS is kept too, for the log's model-residual ratio, which would otherwise be comparing
a windowed figure against an all-time one - see the "model |" line in vst3/msPlugin.c.

TRIMMED BY TIME, NOT BY A BLOCK COUNT, so that the window is the same ten seconds whatever the
host's buffer size - and so a suspension inside it removes samples rather than stretching the
span. MS_STATS_WINDOW_MAX caps the memory: it is enough for ten seconds at 64 frames / 48 kHz,
and at settings finer than that the window simply becomes shorter, which the published span says
out loud rather than hiding.

## 3. `MS_STATS_GAP_MS`

A BLOCK PERIOD ERROR THIS LARGE IS NOT A MEASUREMENT, IT IS A GAP.

The RMS below is an all-time sum that never forgets, so a single interval that is not really a
block period poisons the figure for the rest of the session. And they happen: the host suspends
the plug-in, the audio device is released, or - the one that was actually hit - Ableton goes to
the BACKGROUND and stops being scheduled while another application is brought to the front.

Measured cost of exactly one of them: a host delivering blocks with ZERO error for five minutes,
backgrounded for thirty seconds, then perfect for five minutes more, reports 89.44 ms RMS. It
reads as a catastrophically bad host and it is entirely the metric's fault.

250 ms is some three hundred times any error a real callback has produced here (Live's worst was
5.2 ms, and that was a block SIZE change rather than lateness) and far below any suspension. The
count is published rather than swallowed - the project's own rule that a metric which drops
samples must say how many, or it is just a prettier lie. See also ms_stats_gap().

## 4. `ms_stats_gap()`

THE HOST IS ABOUT TO STOP FEEDING BLOCKS, or has just started again. Drops only the previous
block's timestamp, so the interval ACROSS the pause is never mistaken for a callback period; every
figure accumulated so far is kept, which is the point - a suspension is not a reason to throw away
a run's history, only a reason not to measure the hole it leaves.

This is the same bug the timebase model's `haveBase` had, in a second place: a flag set in one
place and cleared in none. Whenever a metric is a step between two samples, ask what clears it.
