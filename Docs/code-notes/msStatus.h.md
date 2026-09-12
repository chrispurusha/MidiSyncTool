# msStatus.h notes

The longer comments from `msStatus.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_STATUS_SLOTS`

ONE OF THESE PER PLUG-IN INSTANCE, not one per process - the mechanism is GenBridge's and the
reason it is not a single global is a bug that project actually had: with two instances loaded,
every editor read whichever processor wrote last, so a panel showing one device reported figures
from another. Two answers, one of them a lie.

The processor claims a slot on construction and tells its controller which one over
IConnectionPoint, the channel VST3 provides for exactly this. The two are separate registered
classes precisely so a host MAY keep them apart, and nothing else bridges them.

Everything here is written by the audio thread and read by the UI thread, so it is all atomic and
none of it is a pointer. The destination name is the exception - a fixed buffer copied under no
lock, on the grounds that the worst case is a torn string in a readout that refreshes thirty
times a second.

## 2. file scope

THE ONE FIELD THAT TRAVELS THE OTHER WAY - written by the UI thread, consumed by the audio
thread, which is the reverse of everything else in here and so is worth calling out rather
than leaving to be noticed.

A CLEAR IS AN ACTION AND MUST NOT BE A PARAMETER. VST3 parameters are the sanctioned route
from controller to processor, but a parameter is automatable and is saved in the project -
so a momentary "clear" would be recallable, and a project reloaded with it saved as 1 would
clear the figures on load. This is a post, not a value: the UI raises it, the audio thread
takes it with an exchange, and nothing anywhere remembers it afterwards.

## 3. file scope

THE PANEL'S FIGURE IS THE RECENT ONE - see MS_STATS_WINDOW_SECONDS in msStats.h. The all-time
RMS is kept because the log's model-residual ratio is an all-time figure and comparing it
against a windowed denominator would be a quiet lie; the recent one is what a reader watching
the panel after a hiccup actually needs, since the all-time figure takes minutes to forget one
bad block. blockRecentSeconds is what the window really spans, which is shorter while it fills
and shorter again on a host whose buffer is small enough to saturate the ring.

## 4. file scope

THE WORST SINGLE BLOCK, beside the RMS. An all-time RMS moves slowly and hides the shape of
what is in it: a host that is steady except for one event per loop reads much like a host
that is mildly unsteady throughout, and telling those apart by watching a third decimal
place is not a measurement. The worst case is the figure a fabricated error goes straight
into - a split block put a whole buffer in it - so it is what says whether one is happening.

## 5. file scope

The breakdown. scheduleLeadMs is exact and ours; inputPathMs is the interface's A/D plus the
host's input buffering and is NOT knowable from inside the plug-in - it stays at zero, and is
shown as unknown, until an audio loopback measures it. Everything else is derived from those
two and the measured round trip, so the panel never invents a term it does not have.

## 6. file scope

THE HOST'S BUFFER. A hosted plug-in can report this and never set it - the host owns it. It is
reported because it is an exactly knowable part of the input path: the audio in a block was
captured at least one buffer before the block was handed over.
THE DEVICE CYCLE, not whatever the last process() call happened to carry. Live hands one
cycle over in two calls at a loop boundary, and publishing the fragment made the breakdown's
Host buffer bar collapse and the whole chart jump once per loop.

## 7. file scope

WHICH MODE, as a tMsMode - and it is the panel's licence to draw a figure as live, as HELD or
as a dash. Not every number on the panel means something in every mode: in monitor nothing is
generated, so the round trip has no reference and the latency breakdown has nothing to break
down; in clock only nothing is listened for, so every measured figure is the last run's.
Showing any of those the way a live reading is shown is how a stale number gets written down
as a measurement an hour later.

## 8. file scope

---- THE CLOCK COMING IN, which is measured and never acted on ------------------------

Deliberately separate from every figure above. Those describe a clock this plug-in generated
against a timebase it owns; these describe someone else's clock arriving on a wire, and the
only thing the two share is the panel they are drawn on. Mixing them would invite exactly the
comparison that has to be made carefully or not at all.
