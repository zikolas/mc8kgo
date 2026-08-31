/* TDKSYN - resident General MIDI synthesiser for the TDK MC-8000.
 *
 * DELIVERABLE 1, the synthesis half. MPUSHIM traps 330h/331h and handles the
 * MPU-401 facade; where it currently forwards a data byte to a UART, it calls
 * us instead. Two resident pieces, loosely coupled through a software
 * interrupt, so each stays in its own language and can be tested alone.
 *
 * ENTRY POINT - INT 2Fh multiplex, AH = 0xBD:
 *      AL=00  install check -> AL=0xFF, BX='TS', CX='YN'
 *      AL=01  MIDI byte in DL
 *      AL=02  reset: all notes off, parser cleared
 *      AL=03  return dropped-byte count in BX
 *
 * From MPUSHIM's trap handler the OUT 330h case becomes roughly:
 *      mov  dl, al
 *      mov  ax, 0BD01h
 *      int  2Fh
 *
 * WHAT RUNS WHERE. midi_push only enqueues. The drain (and therefore the
 * synthesis) happens on this same call, which is what we chose over a timer
 * hook: games write MIDI bytes constantly, so the work spreads naturally
 * across those writes and we never touch timer state - historically risky on
 * this machine.
 *
 * INTERRUPT SAFETY. Everything after install is port I/O only: no DOS calls,
 * no C library, no file access. The handler switches to a private stack
 * because a trap handler's stack may be very small.
 */

#include "SYNTH.C"
#include "MIDIRT.C"
#include <dos.h>

#define MPLEX_AH   0xBD
#define SIG_BX     0x5453      /* 'TS' */
#define SIG_CX     0x594E      /* 'YN' */

static void (__interrupt __far *old2f)(void);

/* __loadds is REQUIRED, not optional. A Watcom __interrupt handler runs with
 * whatever DS the caller happened to have; without __loadds every reference
 * to our resident state - the preset map, the voice pool, the ring - reads
 * through the wrong segment. The symptom is not a crash but near-silence with
 * occasional stray pokes at the card. */

/* NOTE: this handler runs on whatever stack the caller provides. The synth
 * uses only a few dozen bytes of locals, so no stack switch is done here. If
 * MPUSHIM's trap context turns out to run on a very small stack this is the
 * first thing to revisit. */
static int      busy;
static unsigned resident_psp;

static void __interrupt __far __loadds handler(union INTPACK r)
{
    if ((r.h.ah != MPLEX_AH)) { _chain_intr(old2f); }

    switch (r.h.al) {
    case 0x00:                      /* install check */
        r.h.al = 0xFF;
        r.w.bx = SIG_BX;
        r.w.cx = SIG_CX;
        return;

    case 0x01:                      /* MIDI byte in DL */
        midi_push(r.h.dl);
        if (!busy) {                /* never re-enter the synth */
            busy = 1;
            midi_poll();
            busy = 0;
        }
        return;

    case 0x02:                      /* reset */
        if (!busy) { busy = 1; allsil(); midi_reset(); busy = 0; }
        return;

    case 0x03:                      /* dropped-byte count */
        r.w.bx = midi_dropped();
        return;

    case 0x08:                      /* discarded SysEx: BX bytes, CX messages */
        r.w.bx = midi_sysex_bytes();
        r.w.cx = midi_sysex_msgs();
        return;

    case 0x04:                      /* state check: is our DS actually ours? */
        r.w.bx = gmap.nzone;        /* should read 884 */
        r.w.cx = gmap.dcount;       /* should read 65  */
        r.w.dx = (unsigned)gmap.count[0];  /* piano zones, should be 9 */
        return;

    case 0x05: {                    /* fire a note directly, bypassing the
                                     * stream parser and the ring entirely,
                                     * and report what it actually got */
        SF2ZP z = sf2_pick(&gmap, 0, 60);
        if (!z) { r.w.bx = 0xFFFF; return; }
        r.w.bx = (unsigned)(z->st & 0xFFFFL);   /* want 53031 = 0xCF27 */
        r.w.cx = (unsigned)z->root;             /* want 76 */
        r.w.dx = z->voldcysus;                  /* want 0102 */
        play_zone(0, z, 60, 100, 0, 0);
        return;
    }

    case 0x06:                      /* silence everything */
        allsil();
        return;

    case 0xFE:                      /* uninstall: silence, unhook, and tell
                                     * the caller which blocks to free */
        allsil();
        HWCF3(0x0000);
        _dos_setvect(0x2F, old2f);
        r.w.bx = resident_psp;
        r.w.cx = gmap.zseg;         /* the zone table's own block */
        return;

    case 0x07: {                    /* re-init the chip, THEN play.
                                     * If this sounds and AL=05 does not, the
                                     * card's state is not surviving between
                                     * install and use. */
        SF2ZP z;
        chip_init();
        synth_reset();
        z = sf2_pick(&gmap, 0, 60);
        if (z) play_zone(0, z, 60, 100, 0, 0);
        return;
    }

    default:
        return;
    }
}

static int already_resident(void)
{
    union REGS ri, ro;
    ri.h.ah = MPLEX_AH;
    ri.h.al = 0x00;
    int86(0x2F, &ri, &ro);
    return (ro.h.al == 0xFF && ro.x.bx == SIG_BX && ro.x.cx == SIG_CX);
}

int main(int argc, char **argv)
{
    static char *deflt = "C:\\WINDOWS\\SYSTEM\\SYNTH2GM.SF2";
    char *path = deflt;
    int i, rc, mt32 = 0, atkmax = 120, maskgiven = 0, iogiven = 0, agiven = 0;
    unsigned chmask = 0xFFFF, iobase = 0;
    unsigned paras;

    o_str("TDKSYN - GM synth for the TDK MC-8000/DMC-9000 wavetable\r\n");

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '/' || argv[i][0] == '-') {
            if (argv[i][1] == 'h' || argv[i][1] == 'H')
                p_hex3(argv[i] + 2, &g_h1, &g_h2, &g_h3);
            else if (argv[i][1] == 'f' || argv[i][1] == 'F') path = argv[i] + 2;
            else if (argv[i][1] == 'm' || argv[i][1] == 'M') mt32 = 1;
            else if (argv[i][1] == 'a' || argv[i][1] == 'A') {
                atkmax = p_dec(argv[i] + 2);
                agiven = 1;
            }
            else if (argv[i][1] == 'x' || argv[i][1] == 'X') {
                /* Windows MIDI Mapper behaviour: play channels 1-10 only.
                 * Microsoft-authored .MIDs (CANYON etc.) duplicate the music
                 * on 13-16 as a base-level arrangement - ch 16 carries the
                 * whole DRUM pattern on program 0, so unmasked it plays as
                 * low PIANO doubling every hit. */
                chmask = 0x03FF; maskgiven = 1;
            } else if (argv[i][1] == 'c' || argv[i][1] == 'C') {
                chmask = p_hex(argv[i] + 2); maskgiven = 1;
            } else if (argv[i][1] == 'i' || argv[i][1] == 'I') {
                char *q = argv[i] + 2;      /* /IO=260 or /I260 */
                while (*q == 'o' || *q == 'O' || *q == '=') q++;
                iobase = p_hex(q);
                iogiven = 1;
            }
        }
    }
    if (mt32 && !maskgiven)
        chmask = 0x03FE;   /* an MT-32/CM-32L receives channels 2-10 only */

    for (i = 1; i < argc; i++)
        if ((argv[i][0] == '/' || argv[i][0] == '-')
            && (argv[i][1] == 'u' || argv[i][1] == 'U')) {
            union REGS ri, ro;
            struct SREGS sr;
            unsigned rpsp, rzone;
            if (!already_resident()) { o_str("not resident.\r\n"); return 1; }
            ri.h.ah = 0xBD; ri.h.al = 0xFE;
            ri.x.cx = 0;                      /* an OLDER resident copy
                                               * echoes CX untouched - never
                                               * free a garbage segment */
            int86(0x2F, &ri, &ro);
            /* Latch both segments NOW. The frees below write their own
             * results into ro, so reading ro.x.bx after the first one
             * frees whatever DOS happened to leave in BX - which silently
             * leaked the whole resident image on every unload. */
            rpsp  = ro.x.bx;
            rzone = ro.x.cx;
            segread(&sr);
            if (rzone) {                      /* the zone table's block */
                sr.es = rzone;
                ri.h.ah = 0x49;
                int86x(0x21, &ri, &ro, &sr);
            }
            sr.es = rpsp;                     /* the resident PSP */
            ri.h.ah = 0x49;                   /* free the memory block */
            int86x(0x21, &ri, &ro, &sr);
            o_str("unloaded.\r\n");
            return 0;
        }

    if (already_resident()) {
        o_str("already resident. Use /U to unload first.\r\n");
        return 1;
    }

    /* Window 0 base: the MC-8000 decodes 240h, the DMC-9000 260h.  Probe
     * with the published EMU handshake unless /IO pinned it - and refuse
     * to install against a dead bus, which also catches a card nobody
     * enabled (run MC8KGO first). */
    if (iogiven)
        synth_base(iobase);
    else if (!synth_detect()) {
        synth_base(0x260);
        if (!synth_detect()) {
            o_str("no EMU answers at 240h or 260h - is the card enabled"
                  " (MC8KGO)?\r\n/IO=hex overrides the probe.\r\n");
            return 1;
        }
    }
    if (g_w0 == 0x260 && path == deflt)
        o_str("DMC-9000 window (260h): this SoundFont maps the MC-8000's\r\n"
              "ROM - instruments will be WRONG until the 9000's own bank\r\n"
              "is dumped (/F<file> loads another bank).\r\n");

    /* Pick the banks BEFORE loading - sf2_load builds the map from them.
     * A game set to MT-32 output sends MT-32 timbre numbers, so resolving it
     * against General MIDI gets the wrong instrument for every program. */
    sf2_mode(mt32);
    sf2_attack_max(atkmax);
    synth_chmask(chmask);
    rc = sf2_load(path, &gmap);
    if (rc == SF2_ENOFILE) {
        o_str("cannot open "); o_str(path); o_str("\r\n");
        o_str("This needs the SoundFont the vendor installer puts at that\r\n"
              "path; /F<path> overrides. No preset data is built in.\r\n");
        return 1;
    }
    if (rc == SF2_ENOMEM) {
        o_str("no DOS memory for the zone table (");
        o_u((unsigned)((SF2_MAXZONE * sizeof(SF2ZONE) + 15L) / 16));
        o_str(" paras)\r\n");
        return 1;
    }
    if (rc != SF2_OK) { o_str("could not parse that SoundFont\r\n"); return 1; }
    if (gmap.nzone == 0) {
        o_str("no zones for that mode - is this the right SoundFont?\r\n");
        return 1;
    }

    o_str("EMU "); o_x(g_w0, 3); o_str("h  ");
    o_str(mt32 ? "MT-32 (bank 127 + CM-64/32 kit)"
               : "General MIDI (bank 0 + Standard kit)");
    o_str("  "); o_u(gmap.nzone - gmap.dcount);
    o_str("+");  o_u(gmap.dcount); o_str(" zones");
    if (chmask == 0x03FF)      o_str("  channels 1-10");
    else if (chmask == 0x03FE) o_str("  channels 2-10");
    else if (chmask != 0xFFFF) { o_str("  channel mask "); o_x(chmask, 4); }
    if (agiven) { o_str("  attack cap "); o_d(atkmax); }
    o_str("\r\n");

    chip_init();
    synth_reset();
    midi_reset();
    busy = 0;

    resident_psp = _psp;
    old2f = _dos_getvect(0x2F);
    _dos_setvect(0x2F, handler);

    o_str("resident on INT 2Fh id "); o_x(MPLEX_AH, 2);
    o_str("h - MPUSHIM /SYNTH feeds it; /U unloads\r\n");

    /* Stay resident, keeping exactly the block DOS gave us. Its size in
     * paragraphs is in our MCB, one paragraph below the PSP at offset 3.
     * The C library's file and formatting code is no longer in here (see
     * DOSIO.H). What still rides along is the SoundFont parser, which only
     * runs during install; discarding that as well means splitting the
     * image, which this does not attempt. */
    {   /* the environment block is transient too */
        unsigned envseg = *(unsigned __far *)MK_FP(_psp, 0x2C);
        if (envseg) _dos_freemem(envseg);
    }
    paras = *(unsigned __far *)MK_FP(_psp - 1, 3);
    _dos_keep(0, paras);
    return 0;
}
