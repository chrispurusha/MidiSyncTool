# Third-Party Software

MidiSyncTool is licensed under the **GNU General Public License v3.0** (see `LICENSE`).

> **PROVISIONAL.** Nothing is built yet, so nothing here has been verified against a shipped
> binary. GenBridge's equivalent file was checked with `otool -L`, `nm -u` and `strings` because
> the build script and the binary disagreed — the binary is the thing that ships and the thing
> that must be described. **Redo this check before the first release**, and delete this note when
> it has been done.
>
> What follows is what the project is *expected* to link, given it is a VST3 plug-in built the
> same way GenBridge is.

---

## Expected in the shipped binary

### FreeType 2 (statically linked)

- **Website**: https://freetype.org
- **Licence**: The FreeType Project License (FTL) — see `SynthLib/ThirdParty/freetype/docs/FTL.TXT`

FreeType is dual-licensed FTL or GPLv2, and this project takes the **FTL** option. FreeType's own
`LICENSE.TXT` is explicit that the FTL "is compatible to the GNU General Public License version 3,
but not version 2", so a GPLv3 project must take the FTL.

**Binary redistribution obligation (FTL §2):** a binary distribution must provide, *in the
distribution documentation*, a disclaimer stating the software is based in part on the work of the
FreeType Team. Mandatory, not a courtesy — `do-release` must write it into the `.dmg`'s
`Read Me First.txt` and copy `FTL.TXT` into the disk image, and fail rather than ship without them.

Reached through SynthLib's renderer, which the editor draws with.

### VST3 SDK — interface headers and IID definitions (statically linked)

- **Website**: https://github.com/steinbergmedia/vst3sdk
- **Licence**: MIT, Copyright (c) 2025 Steinberg Media Technologies GmbH

Expected to compile the same four translation units GenBridge does — `funknown.cpp`,
`coreiids.cpp`, `vstinitiids.cpp`, `commoniids.cpp` — plus the `pluginterfaces/` headers. No CMake,
no VSTGUI, no `public.sdk` helper classes.

Since 2025 the SDK is MIT, **not** the older dual GPLv3/proprietary arrangement, so the
proprietary-licence question does not arise.

**Binary redistribution obligation (MIT):** the copyright and permission notice must travel with
the binary — `do-release` must copy the SDK's `LICENSE.txt` into the `.dmg`.

---

## Expected NOT to be in the shipped binary

| Library | Why it should be absent |
|---|---|
| **GLFW** (zlib/libpng) | A plug-in is handed a window by its host and never creates one. GenBridge compiles it out; verify the same holds here. |
| **libusb** (LGPL v2.1) | Used by G2-Edit to talk to a Nord G2. This project speaks CoreMIDI and CoreAudio only. |

Everything else should be an Apple system framework: CoreAudio, CoreMIDI, CoreFoundation,
Cocoa/AppKit, Metal, QuartzCore, libc++, libobjc, libSystem.

---

## SynthLib

`SynthLib/` is a submodule of first-party code under GPLv3 (`SynthLib/LICENSE`), with the licence
header on every source file. It is compiled in as source, so it raises no distribution obligation
beyond this project's own.

Its own `THIRD_PARTY.md` describes what SynthLib *can* bundle across all the projects that use it,
which is a broader list than any one binary contains.

---

## Source availability (GPLv3 §6)

Anyone given a binary receives GPLv3 rights, which include the right to the corresponding source.
Publishing a `.dmg` therefore requires the source for that version to be reachable by the people
who get it — a public repository at the tagged version is the simplest way, and is what the `.dmg`
should point at.

---

## A note on JUCE

JUCE is worth **reading** for mechanism and must not be copied from. JUCE 8 is dual licensed
**AGPLv3** / commercial — not GPLv3, which it was up to JUCE 6 — and AGPLv3 code cannot be taken
into a GPLv3 project and left GPLv3. Ideas are not copyrightable; expression is.
