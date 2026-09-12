# msDraw.c notes

The longer comments from `msDraw.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_HELD_CAPTION`

A FOURTH TIER, BELOW ALL THREE: A FIGURE THAT IS NOT LIVE.

Not every number on this panel means something in every mode. In clock-only nothing is listened
for, so the measured figures are the last run's; in monitor nothing is sent, so there is no
reference and the latency breakdown has nothing to break down. Those figures are KEPT rather than
blanked - the compensation in force was derived from them, and a value in force should have its
provenance on screen - but a held number drawn like a live one is how it gets written down an
hour later as this session's measurement.

Both tiers drop together and keep their usual relationship (the figure still brighter than its
caption), so an inactive block reads as a dimmed block rather than as a caption with a broken
number beside it. The contrast is deliberately poor - about 2:1 - because "readable, but not
current" is exactly the statement.

## 2. `ROW_X`

---- the control rows, in canvas units --------------------------------------------------------

Laid out from one origin so a change to the block moves everything together, rather than a set of
literals per row that drift apart the first time a row is inserted - which has now happened
twice, for the mode and for the clock input, and cost nothing either time.

## 3. `ROW_STEP_W`

A VALUE BOX IS ONLY AS WIDE AS ITS CONTENT NEEDS. A port name or the mode sentence wants the full
width; a compensation reading is at most "100.0 ms", and giving that a 330 px box put its arrows
a third of the panel apart - with the right-hand one standing directly over the mode toggle below
and reading as though it belonged to that row rather than this one.

IT IS FOUR ARROWS NOW, not two, so the width is the reading plus two pairs. See row_arrow().

## 4. `MS_STEP_COARSE`

TWO STEP SIZES, because one cannot do both jobs. The coarse step has to reach a device's figure
from nothing without a hundred clicks; the fine step has to TRIM one, and "Use measured" lands on
whatever the measurement actually was - 11.8 ms - which a whole-millisecond step can only carry
up and down the range keeping the .8 forever. The tenth is also the panel's own resolution: the
reading is printed to one decimal, so a step finer than this could not be seen.

## 5. `button_face()`

draw_button() DRAWS A BOX 2 * DRAW_BUTTON_MARGIN WIDER AND TALLER than the rect handed to it, from
the same origin - see draw_button_bounds() in utilsGraphics.c. So a button given a whole row's
rect covers the 4 px ROW_GAP leaves between rows and runs into the one underneath. Insetting here
makes the drawn box exactly the rect the rest of the layout - and the hit test - believes in.

## 6. `row_arrow()`

THE FOUR ARROWS OF A STEPPED ROW, outermost coarse and innermost fine, laid out

```
     [<<] [<]   value   [>] [>>]

```
so that distance from the reading reads as size of change. Slot 0 is the leftmost. The pairs are
anchored to their own end of the box rather than measured across it, so the row can be made wider
for a longer reading without either pair moving off the edge it belongs to.

## 7. in `open_menu()`

22 px a row. SynthLib scrolls the menu itself once it will not fit, which is what makes an
unbounded destination list safe - a studio's port count is not predictable.

A CELL WIDTH OF ZERO MEANS "measure the labels", which is what the mode list needs: its
entries are sentences rather than names, and the fixed 240 the other two use would clip the
longest of them at exactly the point where it stops being a warning.

## 8. `control_row()`

A LABELLED CONTROL: caption, a value box that opens a drop-down, and for a continuous value a
pair of arrows instead. The arrows exist only where a list would be meaningless - GenBridge's rule
that a drop-down replaces the steppers rather than sitting beside them.

A ROW THAT HAS NO EFFECT IN THE CURRENT MODE IS DIMMED RATHER THAN HIDDEN, and it still works.
The port in monitor mode and the analyse channel in clock-only are both REMEMBERED settings that
nothing is currently acting on: removing them would lose the reading of what will be used again
the moment the mode changes back, and leaving them at full strength says they are in force when
they are not. Dimming says exactly the true thing - set, but not doing anything right now - and
it stays clickable so it can be set up before switching modes.

## 9. `mode_row()`

THE MODE ROW: a drop-down like the others, but with the value box TINTED BY THE MODE.

It was a two-state toggle, and the reason it did not simply become a plain control_row() is the
reason it was a coloured toggle in the first place: a mode that silently changes what every figure
below it MEANS has to be unmissable, not a word in a box the same colour as everything else. So
the drop-down is the mechanism and the colour is retained on top of it - a click opens the list
rather than advancing to the next state, which is what a third mode requires.

NOT draw_button(), WHICH SIZES ITS LABEL OFF THE RECT'S HEIGHT - see internal_render_text(). A
20 px row therefore meant a 20 px font, and this row's sentence ran clean off the end of its box
and off the panel. Every other row's value text is TEXT_H, so this is drawn the same way: the box
painted, the label over it.

## 10. `term()`

A TERM IN THE LATENCY BREAKDOWN, drawn as a proportional bar as well as a number.

The bar is what makes a breakdown worth having rather than a list: it shows at a glance which
term dominates, and an unknown term shows as an empty outline rather than as zero. Zero and
"not measured" are completely different statements and a panel that renders them the same way is
lying by omission.
A HELD TERM KEEPS ITS BAR but drains the colour out of it, which is the one place a bar is better
than a number at saying this: the proportions stay readable at a glance - which term dominated is
exactly what a held breakdown is still good for - while the drab fill says none of it is being
re-measured. A bar left in its live colour is the most confidently live-looking thing on the panel.

## 11. in `graph()`

THE TRACE IS THE MOST LIVE-LOOKING THING ON THE PANEL - it is a scrolling graph, and a viewer
reads motion into one whether or not it is moving. It stops on its own when nothing is being
measured (a point is written per detection, not per block), so a held trace is a frozen one
and looks exactly like a device that has become perfectly steady. The colour has to say which.

## 12. in `ms_draw_frame()`

---- what this plug-in is set to do ----

NOT "MIDI OUTPUT" any more. Two of the five rows below are inputs - the channel the detector
analyses, and the port an incoming clock is measured on - and a heading naming only the
output was already half wrong before the clock input made it plainly so.

## 13. in `ms_draw_frame()`

---- the controls -----------------------------------------------------------------------

THESE HAVE TO BE VST3 PARAMETERS, not environment variables. A host launched from the Dock
inherits no shell, so every MST_* variable this was developed against is empty inside Live -
the clock had no destination and generated nothing while every other figure on this panel read
perfectly. The same trap is recorded against $G2_VST3_PATCH in G2-Edit's plug-in notes.

## 14. in `ms_draw_frame()`

COMPENSATION STAYS LIVE IN CLOCK-ONLY MODE and that is the whole reason the figures behind it
are preserved rather than cleared: it is a phase advance applied to the generated grid, so it
is in force whenever a clock is going out, measured or not. In monitor nothing is sent, so
there is nothing to advance.

## 15. in `ms_draw_frame()`

THE CLEAR, ON THE FIRST ROW OF THE FIGURES IT CLEARS rather than on a row of its own, which is
the same bargain the "Use measured" button strikes lower down: the canvas has no height to
spare and a button beside the first of a group reads as belonging to the group.

WHAT IT CLEARS IS EVERYTHING FROM HERE TO THE BPM ROW - commit margin, late clocks, block
jitter and its worst case, the drift window, the measured tempo, the trace - because that is
exactly what ms_stats_reset() covers and a button that cleared some of a group would be worse
than none. It does NOT touch the measured device, which has its own lifecycle, and it does not
disturb the running clock: see ms_clock_reset_stats().

IT IS THE WORST CASE THAT MAKES IT WORTH HAVING. The RMS forgets on its own now, over
MS_STATS_WINDOW_SECONDS; the worst case deliberately does not, so once a hiccup you already
know about is in it, only this gets it out of the way so the next one is visible.

## 16. in `ms_draw_frame()`

CLOCKS SENT IS ZERO BY DESIGN HERE and saying so is better than showing the zero, which
reads as a fault. The grid figure replaces it: it is what the plug-in has worked out the
external master is doing, and a plausible tempo there is the confirmation that the
detector is locked onto the pattern rather than onto noise.

## 17. in `ms_draw_frame()`

COMMIT MARGIN, which is the plug-in's own health and the one number that says whether the
schedule is comfortable. While it stays positive CoreMIDI holds each tick early and delivers
it on its own timer; the moment it goes negative, ticks bunch.

THESE THREE DESCRIBE A CLOCK BEING GENERATED, so in monitor mode there is none to describe.
Worse than stale: ms_clock_process() returns at the top with no destination and the stats are
reset on the way in, so they read a PERFECT 0.00 ms margin and 0 late clocks - the panel's most
reassuring possible reading, produced by sending nothing at all.

## 18. in `ms_draw_frame()`

THE PAIR, always together. The raw figure is the host's; the residual is what survives the
timebase model and is therefore what the output clock actually inherits. Showing only the
first would look like a fault the plug-in was not fixing; showing only the second would hide
how much work is being done.

The raw figure is the ONE clock-side number that stays live in every mode: ms_stats_block() is
called before the clock and unconditionally, so the host's block delivery is measured whether
or not anything is being sent. The residual below it is the model's, and the model does not
run when the clock does not.
THE GAP COUNT BELONGS BESIDE THE FIGURE, not in a log. A gap is an interval the metric
REFUSED to measure - the host stopped being scheduled, which on this machine meant Ableton
going to the background - and without the count the same reading means two different things:
a steady host, or a host that stalled twice and had it quietly discounted.

## 19. in `ms_draw_frame()`

THE WORST CASE TRAVELS WITH THE RMS. A fault that happens once per loop and a host that is
mildly unsteady all the time look alike in an RMS; the worst case separates them at a glance.
It is also the number a split block used to land a whole buffer in, which is the reading this
row exists to make legible.

THE TWO ARE OVER DIFFERENT SPANS, AND THE ROW HAS TO SAY SO. The RMS is now taken over the
last MS_STATS_WINDOW_SECONDS, because an all-time one takes minutes to forget a single bad
block and so answers "what happened at some point" when the question being asked of it is
"what is the host doing now". The worst case stays ALL-TIME - which is what makes forgetting
safe, since the event the window drops is still on the row - so it is labelled "worst ever"
rather than left to be read as belonging to the same window as the figure beside it.

The span is printed rather than assumed: it is short while the window fills, and shorter
still on a host whose buffer saturates the ring.

## 20. in `ms_draw_frame()`

ONLY WHEN IT HAPPENS, because on a host that does not split there is nothing to say and a
permanent "0" would read as a fault waiting to happen. When it does happen it belongs here:
it is the host's own behaviour at a loop boundary, it is corrected for rather than measured
as jitter (see the note in msPlugin.c's ms_process()), and a reader comparing this figure with
an older session's needs to know which of the two builds produced it.

## 21. in `ms_draw_frame()`

---- somebody else's clock, arriving ----

MEASURED, NEVER ACTED ON. Nothing here steers the generated clock, and the section is kept
visually apart from the figures above for that reason: those describe a clock this plug-in
made against a timebase it owns, these describe one arriving on a wire.

THE ppm ROW IS THE POINT OF THE WHOLE SECTION. The standing question has been "how much better
is our clock than the host's own?", and until now the only reference was a drum machine's
audio onsets - which carry the device, the desk and the A/D on top of whatever the wire did.
Point Live's Sync at IAC, point this at the same port, and both clocks are timed by the same
code against the same reference, on a path that measures at 0.013 ms rather than 0.132.

## 22. in `ms_draw_frame()`

AGAINST THE HOST'S OWN TEMPO, in ppm, because a BPM difference in the fourth decimal
place is unreadable and the same difference as "+18 ppm" is not. Two free-running
crystals is the expected answer here, not a fault - this rig's own audio clock measures
about -20 ppm against mach time, and a figure in that region means the two agree.

## 23. in `ms_draw_frame()`

GAPS, ON THE SAME PRINCIPLE AS THE HOST'S. An interval too long to be a clock period means
the master stopped or the port dropped out; the window restarts rather than fitting a line
through the hole. Saying how often that happened is what stops a steady-looking figure
being mistaken for a steady clock.

## 24. in `ms_draw_frame()`

---- what came back ----

HELD MEANS THESE ARE REAL FIGURES FROM A RUN THAT HAS ENDED. The detector deliberately keeps
them when it goes quiet - see ms_detect_set_source() - because the compensation dialled in
above came out of them, and a value in force with no visible provenance is worse than a dim
number. Three cues carry it, only one of which is colour: the heading says HELD, a line under
it names the way back, and every figure drops a tier.

## 25. in `ms_draw_frame()`

---- the breakdown ----

WHAT IS KNOWN, WHAT IS MEASURED, AND WHAT IS NEITHER. The whole value of showing a breakdown
rather than one number is that it says which part a user can do something about, so each term
has to be honest about where it came from.
THE WHOLE BREAKDOWN IS DEAD IN MONITOR MODE, and it used to be the panel's most confident lie.
Every term is a share of `lead + roundTrip`, and in monitor there is no round trip - so the
schedule lead came out as 100 % of the total and drew a full-width bar, over a "Total musical
delay" of 10 ms that nothing on the rig was actually waiting. In clock-only it is HELD, like
the figures it is built from.

## 26. in `ms_draw_frame()`

WHAT IS LEFT, labelled by what is still mixed into it. While the interface term is unmeasured
this is the device AND the converter, and calling it "device latency" would be the exact
overclaim this panel exists to avoid. One buffer comes out because that much is arithmetic:
the audio in a block was captured at least a buffer before the block was handed over.

## 27. in `ms_draw_frame()`

ONLY TWO OF THE FOUR BARS ARE DELAY THE MUSIC SUFFERS, and this row is where that stops being
implicit. The bars decompose the measured LOOP - stamp to transient - and half that loop is
the way back: the host's input buffer and the interface's A/D delay when this plug-in SEES
the sound, not when the sound exists. Advance the grid by them and the device plays early.

The row used to read "Total musical delay" and to show lead + round trip, which is the loop.
It was the panel's instruction for setting compensation and it was the wrong instruction by
one host buffer plus a converter - 10.7 ms at 512 frames before the A/D is counted.

## 28. in `ms_draw_frame()`

COMPENSATION IS THE ONE LIVE LINE IN A HELD BREAKDOWN, because it is still being applied: a
phase advance on a clock that is still going out. That is exactly why it is worth keeping the
figures above it on screen at all.

THE ADVANCE IS SHOWN BESIDE THE SETTING, because they are not the same number and the
difference is not the user's to remember: ms_clock_set_compensation_ms() takes the DEVICE's
figure and adds the schedule lead itself. Without both on screen, "17 ms" set against a
"27 ms" musical delay reads like a value 10 ms short of the job.

## 29. in `ms_draw_frame()`

THE BUTTON, on the compensation row rather than a line of its own - it acts on that row, and
a row of its own would cost height the canvas does not have spare.

IT EXISTS BECAUSE THE PANEL COULD NOT ANSWER "WHICH FIGURE DO I TYPE IN". Four plausible
numbers were on screen - the round trip, the device term, the loop total, the lead - and
three of them are wrong. A button that applies the right one is a smaller thing to get right
once than a caption explaining the choice to every reader for ever.

LIVE WHENEVER IT HAS SOMETHING TO APPLY, including when the figures are HELD, because that is
the actual workflow: measure in Generate + measure, switch to Clock only so the drum machine
stops being listened for, then apply. It is an action, not a reading, so it does not dim with
the numbers around it.

## 30. in `ms_draw_frame()`

WHAT IS LEFT ONCE THE GRID HAS BEEN ADVANCED - the number that says whether the job is done,
and it is against the DEVICE term, not the loop. The advance is compensation + lead and the
delay is device + lead, so the lead cancels and what remains is device - compensation. Set
the compensation from the button and this reads zero, which is the whole point of showing it.

## 31. in `ms_draw_frame()`

---- the verdict and the trace ----

THE TRACE FOLLOWS THE CONTENT AND SPENDS WHAT IS LEFT ON ITS OWN HEIGHT.

It used to be anchored to the canvas bottom, so that a row added above could never push it
off the edge. That guarantee is kept - the HEIGHT is what absorbs a taller panel now, and it
is floored so a collision would still be visible rather than silently off-screen - but the
anchor put the sections' own variation on screen as a hole. The content above ends anywhere
across some 130 units depending on whether a clock source is chosen, whether the run is held,
whether the dropout and split rows are showing; every one of those left exactly that much
blank between the breakdown and the graph.

Leftover now collects at the BOTTOM EDGE, where it reads as margin, instead of in the middle,
where it reads as a bug. MS_CANVAS_H is therefore no longer a number the layout is sensitive
to: too tall only makes the trace taller, up to the cap.

## 32. in `ms_draw_frame()`

THE VERDICT LOSES ITS COLOUR WHEN IT IS HELD, and that matters more than any other figure on
the panel: a green EXCELLENT is the one thing here that a user reads without reading, and a
held one would be a green light on a measurement that stopped ten minutes ago. Dimmed to the
held tier with the word "(held)" beside it, it can only be read as what it is.

## 33. `step_value()`

Stepping a continuous parameter. Clamped rather than wrapped: an arrow that jumps from the top of
the range back to the bottom looks like a glitch.

SNAPPED TO THE FINE GRID AFTERWARDS, which is what makes a coarse step usable on a measured
figure. "Use measured" dials in the measurement itself - 11.83 ms, not 11.8 - and stepping that
without snapping carries the stray hundredths through every later click, so the reading advances
12.8, 13.8, 14.8 and no arrow can ever reach a whole millisecond. Rounding to the tenth the panel
prints means the first click lands on the grid and every one after it stays there.

## 34. in `ms_draw_click()`

A CLICK IS WHERE THE LIST IS RE-READ, and the only place it can be. Enumerating CoreMIDI
takes its locks, so it cannot go in the 30 Hz repaint that draws this row - GenBridge's
trap exactly - and it must not go on the audio thread. A menu about to be opened is the
one moment the list has to be right, and this thread is the one allowed to make it so.

## 35. in `ms_draw_click()`

THE MODE. A drop-down now that there are three of them: cycling through on each click was
right for two states and is wrong for three, because reaching the one you want may take you
THROUGH the one you do not - and the one in the middle stops the audio analysis while the one
at the end stops the clock. A rig should not have to go silent on its way to another mode.
