# msDraw.h notes

The longer comments from `msDraw.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_CANVAS_H`

TALL ENOUGH FOR WHAT IS ON IT, AND NO TALLER. The content ran to about 740 units while this said
600, which put the graph off the bottom of the window.

THE GRAPH NO LONGER HANGS OFF THIS NUMBER (2026-09-03). It was anchored to the canvas bottom so a
row added above could never push it off the edge; what that actually did was leave the sections'
own variation - a clock source chosen or not, a held run, the dropout and split rows, some 130
units between them - on screen as a hole under the latency breakdown. The graph now follows the
content and spends the remainder on its own height.

"TOO TALL ONLY COSTS A TALLER TRACE" WAS TRUE ONLY UP TO MS_GRAPH_MAX_H, which is where 890 was
wrong. Past that cap the trace stops absorbing and the remainder goes back to being a hole - at
the bottom this time. MEASURED by stubbing the renderer and recording the lowest pixel any draw
call asked for, across seven arrangements:

```
    canvas   shortest arrangement        tallest arrangement
     890     bottom 816, blank 74        bottom 876, blank 14
     850     bottom 816, blank 34        bottom 836, blank 14
     830     bottom 816, blank 14        bottom 816, blank 14   <- every arrangement, exactly
     810     bottom 796, blank 14        bottom 800, blank 10   <- the tallest starts to lose it
     790     bottom 776, blank 14        bottom 800, blank -10  <- and then collides

```
830 is the top of the range that works. The tallest arrangement bottoms out at 800 once the trace
is squeezed to MS_GRAPH_MIN_H, so anything from 814 up leaves no gap - but 830 keeps the trace as
tall as it can be AND keeps 16 units of slack in it, so a row added later still shortens the
trace before anything collides. That is the property worth having, and it is the reason not to
trim to 814.

## 2. `tMsEdit`

A click landed on something the panel cannot act on by itself.

ALL BUT THE LAST ARE VST3 PARAMETERS, and those must go to the HOST rather than straight to the
processor - changing one behind the host's back would leave its automation and its saved state
wrong. eMsEditClearStats is the exception and is deliberately NOT a parameter: it is an action
with no value and nothing to recall, so it goes to the processor through tMsStatus instead. Any
new member has to be sorted into one of those two kinds before it is added, because the editor's
switch on this enum decides which route it takes.

## 3. `tMsMode`

WHAT THE PLUG-IN IS DOING, and therefore which of the panel's figures mean anything.

THE ORDER IS PART OF THE FILE FORMAT AND MUST NOT CHANGE. This replaced a two-state Monitor
toggle on the same parameter id, which wrote exactly 0.0 or 1.0 into the saved state - so with
Measure at 0.0 and Monitor at 1.0 every set saved by the older build still opens in the mode it
was saved in, with no migration branch anywhere. Insert a fourth mode in the MIDDLE, never at
either end, or that stops being true.
