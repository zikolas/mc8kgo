# The EMU8000 wavetable synth (TDK MC-8000/DMC-9000)

Four DOS programs that drive the TDK MusicCard's EMU8000-family
wavetable synth directly — cards TDK shipped with Windows-only drivers:

- **TDKSYN** — a resident General MIDI synth TSR: MIDI bytes in over an
  INT 2Fh multiplex, the card's own ROM instruments out. This is what
  lets DOS **games** play the wavetable, through
  [MPUSHIM](https://github.com/zikolas/mpushim).
- **TDKPLAY** — a standalone .MID file player through the same engine.
- **TDKSEND** — a diagnostic that sends test MIDI through the resident
  TSR and reads its counters (state check, dropped bytes, discarded
  SysEx).
- **TDKROMF** — finds and dumps the DMC-9000's preset map out of the
  card's own ROM (see The preset map below).

## Why

Both cards pair an EMU8000-family wavetable with a CS4216 codec behind
a two-window ASIC, and TDK never shipped a DOS driver — the cards were
silent outside Windows. MC8KGO (this repo) enables them; these programs
are the DOS synthesis that never existed. The instruments live in
sample ROM on the card — 2MB of General MIDI on the MC-8000, 4MB of
GM/GS on the DMC-9000 — and the preset map that describes them is the
vendor's own data, read at runtime, so nothing derived from it ships
here.

## TDKSYN

    TDKSYN [/M] [/X] [/C=hex] [/A=n] [/IO=hex] [/F<file>] [/H<a,b,c>]
    TDKSYN /U                                   unload, no reboot needed

    /M       MT-32 mode: bank 127 timbres + the CM-64/32 kit, receiving
             channels 2-10 — what MT-32-scored games expect
    /X       play channels 1-10 only (the Windows MIDI Mapper behaviour;
             Microsoft-authored .MIDs like CANYON duplicate the music on
             channels 13-16, which otherwise plays as extra piano)
    /C=hex   raw 16-bit channel mask (bit n = channel n+1)
    /A=n     volume-attack cap (default 120 - a few ms of ramp against
             onset clicks; 127 = uncapped, as the vendor runs it)
    /IO=hex  window 0 base; otherwise probed (240h then 260h)
    /F<file> SoundFont path (default C:\WINDOWS\SYSTEM\SYNTH2GM.SF2 —
             the MC-8000's map; a DMC-9000 needs /F with its own, see
             The preset map)
    /H<a,b,c> HWCF1/2/3 override, hex (bench diagnostics)

The resident interface (`AH` = BDh): `AL=00` install check -> AL=FFh,
`AL=01` MIDI byte in DL; further diagnostic calls in the source.

Games reach it through MPUSHIM's MPU-401 facade at 330h, in every world
a DOS game lives in:

    MC8KGO                  the enabler
    TDKSYN                  this synth, resident (/F... on a DMC-9000)
    ...trap hosts...        JEMM+QPIEMU, HDPMI16i, HDPMI32i
    MPUSHM16 /SYNTH         the 16-bit protected-mode shim
    MPUSHIM  /SYNTH         the 32-bit + V86 shim

MPUSHIM's `go/GOTSYN.BAT` and `go/GOTSYN9.BAT` wrap this recipe.
Bench-proven catalogue: Monkey Island and DOSMID (V86), DOOM (32-bit),
Tyrian (16-bit), all from one boot on the MC-8000; DOSMID and DOOM
verified the same way on the DMC-9000 with its extracted map. The
synthesis follows the ALSA emu8000/emux drivers and was cross-checked
register-by-register against the vendor's own driver running on the
hardware — sustain, one-shot loop handling and the exclusiveClass drum
chokes included.

## TDKPLAY

    TDKPLAY <file.mid> [/M] [/P] [/F<file>] [/H<a,b,c>]

Standard MIDI File format 0/1; CC7/CC10/CC11/CC64, program changes and
the drum channel handled; sub-ms timing off the 8253 latch without
reprogramming the timer. `/P` ignores the SoundFont envelopes (a
diagnostic baseline).

## The preset map

The engine needs a SoundFont describing where each instrument lives in
the card's ROM. Both cards' maps are E-mu's data and are never
redistributed here — each comes from your own card or driver set:

- **MC-8000**: the file the vendor's Windows installer places at
  `C:\WINDOWS\SYSTEM\SYNTH2GM.SF2`. Its sample-data chunk is empty —
  every sample lives in ROM — so the file is purely a map. Install the
  vendor driver set once, or point `/F` at the file.
- **DMC-9000**: no bank file exists anywhere in the vendor set. Its map
  is a silicon SoundFont — a standard SF2 image embedded in the card's
  own 4MB ROM ("4MB GMGS Rev E") — and TDKROMF dumps it off the card.
  The extracted map loads with `/F` exactly like SYNTH2GM.SF2.

Both banks carry the full MT-32 timbre set in bank 127 and a CM-64/32
drum kit, which is what `/M` selects.

## TDKROMF

    TDKROMF CAL [addrhex]                       read-stability check
    TDKROMF SCAN                                find the SF2 image
    TDKROMF DUMP <starthex> <counthex> <file> [/SWAP]
    /IO=hex  window 0 override (240h then 260h probed)

On a DMC-9000, SCAN reports the image at ROM byte 380h, byte-swapped
(the ROM byte stream is high-byte-first — dump with `/SWAP`), RIFF
size 4188956. Assembling the dump into the bank file is host-side for
now: walk the RIFF, keep INFO, empty the sdta LIST, copy pdta, and add
the smpl chunk's ROM word base to every shdr start/end/loop offset so
they are ROM-absolute. A one-run extractor that writes the file
directly is planned.

The ROM read path races: SMLD reads intermittently return the word one
ahead, in sticky stretches that survive double-read verification.
TDKROMF anchors every read against a known ROM word and re-verifies —
the how and why are in its header comment. Dump twice and compare
before trusting a byte.

The anchoring assumes the DMC-9000's image (the sentinel is word 1C3h
of its ROM): on a card without that image — an MC-8000 included — the
anchor never validates and every mode returns unreliable data with no
warning. This is a DMC-9000 tool.

## Build

Open Watcom 1.9, 16-bit small model, C89 (an on-box BLD.BAT with
`wcc -ms` works the same):

    wcc -ms TDKSYN.C
    wlink system dos file TDKSYN.obj

TDKPLAY, TDKSEND and TDKROMF build the same way. TDKSYN.C and
TDKPLAY.C are one-file builds — they include SYNTH.C (the voice
engine), SF2RT.C (the SoundFont reader) and MIDIRT.C (the byte-stream
parser) directly; TDKROMF.C stands alone.

## Provenance

The EMU8000 programming model is from the published AWE32/EMU8000
documentation and the Linux ALSA drivers: EMUINIT.H carries the init
arrays from sound/isa/sb/emu8000.c and SYNTH.C the pan/volume/expression
tables from sound/synth/emux/emux_synth.c (both GPL-2.0); the SF2
generator conversions follow awesfx (GPL). The SoundFont 2.0 format is
publicly documented and SF2RT.C is original work. The card's register
windows were derived by measurement on the hardware, and the chip-enable
values were measured from the running vendor driver — no vendor binary
was read or disassembled for any of this code.

## License

GPL v2, see the repo LICENSE.
