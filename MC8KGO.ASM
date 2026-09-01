;==========================================================================
;  MC8KGO 0.2 - unified clean-room DOS enabler for the TDK MusicCard
;              MC-8000 (MW-8232) and DMC-9000 (MW-8432) PCMCIA sound
;              cards (EMU8000-family wavetable + CS4216 codec + a 16550
;              MIDI DIN UART), three host backends in one .COM:
;
;    /PCIC  Intel 82365-class point enabler (direct PCIC programming).
;    /CS    PCMCIA Card Services 2.1 client, stays TSR (hot-plug, live
;           reconfigure via resident handoff).
;    /OB    HP OmniBook 300/425/430 Socket Services direct, with the
;           polite I/O window allocator.  UNVERIFIED on this card: the
;           16-bit window characteristic is untested on the OB SS.
;
;  With no mode switch the host is auto-detected: Card Services first (if a
;  CS arbiter is loaded we must go through it), then the 'SS' Socket
;  Services signature (OmniBook ROM), then an 82365 probe at 3E0h.
;
;  THE CARDS (from their own CIS + live probing):
;    MANFID 0105/0100 - IDENTICAL on both models, so the model comes from
;    the VERS_1 product string: "MW-8232" = MC-8000, "MW-8432" = DMC-9000.
;    Config registers at attribute 100h (COR + CCSR + PRR + SCR).
;    Config index 0 = TWO 16-byte I/O windows at the CIS entry-0 bases:
;      MC-8000: 240h + 320h        DMC-9000: 260h + 320h
;    window 0 MUST BE 16-BIT: the ASIC's EMU8200 register block (PTR at
;      +0Eh, DATA0 at +4/+6, DATA1..3 at +8/+0Ah/+0Ch) reads torn words
;      through an 8-bit window;
;    window 1 (8-bit): a 16550 UART with registers 2 apart = the MIDI DIN
;      (divisor 5 = 31250 baud); its idle LSR at +0Ah reads 60h, which is
;      the post-enable verify.
;    This enabler ONLY configures the card - the vendor's TDKMC.SYS does
;    exactly the same (windows + IRQ, no audio code).  Synth init is
;    TDKSYN's job; the VSBPCM backend to come needs only these windows.
;    No IRQ is requested by default - the DOS stack is trap-driven
;    (/I=n if ever needed; the CIS IRQ mask allows 5, 7 and 10).
;    CCSR (attr 102h) is written to 02h - the Audio bit, matching the
;    state observed on a vendor-enabled card.
;
;  Usage:
;    MC8KGO [/PCIC|/CS|/OB] [/IO1=240] [/IO2=320] [/I=n] [/S=n]
;          [/W=D000] [/F[ORCE]] [/OFF] [/?]
;
;    /IO1=hex  window 0 base (EMU block; default per model: 240/260)
;    /IO2=hex  window 1 base (DIN UART; default 320)
;    /I=dec    IRQ to route: 5, 7 or 10 (default 0 = none)
;    /S=dec    socket (PCIC 0-7, OB 1-2, CS: probe only this socket)
;    /W=hex    attr-window segment for the CIS/COR access (PCIC; def D000)
;    /FORCE    configure without the CIS identity check (needs /S)
;    /OFF      PCIC: power the socket down; CS: release + go dormant
;
;  Build (host or on-box):  nasm -f bin MC8KGO.ASM -o MC8KGO.COM
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
;  Clean-room provenance: the cards' own CIS read
;  off the hardware, the public Intel 82365SL
;  register set, the PCMCIA CS/SS specs via RBIL61 + the SystemSoft
;  CardSoft technical guide (API binding), HP OmniBook Socket Services
;  behavior probed live (es1688go repo), and the published 16550 register
;  model.  No vendor driver was read or disassembled.  Structure forked
;  from our SCP55GO 2.1 (same author).
;==========================================================================
        cpu  8086
        org  100h

MODE_CS   equ 1
MODE_OB   equ 2
MODE_PCIC equ 3

; Resident-handoff protocol version.  The handoff copies the option/result
; blocks BY OFFSET between two runs of the same binary, so bump this on
; EVERY release - offsets shift with any rebuild, and a mismatched resident
; copy must be refused (reboot to replace), never driven.
SGVER   equ 0001h

TDMAX   equ 40h                       ; CS tuple-data buffer size
VBMAX   equ 40h                       ; VERS_1 capture buffer size
SETTLEP equ 250                       ; CIS settle gate cap: 250 x 20ms = ~5s
CISLEN  equ 1024                      ; dense CIS bytes pulled per probe.
                                      ; Generous on purpose: the SCP-55 packs
                                      ; ~20 CFTABLE_ENTRY tuples BEFORE its
                                      ; MANFID, which lands at dense offset
                                      ; 298 - a 256-byte pull missed it and
                                      ; the card read as "not present".
                                      ; cisbuf is trailing BSS, so this costs
                                      ; no file or resident bytes.
PCIC_BASE equ 3E0h

; OB (OmniBook 300/425/430) probed constants - see es1688go doc/RECON-425.md
OB_WIN_ATTR equ 0Eh                   ; mem window attrs (DH bit0 INVERTED here)
OB_WIN_IO   equ 05h                   ; I/O window attrs
OB_SPEED    equ 20h                   ; 300 ns
OB_ATTRSEG  equ 0D000h                ; window 0 host base (probed idle/free)

; The cards (from their own CIS)
TDK_MANF  equ 0105h                   ; MANFID: TDK (both models)
TDK_CARD  equ 0100h
TDK_COR   equ 0100h                   ; config registers base (attribute)

start:  jmp  init                     ; 3 bytes: sig block lands at 103h

;==========================================================================
;  RESIDENT DATA  (kept when /CS mode goes TSR)
;==========================================================================
; Control-channel signature at fixed offset 103h: a later run finds the
; resident copy by scanning MCBs for this, copies fresh options in, and
; far-calls sigentry so the RESIDENT client swaps its own CS resources.
sig      db 'M8GO'                    ; @103
sigver   dw SGVER                     ; @107 must match exactly for a handoff
sigentry dw cs_reconfig               ; @109

handle   dw 0                         ; CS client handle
live     db 0                         ; 0 = queue events, 1 = act on them
inbusy   db 0                         ; re-entrancy guard
regdone  db 0                         ; saw REGISTRATION_COMPLETE
cfged    db 0                         ; card is configured
oursock  dw 0FFFFh
n_sock   db 2                         ; CS socket count

; ---- option block (contiguous: a CS handoff copies o_io1..O_LEN in one
;      sweep) -------------------------------------------------------------
o_io1    dw 0240h                     ; window 0 base (EMU block)
o_io2    dw 0320h                     ; window 1 base (DIN UART)
o_io1gv  db 0                         ; 1 = /IO1 given (model default off)
o_irq    db 0                         ; 0 = none (the DOS stack needs none)
o_sock   db 0FFh                      ; FFh = auto
o_force  db 0
o_off    db 0
coridx   db 00h                       ; config index 0 = the two-window entry
O_LEN    equ $-o_io1

; ---- identity / result block (contiguous: handoff copies R_LEN back) ----
arg_sock dw 0                         ; input to cs_configure
r_stage  db 0                         ; 0=ok 2=io 4=cfg 5=codec 6=wrongcard 7=nocard
r_code   dw 0                         ; CS status of the failing call
manfid   dw 0                         ; CISTPL_MANFID: TPLMID_MANF
         dw 0                         ;                TPLMID_CARD
r_irq    db 0FFh                      ; CS granted IRQ (FFh = none)
r_w0     db 0                         ; window 0 decodes (not an FF wall)
r_uart   db 0                         ; window 1 LSR reads like an idle 16550
r_ccsr   db 0                         ; CCSR readback (want bit1 = Audio)
is9000   db 0                         ; model: 0 = MC-8000, 1 = DMC-9000
matched  db 0                         ; 1 = MANFID hit (0 = forced)
k_name   dw n_unk                     ; display name
cfgbase  dw TDK_COR                   ; COR base from CISTPL_CONFIG
verslen  dw 0                         ; VERS_1 strings length in versbuf
versbuf  times VBMAX db 0             ; VERS_1 strings (0-separated)
R_LEN    equ $-arg_sock

qcnt     db 0                         ; CS event queue (registration window)
queue    times 16 db 0                ; 8 x (event, socket)

; ---- Card Services function codes ----------------------------------------
CS_DEREG        equ 02h
CS_FIRSTTUPLE   equ 07h
CS_NEXTTUPLE    equ 0Ah
CS_INFO         equ 0Bh
CS_TUPLEDATA    equ 0Dh
CS_REGISTER     equ 10h
CS_RELIO        equ 1Bh
CS_RELIRQ       equ 1Ch
CS_RELCFG       equ 1Eh
CS_REQIO        equ 1Fh
CS_REQIRQ       equ 20h
CS_REQCFG       equ 30h

EV_REMOVAL      equ 05h
EV_CLIENTINFO   equ 14h
EV_INSERT       equ 40h
EV_REGCOMPLETE  equ 82h

; ---- CS argument packets -------------------------------------------------
p_reg:   dw 0004h                     ; attributes: I/O client
         dw 0080h                     ; event mask: card-detect change
         dw 0                         ; client data word  (-> DI)
p_regsel dw 0                         ; client data selector (-> DS), = our CS
         dw 0                         ; client data offset (-> SI)
         dw 0                         ; reserved
         dw 0210h                     ; expected CS version 2.10
P_REG_LEN equ 0Eh

p_tup:   dw 0                         ; +00 socket
         dw 0                         ; +02 attributes
         db 0                         ; +04 desired tuple code
         db 0                         ; +05 tuple offset (GetTupleData)
         dw 0                         ; +06 flags
         dd 0                         ; +08 Link state
         dd 0                         ; +0C CIS state
         dw 0                         ; +10 first/next: ret code+link / data: maxlen
         dw 0                         ; +12 data: (ret) length
tdata:   times TDMAX db 0             ; +14 tuple data
P_TUP1_LEN equ 12h
P_TUPD_LEN equ 14h + TDMAX

p_io:    dw 0                         ; +00 socket
         dw 0                         ; +02 base port 1 (MPU + codec block)
         db 0                         ; +04 size 1
         db 0                         ; +05 attrs 1 (0 = 8-bit)
         dw 0                         ; +06 base port 2 (none)
         db 0                         ; +08 size 2
         db 0                         ; +09 attrs 2
         db 16                        ; +0A address lines decoded (per CIS)
P_IO_LEN equ 0Bh

p_irq:   dw 0                         ; +00 socket
         dw 0                         ; +02 attributes
         db 0                         ; +04 (ret) assigned IRQ
p_irq_i1 db 0                         ; +05 IRQ info 1
p_irq_i2 dw 0                         ; +06 IRQ info 2 (bitmap)
P_IRQ_LEN equ 8

p_cfg:   dw 0                         ; +00 socket
p_cfg_at dw 0002h                     ; +02 attrs: bit1 = IRQ active
p_cfg_vc db 50                        ; +04 Vcc  (tenths of a volt)
p_cfg_v1 db 50                        ; +05 Vpp1
p_cfg_v2 db 50                        ; +06 Vpp2
         db 02h                       ; +07 interface: I/O+memory
p_cfg_cb dd 00000100h                 ; +08 config register base
p_cfg_st db 02h                       ; +0C status reg (CCSR): Audio bit
         db 0                         ; +0D pin reg
         db 0                         ; +0E copy reg
p_cfg_op db 00h                       ; +0F option reg: config index 0
p_cfg_pr db 03h                       ; +10 present regs: option + status
         db 0                         ; +11 ext status
P_CFG_LEN equ 12h

p_rel:   dw 0                         ; ReleaseConfiguration: socket
P_REL_LEN equ 2

p_rirq:  dw 0                         ; ReleaseIRQ: socket
         dw 0                         ;   attributes
p_rirq_i db 0                         ;   IRQ to release
P_RIRQ_LEN equ 5

;==========================================================================
;  KNOWN-CARD MANIFEST (resident: the CS hot-plug path identifies too).
;  record: +0 manf, +2 card, +4 name ptr
;==========================================================================
KREC     equ 6
ktab:
        dw TDK_MANF, TDK_CARD
        dw n_tdk
        dw 0, 0                       ; end of table

; identify - match [manfid] against the manifest.  CF clear = hit
; ([matched]/[k_name] filled).
identify:
        push si
        push ax
        mov  si, ktab
.next:  mov  ax, [si]
        or   ax, [si+2]
        jz   .unk
        mov  ax, [manfid]
        cmp  ax, [si]
        jne  .miss
        mov  ax, [manfid+2]
        cmp  ax, [si+2]
        jne  .miss
        mov  ax, [si+4]
        mov  [k_name], ax
        mov  byte [matched], 1
        pop  ax
        pop  si
        clc
        ret
.miss:  add  si, KREC
        jmp  .next
.unk:   pop  ax
        pop  si
        stc
        ret

; ---- force_ident: /FORCE identity for an unknown/unreadable card --------
force_ident:
        mov  word [k_name], n_unk
        mov  byte [matched], 0
        ret

; ---- vers_model: both models carry MANFID 0105/0100, so the model comes
;      from the VERS_1 product string: "MW-8232" = MC-8000, "MW-8432" =
;      DMC-9000.  Sets [is9000], the display name, and - unless /IO1
;      pinned it - the model's entry-0 window 0 base (240h vs 260h).
;      Resident: the CS hot-plug path identifies too. ---------------------
vers_model:
        push ax
        push bx
        push cx
        cmp  byte [matched], 0        ; /FORCE: keep 'Unknown card' and the
        je   .done                    ; user's own /IO1
        mov  byte [is9000], 0
        mov  word [k_name], n_tdk8
        mov  cx, [verslen]
        jcxz .dflt
        xor  bx, bx
.scan:                                ; find "MW-8" and judge the next char
        cmp  cx, 5
        jb   .dflt
        cmp  byte [versbuf+bx], 'M'
        jne  .next
        cmp  byte [versbuf+bx+1], 'W'
        jne  .next
        cmp  byte [versbuf+bx+2], '-'
        jne  .next
        cmp  byte [versbuf+bx+3], '8'
        jne  .next
        cmp  byte [versbuf+bx+4], '4'
        jne  .dflt                    ; "MW-82xx" (or unknown) = MC-8000
        mov  byte [is9000], 1
        mov  word [k_name], n_tdk9
.dflt:
        mov  ax, 0240h                ; window 0 entry-0 base per model
        cmp  byte [is9000], 0
        je   .base
        mov  ax, 0260h
.base:
        cmp  byte [o_io1gv], 0        ; /IO1 pins it
        jne  .done
        mov  [o_io1], ax
.done:
        pop  cx
        pop  bx
        pop  ax
        ret
.next:
        inc  bx
        dec  cx
        jmp  .scan

;==========================================================================
;  cs_call - invoke Card Services (INT 1Ah, AH=AFh; SystemSoft binding).
;  in:  AL=function, BX=packet offset, CX=ArgLength, DX=handle
;  out: CF + AX status.  Preserves BX/CX/DX/SI/DI/ES.
;==========================================================================
cs_call:
        push es
        push bx
        push cx
        push dx
        push si
        push di
        mov  ah, 0AFh
        push cs
        pop  es
        int  1Ah
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  es
        ret

;==========================================================================
;  cs_callback - Card Services event handler (FAR).
;  AL=event, CX=socket, ES:BX=buffer.  Returns AX=0, CF clear.
;==========================================================================
cs_callback:
        pushf
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push ds
        push es
        push cs
        pop  ds

        cmp  al, EV_REGCOMPLETE
        jne  .n1
        mov  byte [regdone], 1
        jmp  .done
.n1:
        cmp  al, EV_CLIENTINFO
        jne  .n2
        mov  word [es:bx+2], 0        ; returned-info length = 0
        jmp  .done
.n2:
        cmp  al, EV_INSERT
        je   .ins
        cmp  al, EV_REMOVAL
        je   .rem
        jmp  .done
.ins:
        cmp  byte [live], 0
        je   .queue
        cmp  byte [inbusy], 0
        jne  .done
        cmp  byte [o_off], 0
        jne  .done                    ; /OFF'd: stay dormant until re-enabled
        cmp  byte [cfged], 0
        jne  .done                    ; already driving a card
        mov  al, [o_sock]             ; /S pins hot-plug to one socket too
        cmp  al, 0FFh
        je   .insgo
        xor  ah, ah
        cmp  ax, cx
        jne  .done
.insgo: mov  [arg_sock], cx
        call cs_configure             ; resident, no DOS calls inside
        jmp  .done
.rem:
        cmp  byte [live], 0
        je   .queue
        cmp  byte [cfged], 0
        je   .done
        cmp  cx, [oursock]
        jne  .done
        call release_all
        mov  byte [cfged], 0
        jmp  .done
.queue:
        mov  dl, [qcnt]
        cmp  dl, 8
        jae  .done
        xor  dh, dh
        mov  si, dx
        shl  si, 1
        mov  [queue+si], al           ; event
        mov  [queue+si+1], cl         ; socket (low byte)
        inc  byte [qcnt]
.done:
        pop  es
        pop  ds
        pop  bp
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        popf
        clc
        xor  ax, ax
        retf

;==========================================================================
;  cs_configure - full CS bring-up of the card in [arg_sock].
;  Resident-safe: INT 1A + port I/O only.  Results in r_*; r_stage=0 ok.
;==========================================================================
cs_configure:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        mov  byte [inbusy], 1
        mov  byte [r_stage], 0
        mov  byte [r_w0], 0
        mov  byte [r_uart], 0
        mov  byte [r_ccsr], 0
        mov  byte [r_irq], 0FFh
        mov  byte [matched], 0

        mov  ax, [arg_sock]           ; stamp the socket into every packet
        mov  [p_tup], ax
        mov  [p_io], ax
        mov  [p_irq], ax
        mov  [p_cfg], ax
        mov  [p_rel], ax
        mov  [p_rirq], ax

        ; ---- CIS identity: MANFID (+ VERS_1 for display) ------------------
        mov  byte [p_tup+4], 20h      ; CISTPL_MANFID
        call tup_get
        jnc  .midok
        jmp  .nocard
.midok: mov  ax, [tdata]
        mov  [manfid], ax
        mov  ax, [tdata+2]
        mov  [manfid+2], ax

        mov  word [verslen], 0        ; CISTPL_VERS_1 strings
        mov  byte [p_tup+4], 15h
        call tup_get
        jc   .novers
        mov  cx, [p_tup+12h]          ; returned data length
        cmp  cx, 2
        jbe  .novers
        sub  cx, 2                    ; skip the major/minor bytes
        cmp  cx, VBMAX
        jbe  .vc
        mov  cx, VBMAX
.vc:
        mov  [verslen], cx
        push es
        push cs
        pop  es
        mov  si, tdata+2
        mov  di, versbuf
        cld
        rep  movsb
        pop  es
.novers:
        call identify
        jnc  .known
        mov  byte [r_stage], 6        ; not one of ours
        jmp  .out
.known:
        call vers_model               ; model + window 0 default base
        mov  al, [coridx]             ; plain index 0; the level-IREQ bit
        mov  [p_cfg_op], al           ; is added only if an IRQ is granted

        ; ---- CISTPL_CONFIG -> COR base (default 400h on any trouble) -----
        mov  word [cfgbase], TDK_COR
        mov  byte [p_tup+4], 1Ah
        call tup_get
        jc   .cfb
        mov  al, [tdata]              ; TPCC_SZ: bits1:0 = RADR size - 1
        and  ax, 3
        mov  dl, [tdata+2]            ; RADR low
        xor  dh, dh
        test ax, ax
        jz   .cfs
        mov  dh, [tdata+3]            ; RADR high (rasz >= 2)
.cfs:
        mov  [cfgbase], dx
.cfb:
        mov  ax, [cfgbase]
        mov  [p_cfg_cb], ax
        mov  word [p_cfg_cb+2], 0

        ; ---- RequestIO: TWO 16-byte ranges - the EMU block (16-bit data
        ;      path) and the DIN UART.  Not every CS stack honours the
        ;      16-bit attribute or the same IOAddrLines, so walk the
        ;      candidate pairs until one is accepted. ----------------------
        mov  ax, [o_io1]
        mov  [p_io+2], ax
        mov  byte [p_io+4], 16
        mov  ax, [o_io2]
        mov  [p_io+6], ax
        mov  byte [p_io+8], 16
        mov  byte [p_io+9], 0
        mov  si, iotries
.iotry:
        lodsb
        cmp  al, 0FFh
        je   .iofail
        mov  [p_io+5], al             ; attrs 1 (08h = 16-bit data path)
        lodsb
        mov  [p_io+0Ah], al           ; address lines decoded
        push si
        call req_io
        pop  si
        jnc  .io_done
        mov  [r_code], ax             ; remember the last refusal
        jmp  .iotry
.iofail:
        mov  byte [r_stage], 2
        jmp  .out
.io_done:

        ; ---- RequestIRQ (only when /I asked): (1) specific IRQ, (2) a
        ;      bitmap holding ONLY that IRQ, (3) any card-capable IRQ.
        ;      All optional - MPU-401 output needs no IRQ at all. ----------
        cmp  byte [o_irq], 0
        je   .noirq
        mov  word [p_irq+2], 0
        mov  al, [o_irq]
        and  al, 0Fh
        or   al, 20h                  ; level + specific IRQ
        mov  [p_irq_i1], al
        mov  word [p_irq_i2], 0
        mov  al, CS_REQIRQ
        mov  bx, p_irq
        mov  cx, P_IRQ_LEN
        mov  dx, [handle]
        call cs_call
        jnc  .irqok
        mov  byte [p_irq_i1], 30h     ; level + use bitmap
        mov  cl, [o_irq]
        mov  ax, 1
        shl  ax, cl
        mov  [p_irq_i2], ax           ; bitmap = only the preferred IRQ
        mov  al, CS_REQIRQ
        mov  cx, P_IRQ_LEN
        call cs_call
        jnc  .irqok
        mov  byte [p_irq_i1], 30h
        mov  word [p_irq_i2], 04A0h   ; IRQs 5, 7 and 10 (the CIS set)
        mov  al, CS_REQIRQ
        call cs_call
        jc   .noirq
.irqok:
        mov  al, [p_irq+4]
        mov  [r_irq], al
        or   byte [p_cfg_op], 40h     ; level-mode IREQ with the IRQ
.noirq:

        ; ---- RequestConfiguration ----------------------------------------
        mov  word [p_cfg_at], 0002h   ; IRQ active
        cmp  byte [r_irq], 0FFh
        jne  .cat
        mov  word [p_cfg_at], 0
.cat:
        mov  byte [p_cfg_vc], 50
        mov  byte [p_cfg_v1], 50
        mov  byte [p_cfg_v2], 50
        mov  byte [p_cfg_pr], 03h     ; present: option + status (CCSR)
        mov  byte [p_cfg_st], 02h     ; CCSR value: the Audio bit
        call try_cfg                  ; walks candidate ArgLengths (1Bh dance)
        jnc  .cfgok
        mov  byte [p_cfg_v1], 0       ; some stacks reject Vpp=Vcc
        mov  byte [p_cfg_v2], 0
        call try_cfg
        jnc  .cfgok
        mov  byte [p_cfg_pr], 01h     ; or dislike the CCSR write: COR only
        call try_cfg
        jnc  .cfgok
        mov  [r_code], ax
        mov  byte [r_stage], 4
        call rel_io_irq               ; undo the partial claim
        jmp  .out
.cfgok:

        ; ---- post-enable verify (I/O reads only - resident-safe) ---------
        call tdk_verify
        cmp  byte [r_w0], 0
        jne  .alive
        mov  byte [r_stage], 5        ; configured but window 0 reads FF
.alive:
        mov  ax, [arg_sock]
        mov  [oursock], ax
        mov  byte [cfged], 1
.out:
        mov  byte [inbusy], 0
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        ret
.nocard:
        mov  [r_code], ax
        mov  byte [r_stage], 7
        jmp  .out

; ---- tup_get: GetFirstTuple (code in p_tup+4) then GetTupleData ---------
;      out: CF set if either failed (AX = status); data in tdata
tup_get:
        mov  word [p_tup+2], 0        ; attributes
        mov  byte [p_tup+5], 0        ; tuple offset
        mov  al, CS_FIRSTTUPLE
        mov  bx, p_tup
        mov  cx, P_TUP1_LEN
        mov  dx, [handle]
        call cs_call
        jc   .r
        mov  word [p_tup+10h], TDMAX  ; max data to return
        mov  al, CS_TUPLEDATA
        mov  cx, P_TUPD_LEN
        call cs_call
.r:
        ret

; ---- try_cfg: RequestConfiguration, walking candidate ArgLengths --------
;      RBIL says 12h but SystemSoft 2.06 rejects it with 1Bh (bad length);
;      keep trying plausible packet sizes until the answer isn't 1Bh.
try_cfg:
        push si
        mov  si, cfglens
.t:
        lodsb
        test al, al
        jz   .exhaust
        mov  cl, al
        xor  ch, ch
        mov  al, CS_REQCFG
        mov  bx, p_cfg
        mov  dx, [handle]
        call cs_call
        jnc  .out                     ; success (CF clear)
        cmp  ax, 1Bh
        je   .t                       ; wrong length - next candidate
        stc                           ; a real refusal - surface it
        jmp  .out
.exhaust:
        mov  ax, 1Bh
        stc
.out:
        pop  si
        ret

cfglens: db 12h, 11h, 13h, 14h, 10h, 16h, 1Ah, 0Eh, 0

; RequestIO (attrs1, IOAddrLines) candidates: 16-bit data path first
iotries: db 08h, 16, 08h, 10, 00h, 16, 00h, 10, 0FFh

; ---- req_io: RequestIO with the current p_io ----------------------------
req_io:
        mov  al, CS_REQIO
        mov  bx, p_io
        mov  cx, P_IO_LEN
        mov  dx, [handle]
        call cs_call
        ret

; ---- release_all / rel_io_irq -------------------------------------------
release_all:
        mov  al, CS_RELCFG
        mov  bx, p_rel
        mov  cx, P_REL_LEN
        mov  dx, [handle]
        call cs_call
rel_io_irq:
        mov  al, CS_RELIO
        mov  bx, p_io                 ; same packet we requested with
        mov  cx, P_IO_LEN
        mov  dx, [handle]
        call cs_call
        mov  al, [r_irq]
        cmp  al, 0FFh
        je   .r
        mov  [p_rirq_i], al
        mov  word [p_rirq+2], 0
        mov  al, CS_RELIRQ
        mov  bx, p_rirq
        mov  cx, P_RIRQ_LEN
        mov  dx, [handle]
        call cs_call
.r:
        ret

;==========================================================================
;  cs_reconfig - FAR control entry for a later SCP55GO run (found via the
;  sig block).  The caller has copied fresh options in.  Release whatever
;  we hold, then - unless /OFF - probe and reconfigure with the new
;  options.  Results land in the r_* block for the caller to copy back.
;==========================================================================
cs_reconfig:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push ds
        push es
        push cs
        pop  ds
        cmp  byte [cfged], 0
        je   .go
        call release_all
        mov  byte [cfged], 0
.go:
        cmp  byte [o_off], 0
        jne  .done                    ; /OFF: stay dormant (hot-plug re-arms)
        xor  dx, dx
.p:
        cmp  byte [cfged], 0
        jne  .done
        mov  al, [o_sock]             ; /S pins the probe to one socket
        cmp  al, 0FFh
        je   .any
        cmp  al, dl
        jne  .skip
.any:
        mov  [arg_sock], dx
        call cs_configure
.skip:
        inc  dx
        cmp  dl, [n_sock]
        jb   .p
.done:
        pop  es
        pop  ds
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        retf

;==========================================================================
;  SHARED CARD BRING-UP / VERIFY  (resident: the CS hot-plug path too)
;
;  The card needs NO register programming beyond the COR (and CCSR's Audio
;  bit, written where a backend holds the attribute window) - the vendor's
;  own TDKMC.SYS proves windows + IRQ is the whole job.  Synth init is
;  TDKSYN's.  What CAN be judged from I/O space, politely and read-only:
;    window 0: an enabled card returns live EMU data; a dead or
;      misconfigured one an all-FF wall (+0, +2 and +0Eh all FF);
;    window 1: the DIN 16550's LSR at +0Ah (registers 2 apart) reads 60h
;      (THRE|TEMT) when idle - a strong signature, and reading the LSR
;      disturbs nothing.
;==========================================================================
tdk_verify:
        push ax
        push bx
        push dx
        mov  byte [r_w0], 0
        mov  byte [r_uart], 0
        mov  ax, 20                   ; let the windows settle
        call ms_delay
        mov  dx, [o_io1]
        in   al, dx
        mov  bl, al
        mov  dx, [o_io1]
        add  dx, 2
        in   al, dx
        and  bl, al
        mov  dx, [o_io1]
        add  dx, 0Eh                  ; the EMU PTR port
        in   al, dx
        and  bl, al
        cmp  bl, 0FFh                 ; all three FF = an FF wall
        je   .w0done
        mov  byte [r_w0], 1
.w0done:
        mov  dx, [o_io2]
        add  dx, 0Ah                  ; 16550 LSR at stride 2
        in   al, dx
        cmp  al, 0FFh
        je   .udone
        and  al, 60h
        cmp  al, 60h                  ; THRE|TEMT = an idle transmitter
        jne  .udone
        mov  byte [r_uart], 1
.udone:
        pop  dx
        pop  bx
        pop  ax
        ret

iodn:                                 ; CX port-80h reads (AX/CX preserved)
        push ax
        push cx
.l:
        in   al, 80h
        loop .l
        pop  cx
        pop  ax
        ret

ms_delay:                             ; AX = ~milliseconds of port-80h reads
        push ax
        push bx
        push cx
        mov  bx, ax                   ; BX counts: the inner reads eat AL
.m:
        mov  cx, 1000
.l:
        in   al, 80h
        loop .l
        dec  bx
        jnz  .m
        pop  cx
        pop  bx
        pop  ax
        ret

RESEND:                               ; end of resident image

;==========================================================================
;  INIT  (transient: discarded when /CS mode goes TSR)
;==========================================================================
init:
        cld
        mov  si, m_banner
        call prstr

        mov  si, 81h
        call parse_args

        ; ---- global sanity: window bases 16-byte aligned and never over
        ;      the COMrade link (COM1) or the PCIC index pair -------------
        mov  ax, [o_io1]
        call io_check
        jc   .badio
        mov  ax, [o_io2]
        call io_check
        jnc  .iook
.badio:
        mov  si, m_badio
        jmp  errout
.iook:
        mov  al, [o_irq]              ; IRQ: none, or 5/7/10 (the CIS set)
        or   al, al
        jz   .irqok
        cmp  al, 5
        je   .irqok
        cmp  al, 7
        je   .irqok
        cmp  al, 10
        je   .irqok
        mov  si, m_badirq
        jmp  errout
.irqok:
        cmp  byte [o_force], 0        ; /FORCE needs /S in every mode
        je   .fok
        cmp  byte [o_sock], 0FFh
        jne  .fok
        mov  si, m_fneeds
        jmp  errout
.fok:

        ; ---- pick the host backend --------------------------------------
        mov  al, [o_mode]
        or   al, al
        jnz  .dispatch
        call chk_cs
        jnc  .a_cs
        call chk_ss
        jnc  .a_ob
        call chk_pcic
        jnc  .a_pc
        mov  si, m_nomode
        jmp  errout
.a_cs:  mov  al, MODE_CS
        jmp  .dset
.a_ob:  mov  al, MODE_OB
        jmp  .dset
.a_pc:  mov  al, MODE_PCIC
.dset:  mov  [o_mode], al
.dispatch:
        cmp  al, MODE_CS
        jne  .d2
        mov  si, m_hostcs
        call prstr
        jmp  cs_mode
.d2:
        cmp  al, MODE_OB
        jne  .d3
        mov  si, m_hostob
        call prstr
        jmp  ob_mode
.d3:
        mov  si, m_hostpc
        call prstr
        jmp  pc_mode

; ---- io_check: AX = a 16-byte window base.  CF set = refused: zero,
;      unaligned, or the window would touch the COMrade link (COM1
;      3F8h-3FFh: base 3F0h covers it) or the PCIC index/data pair
;      (3E0h-3E7h).  Spanning either wedges the box until a physical
;      power cycle - see pc110-io-window-avoid-com1. ----------------------
io_check:
        or   ax, ax
        jz   .bad
        test ax, 000Fh                ; 16-byte aligned
        jnz  .bad
        cmp  ax, 03F0h                ; would cover 3F8h-3FFh (COM1)
        je   .bad
        cmp  ax, 03E0h                ; would cover the PCIC at 3E0h
        je   .bad
        clc
        ret
.bad:
        stc
        ret

; ---- chk_cs: Card Services present?  CF clear = yes ([n_sock] filled) ---
chk_cs:
        mov  word [ibuf], 50h
        mov  word [ibuf+2], 0         ; scrub the signature slot first
        mov  al, CS_INFO
        mov  bx, ibuf
        mov  cx, 50h
        xor  dx, dx
        xor  si, si
        xor  di, di
        call cs_call
        jc   .no
        cmp  word [ibuf+2], 5343h     ; "CS"
        je   .yes
        cmp  word [ibuf+2], 4353h     ; "SC"
        je   .yes
.no:
        stc
        ret
.yes:
        mov  ax, [ibuf+4]
        cmp  ax, 8
        jbe  .ns
        mov  ax, 8
.ns:    mov  [n_sock], al
        clc
        ret

; ---- chk_ss: Socket Services 'SS' signature?  CF clear = yes ------------
chk_ss:
        push cx
        xor  cx, cx                   ; a stopped-clock BIOS must not fake it
        mov  ax, 8000h
        call ob_ssvc
        jc   .no
        cmp  cx, 5353h
        jne  .no
        pop  cx
        clc
        ret
.no:
        pop  cx
        stc
        ret

; ---- chk_pcic: any 82365-class controller at 3E0/3E2/3E4/3E6? -----------
chk_pcic:
        push cx
        mov  cx, 4
        mov  word [pc_idx], PCIC_BASE
        mov  byte [pc_off], 0
.c:
        call pc_present
        jnc  .yes
        add  word [pc_idx], 2
        loop .c
        pop  cx
        stc
        ret
.yes:
        pop  cx
        clc
        ret

;==========================================================================
;  CS MODE  (transient init; the resident half is above)
;==========================================================================
cs_mode:
        cmp  byte [o_force], 0
        je   .f2
        mov  si, m_csnoforce
        call prstr
.f2:
        ; ---- CS present? (also true when auto-detected; cheap to redo) ---
        call chk_cs
        jnc  .csok
        mov  si, m_nocs
        jmp  errout
.csok:
        mov  si, m_cs
        call prstr
        mov  al, [n_sock]
        call prdec99
        call crlf

        ; ---- already resident?  Hand the new options to that copy -------
        call find_resident
        jnc  cs_handoff
        cmp  byte [res_badver], 0
        je   .nover
        mov  si, m_vermsg
        jmp  errout
.nover:
        cmp  byte [o_off], 0
        je   .fresh
        mov  si, m_offnone
        jmp  errout
.fresh:

        ; ---- RegisterClient (inline: the handle comes back in DX) --------
        mov  [p_regsel], cs           ; callback's DS
        mov  ah, 0AFh
        mov  al, CS_REGISTER
        push cs
        pop  es
        mov  bx, p_reg
        mov  cx, P_REG_LEN
        xor  dx, dx
        mov  di, cs                   ; ClientEntry = DI:SI
        mov  si, cs_callback
        int  1Ah
        jnc  .regok
        mov  [r_code], ax
        mov  si, m_regfail
        call prstr
        mov  ax, [r_code]
        call prhex4
        call crlf
        mov  ax, 4C03h
        int  21h
.regok:
        mov  [handle], dx

        ; ---- give CS a moment to deliver REGISTRATION_COMPLETE -----------
        xor  ax, ax
        mov  es, ax
        mov  bx, [es:46Ch]            ; BIOS tick count (low word)
.wait:
        int  28h                      ; DOS idle - let TSR stacks breathe
        call drain_queue
        cmp  byte [cfged], 0
        jne  .wend                    ; configured via a queued event - done
        cmp  byte [regdone], 0
        jne  .wend
        xor  ax, ax
        mov  es, ax
        mov  ax, [es:46Ch]
        sub  ax, bx
        cmp  ax, 36                   ; ~2 s
        jb   .wait
.wend:
        mov  si, m_reg
        call prstr
        mov  si, m_regyes
        cmp  byte [regdone], 0
        jne  .rp
        mov  si, m_regno
.rp:
        call prstr

        ; ---- actively probe the sockets: SystemSoft CS does NOT generate
        ;      artificial CARD_INSERTION events, so waiting for the
        ;      callback only works for a future physical insert -----------
        cmp  byte [cfged], 0
        jne  cs_report
        xor  dx, dx                   ; DX = socket number
.scan:
        cmp  byte [cfged], 0
        jne  cs_report
        mov  al, [o_sock]             ; /S pins the probe to one socket
        cmp  al, 0FFh
        je   .sgo
        cmp  al, dl
        jne  .snext
.sgo:
        mov  [arg_sock], dx
        push dx
        mov  si, m_probe
        call prstr
        mov  al, dl
        call prdec99
        call crlf
        call cs_configure
        call cs_repsock               ; per-socket verdict while we scan
        pop  dx
.snext:
        inc  dx
        cmp  dl, [n_sock]
        jb   .scan

; ---- CS report + TSR -----------------------------------------------------
cs_report:
        cmp  byte [cfged], 0
        je   .nocard
        mov  ax, [arg_sock]
        mov  [rep_sock], al
        mov  al, [r_irq]
        mov  [rep_irq], al
        mov  byte [wcount], 0         ; no window line in CS mode
        call report
        jmp  cs_tsr
.nocard:
        mov  si, m_nocard
        call prstr
        cmp  byte [hndoff], 0
        jne  cs_tsr
        mov  si, m_resident
        call prstr
cs_tsr:
        cmp  byte [hndoff], 0
        jne  .exit                    ; handoff run: the OLD copy stays
        mov  byte [live], 1           ; callback now acts on events itself
        mov  dx, (RESEND - $$ + 100h + 15) >> 4   ; paragraphs incl. PSP
        mov  ax, 3100h
        int  21h
.exit:
        mov  ax, 4C00h
        int  21h

; ---- cs_handoff: drive the resident copy with the new options -----------
cs_handoff:
        cld
        mov  es, [res_seg]            ; options in (same binary = offsets)
        mov  si, o_io1
        mov  di, o_io1
        mov  cx, O_LEN
        rep  movsb
        mov  ax, [res_off]
        mov  [fptr], ax
        mov  ax, [res_seg]
        mov  [fptr+2], ax
        call far [fptr]               ; resident releases + reconfigures
        push ds                       ; results back: resident -> our vars
        push cs
        pop  es
        mov  si, arg_sock
        mov  di, arg_sock
        mov  ds, [res_seg]
        mov  cx, R_LEN
        rep  movsb
        mov  al, [cfged]
        mov  [es:cfged], al
        pop  ds
        mov  byte [hndoff], 1
        cmp  byte [o_off], 0
        jne  .offmsg
        mov  si, m_via
        call prstr
        jmp  cs_report
.offmsg:
        mov  si, m_offok
        call prstr
        mov  ax, 4C00h
        int  21h

; ---- find_resident: MCB walk for a process block carrying our sig -------
;      CF clear + res_seg/res_off set when found; res_badver notes a
;      version-mismatched SCP55GO copy.
find_resident:
        mov  ah, 52h
        int  21h                      ; ES:BX -> DOS List of Lists
        mov  ax, [es:bx-2]            ; first MCB segment
        mov  cx, 300                  ; chain-walk guard
.w:
        mov  es, ax
        mov  dl, [es:0]               ; 'M' = more, 'Z' = last
        cmp  dl, 'M'
        je   .chk
        cmp  dl, 'Z'
        jne  .no                      ; broken chain - stop
.chk:
        mov  si, [es:1]               ; owner PSP
        mov  di, ax
        inc  di                       ; block data = MCB + 1
        cmp  si, di                   ; a program block owns itself
        jne  .next
        mov  bx, cs
        cmp  di, bx                   ; skip our own image
        je   .next
        push es
        mov  es, di
        cmp  word [es:103h], 'M8'
        jne  .pn
        cmp  word [es:105h], 'GO'
        jne  .pn
        cmp  word [es:107h], SGVER    ; exact version match required
        jne  .bv
        mov  ax, [es:109h]
        pop  es
        mov  [res_seg], di
        mov  [res_off], ax
        clc
        ret
.bv:
        mov  byte [res_badver], 1     ; note it, keep walking
.pn:
        pop  es
.next:
        cmp  dl, 'Z'
        je   .no
        mov  si, [es:3]               ; block size in paragraphs
        add  ax, si
        inc  ax                       ; next MCB
        dec  cx                       ; chain-walk guard
        jz   .no
        jmp  .w
.no:
        stc
        ret

; ---- cs_repsock: one-line verdict for the socket just probed ------------
cs_repsock:
        mov  al, [r_stage]
        cmp  al, 0
        je   .r                       ; success: main summary reports it
        cmp  al, 5
        je   .r                       ; codec verdict handled by main summary
        cmp  al, 7
        jne  .n6
        mov  si, m_s_tup              ; empty socket / unreadable CIS
        call prstr
        mov  ax, [r_code]
        call prhex4
        jmp  .nl
.n6:
        cmp  al, 6
        jne  .n2
        mov  si, m_s_wrong
        call prstr
        mov  ax, [manfid]
        call prhex4
        mov  al, '/'
        call prchar
        mov  ax, [manfid+2]
        call prhex4
        jmp  .nl
.n2:
        cmp  al, 2
        jne  .n4
        mov  si, m_s_io
        call prstr
        mov  ax, [r_code]
        call prhex4
        jmp  .nl
.n4:
        mov  si, m_s_cfg
        call prstr
        mov  ax, [r_code]
        call prhex4
.nl:
        call crlf
.r:
        ret

; ---- drain_queue: act on events queued during the registration window ---
drain_queue:
        push es
        cli
        mov  cl, [qcnt]
        mov  byte [qcnt], 0
        push cs                       ; snapshot the queue with events masked
        pop  es                       ; so a mid-drain callback can't clobber
        push cx                       ; entries we are still reading
        mov  si, queue
        mov  di, qcopy
        mov  cx, 8
        rep  movsw
        pop  cx
        sti
        pop  es
        xor  ch, ch
        jcxz .r
        mov  si, qcopy
.e:
        lodsw                         ; AL = event, AH = socket
        cmp  al, EV_INSERT
        jne  .skip
        cmp  byte [cfged], 0
        jne  .skip
        push cx
        push si
        mov  al, ah
        xor  ah, ah
        mov  [arg_sock], ax
        call cs_configure
        pop  si
        pop  cx
.skip:
        loop .e
.r:
        ret

;==========================================================================
;  OB MODE - HP OmniBook 300/425/430 Socket Services direct.
;  Quirks probed on the 425 (es1688go doc/RECON-425.md): Vcc/Vpp power-
;  table indices are 1-BASED; SetWindow DH bit0 is INVERTED from RBIL
;  (memory windows want it clear = 0Eh, I/O windows set = 05h); socket
;  status RDY/WP bits float - a socket is "in use by the OS" iff an
;  enabled COMMON-memory window maps it; SetSocket to the I/O interface
;  REFUSES DI=0 (BAD_IRQ) so steering is always on; I/O window bases are
;  10-BIT (>3FFh refused) - fine here, all four card bases are <=3F0h.
;  Only sockets 1-2 are probed: 3/4 hold the machine's permanently
;  attached storage cards (not user-changeable), so a sound card can
;  never be there - and its C: drive is nothing to gamble with.
;==========================================================================
FW_NOCARD  equ 0
FW_DRIVE   equ 1
FW_UNKNOWN equ 2

ob_mode:
        mov  word [o_wseg], OB_ATTRSEG ; CIS/COR window fixed here (probed)
        cmp  byte [o_off], 0
        je   .noff
        mov  si, m_obnooff
        jmp  errout
.noff:
        ; ---- Socket Services present? ------------------------------------
        call chk_ss
        jnc  .ssok
        mov  si, m_noss
        jmp  errout
.ssok:
        ; ---- find + bring up a card --------------------------------------
        mov  al, [o_sock]             ; user slots ONLY: sockets 3/4 hold
        cmp  al, 0FFh                 ; the permanently attached storage
        je   .scan                    ; cards - never probe or claim them
        cmp  al, 1
        jb   .badsock
        cmp  al, 2
        ja   .badsock
        mov  [sock], al
        call ob_try
        jc   .failx
        jmp  ob_got
.badsock:
        mov  si, m_obbadsock
        jmp  errout
.failx: jmp  ob_failx
.scan:
        mov  byte [sock], 1
.next:
        call ob_try
        jnc  ob_got
        inc  byte [sock]
        cmp  byte [sock], 2
        jbe  .next
        jmp  ob_fails

; ---------- card accepted: window 0 still mapped on attr space -----------
ob_got:
        mov  bx, [cfgbase]
        cmp  bx, 3FFEh
        jbe  .corok0
        mov  si, m_badcis
        call prstr
        call ob_restw0
        int  20h
.corok0:
        mov  al, [coridx]             ; plain index 0 (this COR pins nothing)
        mov  cx, [o_wseg]
        mov  es, cx
        mov  [es:bx], al              ; write the COR
        mov  cx, 1
        call ticks
        mov  ah, [es:bx]
        mov  [r_corrb], ah            ; judge the index bits only
        and  ah, 3Fh
        mov  al, [coridx]
        and  al, 3Fh
        mov  byte [r_corvf], 1
        cmp  ah, al
        je   .corok
        mov  byte [r_corvf], 2
.corok:
        mov  byte [es:bx+2], 02h      ; CCSR (attr +2): the Audio bit
        mov  cx, 1
        call ticks
        mov  al, [es:bx+2]
        mov  [r_ccsr], al
        call ob_restw0

        ; ---- socket to I/O interface + IRQ steering ----------------------
        ; The SS REFUSES DI=0 on the I/O interface (BAD_IRQ, AH=06 - seen
        ; live on the 425): steering must always be enabled here.  With no
        ; /I we steer IRQ 10 (from the card's valid set) but report none -
        ; MPU-401 output never raises IREQ unless the software asks.
        mov  bh, 0C0h
        mov  bl, [sock]
        mov  cx, 0111h                ; Vcc idx 1 (5V), Vpp 1/1 - 1-BASED
        xor  dx, dx
        mov  si, 0002h                ; I/O interface
        mov  al, [o_irq]
        or   al, al
        jnz  .steer
        mov  al, 10
.steer:
        xor  ah, ah
        mov  di, ax
        or   di, 8000h                ; IRQ steering on, level
        mov  ax, 8E00h
        call ob_ssvc
        jc   ob_esserr

        ; ---- I/O window (politely) + chip --------------------------------
        call ob_map_all               ; exits internally on failure
        call tdk_verify

        mov  al, [o_irq]
        or   al, al
        jnz  .ri
        mov  al, 0FFh
.ri:    mov  [rep_irq], al
        mov  al, [sock]
        mov  [rep_sock], al
        call report
        int  20h

; ---------- failure reporting --------------------------------------------
ob_failx:                             ; /S given; [failwhy] says what
        cmp  byte [failwhy], FW_UNKNOWN
        je   .unk
        cmp  byte [failwhy], FW_DRIVE
        je   .drv
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_nocards
        call prstr
        int  20h
.drv:
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_indrive
        call prstr
        int  20h
.unk:
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_unrec
        call prstr
        call pr_lastid
        mov  si, m_addman
        call prstr
        int  20h

ob_fails:
        cmp  byte [sawunk], 0
        jne  .seen
        mov  si, m_noknown
        call prstr
        int  20h
.seen:
        mov  si, m_nomatch
        call prstr
        call pr_lastid
        mov  si, m_addman2
        call prstr
        int  20h

pr_lastid:                            ; 'MANFID xxxx/xxxx "vers".'
        mov  si, m_manf2
        call prstr
        mov  ax, [manfid]
        call prhex4
        mov  al, '/'
        call prchar
        mov  ax, [manfid+2]
        call prhex4
        mov  al, ' '
        call prchar
        mov  al, '"'
        call prchar
        call pr_vers
        mov  al, '"'
        call prchar
        mov  al, '.'
        call prchar
        call crlf
        ret

; ---- ob_try: probe socket [sock].  CF=0 accepted (window 0 left mapped,
;      identity set).  CF=1 rejected, [failwhy] = why. --------------------
ob_try:
        mov  byte [failwhy], FW_NOCARD
        mov  bl, [sock]
        call ob_getstat
        test al, 80h                  ; card detect
        jz   .no
        call ob_chkdrive
        jnc  .safe
        mov  byte [failwhy], FW_DRIVE
.no:
        stc
        ret
.safe:
        mov  ax, 8E00h                ; power 5V, memory IF for CIS phase
        mov  bh, 0C0h
        mov  bl, [sock]
        mov  cx, 0111h
        xor  dx, dx
        mov  si, 0001h
        xor  di, di
        call ob_ssvc
        jc   .no
        mov  cx, 4                    ; ~220ms settle
        call ticks
        mov  ax, 9000h                ; ResetCard
        xor  bh, bh
        mov  bl, [sock]
        call ob_ssvc
        jc   .no
        mov  bp, 40
.rdy:
        mov  cx, 1
        call ticks
        mov  bl, [sock]
        call ob_getstat
        test al, 40h                  ; READY
        jnz  .rdyok
        dec  bp
        jnz  .rdy
        jmp  .no
.rdyok:
        mov  ax, 8800h                ; save window 0 state
        xor  bx, bx
        call ob_ssvc
        jc   .no
        mov  [w0_bl], bl
        mov  [w0_cx], cx
        mov  [w0_dx], dx
        mov  [w0_si], si
        mov  [w0_di], di
        mov  ax, 8900h                ; map window 0 on attr space @ D0000
        xor  bh, bh
        mov  bl, [sock]
        mov  cx, 0004h
        mov  dh, OB_WIN_ATTR
        mov  dl, OB_SPEED
        mov  si, 00D0h
        xor  di, di
        call ob_ssvc
        jc   .no
        call cis_settle               ; data-gate: READY is not enough
        call cis_even                 ; pull the even bytes
        call cis_parse                ; CF ignored: defaults hold
        call identify
        jnc  .ok
        mov  byte [sawunk], 1
        mov  byte [failwhy], FW_UNKNOWN
        cmp  byte [o_force], 0
        jne  .forced
        call ob_restw0
        stc
        ret
.forced:
        call force_ident
.ok:
        call vers_model               ; model + window 0 default base
        clc
        ret

; ---- ob_chkdrive: CF=1 if an enabled common-mem window maps [sock] ------
ob_chkdrive:
        mov  byte [gw_n], 1
.w:
        mov  ax, 8800h
        mov  bh, [gw_n]
        call ob_ssvc
        jc   .nx
        cmp  bl, [sock]
        jne  .nx
        test dh, 04h                  ; enabled
        jz   .nx
        test dh, 02h                  ; attribute space? then not a drive map
        jnz  .nx
        stc
        ret
.nx:
        inc  byte [gw_n]
        cmp  byte [gw_n], 3
        jbe  .w
        clc
        ret

ob_ssvc:
        push ds
        push es
        push bp
        int  1Ah
        pop  bp
        pop  es
        pop  ds
        ret

ob_getstat:                           ; BL = socket -> AL = card status
        push bx
        mov  ax, 8F00h
        xor  bh, bh
        call ob_ssvc
        pop  bx
        jc   ob_esserr
        mov  al, dl
        ret

ob_restw0:
        mov  ax, 8900h
        xor  bh, bh
        mov  bl, [w0_bl]
        mov  cx, [w0_cx]
        mov  dx, [w0_dx]
        mov  si, [w0_si]
        mov  di, [w0_di]
        call ob_ssvc
        ret

ob_esserr:
        mov  bl, ah
        mov  si, m_sserr
        call prstr
        mov  al, bl
        call prhex2
        call crlf
        int  20h

; ---- polite window allocation --------------------------------------------
; ob_classify: survey I/O windows 4-7 into q* tables.
; qstat: 0 = occupied (another socket, card present), 1 = free, 2 = claimed.
ob_classify:
        mov  byte [cw_i], 0
.next:
        mov  al, [cw_i]
        xor  ah, ah
        mov  di, ax
        mov  bh, al
        add  bh, 4
        mov  ax, 8800h                ; GetWindow
        call ob_ssvc
        jc   .busy                    ; unreadable: occupied, zero range
        push di
        shl  di, 1
        mov  [qbase+di], si
        mov  [qsize+di], cx
        pop  di
        test dh, 04h                  ; disabled -> free
        jz   .free
        cmp  bl, [sock]               ; ours from a previous run -> reclaim
        je   .free
        push di
        call ob_getstat               ; BL socket -> AL card status
        pop  di
        test al, 80h
        jz   .free                    ; enabled but socket empty -> stale
        mov  byte [qstat+di], 0
        jmp  .adv
.busy:
        push di
        shl  di, 1
        mov  word [qsize+di], 0       ; empty range: never matches overlap
        pop  di
        mov  byte [qstat+di], 0
        jmp  .adv
.free:
        mov  byte [qstat+di], 1
.adv:
        inc  byte [cw_i]
        cmp  byte [cw_i], 4
        jb   .next
        ret

; ob_alloc: SI=base, CX=size -> map into any free window.
; CF=0 done (recorded for the summary). CF=1: AL=0 conflict with an
; occupied window's range, AL=1 out of free windows.
ob_alloc:
        mov  [aw_base], si
        mov  [aw_size], cx
        mov  byte [aw_i], 0
.chk:
        mov  al, [aw_i]
        xor  ah, ah
        mov  di, ax
        cmp  byte [qstat+di], 0
        jne  .cnext
        shl  di, 1
        mov  ax, [qbase+di]
        add  ax, [qsize+di]
        cmp  si, ax                   ; desired base >= their end?
        jae  .cnext
        mov  ax, si
        add  ax, cx
        cmp  ax, [qbase+di]           ; desired end <= their base?
        jbe  .cnext
        mov  al, 0                    ; overlap
        stc
        ret
.cnext:
        inc  byte [aw_i]
        cmp  byte [aw_i], 4
        jb   .chk
        mov  byte [aw_i], 0
.find:
        mov  al, [aw_i]
        xor  ah, ah
        mov  di, ax
        cmp  byte [qstat+di], 1
        je   .take
        inc  byte [aw_i]
        cmp  byte [aw_i], 4
        jb   .find
        mov  al, 1                    ; no free window
        stc
        ret
.take:
        mov  bh, [aw_i]
        add  bh, 4
        mov  ax, 8900h
        mov  bl, [sock]
        mov  si, [aw_base]
        mov  cx, [aw_size]
        mov  dh, [aw_attr]            ; probed: I/O windows want DH bit0=1
        mov  dl, OB_SPEED
        xor  di, di
        call ob_ssvc
        jc   ob_esserr
        mov  al, [aw_i]
        xor  ah, ah
        mov  di, ax
        mov  byte [qstat+di], 2
        mov  al, [wcount]
        xor  ah, ah
        mov  bx, ax
        mov  al, [aw_i]
        add  al, 4
        mov  [wnum+bx], al
        shl  bx, 1
        mov  ax, [aw_base]
        mov  [wbase+bx], ax
        mov  ax, [aw_size]
        mov  [wsize+bx], ax
        inc  byte [wcount]
        clc
        ret

; ob_map_all: BOTH 16-byte windows are required - no degrade.  Window 0
; carries the 16-bit characteristic (SS window flags bit3) - UNVERIFIED on
; the OB SS for this card; if the EMU block reads torn on an OmniBook,
; start here (and see ob-io-window-dl-bug for the DL=01h WAIT question on
; 8-bit I/O windows).
ob_map_all:
        mov  byte [wcount], 0
        call ob_classify
        mov  byte [aw_attr], OB_WIN_IO | 08h   ; window 0: I/O, 16-bit
        mov  si, [o_io1]
        mov  cx, 16
        call ob_alloc
        jc   .fatal
        mov  byte [aw_attr], OB_WIN_IO         ; window 1: I/O, 8-bit
        mov  si, [o_io2]
        mov  cx, 16
        call ob_alloc
        jc   .fatal
        ret
.fatal:
        jmp  ob_iofatal

ob_iofatal:
        push ax
        mov  si, m_iofail
        call prstr
        pop  ax
        or   al, al
        jnz  .nowin
        mov  si, m_conflict
        call prstr
        int  20h
.nowin:
        mov  si, m_outwin
        call prstr
        int  20h

;==========================================================================
;  PCIC MODE - Intel 82365-class point enabler (port of GSGO.C 1.2).
;
;  Two I/O windows: [o_io1] = the EMU block (16-BIT), [o_io2] = the DIN
;  UART (8-bit).  The CIS probe picks a host segment no other card has a
;  memory window on, so
;  scanning never collides with a card another enabler has running - and
;  the probe restores an already-running card instead of powering it off.
;==========================================================================
pc_mode:
        ; ---- /OFF: power the socket down and exit ------------------------
        cmp  byte [o_off], 0
        je   .noff
        mov  al, [o_sock]
        cmp  al, 0FFh
        jne  .osk
        xor  al, al
.osk:
        mov  [sock], al
        call pc_select
        call pc_present
        jnc  .offok
        jmp  pc_nochip
.offok:
        mov  al, 06h
        xor  ah, ah
        call pc_wr
        mov  al, 03h
        xor  ah, ah
        call pc_wr
        mov  al, 02h
        xor  ah, ah
        call pc_wr
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_offdone
        call prstr
        int  20h
.noff:
        ; ---- unless /W pinned it, pick a probe segment no other card has
        ;      a memory window on ----------------------------------------
        cmp  byte [o_wgiven], 0
        jne  .wok
        call pc_findfree
.wok:

        ; ---- identify the card; only the match is left powered + mapped --
        mov  al, [o_sock]
        cmp  al, 0FFh
        je   .scan
        mov  [sock], al
        call pc_select
        call pc_present
        jnc  .p1
        jmp  pc_nochip
.p1:
        call pc_probe
        jnc  pc_found
        cmp  word [manfid], 0
        je   .norc
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_unrec
        call prstr
        call pr_lastid
        mov  si, m_addman3
        call prstr
        int  20h
.norc:
        mov  si, m_socket
        call prstr
        mov  al, [sock]
        call prdec99
        ; a CIS was read but held no MANFID tuple (dead/exotic CIS) - say
        ; so instead of the misleading "no card present"
        mov  si, m_nocards
        cmp  byte [sawunk], 0
        je   .norc2
        mov  si, m_nomanf
.norc2:
        call prstr
        int  20h
.scan:
        mov  byte [any_chip], 0
        mov  byte [sock], 0
.sc1:
        mov  al, [sock]
        call pc_select
        call pc_present
        jc   .sc2
        mov  byte [any_chip], 1
        call pc_probe
        jnc  pc_found
.sc2:
        inc  byte [sock]
        cmp  byte [sock], 7
        jbe  .sc1
        cmp  byte [any_chip], 0
        jne  .nomatch
        mov  si, m_nopcic
        jmp  errout
.nomatch:
        cmp  word [manfid], 0
        je   .noknown
        mov  si, m_nomatch
        call prstr
        call pr_lastid
        mov  si, m_addman2
        call prstr
        int  20h
.noknown:
        mov  si, m_noknown2
        call prstr
        int  20h

; ---------- card accepted: attr window still mapped ----------------------
pc_found:
        ; ---- COR via the attribute window pc_probe left mapped.  Base
        ;      from the card's CIS when it was readable; 400h otherwise.
        mov  bx, TDK_COR
        cmp  byte [matched], 0
        je   .cw
        mov  bx, [cfgbase]
.cw:
        cmp  bx, 3FFEh
        jbe  .cw2
        mov  si, m_badcis
        jmp  errout
.cw2:
        mov  cx, [o_wseg]
        mov  es, cx
        mov  al, [coridx]             ; plain index (as the verified C did -
        mov  [es:bx], al              ; this card pins nothing)
        mov  ax, 5
        call ms_delay
        mov  ah, [es:bx]
        mov  [r_corrb], ah
        and  ah, 3Fh                  ; judge the index bits only
        mov  al, [coridx]
        and  al, 3Fh
        mov  byte [r_corvf], 1
        cmp  ah, al
        je   .corv
        mov  byte [r_corvf], 2
.corv:
        mov  byte [es:bx+2], 02h      ; CCSR (attr +2): the Audio bit
        mov  ax, 2
        call ms_delay
        mov  al, [es:bx+2]
        mov  [r_ccsr], al

        ; ---- two I/O windows: EMU block (16-BIT) + DIN UART (8-bit).
        ;      Programmed while DISABLED, enabled in one final write -
        ;      never rewrite an enabled window's bounds in place (a
        ;      half-written window once spanned the link and killed the
        ;      box: pc110-io-window-avoid-com1). --------------------------
        mov  al, 06h
        call pc_rd
        and  al, 3Fh                  ; both I/O window enables off first
        mov  ah, al
        mov  al, 06h
        call pc_wr
        mov  ax, [o_io1]
        mov  [p_w0s], ax
        add  ax, 0Fh
        mov  [p_w0e], ax
        mov  al, 08h
        mov  ah, [p_w0s]
        call pc_wr
        mov  al, 09h
        mov  ah, [p_w0s+1]
        call pc_wr
        mov  al, 0Ah
        mov  ah, [p_w0e]
        call pc_wr
        mov  al, 0Bh
        mov  ah, [p_w0e+1]
        call pc_wr
        mov  ax, [o_io2]
        mov  [p_w1s], ax
        add  ax, 0Fh
        mov  [p_w1e], ax
        mov  al, 0Ch
        mov  ah, [p_w1s]
        call pc_wr
        mov  al, 0Dh
        mov  ah, [p_w1s+1]
        call pc_wr
        mov  al, 0Eh
        mov  ah, [p_w1e]
        call pc_wr
        mov  al, 0Fh
        mov  ah, [p_w1e+1]
        call pc_wr
        mov  al, 07h                  ; window 0 16-bit, window 1 8-bit
        mov  ah, 01h                  ; (bench-proven: TDKEMU3 idx07=01h)
        call pc_wr
        mov  ah, 60h                  ; I/O-card mode, reset off
        mov  al, [o_irq]
        or   al, al
        jz   .nirq
        or   ah, 10h                  ; + IRQ steer
        or   ah, al
.nirq:
        mov  al, 03h
        call pc_wr
        mov  al, 06h                  ; enable I/O win 0 + 1; mem win off
        mov  ah, 0C0h
        call pc_wr

        ; ---- post-enable verify -----------------------------------------
        call tdk_verify

        ; ---- window list for the summary ---------------------------------
        mov  byte [wcount], 2
        mov  byte [wnum], 0
        mov  byte [wnum+1], 1
        mov  ax, [p_w0s]
        mov  [wbase], ax
        mov  word [wsize], 16
        mov  ax, [p_w1s]
        mov  [wbase+2], ax
        mov  word [wsize+2], 16
        mov  al, [sock]
        mov  [rep_sock], al
        mov  al, [o_irq]
        or   al, al
        jnz  .ri
        mov  al, 0FFh
.ri:
        mov  [rep_irq], al
        call report
        int  20h

pc_nochip:
        mov  si, m_nochip1
        call prstr
        mov  al, [sock]
        call prdec99
        mov  si, m_nochip2
        call prstr
        mov  ax, [pc_idx]
        call prhex3
        mov  al, ')'
        call prchar
        call crlf
        int  20h

; ---- PCIC register access ------------------------------------------------
pc_select:                            ; AL = socket 0..7
        push ax
        push bx
        mov  bl, al
        xor  bh, bh
        and  bl, 0FEh                 ; chip base = 3E0h + (s & ~1)
        add  bx, PCIC_BASE
        mov  [pc_idx], bx
        mov  byte [pc_off], 0         ; bank B of a chip sits at +40h
        test al, 1
        jz   .r
        mov  byte [pc_off], 40h
.r:
        pop  bx
        pop  ax
        ret

pc_rd:                                ; AL = reg -> AL = value
        push dx
        add  al, [pc_off]
        mov  dx, [pc_idx]
        out  dx, al
        inc  dx
        in   al, dx
        pop  dx
        ret

pc_wr:                                ; AL = reg, AH = value
        push ax
        push dx
        add  al, [pc_off]
        mov  dx, [pc_idx]
        out  dx, al
        inc  dx
        mov  al, ah
        out  dx, al
        pop  dx
        pop  ax
        ret

pc_present:                           ; CF clear = 82365-class ID answers
        push ax
        mov  al, 0                    ; identification/revision register
        call pc_rd
        and  al, 0C0h
        cmp  al, 80h
        je   .yes
        pop  ax
        stc
        ret
.yes:
        pop  ax
        clc
        ret

; ---- pc_probe: power socket [sock] (already selected), map its attribute
;      window at [o_wseg], read the CIS, identify.  CF=0: ours (left
;      powered + mapped).  CF=1: put back the way we found it - a card
;      ALREADY running there is restored exactly, not powered off. --------
pc_probe:
        call pc_present
        jnc  .cd
        jmp  .nope
.cd:
        mov  al, 01h                  ; both card-detect bits?
        call pc_rd
        and  al, 0Ch
        cmp  al, 0Ch
        je   .have
        jmp  .nope
.have:
        mov  al, 01h
        call pc_rd
        and  al, 40h                  ; power already up = a running card
        mov  [p_wason], al
        mov  al, 03h                  ; save the regs we borrow
        call pc_rd
        mov  [p_s03], al
        mov  al, 06h
        call pc_rd
        mov  [p_s06], al
        mov  al, 10h
        call pc_rd
        mov  [p_s10], al
        mov  al, 11h
        call pc_rd
        mov  [p_s11], al
        mov  al, 12h
        call pc_rd
        mov  [p_s12], al
        mov  al, 13h
        call pc_rd
        mov  [p_s13], al
        mov  al, 14h
        call pc_rd
        mov  [p_s14], al
        mov  al, 15h
        call pc_rd
        mov  [p_s15], al
        cmp  byte [p_wason], 0
        jne  .waspow
        mov  al, 02h                  ; power up
        mov  ah, 0B0h
        call pc_wr
        ; Poll power-good rather than a fixed wait: the 82365SL asserts it
        ; near instantly, but CardBus-era bridges ramp Vcc through a soft
        ; power switch and can take a few hundred ms.
        mov  bx, 50
.pw:
        mov  ax, 10
        call ms_delay
        mov  al, 01h
        call pc_rd
        test al, 40h
        jnz  .pgood
        dec  bx
        jnz  .pw
        mov  al, 02h                  ; power never came good
        xor  ah, ah
        call pc_wr
        jmp  .nope
.pgood:
        mov  ax, 100                  ; card settle after power-good
        call ms_delay
.waspow:
        mov  al, 03h                  ; reset off, memory mode
        mov  ah, 40h
        call pc_wr
        mov  ax, 100
        call ms_delay
        ; never read card memory before the card raises READY (reg 01h
        ; bit5); proceed on a timeout - some sockets float the bit
        mov  bx, 300
.rdy:
        mov  al, 01h
        call pc_rd
        test al, 20h
        jnz  .rdyok
        mov  ax, 5
        call ms_delay
        dec  bx
        jnz  .rdy
.rdyok:
        ; ---- memory window 0 -> [o_wseg], REG active ---------------------
        mov  ax, [o_wseg]
        mov  cl, 8
        shr  ax, cl                   ; start page (A19..A12)
        mov  [pw_pg], ax
        mov  al, 10h
        mov  ah, [pw_pg]
        call pc_wr
        mov  al, 11h
        mov  ah, [pw_pg+1]
        and  ah, 3Fh
        call pc_wr
        mov  ax, [pw_pg]
        add  ax, 3                    ; 16 KB window
        mov  [pw_pg2], ax
        mov  al, 12h
        mov  ah, [pw_pg2]
        call pc_wr
        mov  al, 13h
        mov  ah, [pw_pg2+1]
        and  ah, 3Fh
        call pc_wr
        mov  ax, [pw_pg]
        neg  ax                       ; offset = (0 - start) & 3FFF, REG bit
        and  ax, 3FFFh
        or   ax, 4000h
        mov  [pw_pg2], ax
        mov  al, 14h
        mov  ah, [pw_pg2]
        call pc_wr
        mov  al, 15h
        mov  ah, [pw_pg2+1]
        call pc_wr
        mov  al, 06h
        call pc_rd
        or   al, 01h                  ; enable mem win0 (keep others)
        mov  ah, al
        mov  al, 06h
        call pc_wr
        mov  ax, 20
        call ms_delay
        call cis_settle               ; data-gate: READY is not enough
        call cis_even
        call cis_parse                ; CF ignored: cfgbase stays 400h
        call identify
        jnc  .ours
        mov  byte [sawunk], 1
        cmp  byte [o_force], 0
        je   .undo
        call force_ident              ; /FORCE (validated: only with /S)
        jmp  .ours
.undo:
        cmp  byte [p_wason], 0
        je   .pdown
        mov  al, 10h                  ; already-running card: restore exactly
        mov  ah, [p_s10]
        call pc_wr
        mov  al, 11h
        mov  ah, [p_s11]
        call pc_wr
        mov  al, 12h
        mov  ah, [p_s12]
        call pc_wr
        mov  al, 13h
        mov  ah, [p_s13]
        call pc_wr
        mov  al, 14h
        mov  ah, [p_s14]
        call pc_wr
        mov  al, 15h
        mov  ah, [p_s15]
        call pc_wr
        mov  al, 06h
        mov  ah, [p_s06]
        call pc_wr
        mov  al, 03h
        mov  ah, [p_s03]
        call pc_wr
        jmp  .nope
.pdown:
        mov  al, 06h                  ; we powered it just to peek
        xor  ah, ah
        call pc_wr
        mov  al, 03h
        xor  ah, ah
        call pc_wr
        mov  al, 02h
        xor  ah, ah
        call pc_wr
.nope:
        stc
        ret
.ours:
        call vers_model               ; model + window 0 default base
        clc
        ret

; ---- pc_findfree: pick a probe segment no other card has an enabled
;      memory window over (prefers [o_wseg], then the candidate list) -----
pc_findfree:
        mov  ax, [o_wseg]
        mov  [mw_seg], ax
        call mw_overlap
        jc   .hunt
        ret
.hunt:
        mov  si, mw_cands
.c:
        lodsw
        or   ax, ax
        jz   .none
        mov  [mw_seg], ax
        push si
        call mw_overlap
        pop  si
        jnc  .found
        jmp  .c
.found:
        mov  si, m_wbusy1
        call prstr
        mov  ax, [o_wseg]
        call prhex4
        mov  si, m_wbusy2
        call prstr
        mov  ax, [mw_seg]
        call prhex4
        mov  al, '.'
        call prchar
        call crlf
        mov  ax, [mw_seg]
        mov  [o_wseg], ax
.none:
        ret

mw_cands: dw 0D000h, 0CC00h, 0D400h, 0D800h, 0DC00h
          dw 0E000h, 0E400h, 0E800h, 0C800h, 0

; ---- mw_overlap: does [mw_seg]..+16KB overlap an ENABLED memory window on
;      any present socket?  Read-only, so it disturbs nothing.  Memory
;      windows 0..4 = bits 0..4 of the Window-Enable reg (06h); each
;      window's start/stop host page lives in regs 10h+w*8 .. +3.
;      CF set = overlap. -------------------------------------------------
mw_overlap:
        push ax
        push cx
        mov  ax, [mw_seg]
        mov  cl, 8
        shr  ax, cl
        mov  [mw_cs], ax
        add  ax, 3
        mov  [mw_ce], ax
        mov  byte [mw_sk], 0
.sock:
        mov  al, [mw_sk]
        call pc_select
        call pc_present
        jc   .next                    ; unimplemented bank floats 0FFh
        mov  al, 06h
        call pc_rd
        mov  [mw_en], al
        mov  byte [mw_w], 0
.win:
        mov  cl, [mw_w]
        mov  al, 1
        shl  al, cl
        test [mw_en], al
        jz   .nw                      ; window w not enabled
        mov  al, [mw_w]               ; reg base = 10h + w*8
        mov  cl, 3
        shl  al, cl
        add  al, 10h
        mov  [mw_rb], al
        mov  al, [mw_rb]              ; ws = ((rd(b+1)&0F)<<8) | rd(b)
        inc  al
        call pc_rd
        and  al, 0Fh
        mov  [mw_ws+1], al
        mov  al, [mw_rb]
        call pc_rd
        mov  [mw_ws], al
        mov  al, [mw_rb]              ; we = ((rd(b+3)&0F)<<8) | rd(b+2)
        add  al, 3
        call pc_rd
        and  al, 0Fh
        mov  [mw_we+1], al
        mov  al, [mw_rb]
        add  al, 2
        call pc_rd
        mov  [mw_we], al
        mov  ax, [mw_cs]              ; overlap iff cs <= we && ws <= ce
        cmp  ax, [mw_we]
        ja   .nw
        mov  ax, [mw_ws]
        cmp  ax, [mw_ce]
        ja   .nw
        pop  cx                       ; ranges overlap
        pop  ax
        stc
        ret
.nw:
        inc  byte [mw_w]
        cmp  byte [mw_w], 5
        jb   .win
.next:
        inc  byte [mw_sk]
        cmp  byte [mw_sk], 8
        jb   .sock
        pop  cx
        pop  ax
        clc
        ret

;==========================================================================
;  SHARED CIS PLUMBING  (transient: PCIC + OB probe paths)
;==========================================================================
; ---- cis_settle: gate the CIS read on the DATA, not on delays or READY.
;      Attribute memory lags socket power by a HOST-specific time (PC110
;      ~30ms, ThinkPad 235 ~110ms measured), and the READY bit can assert
;      well BEFORE the CIS reads true (235: ~55ms) - a too-early read
;      returns an all-FF wall for ANY card, so a healthy card is reported
;      as unknown (or the socket as empty) and the enabler refuses to
;      configure it.  See ~/Projects/pcmcia-cis-ff-bug.md.  Until now this
;      enabler was safe only by ~200ms of accumulated fixed delay, with
;      nothing verifying that on a slower host or card - measured on the
;      235, an ES1688GO built the same way had only ~90-110ms to spare.
; Two-clause gate: un-settled reads are all-FF on some hosts (235 cold
; power) but UNSTABLE NON-FF GARBAGE on others (PC110 after a power
; cycle) - so require byte0 != FF AND dense bytes 0-3 identical across
; two reads 20ms apart, capped at ~5s.  Silent and free on a settled
; socket (exits on the first pass); prints one line only when the socket
; actually made us wait, which is worth knowing about that host.
cis_settle:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push es
        mov  word [settlen], 0
        mov  es, [o_wseg]
        mov  cx, SETTLEP
        call .snap                    ; DX:BX = dense bytes 0-3
        mov  ax, dx
        mov  di, bx                   ; previous snapshot in AX:DI
.w:
        push ax
        mov  ax, 20
        call ms_delay
        pop  ax
        push ax
        push di
        call .snap
        pop  di
        pop  ax
        cmp  bl, 0FFh                 ; clause 1: byte0 != FF
        je   .miss
        cmp  ax, dx                   ; clause 2: stable vs previous
        jne  .miss
        cmp  di, bx
        je   .ok
.miss:
        inc  word [settlen]
        mov  ax, dx                   ; roll the snapshot forward
        mov  di, bx
        loop .w
.ok:
        cmp  word [settlen], 0
        je   .quiet
        call .report
.quiet:
        pop  es
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        ret
.snap:                                ; dense 0..3 (even host offsets)
        mov  bl, [es:0]
        mov  bh, [es:2]
        mov  dl, [es:4]
        mov  dh, [es:6]
        ret
.report:
        mov  ax, [settlen]
        cmp  ax, SETTLEP
        jb   .slow
        mov  si, m_nosettle           ; never settled: the read is suspect
        call prstr
        ret
.slow:
        mov  si, m_settle
        call prstr
        cmp  ax, 100
        jb   .n
        mov  si, m_settlot            ; >=2s: past prdec99's range
        call prstr
        ret
.n:
        call prdec99
        mov  si, m_settle2
        call prstr
        ret

; cis_even: copy CISLEN even host bytes from [o_wseg]:0 into cisbuf.
; (Attribute memory only implements even card addresses.)
cis_even:
        push ax
        push cx
        push si
        push di
        push ds
        push es
        push cs
        pop  es
        mov  di, cisbuf
        mov  cx, CISLEN
        mov  ax, [o_wseg]
        mov  ds, ax
        xor  si, si
        cld
.c:
        lodsb
        stosb
        inc  si
        loop .c
        pop  es
        pop  ds
        pop  di
        pop  si
        pop  cx
        pop  ax
        ret

; cis_parse: walk the dense CIS in cisbuf.  Fills [manfid], [cfgbase]
; (default 400h), versbuf/[verslen].  CF set = no CISTPL_CONFIG found.
cis_parse:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        xor  bx, bx
        mov  byte [gotcfg], 0
        mov  word [verslen], 0
        mov  word [manfid], 0
        mov  word [manfid+2], 0
        mov  word [cfgbase], TDK_COR
        mov  byte [matched], 0
.loop:
        mov  al, [cisbuf+bx]
        cmp  al, 0FFh
        je   .end
        or   al, al
        jnz  .real
        inc  bx                       ; null tuple
        jmp  .chk
.real:
        mov  ah, [cisbuf+bx+1]        ; link
        cmp  ah, 0FFh
        je   .end
        cmp  al, 15h                  ; CISTPL_VERS_1
        jne  .n15
        call .vers
.n15:
        cmp  al, 20h                  ; CISTPL_MANFID
        jne  .n20
        mov  dl, [cisbuf+bx+2]
        mov  dh, [cisbuf+bx+3]
        mov  [manfid], dx
        mov  dl, [cisbuf+bx+4]
        mov  dh, [cisbuf+bx+5]
        mov  [manfid+2], dx
.n20:
        cmp  al, 1Ah                  ; CISTPL_CONFIG
        jne  .n1a
        push ax
        mov  al, [cisbuf+bx+2]        ; TPCC_SZ: bits1:0 = RADR size - 1
        and  al, 03h
        mov  dl, [cisbuf+bx+4]        ; RADR low
        xor  dh, dh
        or   al, al
        jz   .rd1
        mov  dh, [cisbuf+bx+5]        ; RADR high
.rd1:
        mov  [cfgbase], dx
        mov  byte [gotcfg], 1
        pop  ax
.n1a:
        xor  dx, dx
        mov  dl, ah
        add  bx, dx
        add  bx, 2
.chk:
        cmp  bx, CISLEN-2
        jb   .loop
.end:
        cmp  byte [gotcfg], 0
        je   .bad
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        clc
        ret
.bad:
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        stc
        ret
; local: copy the VERS_1 strings (skip major/minor) into versbuf
.vers:
        push ax
        push cx
        push si
        push di
        mov  cl, ah                   ; body length
        xor  ch, ch
        cmp  cx, 2
        jbe  .vdone
        sub  cx, 2
        cmp  cx, VBMAX
        jbe  .vlen
        mov  cx, VBMAX
.vlen:
        mov  [verslen], cx
        lea  si, [cisbuf+bx+4]        ; body+2 = first string
        mov  di, versbuf
        push es
        push cs
        pop  es
        cld
        rep  movsb
        pop  es
.vdone:
        pop  di
        pop  si
        pop  cx
        pop  ax
        ret

; pr_vers: print the captured VERS_1 strings, space-separated (max 4)
pr_vers:
        push ax
        push bx
        push cx
        mov  bx, 0
        mov  cx, 4
        cmp  word [verslen], 0
        jne  .l
        mov  si, m_novers
        call prstr
        jmp  .done
.l:
        cmp  bx, [verslen]
        jae  .done
        mov  al, [versbuf+bx]
        inc  bx
        cmp  al, 0FFh
        je   .done
        or   al, al
        jnz  .ch
        mov  al, ' '
        call prchar
        loop .l
        jmp  .done
.ch:
        call prchar
        jmp  .l
.done:
        pop  cx
        pop  bx
        pop  ax
        ret

;==========================================================================
;  SHARED SUMMARY REPORT
;==========================================================================
report:
; line 1: <name>[ (FORCED)]: socket n[ (auto)]
        mov  si, [k_name]
        call prstr
        cmp  byte [matched], 0
        jne  .n1
        mov  si, m_forcedtag
        call prstr
.n1:
        mov  si, m_sock1
        call prstr
        mov  al, [rep_sock]
        call prdec99
        cmp  byte [o_sock], 0FFh
        jne  .n2
        mov  si, m_auto
        call prstr
.n2:
        call crlf
; line 2: CIS + MANFID
        mov  si, m_cis
        call prstr
        call pr_vers
        mov  si, m_manf1
        call prstr
        mov  ax, [manfid]
        call prhex4
        mov  al, '/'
        call prchar
        mov  ax, [manfid+2]
        call prhex4
        mov  al, ')'
        call prchar
        call crlf
; line 3: "   COR @ 400h index 01[ (verified | MISMATCH: readback xx)]"
        mov  si, m_corat
        call prstr
        mov  ax, [cfgbase]
        call prhexadr
        mov  si, m_index
        call prstr
        mov  al, [coridx]
        call prhex2
        cmp  byte [r_corvf], 0
        je   .cornl                   ; CS mode: no direct readback
        cmp  byte [r_corvf], 1
        jne  .cormis
        mov  si, m_corok
        call prstr
        jmp  .cornl
.cormis:
        mov  si, m_cormis
        call prstr
        mov  al, [r_corrb]
        call prhex2
        mov  al, ')'
        call prchar
.cornl:
        call crlf
; line 4: "   EMU 240h (decoding)  DIN UART 320h (idle 16550)  CCSR 02  no IRQ"
        mov  si, m_emu
        call prstr
        mov  ax, [o_io1]
        call prhex3
        mov  si, m_emuok
        cmp  byte [r_w0], 0
        jne  .em
        mov  si, m_emuno
.em:
        call prstr
        mov  si, m_uart
        call prstr
        mov  ax, [o_io2]
        call prhex3
        mov  si, m_uartok
        cmp  byte [r_uart], 0
        jne  .ua
        mov  si, m_uartno
.ua:
        call prstr
        cmp  byte [r_ccsr], 0
        je   .nocc
        mov  si, m_ccsr
        call prstr
        mov  al, [r_ccsr]
        call prhex2
.nocc:
        cmp  byte [rep_irq], 0FFh
        je   .noirq
        mov  si, m_irq
        call prstr
        mov  al, [rep_irq]
        call prdec99
        call crlf
        jmp  .irqd
.noirq:
        mov  si, m_noirq
        call prstr
        call crlf
.irqd:
; line 5: "   I/O windows: win0 330-33F"
        cmp  byte [wcount], 0
        je   .w5done
        mov  si, m_wins
        call prstr
        xor  bx, bx
.w5:
        cmp  bl, [wcount]
        jae  .w5nl
        or   bx, bx
        jz   .w5f
        mov  si, m_comma
        call prstr
.w5f:
        mov  si, m_win
        call prstr
        mov  al, [wnum+bx]
        add  al, '0'
        call prchar
        mov  al, ' '
        call prchar
        push bx
        shl  bx, 1
        mov  ax, [wbase+bx]
        push ax
        call prhex3
        mov  al, '-'
        call prchar
        pop  ax
        add  ax, [wsize+bx]
        dec  ax
        call prhex3
        pop  bx
        inc  bx
        jmp  .w5
.w5nl:
        call crlf
.w5done:
        ret

;==========================================================================
;  COMMAND LINE
;  '/X', '/Xtail', value after ':' or '='; unknown switches reported and
;  ignored.
;==========================================================================
parse_args:
.loop:
        lodsb
        cmp  al, 0Dh
        je   .done
        cmp  al, '/'
        je   .sw
        cmp  al, '-'
        je   .sw
        jmp  .loop
.done:
        ret
.sw:
        lodsb
        cmp  al, '?'                  ; /? help (before the case fold -
        je   .help                    ;  0DFh AND would mangle it)
        and  al, 0DFh
        cmp  al, 'P'
        je   .p
        cmp  al, 'C'
        je   .c
        cmp  al, 'O'
        je   .o
        cmp  al, 'S'
        je   .s
        cmp  al, 'I'
        je   .i
        cmp  al, 'F'
        je   .f
        cmp  al, 'W'
        je   .w
        jmp  .unknown
.p:                                   ; /PCIC
        call swtail
        mov  byte [o_mode], MODE_PCIC
        jmp  .loop
.c:                                   ; /CS
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'S'
        jne  .unk2
        call swtail
        mov  byte [o_mode], MODE_CS
        jmp  .loop
.o:                                   ; /OB or /OFF
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'B'
        je   .ob
        cmp  al, 'F'
        je   .off
        jmp  .unknown
.ob:
        call swtail
        mov  byte [o_mode], MODE_OB
        jmp  .loop
.off:
        call swtail
        mov  byte [o_off], 1
        jmp  .loop
.s:                                   ; /S=dec
        call eat_eq
        mov  al, [si]
        cmp  al, '0'
        jb   .loop
        cmp  al, '9'
        ja   .loop
        call decnum
        cmp  dx, 7
        ja   .loop
        mov  [o_sock], dl
        jmp  .loop
.i:                                   ; /I=dec, /IO1=hex, /IO2=hex
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'O'
        je   .io
        call eat_eq
        mov  al, [si]
        cmp  al, '0'
        jb   .loop
        cmp  al, '9'
        ja   .loop
        call decnum
        cmp  dx, 15
        ja   .loop
        mov  al, dl
        mov  [o_irq], al
        jmp  .loop
.io:
        mov  al, [si+1]               ; the digit: /IO1 or /IO2
        cmp  al, '1'
        je   .io1
        cmp  al, '2'
        je   .io2
        jmp  .unknown
.io1:
        inc  si                       ; past "O1"
        inc  si
        call eat_eq
        call hexnum
        test dx, dx
        jz   .loop
        mov  [o_io1], dx
        mov  byte [o_io1gv], 1
        jmp  .loop
.io2:
        inc  si                       ; past "O2"
        inc  si
        call eat_eq
        call hexnum
        test dx, dx
        jz   .loop
        mov  [o_io2], dx
        jmp  .loop
.f:                                   ; /F[ORCE]
        call swtail
        mov  byte [o_force], 1
        jmp  .loop
.w:                                   ; /W=hex
        call eat_eq
        call hexnum
        test dx, dx
        jz   .loop
        mov  [o_wseg], dx
        mov  byte [o_wgiven], 1
        jmp  .loop
.help:                                ; /? - print the pane, touch nothing
        mov  si, m_help
        call prstr
        int  20h
.unk2:
        dec  si
.unknown:
        dec  si
        push si
        mov  si, m_ignsw
        call prstr
        pop  si
.uecho:
        lodsb                         ; echo the offending token
        cmp  al, 0Dh
        je   .uend
        cmp  al, ' '
        je   .unl
        call prchar
        jmp  .uecho
.unl:
        call crlf
        jmp  .loop
.uend:
        call crlf
        dec  si
        jmp  .loop

swtail:                               ; skip switch tail up to space/'/'/CR
        lodsb
        cmp  al, 0Dh
        je   .stop
        cmp  al, ' '
        je   .stop
        cmp  al, '/'
        je   .stop
        jmp  swtail
.stop:
        dec  si
        ret

eat_eq:                               ; skip rest of switch name + '='/':'
.e1:
        lodsb
        cmp  al, '='
        je   .r
        cmp  al, ':'
        je   .r
        and  al, 0DFh                 ; letters (e.g. the INE of /LINE): go on
        cmp  al, 'A'
        jb   .back
        cmp  al, 'Z'
        jbe  .e1
        ; digits and 0Dh fall through to .back after the AND - compare on
        ; the masked value is fine: '0'..'9' & DF = 10h..19h, 0Dh & DF = 0Dh
.back:
        dec  si
.r:
        ret

decnum:                               ; parse decimal at DS:SI -> DX
        xor  dx, dx
.d1:
        lodsb
        cmp  al, '0'
        jb   .done
        cmp  al, '9'
        ja   .done
        sub  al, '0'
        xor  ah, ah
        push ax
        mov  ax, dx
        mov  dx, 10
        mul  dx                       ; AX = old total * 10 (DX clobbered)
        mov  dx, ax
        pop  ax
        add  dx, ax
        jmp  .d1
.done:
        dec  si
        ret

hexnum:                               ; parse hex at DS:SI -> DX (0 if none)
        xor  dx, dx
.h1:
        lodsb
        and  al, 0DFh                 ; upper-case (digits unharmed for cmp)
        cmp  al, 10h                  ; '0' & DF = 10h.. '9' & DF = 19h
        jb   .done
        cmp  al, 19h
        jbe  .dig
        cmp  al, 'A'
        jb   .done
        cmp  al, 'F'
        ja   .done
        sub  al, 'A' - 10
        jmp  .acc
.dig:
        sub  al, 10h
.acc:
        push cx
        mov  cl, 4
        shl  dx, cl
        pop  cx
        xor  ah, ah
        add  dx, ax
        jmp  .h1
.done:
        dec  si
        ret

hexval:                               ; AL char -> AL value; CF set = not hex
        cmp  al, '0'
        jb   .no
        cmp  al, '9'
        jbe  .dig
        and  al, 0DFh
        cmp  al, 'A'
        jb   .no
        cmp  al, 'F'
        ja   .no
        sub  al, 'A' - 10
        clc
        ret
.dig:
        sub  al, '0'
        clc
        ret
.no:
        stc
        ret

;==========================================================================
;  SMALL HELPERS
;==========================================================================
ticks:                                ; wait CX BIOS ticks (~55 ms each)
        push ax
        push es
.t:
        mov  ax, 40h
        mov  es, ax
        mov  ax, [es:6Ch]
.same:
        cmp  ax, [es:6Ch]
        je   .same
        loop .t
        pop  es
        pop  ax
        ret

prhexadr:                             ; AX as 3 digits if <1000h else 4, +'h'
        cmp  ax, 0FFFh
        ja   .four
        call prhex3
        jmp  .h
.four:
        call prhex4
.h:
        mov  al, 'h'
        call prchar
        ret
prhex3:
        push ax
        mov  al, ah
        and  al, 0Fh
        call prnyb
        pop  ax
        push ax
        call prhex2
        pop  ax
        ret
prhex4:
        push ax
        mov  al, ah
        call prhex2
        pop  ax
prhex2:
        push ax
        push cx
        mov  cl, 4
        shr  al, cl
        call prnyb
        pop  cx
        pop  ax
        push ax
        call prnyb
        pop  ax
        ret
prnyb:
        push ax
        and  al, 0Fh
        add  al, '0'
        cmp  al, '9'
        jbe  .p
        add  al, 7
.p:
        call prchar
        pop  ax
        ret
prdec99:                              ; AL 0-99 decimal, no leading zero
        push ax
        aam                           ; AH = tens, AL = ones
        cmp  ah, 0
        je   .ones
        push ax
        mov  al, ah
        add  al, '0'
        call prchar
        pop  ax
.ones:
        add  al, '0'
        call prchar
        pop  ax
        ret
prdec16:                              ; AX decimal, up to 5 digits
        push ax
        push bx
        push cx
        push dx
        mov  bx, 10
        xor  cx, cx
.div:
        xor  dx, dx
        div  bx
        push dx
        inc  cx
        test ax, ax
        jnz  .div
.out:
        pop  ax
        add  al, '0'
        call prchar
        loop .out
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        ret
prchar:
        push ax
        push dx
        mov  dl, al
        mov  ah, 2
        int  21h
        pop  dx
        pop  ax
        ret
prstr:                                ; ASCIIZ at SI
        push ax
.l:
        lodsb
        or   al, al
        jz   .r
        call prchar
        jmp  .l
.r:
        pop  ax
        ret
crlf:
        push si
        mov  si, m_crlf
        call prstr
        pop  si
        ret

errout:                               ; SI = message; print + exit
        call prstr
        int  20h

;==========================================================================
;  MESSAGES
;==========================================================================
m_banner:  db 'MC8KGO 0.2 - TDK MusicCard MC-8000/DMC-9000 enabler (PCIC/CS/OB)',13,10,0
m_settle:  db 'Socket is slow: CIS settled after ',0
m_settle2: db ' x 20ms.',13,10,0
m_settlot: db '2s or more.',13,10,0
m_nosettle: db 'WARNING: CIS never settled (~5s) - an all-FF read here is NOT',13,10
           db '  proof of a blank card; see pcmcia-cis-ff-bug.md.',13,10,0
m_hostcs:  db 'Host: PCMCIA Card Services',13,10,0
m_hostob:  db 'Host: Socket Services (OmniBook)',13,10,0
m_hostpc:  db 'Host: 82365-class PCIC (direct)',13,10,0
m_nomode:  db 'No supported PCMCIA host found (no Card Services, no Socket'
           db ' Services,',13,10,'no 82365 PCIC at 3E0h). Force a mode with'
           db ' /PCIC, /CS or /OB.',13,10,0
m_badio:   db 'Bad /IO1 or /IO2: 16-byte aligned, nonzero, and never 3E0h'
           db ' or 3F0h',13,10,'(the PCIC / COM1 blocks).',13,10,0
m_badirq:  db 'Bad /I: this card can use IRQ 5, 7 or 10 (or 0 = none).',13,10,0
m_fneeds:  db '/FORCE needs /S=n : name the socket to configure without'
           db ' the CIS check.',13,10,0
m_ignsw:   db 'Ignoring unknown switch: ',0
m_help:    db 'Usage: MC8KGO [switches]',13,10
           db '  /PCIC /CS /OB  host backend (default: auto-detect CS > SS > PCIC)',13,10
           db '  /IO1=hex   window 0 base, the EMU block (default per model: 240/260)',13,10
           db '  /IO2=hex   window 1 base, the MIDI DIN 16550 (default 320)',13,10
           db '  /I=dec     IRQ 5, 7 or 10 (default 0 = none - the DOS stack needs none)',13,10
           db '  /S=dec     socket (PCIC 0-7, OB 1-2; CS: probe only this socket)',13,10
           db '  /W=hex     attr-window segment for CIS/COR (PCIC, default D000)',13,10
           db '  /FORCE     configure without the CIS identity check (needs /S)',13,10
           db '  /OFF       power the socket down (PCIC) / release + dormant (CS)',13,10,0
m_crlf:    db 13,10,0
m_novers:  db 'unreadable',0
; ---- summary block ----
m_forcedtag: db ' (FORCED)',0
m_sock1:   db ': socket ',0
m_auto:    db ' (auto)',0
m_cis:     db '   CIS: ',0
m_manf1:   db '  (MANFID ',0
m_manf2:   db 'MANFID ',0
m_corat:   db '   COR @ ',0
m_index:   db ' index ',0
m_corok:   db ' (verified)',0
m_cormis:  db ' (MISMATCH: readback ',0
m_emu:     db '   EMU block ',0
m_emuok:   db 'h (decoding)',0
m_emuno:   db 'h (FF WALL - not decoding)',0
m_uart:    db '  DIN UART ',0
m_uartok:  db 'h (idle 16550)',0
m_uartno:  db 'h (NO 16550 signature)',0
m_ccsr:    db '  CCSR ',0
m_irq:     db '  IRQ ',0
m_noirq:   db '  no IRQ',0
m_wins:    db '   I/O windows: ',0
m_win:     db 'win',0
m_comma:   db ', ',0
; ---- socket verdicts (PCIC + OB) ----
m_socket:  db 'Socket ',0
m_nocards: db ': no card present/ready.',13,10,0
m_nomanf:  db ': card present, but its CIS has no MANFID tuple.',13,10,0
m_indrive: db ': in use by the system (drive window) - refused.',13,10,0
m_unrec:   db ': unrecognized card - ',0
m_addman:  db '  Use /F to force, if you are sure.',13,10,0
m_addman2: db '  Use /F /S=n to force, if you are sure.',13,10,0
m_addman3: db '  Use /FORCE to force, if you are sure.',13,10,0
m_noknown: db 'No TDK MusicCard in any socket.',13,10,0
m_noknown2: db 'No TDK MusicCard found in any socket.',13,10,0
m_nomatch: db 'Not a TDK MusicCard (last seen ',0
m_badcis:  db 'CIS invalid / config register base out of range',13,10,0
; ---- OB mode ----
m_noss:    db 'No PCMCIA Socket Services (INT 1A) on this machine.',13,10,0
m_sserr:   db 'Socket Services error AH=',0
m_obbadsock: db 'Bad /S: OB user sockets are 1..2 (3/4 = internal storage).',13,10,0
m_obnooff: db '/OFF is not supported in OB mode.',13,10,0
m_conflict: db ' - port range in use by another card',13,10,0
m_outwin:  db ' - out of I/O windows',13,10,0
m_iofail:  db 'EMU/DIN I/O windows unavailable',0
; ---- PCIC mode ----
m_nopcic:  db 'No 82365-class PCIC found (scanned 3E0/3E2/3E4/3E6).',13,10,0
m_nochip1: db 'No PCIC controller for socket ',0
m_nochip2: db ' (port ',0
m_wbusy1:  db 'Probe window ',0
m_wbusy2:  db ' in use by another card; using ',0
m_offdone: db ' powered off.',13,10,0
; ---- CS mode ----
m_nocs:    db 'No PCMCIA Card Services (INT 1Ah AH=AFh not answered).',13,10
           db 'Load the CS stack first, or force /PCIC.',13,10,0
m_cs:      db 'Card Services found, sockets: ',0
m_regfail: db 'RegisterClient failed, status ',0
m_reg:     db 'Registration complete event: ',0
m_regyes:  db 'yes',13,10,0
m_regno:   db 'NO (continuing anyway)',13,10,0
m_probe:   db 'Probing socket ',0
m_s_tup:   db '  no card / CIS unreadable, status ',0
m_s_wrong: db '  not a TDK MusicCard: MANFID ',0
m_s_io:    db '  RequestIO refused, status ',0
m_s_cfg:   db '  RequestConfiguration refused, status ',0
m_nocard:  db 'Card not enabled.',13,10,0
m_resident: db 'Staying resident - insert the card and it will be'
           db ' configured.',13,10,0
m_via:     db 'Reconfigured via the resident copy:',13,10,0
m_offok:   db 'Released - the resident copy is dormant (run again to'
           db ' re-enable).',13,10,0
m_vermsg:  db 'A different MC8KGO version is resident - reboot to'
           db ' replace it.',13,10,0
m_offnone: db 'Nothing resident to switch off.',13,10,0
m_csnoforce: db 'Note: /FORCE is not supported in CS mode - ignored.',13,10,0
; ---- names ----
n_tdk:     db 'TDK MusicCard',0
n_tdk8:    db 'TDK MC-8000 (MW-8232)',0
n_tdk9:    db 'TDK DMC-9000 (MW-8432)',0
n_unk:     db 'Unknown card',0

;==========================================================================
;  TRANSIENT DATA
;==========================================================================
o_mode:    db 0                       ; 0 = auto-detect
o_wseg:    dw 0D000h                  ; attr probe window segment
o_wgiven:  db 0
sock:      db 0                       ; current socket (OB/PCIC)
sawunk:    db 0
failwhy:   db 0
gotcfg:    db 0
rep_sock:  db 0                       ; summary inputs
rep_irq:   db 0FFh
r_corvf:   db 0                       ; COR readback: 0 none, 1 ok, 2 bad
r_corrb:   db 0
any_chip:  db 0
wcount:    db 0                       ; mapped-window list for the summary
wnum:      db 0,0,0,0
wbase:     dw 0,0,0,0
wsize:     dw 0,0,0,0
; OB scratch
cw_i:      db 0
aw_i:      db 0
aw_attr:   db 0
aw_base:   dw 0
aw_size:   dw 0
qstat:     db 0,0,0,0
qbase:     dw 0,0,0,0
qsize:     dw 0,0,0,0
gw_n:      db 0
w0_bl:     db 0
w0_cx:     dw 0
w0_dx:     dw 0
w0_si:     dw 0
w0_di:     dw 0
; PCIC scratch
pc_idx:    dw PCIC_BASE
pc_off:    db 0
p_w0s:     dw 0
p_w0e:     dw 0
p_w1s:     dw 0
p_w1e:     dw 0
p_wason:   db 0
p_s03:     db 0
p_s06:     db 0
p_s10:     db 0
p_s11:     db 0
p_s12:     db 0
p_s13:     db 0
p_s14:     db 0
p_s15:     db 0
pw_pg:     dw 0
pw_pg2:    dw 0
settlen:   dw 0                       ; 20ms polls the CIS settle gate took
mw_seg:    dw 0
mw_cs:     dw 0
mw_ce:     dw 0
mw_sk:     db 0
mw_en:     db 0
mw_w:      db 0
mw_rb:     db 0
mw_ws:     dw 0
mw_we:     dw 0
; CS transient scratch
ibuf:      times 50h db 0
qcopy:     times 16 db 0
res_seg:   dw 0
res_off:   dw 0
fptr:      dd 0
hndoff:    db 0
res_badver: db 0

; ---- trailing BSS (beyond the .COM image; transient-only buffers) -------
cisbuf:                               ; CISLEN dense CIS bytes
