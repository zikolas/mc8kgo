# MC8KGO

Clean-room DOS enabler for the TDK MusicCard **DMC-8000** (MW-8232) and
**DMC-9000** (MW-8432) PCMCIA sound cards — an EMU8000-family wavetable
synth plus a CS4216 codec and a 16550 MIDI DIN UART, which TDK shipped
with Windows-only drivers. MC8KGO replaces the vendor's three-driver DOS
stack (Socket Services + Card Services + TDKMC.SYS) with one .COM.

Three host backends, auto-detected when no mode switch is given:

- `/PCIC` — Intel 82365-class point enabler (direct controller programming)
- `/CS` — PCMCIA Card Services 2.1 client; stays resident with hot-plug
  and live reconfigure via a resident handoff
- `/OB` — HP OmniBook 300/425/430 Socket Services direct, with a polite
  I/O window allocator (unverified on this card so far)

The two models share MANFID 0105/0100, so the model is identified from
the VERS_1 product string, which also picks the entry-0 window base
(240h for the DMC-8000, 260h for the DMC-9000; `/IO1`/`/IO2` override).
The enabler maps the card's two 16-byte I/O windows — the EMU8200
register block (16-bit) and the MIDI DIN 16550 (8-bit, registers 2
bytes apart, divisor 5 for MIDI baud) — writes the COR (config index 0)
and the CCSR Audio bit, and verifies the result from I/O space only.

## Usage

    MC8KGO [/PCIC|/CS|/OB] [/IO1=240] [/IO2=320] [/I=n] [/S=n]
           [/W=D000] [/F[ORCE]] [/OFF] [/?]

No IRQ is requested by default: DOS-side MIDI on these cards is
trap-driven. Games see an MPU-401 at 330h through
[MPUSHIM](https://github.com/zikolas/mpushim), which traps the MPU ports
in every world (V86, 16-bit and 32-bit protected mode) and delivers the
bytes either to a resident synth TSR driving the card's wavetable, or to
the card's own DIN UART (`/UART=320 /STRIDE=2 /DIV=5`).

## Build

    nasm -f bin MC8KGO.ASM -o MC8KGO.COM

## Provenance

Clean-room: the cards' own CIS read off the hardware, the public Intel
82365SL register set, the PCMCIA CS/SS specifications via RBIL61 and the
SystemSoft CardSoft technical guide, HP OmniBook Socket Services
behaviour probed live, and the published 16550 register model. No vendor
driver was read or disassembled for this enabler. Forked from SCP55GO
2.1 (same author). MIT.
