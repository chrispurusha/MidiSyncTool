# msPlugin.c notes

The longer comments from `msPlugin.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

WHAT MIDISYNCTOOL IS, AS FAR AS A PLUG-IN FORMAT NEEDS TO KNOW - and nothing about any format.

This is what msVst3.cpp and msEditor.mm used to be, less every line that was VST3. The COM
plumbing, the controller, the factory and the IPlugView are SynthLib's now (SynthLib/plugin/),
shared with G2 Alike and GenBridge, and `./do-plugin` builds MidiSyncTool.vst3 and
MidiSyncTool.component from the same objects.

THE LOGIC MOVED ACROSS UNCHANGED. The clock, the split-block correction, the telemetry and the
saved format are what they were in msVst3.cpp's processor, now in C beside the rest of the
plug-in; what changed is only where a ProcessContext's fields come from - SynthLib hands over a
tSynthLibTransport, which an Audio Unit host fills as well as a VST3 one can.

EVERY INSTANCE IS ITS OWN. Nothing below is file-scope except the descriptor: two copies on two
tracks share nothing but the process-wide MIDI client, which msMidi.c already keeps one of.

## 2. file scope

------------------------------------------------------------------------------------------------
Parameters
------------------------------------------------------------------------------------------------

A HOST LAUNCHED FROM THE DOCK INHERITS NO SHELL ENVIRONMENT, so every MST_* variable this plug-in
was developed against is empty inside Live: the clock had no destination and generated nothing,
while the panel showed every other figure perfectly. Anything a user must be able to set has to
be a real parameter.

A STEPPED PARAMETER HAS A FIXED STEP COUNT, decided at registration and cached by the host, so it
cannot follow how many MIDI destinations the machine happens to have. The slot count is fixed at
MS_MIDI_MAX_DEST and only the number of slots pointing at something real varies.

## 3. `gParams`

NOTHING HERE IS SAVED BY THE WRAPPER, and so SynthLib writes this plug-in's own bytes unwrapped -
the saved state is byte-for-byte what msVst3.cpp wrote, and a set saved by this build still opens in
an older one. The destination and clock source are saved by NAME in those bytes, which an index
cannot do: a MIDI port's index depends on what else is switched on that day.

## 4. `MST_STATE_V1_PARAMS`

------------------------------------------------------------------------------------------------
The saved state
------------------------------------------------------------------------------------------------

[4 doubles][destination name][clock source double][clock source name] - msVst3.cpp's layout,
unchanged.

HOW MANY PARAMETERS THE ORIGINAL BLOCK HELD, and it must never change. A fifth parameter CANNOT
simply extend that array: an old block is 4 doubles followed immediately by 64 bytes of name, so
reading 5 doubles from it would quietly load the first eight characters of the port's NAME as a
parameter value. So everything new goes on the END, after the name, where an old file simply runs
out and the default stands.

## 5. `ms_suspend()`

NOTHING BETWEEN TWO BLOCKS IS A BLOCK PERIOD once the host has stopped feeding us. Both the
block-period RMS and the timebase model measure a STEP from the previous sample, so both have to be
told that the next one does not follow the last. Neither figure is cleared - the run's history is
still the run's history - only the stale timestamp behind it.

## 6. `refresh_destination()`

THE DESTINATION IS A FUNCTION OF TWO PARAMETERS, so it gets one place to be decided in. Monitor
mode overrides the chosen port with "none", which is what actually makes the plug-in silent:
ms_clock_process returns immediately on a negative destination and sends nothing at all. The
chosen port is REMEMBERED rather than cleared, so leaving monitor mode puts the rig back exactly
as it was without the user re-picking it.

ONE OWNER FOR THE CHOSEN PORT, and it is NOT the parameter: the port can be chosen by the panel's
parameter or by MST_MIDI_DEST at initialize() for a headless run, and deriving it from the
parameter here meant the environment's choice was silently discarded the first time anything else
called this.

## 7. in `ms_process()`

OUR OWN CLOCK, because the host's is not there. Live's systemTime is always 0, so the field the
scheduler was going to be built on does not exist. AudioGetCurrentHostTime() read here is the
wall clock as the CPU sees it at the moment this block is COMPUTED - not when its audio is
heard, and the difference is the output latency: unknown, but constant for a device and buffer.

## 8. in `ms_process()`

ONE DEVICE CYCLE IS NOT ALWAYS ONE process() CALL, and a loop boundary is where that stops being
a detail. Live SPLITS the buffer at a loop wrap: one 512-frame cycle arrives as two calls -
500 frames and then 12 - microseconds apart, because both are computed inside the same audio
callback. Measured (Live 12.4.5, 512 frames, 1-bar loop at 130 BPM): 2.84 size changes per loop,
one split every time round, and a fabricated -10.412 ms worst case in the block jitter.

THE FIX IS ONE TIMESTAMP: a fragment's audio starts where the frames already handed over this
cycle end. A call is a continuation when its frames FIT in what is left of the cycle AND it
shares the cycle's wall instant (under half a cycle since the last call). THE BUFFER IS THE
LARGEST SINGLE CALL SEEN, not the declared maximum, which a host is entitled to under-use.

## 9. `ms_get_state()`

THE FIRST FOUR ONLY, then the destination's NAME, then everything added since - see
MST_STATE_V1_PARAMS. The name is what actually gets restored; the index is saved only because the
host's automation is expressed in it. A MIDI destination's index depends on what is switched on:
the Tempest was index 6 with a Kronos and a Hydrasynth powered up and index 4 without them.

## 10. `ms_sync()`

THE PANEL: SynthLib's shared view (synthlibPanelView.m) drawing msDraw.c. What follows is all that
is MidiSyncTool's about it - which draw calls, and what "this editor's state" means.

EVERY FRAME AND EVERY CLICK, this editor's instance's status slot and the values the HOST believes -
which on VST3 is the controller's copy, the one the host's own panel shows. The draw layer keeps
both file-scope, so with two editors open whichever pushed last would otherwise speak for both.
