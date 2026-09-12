# msMidi.c notes

The longer comments from `msMidi.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `gCount`

THE COUNTS ARE PUBLISHED LAST, AND ATOMIC, because these tables are process-global and a host
loads every plug-in into one process. A SECOND MidiSyncTool arriving in a running session used to
stop the FIRST one's clock: refresh() set gCount = 0 and then spent milliseconds inside CoreMIDI,
and for that whole window every send in the process saw "index >= gCount" and returned false -
ticks dropped, on an instance that was not the one enumerating and had no idea why.

The lists are built into shadows and copied in, and the COUNT goes last with a release, so a
reader indexing below the count it loaded sees entries written before it. A slot can still name a
different endpoint after a rebuild, which is why a destination is saved and restored by NAME.
GenBridge arrived at exactly this shape on 2026-09-08; see gbMidi.c.

## 2. `gListeningEnd`

WHAT IS BEING LISTENED TO IS AN ENDPOINT, NOT AN INDEX, and that is the whole of the state.

Before the lists could be rebuilt at runtime an index was as good as an endpoint. Now a rebuild
can move a source under a live connection, so a stored index would go stale in two ways that both
matter: a disconnect aimed at the old slot would disconnect somebody else's port and leave ours
connected, and getState() would write down the wrong name to restore. Holding the endpoint and
working the index out from it when anybody asks removes both, and removes the write that the
audio thread and a rebuilding editor would otherwise be racing over.

## 3. in `ensure_client()`

A NOTIFY PROC, so the lists do not have to be polled and do not have to be right forever.
Passing NULL here meant the destination and source menus were whatever existed at load: a
synth switched on afterwards never appeared in either, and nothing looked again. CoreMIDI
delivers this on the run loop of the thread that created the client, so a client created
without one simply gets no notifications and behaves as it did before - which is why the
first enumeration still happens explicitly at load.

## 4. `refresh_sources_locked()`

SOURCES ARE ENUMERATED SEPARATELY AND THE INDICES DO NOT LINE UP with the destination list. They
are different endpoints - a machine can have a source with no destination and the reverse - so a
saved setup stores the NAME, exactly as the destination side already learned to.

CALLED WITH gRefreshLock HELD.

## 5. `read_proc()`

ON CoreMIDI'S OWN RECEIVE THREAD, which is high priority and must not be made to wait. Nothing
here allocates, logs or takes a lock: it walks the packets and hands each status byte on.

ONLY SYSTEM REAL-TIME BYTES ARE FORWARDED (0xF8 and above). They are single bytes and are the only
things this tool cares about - and, importantly, they are permitted to appear INSIDE another
message, which is why each byte is examined rather than each packet's first byte. A clock landing
in the middle of a SysEx dump is legal and would otherwise be missed.

## 6. in `ms_midi_listen()`

DISCONNECT FIRST, ALWAYS. Two sources connected to one port interleave into a single stream
and the estimator would fit a line through both masters at once - a plausible-looking tempo
belonging to neither.

BY ENDPOINT, because the list can be rebuilt under a live connection - see gListeningEnd. A
disconnect from an endpoint that has gone away simply fails, which is the right outcome.

## 7. `ms_midi_listening()`

WORKED OUT, NOT REMEMBERED. The answer is "where in the list is the endpoint I am connected to",
and a list that can be rebuilt at any moment is exactly why that question has to be asked again
each time rather than answered from a stored index. An endpoint that has been switched off is in
no list, so this says -1 - which is the truth: nothing is arriving.

## 8. in `ms_midi_send_at()`

A clock tick is one byte and a transport message three; the largest thing this tool sends is
a Song Position Pointer at three. 64 bytes of packet list is ample and keeps the frame small
enough to sit on the audio thread's stack without thought.
THE COMPENSATION, applied to every scheduled event and to nothing immediate. A hostTime of 0
means "now" by contract, and shifting "now" would be meaningless.

## 9. in `ms_midi_send_at()`

CLAMPED AT THE PRESENT, because an event stamped in the past is delivered immediately
and several of them arrive together - the exact bunching this tool exists to remove.
The commit-margin telemetry is what reports that the budget has been overspent; this
only stops it turning into a burst.
