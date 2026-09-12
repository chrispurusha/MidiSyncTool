# msClockIn.c notes

The longer comments from `msClockIn.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `refit()`

THE FIT, AND THE THREE STEPS IT CANNOT SKIP.

A MIDI clock looks like the easiest thing in the world to fit a line to - it is one event every
twenty-fourth of a beat, for ever, with no rests. That is exactly why the first two steps look
unnecessary, and they are not:

```
  1. The period comes from the MEDIAN interval, not the mean. One dropped clock doubles an
     interval, and a mean carries that into the rate for the whole window.
  2. Each arrival is assigned to a SLOT, n = round(t / period), not to its ordinal position. A
     single dropped clock otherwise shifts every arrival after it by one slot and the fit is
     measuring the drop rather than the clock. This is the third place in this project the same
     omission would have bitten - see monitor mode, where a rest in the pattern did it.
  3. Phase and rate are fitted TOGETHER. The two ends of this measurement are different crystals,
     tens of ppm apart, and a phase-only fit reports that separation as jitter growing with the
     length of the run.
```

## 2. `restart_window()`

Drop the window but keep the counters: a gap ends a measurement, it does not undo the ones before
it. The published figures are left standing until a new fit replaces them, for the same reason the
detector keeps its figures when it goes quiet - a reading that vanishes the moment the master
pauses is less useful than one that says what it last saw.
