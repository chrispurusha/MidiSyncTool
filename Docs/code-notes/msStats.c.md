# msStats.c notes

The longer comments from `msStats.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

THE THREADING SHAPE, which is GenBridge's and is deliberate.

Everything the audio thread needs to accumulate lives in plain, non-atomic fields that ONLY the
audio thread touches. Once a block it copies the derived figures into atomics that the UI reads.
The UI therefore never contends with the audio thread for a lock it must not wait on, and the
audio thread never pays for an atomic per tick.

The snapshot is not a consistent set - the UI can read a mean from one block and a worst case
from the next. For a display refreshed 30 times a second that is invisible and it costs nothing;
a seqlock here would buy correctness nobody could observe.

## 2. in `ms_stats_reset()`

THE BLOCK SIZE FIELDS ARE PART OF THE RESET TOO, and leaving them out was a bug rather than a
decision: the atomics below were being published as zero while the working fields kept their
old values, so the panel showed a cleared range and the very next block put the stale one
straight back. Every other figure here describes the run since the reset and these must as
well - not least so that a host whose buffer size is changed between runs stops reporting a
range it no longer uses.

## 3. `window_tail()`

THE SLIDING WINDOW BEHIND THE RECENT BLOCK-JITTER RMS - see MS_STATS_WINDOW_SECONDS in the header
for why the panel's figure forgets and the worst case does not.

Kept as a running sum with each leaving sample subtracted, which is O(1) per block. A running sum
drifts, so it is recomputed exactly once every MS_STATS_WINDOW_MAX pushes: one pass over at most
8192 doubles, tens of seconds apart, which is nothing beside the arithmetic it keeps honest over a
session hours long.

## 4. in `window_add()`

A FULL RING OVERWRITES ITS OWN OLDEST ENTRY, so that entry has to leave the sum before it is
lost. Saturating means the host's buffer is small enough that MS_STATS_WINDOW_SECONDS does not
fit in the ring; the published span then reads shorter than the target, which is the truth and
is why it is published at all.

## 5. in `ms_stats_block()`

AGAINST THE PREVIOUS BLOCK'S DURATION, NOT THIS ONE'S. The wall time between callback n-1 and
callback n is the time the device took to consume block n-1's samples, so block n-1 is what it
has to be judged against.

With a fixed block size the two are identical and the error is invisible - which is exactly
why it survived every offline run. A host that VARIES the block size, as Live does, then has
every size change reported as a timing error: a 256-frame change at 48 kHz is 5.33 ms, and it
turned up as a 5.209 ms worst case in Live with the clock itself perfectly steady.

## 6. in `ms_stats_block()`

A GAP IS NOT A LATE BLOCK. ms_stats_gap() covers the suspensions the host announces; this
covers the ones it does not, and backgrounding is the case in point - the process simply
stops being scheduled, with no setProcessing(false) to say so. Counted, never summed: one
of these is worth more than the whole rest of the session put together.

## 7. in `ms_stats_block()`

DRIFT IS ONLY MEANINGFUL WHILE PLAYING, and only from an anchor taken while playing. Measuring
across a stop would count the stopped wall time against no musical time at all and report an
enormous fictional drift.
WHETHER THE ANCHOR WAS ALREADY SET WHEN THIS BLOCK ARRIVED, which decides whether this block's
frames belong inside the window. They do not: the anchor's wall time is taken at the START of
this block, so counting the frames it goes on to deliver puts one whole block of audio time
against no wall time at all.

It showed as a drift figure decaying like 1/t - 158 ppm at 30 seconds, 138 at 34 - which is
the signature of a fixed offset being amortised rather than of any real drift. One 256-frame
block is 5.33 ms, and 5.33 ms over 30 s is 178 ppm.

## 8. in `ms_stats_block()`

COUNTED IN FRAMES, NOT IN TICKS.

Ticks are integers arriving about every 19 ms, so over a short window the quantisation alone
dominates: one tick either way across five seconds is some 3800 ppm, and the figure jumped
about far too much to read. Frames are the audio clock itself and have no such step.

WHAT IT NOW MEANS, precisely: the host's audio clock against the system clock. The blocks
arrive at the interface's sample rate while the timestamps this plug-in hands CoreMIDI are
mach time, so this is exactly the quantity that decides whether a generated clock stays with
the music over a long session. It is normally tens of ppm - GenBridge measures the same
quantity between two devices and steers a ring on it.

## 9. in `ms_stats_block()`

THE WINDOW'S SPAN IS MEASURED, NOT ASSUMED. It is shorter than MS_STATS_WINDOW_SECONDS while
the window fills, shorter again if the ring saturated, and shorter than the wall time it
covers if a suspension took samples out of it - and every one of those is worth seeing, so
the figure comes from the entries themselves rather than from a block count and a rate.
