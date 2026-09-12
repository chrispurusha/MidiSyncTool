# msDetect.h notes

The longer comments from `msDetect.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_DETECT_EXPECTED`

WHAT IS ACTUALLY BEING MEASURED, stated plainly because it is easy to overclaim.

The figure this produces is the WHOLE ROUND TRIP: the moment a clock tick was stamped for, to the
moment the resulting transient appeared in a buffer this plug-in was handed. That includes the
drum machine's own MIDI-in to audio-out delay - the number the concept calls "device latency" and
the one worth reporting - but also the interface's A/D, the host's input buffering, and whatever
the host chooses to tell the plug-in about its block times.

Those extra terms are CONSTANT for a given rig and buffer size, which is what makes the whole
exercise viable: a constant is subtractable once measured. GenBridge learned to enumerate exactly
these terms (device latency, safety offset, buffer, stream latency) and SynthLib/audio/device.c already
reports them. Until they are subtracted, this is a round-trip figure and is labelled as one.

THE JITTER FIGURE NEEDS NO SUCH APOLOGY. Every constant cancels in the deviation, so the spread is
the drum machine's own timing spread plus the detector's, and the detector's is small against it.

## 2. `tMsDetectSnapshot`

ONSET DETECTION IS BIASED BY LEVEL, and the bias is the reason the calibration pattern has to be
programmed a particular way. A threshold crossing on a rising edge happens later for a quieter hit
than a louder one, because the edge takes longer to reach the threshold. Against a FIXED velocity
the bias is a constant and cancels out of both the mean (once calibrated) and the jitter; against
a varying one it is indistinguishable from the drum machine playing badly.

Likewise a slow attack. A kick drum's onset can ramp over 10 ms, so where in that ramp the
threshold falls moves with the threshold. A closed hat or a rim is a far better reference, and the
difference between the two is itself worth measuring once this works.

## 3. file scope

MONITOR MODE ONLY, and zero otherwise. The period the fitted grid settled on, and the tempo
that implies given how far apart the hits were said to be - see ms_detect_set_division().
latencyMean/Min/Max are meaningless here and are left at zero rather than filled with a
number that would look like a measurement.

## 4. file scope

MONITOR: nothing is sent at all, and the grid is recovered FROM THE AUDIO.

The two above measure a transient against a moment this plug-in chose, which is the only way
to get an absolute LATENCY - and it requires being the clock master. That rules out every
device already running on someone else's clock: a drum machine on its internal timebase, a
hardware sequencer master, a modular, a take already in progress.

Here the onsets are timestamped and a grid is FITTED to them afterwards, phase and rate
together, with the jitter reported as the residual about that fitted line. No latency figure
is possible - there is no reference to be late against - but the SPREAD is, and the spread is
what is audible.

THE RATE TERM IS NOT OPTIONAL HERE, and this is the trap the mode exists inside. The host's
musical grid and the external master's are unsynchronised free-running crystals, tens of ppm
apart; over a hundred seconds they separate by milliseconds. Fitting phase alone would report
that separation as jitter and the figure would grow with the length of the run. Fitting rate
as well makes the measurement immune to it, and to any tempo offset at all.

## 5. file scope

OFF: the clock is generated but nothing is listened for.

Measuring is a calibration act, not a permanent condition - once a device's latency is known
the everyday use of this plug-in is as a clock, on whatever track suits, with no obligation to
be the one carrying the hardware's audio return. That is the mode for it.

IT IS NOT A CPU SAVING, and should not be sold as one. Measured on this machine, the whole
detector costs 0.022 % of one core (1.16 us per 256-frame block against a 5.33 ms period), so
there is nothing here to reclaim - the same lesson as the jitter budget, where the term that
looked worth optimising was a rounding error against everything around it.

WHAT IT ACTUALLY FIXES is a panel that lies. Every Nth tick registers an expectation, and
ms_detect_audio() ages those out against silence - so a plug-in sitting on a track the drum
machine does not come back on counts a MISS PER BEAT, for ever, and reads as a fault when
nothing whatever is wrong. Nothing is expected here, so nothing can be missed.

The figures from the last real run are DELIBERATELY LEFT STANDING - see ms_detect_set_source()
- because the compensation dialled in on the panel came from them and a number in force should
have its provenance visible. The panel dims them and says they are held.
