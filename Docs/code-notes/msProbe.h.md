# msProbe.h notes

The longer comments from `msProbe.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_PROBE_DEFAULT_COUNT`

TWO DIFFERENT LATENCIES, AND THE DIFFERENCE BETWEEN THEM IS ITSELF THE MEASUREMENT.

Driving the drum machine's own sequencer from our clock and timing the result measures the whole
path: clock on the wire, the machine's sequencer deciding a step has arrived, its voice sounding.
Sending it a NOTE and timing that measures only the last part - note in, sound out.

Subtract one from the other and what is left is what the sequencer itself contributes, which is
the part a user can do nothing about and the part most worth knowing. A machine with a 3 ms note
latency and a 12 ms clock-driven latency has a 9 ms sequencer, and no amount of tightening the
clock will improve it.

The note probe is also the better CALIBRATION, and by some distance:

```
  * It needs no pattern programmed, so there is nothing for the user to get wrong - no swing left
    on, no velocity variation, no pad that turned out to be a two-layer sound.
  * It works with the transport stopped, so calibration is a button rather than a session.
  * The moment of each note is chosen by this code rather than inferred from a musical grid, so
    the expectation is exact by construction.
  * Velocity can be swept, which is the only way to find out how much the onset detector's own
    level bias is worth on this particular sound.
```
