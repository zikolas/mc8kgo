# The EMU8000 wavetable synth (TDK MC-8000/DMC-9000)

Three DOS programs that drive the TDK MusicCard's EMU8000-family
wavetable synth directly — a card TDK shipped with Windows-only
drivers:

- **TDKSYN** — a resident General MIDI synth TSR: MIDI bytes in over an
  INT 2Fh multiplex, the card's own ROM instruments out. This is what
  lets DOS **games** play the wavetable, through
  [MPUSHIM](https://github.com/zikolas/mpushim).
- **TDKPLAY** — a standalone .MID file player through the same engine.
- **TDKSEND** — a diagnostic that sends test MIDI through the resident
  TSR and reads its counters (state check, dropped bytes, discarded
  SysEx).

## Why

The MC-8000 pairs an EMU8000-family wavetable (2MB General MIDI sample
ROM) with a CS4216 codec behind a two-window ASIC, and TDK never shipped
a DOS driver — the card was silent outside Windows. MC8KGO (this repo)
enables the card; these programs are the DOS synthesis that never
existed. The preset map is read at runtime from the vendor's own
SoundFont, so nothing derived from it ships here.

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
    /F<file> SoundFont path (default C:\WINDOWS\SYSTEM\SYNTH2GM.SF2)
    /H<a,b,c> HWCF1/2/3 override, hex (bench diagnostics)

The resident interface (`AH` = BDh): `AL=00` install check -> AL=FFh,
`AL=01` MIDI byte in DL; further diagnostic calls in the source.

Games reach it through MPUSHIM's MPU-401 facade at 330h, in every world
a DOS game lives in:

    MC8KGO                  the enabler
    TDKSYN                  this synth, resident
    ...trap hosts...        JEMM+QPIEMU, HDPMI16i, HDPMI32i
    MPUSHM16 /SYNTH         the 16-bit protected-mode shim
    MPUSHIM  /SYNTH         the 32-bit + V86 shim

Bench-proven catalogue: Monkey Island and DOSMID (V86), DOOM (32-bit),
Tyrian (16-bit), all from one boot. The synthesis follows the ALSA
emu8000/emux drivers and was cross-checked register-by-register against
the vendor's own driver running on the hardware — sustain, one-shot
loop handling and the exclusiveClass drum chokes included.

## TDKPLAY

    TDKPLAY <file.mid> [/M] [/P] [/F<file>] [/H<a,b,c>]

Standard MIDI File format 0/1; CC7/CC10/CC11/CC64, program changes and
the drum channel handled; sub-ms timing off the 8253 latch without
reprogramming the timer. `/P` ignores the SoundFont envelopes (a
diagnostic baseline).

## The SoundFont

The preset map comes at runtime from the file the vendor's Windows
installer places at `C:\WINDOWS\SYSTEM\SYNTH2GM.SF2`. Its sample-data
chunk is empty — every sample lives in the card's ROM — so the file is
purely a map, and it is not redistributed here: install the vendor
driver set once, or point `/F` at the file.

## DMC-9000 status

Working. MC8KGO enables it (window 0 at 260h, picked by model), the
MIDI DIN works, and the internal wavetable plays. The 9000 ships no
bank file: its map is a silicon SoundFont — a standard SF2 image
embedded in the card's own 4MB ROM ("4MB GMGS Rev E", which also
carries the full MT-32 set in bank 127 and a CM-64/32 kit, so `/M`
works). TDKROMF locates it (RIFF signature scan, both byte orders) and
dumps verified word ranges off the card; the extracted map then loads
with `/F` exactly like SYNTH2GM.SF2, sample data staying in ROM.

The bank data is E-mu's, extracted from your own card and never
redistributed — same policy as SYNTH2GM.SF2 above. Assembling the dump
into the bank file is host-side for now (walk the RIFF, keep INFO,
empty the sdta LIST, copy pdta, add the smpl chunk's ROM word base to
every shdr start/end/loop offset so they are ROM-absolute); a one-run
extractor that writes the file directly is planned.

The ROM read path races: SMLD reads intermittently return the word one
ahead, in sticky stretches that survive double-read verification.
TDKROMF anchors every read against a known ROM word and re-verifies —
the how and why are in its header comment. Dump twice and compare
before trusting a byte.

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
