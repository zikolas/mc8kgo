/* TDKSEND - send MIDI bytes to the resident synth, standing in for MPUSHIM.
 *
 * This is what MPUSHIM's OUT 330h case will do, in three instructions:
 *      mov dl, <byte> ; mov ax, 0BD01h ; int 2Fh
 * Proving the resident path with this first means that when MPUSHIM is wired
 * up, any failure is in the trap layer and not in the synth.
 */

#include <stdio.h>
#include <conio.h>
#include <dos.h>

static void send(unsigned char b)
{
    union REGS r;
    r.h.ah = 0xBD;
    r.h.al = 0x01;
    r.h.dl = b;
    int86(0x2F, &r, &r);
}

static int present(void)
{
    union REGS ri, ro;
    ri.h.ah = 0xBD; ri.h.al = 0x00;
    int86(0x2F, &ri, &ro);
    return (ro.h.al == 0xFF && ro.x.bx == 0x5453 && ro.x.cx == 0x594E);
}

static unsigned drops(void)
{
    union REGS ri, ro;
    ri.h.ah = 0xBD; ri.h.al = 0x03;
    int86(0x2F, &ri, &ro);
    return ro.x.bx;
}

static void note(int ch, int n, int v) { send(0x90 | ch); send(n); send(v); }
static void off(int ch, int n)         { send(0x80 | ch); send(n); send(0); }
static void prog(int ch, int p)        { send(0xC0 | ch); send(p); }

static void pause(int units)
{
    int k; unsigned i;
    for (k = 0; k < units; k++) for (i = 0; i < 60000U; i++) (void)inp(0x80);
}

/* Report the SysEx the synth has thrown away and exit. A game that uploads its
 * own timbres shows thousands of bytes here, and that - not the preset map - is
 * why its instruments sound wrong through the internal wavetable. */
static int query(void)
{
    union REGS ri, ro;
    ri.h.ah = 0xBD; ri.h.al = 0x08;
    int86(0x2F, &ri, &ro);
    printf("discarded SysEx: %u bytes in %u messages\n", ro.x.bx, ro.x.cx);
    if (ro.x.cx)
        printf("this title uploads its own timbres; the stock bank cannot\n"
               "reproduce them, so use the DIN to real MT-32 gear instead.\n");
    else
        printf("none - its instruments come from program changes alone, so a\n"
               "wrong sound here IS our preset map.\n");
    printf("dropped MIDI bytes: %u\n", drops());
    return 0;
}

int main(int argc, char **argv)
{
    static int maj[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    int i, p = 0;

    setbuf(stdout, NULL);

    if (!present()) {
        printf("TDKSYN is not resident. Load it first.\n");
        return 1;
    }
    if (argc > 1 && (argv[1][0] == '/' || argv[1][0] == '-')
                 && (argv[1][1] == 'q' || argv[1][1] == 'Q'))
        return query();
    if (argc > 1) sscanf(argv[1], "%d", &p);
    printf("TDKSYN found. Sending MIDI over INT 2Fh AH=BDh.\n\n");

    {   /* Is the resident state actually addressable from the handler?
         * If DS is wrong these read as garbage rather than 884 / 65 / 9. */
        union REGS ri, ro;
        ri.h.ah = 0xBD; ri.h.al = 0x04;
        int86(0x2F, &ri, &ro);
        printf("  state check: zones=%u (want 884)  drums=%u (want 65)"
               "  piano=%u (want 9)\n", ro.x.bx, ro.x.cx, ro.x.dx);
        if (ro.x.bx != 884)
            printf("  ^^ resident state is NOT addressable - segment problem\n");
    }

    {   /* Same note, but re-initialise the chip first. If this sounds and
         * the plain one does not, the card is not holding its configuration
         * between install and use. */
        union REGS ri, ro;
        printf("  note WITH a fresh chip_init (2s)...\n");
        ri.h.ah = 0xBD; ri.h.al = 0x07;
        int86(0x2F, &ri, &ro);
        pause(30);
        ri.h.ah = 0xBD; ri.h.al = 0x06;
        int86(0x2F, &ri, &ro);
        pause(6);
    }

    {   /* Fire a note from inside the handler, bypassing parser and ring.
         * If this sounds but MIDI bytes do not, the fault is in the stream
         * path; if neither sounds, it is the synth in interrupt context. */
        union REGS ri, ro;
        printf("  direct note from inside the handler (2s)...\n");
        ri.h.ah = 0xBD; ri.h.al = 0x05;
        int86(0x2F, &ri, &ro);
        printf("    handler saw: ROM=%u (want 53031)  root=%u (want 76)"
               "  dcysus=%04X (want 0102)\n", ro.x.bx, ro.x.cx, ro.x.dx);
        pause(30);
        ri.h.ah = 0xBD; ri.h.al = 0x06;
        int86(0x2F, &ri, &ro);
        pause(6);
    }

    printf("  program change -> %d\n", p);
    prog(0, p);

    printf("  scale on channel 1\n");
    for (i = 0; i < 8; i++) {
        note(0, maj[i], 100);
        pause(6);
        off(0, maj[i]);
        pause(1);
    }

    printf("  chord\n");
    note(0, 60, 100); note(0, 64, 100); note(0, 67, 100);
    pause(30);
    off(0, 60); off(0, 64); off(0, 67);
    pause(6);

    printf("  running status (one status byte, many note pairs)\n");
    send(0x90);
    for (i = 0; i < 8; i++) { send(maj[i]); send(90); pause(4); send(maj[i]); send(0); }

    printf("  real-time bytes injected mid-message (must be ignored)\n");
    send(0x90); send(0xFE); send(60); send(0xF8); send(100);
    pause(20);
    send(0x90); send(60); send(0);

    printf("\ndropped bytes: %u\n", drops());
    return 0;
}
