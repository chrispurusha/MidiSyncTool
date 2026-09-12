# msClockIn.h notes

The longer comments from `msClockIn.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_CLOCK_IN_WINDOW`

MEASURE ONLY. Nothing here steers the generated clock, and that separation is deliberate: the
first useful thing to do with a clock input is to say what it is doing, and the second - a PLL
that regenerates from it - is a different feature that should not be built on an estimator nobody
has read the numbers out of yet.

WHAT IT IS ACTUALLY FOR, beyond a tempo readout. The standing question this project has never
been able to answer defensibly is "how much better is our clock than Live's own?" - because the
only reference so far has been a drum machine's audio onsets, which carry the device, the desk
and the A/D on top of anything the wire did. Point Live's Sync at IAC, point this at the same
port, and both clocks can be timed against the SAME reference by the same code. The wire measures
at about 0.013 ms here; the audio path at 0.132 ms at best. It is an order of magnitude sharper.

THREAD OWNERSHIP: everything in here belongs to the CoreMIDI receive thread and to nothing else.
The estimator's window is touched by that thread alone and published as atomics; the audio thread
never reads it and the UI only reads the published snapshot. One owning thread per piece of
state, everyone else posts.

## 2. `MS_CLOCK_IN_GAP_RATIO`

AN INTERVAL THIS MANY TIMES THE EXPECTED ONE IS NOT A CLOCK PERIOD. The master stopped, was
switched, or the port dropped out - and one of those intervals in a least-squares window drags
the fitted rate somewhere meaningless. Counted and the window restarted, never averaged in. Same
discipline, and the same reasoning, as MS_STATS_GAP_MS.
