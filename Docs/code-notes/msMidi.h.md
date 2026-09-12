# msMidi.h notes

The longer comments from `msMidi.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MS_MIDI_MAX_DEST`

LOCAL FOR NOW, with a view to moving into SynthLib later (CT's call). SynthLib already owns the
send primitive in synthlibMidi.c, but that one stamps every packet 0 - "deliver now" - which is
right for a note or a CC and is exactly what this tool must not do. Rather than change shared
code that three other projects depend on while the shape of this is still settling, the
timestamped version lives here until it has proved itself.

The measurement that decided it: 200 clocks at 24 PPQN / 120 BPM through IAC, timed on arrival.
Scheduled ahead gave 0.019 ms RMS with no drift; sent immediately from a sleeping loop gave
4.157 ms RMS and a 3.9 ms MEAN error. See Docs/findings.md.

## 2. `ms_midi_refresh()`

Rebuild the cached destination AND source lists. Talking to CoreMIDI takes its locks, so this is
NEVER called from drawing or from the audio thread - GenBridge learned that one the hard way, with
an enumeration inside a 30 Hz repaint contending with the opens it was driving.

SAFE TO CALL WHILE OTHER INSTANCES ARE SENDING. The lists are process-global - a host loads every
plug-in into one process - and are rebuilt into a shadow with the count published last, so a
second MidiSyncTool arriving in a running session no longer drops the first one's ticks for the
length of an enumeration. Rebuilds serialise against each other.

## 3. `ms_midi_source_count()`

---- LISTENING, which is a separate list from sending -----------------------------------------

A port that can be sent to and a port that can be listened to are DIFFERENT ENDPOINTS with
different indices, even when they carry the same name. IAC Driver Bus 1 is both, and it is the
port this exists for: point Live's Sync at it, point this at it, and Live's own generated clock
can be measured by the same code that measures ours.

## 4. `ms_midi_set_offset_ms()`

LATENCY COMPENSATION, AND THE ONLY PLACE IT BELONGS.

Measuring a device's latency is half the job; the other half is sending to it that much EARLIER,
so its sound lands where the music says it should. Applied here rather than in the clock because
it must apply to everything scheduled - ticks, transport and probe notes alike - and because a
compensation that reached only some of them would put transport and clock out of step.

NEGATIVE MEANS EARLIER, which is the sign a user expects from "the device is 12 ms late, so take
12 ms off". A positive value delays, which is occasionally what a slow-responding device in the
other direction needs.

THIS IS WHAT MS_LOOKAHEAD_MS IS FOR, and the connection is worth stating because the lookahead has
looked unjustified in every measurement so far. An event cannot be sent earlier than the moment
its block was picked up, so the lookahead is the headroom the compensation spends: compensating
12 ms with 10 ms of lookahead cannot work, and the commit-margin telemetry says so plainly by
going negative. The lookahead is not overhead - it is the budget.
