/* SYNTH.C - the MC-8000 wavetable synth, shared by the MIDI file player
 * and the resident MPU-401 TSR so the two cannot drift apart.
 *
 * Everything here after init is port I/O only: no DOS calls, no C library,
 * nothing that is unsafe to run from an interrupt handler.
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <conio.h>
#include <dos.h>

/* EMU8000 pan volume table, from the Linux ALSA emux driver
 * (sound/synth/emux/emux_synth.c, GPL-2.0).
 *
 * The chip does not take a pan POSITION - it takes two independent
 * level fields: apan (PSST bits 31-24) is the LEFT level and aaux
 * (PTRX bits 7-0) is the RIGHT level:
 *     apan = pan_volumes[pan];  aaux = pan_volumes[255 - pan];
 * Leaving aaux at zero silences the right channel entirely.
 */
static const unsigned char pan_volumes[256] = {
  0x00,0x03,0x06,0x09,0x0c,0x0f,0x12,0x14,0x17,0x1a,0x1d,0x20,0x22,0x25,0x28,0x2a,
  0x2d,0x30,0x32,0x35,0x37,0x3a,0x3c,0x3f,0x41,0x44,0x46,0x49,0x4b,0x4d,0x50,0x52,
  0x54,0x57,0x59,0x5b,0x5d,0x60,0x62,0x64,0x66,0x68,0x6a,0x6c,0x6f,0x71,0x73,0x75,
  0x77,0x79,0x7b,0x7c,0x7e,0x80,0x82,0x84,0x86,0x88,0x89,0x8b,0x8d,0x8f,0x90,0x92,
  0x94,0x96,0x97,0x99,0x9a,0x9c,0x9e,0x9f,0xa1,0xa2,0xa4,0xa5,0xa7,0xa8,0xaa,0xab,
  0xad,0xae,0xaf,0xb1,0xb2,0xb3,0xb5,0xb6,0xb7,0xb9,0xba,0xbb,0xbc,0xbe,0xbf,0xc0,
  0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,0xd0,0xd1,
  0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd7,0xd8,0xd9,0xda,0xdb,0xdc,0xdc,0xdd,0xde,0xdf,
  0xdf,0xe0,0xe1,0xe2,0xe2,0xe3,0xe4,0xe4,0xe5,0xe6,0xe6,0xe7,0xe8,0xe8,0xe9,0xe9,
  0xea,0xeb,0xeb,0xec,0xec,0xed,0xed,0xee,0xee,0xef,0xef,0xf0,0xf0,0xf1,0xf1,0xf1,
  0xf2,0xf2,0xf3,0xf3,0xf3,0xf4,0xf4,0xf5,0xf5,0xf5,0xf6,0xf6,0xf6,0xf7,0xf7,0xf7,
  0xf7,0xf8,0xf8,0xf8,0xf9,0xf9,0xf9,0xf9,0xf9,0xfa,0xfa,0xfa,0xfa,0xfb,0xfb,0xfb,
  0xfb,0xfb,0xfc,0xfc,0xfc,0xfc,0xfc,0xfc,0xfc,0xfd,0xfd,0xfd,0xfd,0xfd,0xfd,0xfd,
  0xfd,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,0xfe,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};

/* Channel-volume / velocity / expression -> attenuation curves, from the
 * Linux ALSA emux driver (sound/synth/emux/emux_synth.c, GPL-2.0):
 *     vol = (voltab1[CC7] + voltab2[velocity]) * 8 / 3 + zone attenuation
 *     vol += ((0x100 - vol) * expressiontab[CC11]) / 128
 * The linear formula this replaces got the balance between parts and layers
 * audibly wrong. */
static const unsigned char voltab1[128] = {
   0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63,
   0x63, 0x2b, 0x29, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22,
   0x21, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
   0x19, 0x19, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14,
   0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10,
   0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0e, 0x0d,
   0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b,
   0x0b, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09,
   0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x06,
   0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04,
   0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
   0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01,
   0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char voltab2[128] = {
   0x32, 0x31, 0x30, 0x2f, 0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x2a,
   0x29, 0x28, 0x27, 0x26, 0x25, 0x24, 0x24, 0x23, 0x22, 0x21,
   0x21, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1c, 0x1c, 0x1b, 0x1a,
   0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15,
   0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x11, 0x11, 0x10,
   0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d,
   0x0d, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a,
   0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08,
   0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06,
   0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
   0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03,
   0x03, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
   0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char expressiontab[128] = {
   0x7f, 0x6c, 0x62, 0x5a, 0x54, 0x50, 0x4b, 0x48, 0x45, 0x42,
   0x40, 0x3d, 0x3b, 0x39, 0x38, 0x36, 0x34, 0x33, 0x31, 0x30,
   0x2f, 0x2d, 0x2c, 0x2b, 0x2a, 0x29, 0x28, 0x27, 0x26, 0x25,
   0x24, 0x24, 0x23, 0x22, 0x21, 0x21, 0x20, 0x1f, 0x1e, 0x1e,
   0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x18, 0x18,
   0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15, 0x14, 0x14, 0x13,
   0x13, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x0f, 0x0f,
   0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c,
   0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09,
   0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06,
   0x06, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x03,
   0x03, 0x03, 0x03, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01,
   0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#include "MIDIRT.H"
#include "SF2RT.C"        /* runtime SoundFont reader - one-file build */

#include "EMUINIT.H"

/* Window 0 (the EMU register block) is a RUNTIME base: the MC-8000's CIS
 * entry 0 puts it at 240h, the DMC-9000's at 260h.  Window 1 (the DIN
 * UART) is 320h on both cards. */
static unsigned g_w0 = 0x240;

void synth_base(unsigned b) { g_w0 = b; }

#define W0   g_w0
#define W1   0x320
#define PTRP (W0 + 0x0E)
#define D0   (W0 + 0x04)
#define D1   (W0 + 0x08)
#define D2   (W0 + 0x0A)
#define D3   (W0 + 0x0C)
#define CMD(r, c) ((unsigned)(((r) << 5) | (c)))


static void dly(unsigned n) { while (n--) (void)inp(0x80); }
static void hold(int u) { int k; for (k = 0; k < u; k++) dly(60000); }

static void poke(unsigned dp, unsigned cmd, unsigned val)
{ outpw(PTRP, cmd); outpw(dp, val); }
static void pokedw(unsigned dp, unsigned cmd, unsigned long val)
{ outpw(PTRP, cmd); outpw(dp, (unsigned)(val & 0xFFFF)); outpw(dp + 2, (unsigned)(val >> 16)); }
static unsigned peek(unsigned dp, unsigned cmd)
{ outpw(PTRP, cmd); return inpw(dp); }

/* The published EMU8000 HWCF detect handshake at the CURRENT base (write
 * 0059/0020/0000, judge the readback masks - TDKEMU3/TDKSWEEP-proven on
 * both chips).  chip_init reprograms everything afterwards, so probing is
 * free at install time.  1 = an EMU answers here. */
int synth_detect(void)
{
    unsigned h1, h2;
    poke(D1, CMD(1, 29), 0x0059);
    poke(D1, CMD(1, 30), 0x0020);
    poke(D1, CMD(1, 31), 0x0000);
    h1 = peek(D1, CMD(1, 29));
    h2 = peek(D1, CMD(1, 30));
    return ((h1 & 0x7E) == 0x58 && (h2 & 0x03) == 0x03);
}

#define DCYSUSV(c, v) poke(D1, CMD(5, c), v)
#define IFATN(c, v)   poke(D3, CMD(1, c), v)
#define CPF(c, v)     pokedw(D0, CMD(0, c), v)
#define PTRX(c, v)    pokedw(D0, CMD(1, c), v)
#define CVCF(c, v)    pokedw(D0, CMD(2, c), v)
#define VTFT(c, v)    pokedw(D0, CMD(3, c), v)
#define PSST(c, v)    pokedw(D0, CMD(6, c), v)
#define CSL(c, v)     pokedw(D0, CMD(7, c), v)
#define CCCA(c, v)    pokedw(D1, CMD(0, c), v)
#define IP(c, v)      poke(D3, CMD(0, c), v)
#define PEFE(c, v)    poke(D3, CMD(2, c), v)
#define FMMOD(c, v)   poke(D3, CMD(4, c), v)
#define TREMFRQ(c, v) poke(D3, CMD(5, c), v)
#define FM2FRQ2(c, v) poke(D3, CMD(6, c), v)
#define ATKHLDV(c, v) poke(D2, CMD(4, c), v)
#define LFO1VAL(c, v) poke(D2, CMD(5, c), v)
#define ATKHLD(c, v)  poke(D2, CMD(6, c), v)
#define LFO2VAL(c, v) poke(D2, CMD(7, c), v)
#define ENVVOL(c, v)  poke(D1, CMD(4, c), v)
#define ENVVAL(c, v)  poke(D1, CMD(6, c), v)
#define DCYSUS(c, v)  poke(D1, CMD(7, c), v)
#define SMALR(v)      pokedw(D1, CMD(1, 20), v)
#define SMALW(v)      pokedw(D1, CMD(1, 22), v)
#define HWCF1(v)      poke(D1, CMD(1, 29), v)
#define HWCF2(v)      poke(D1, CMD(1, 30), v)
#define HWCF3(v)      poke(D1, CMD(1, 31), v)
#define HWCF4(v)      pokedw(D1, CMD(1, 9), v)
#define HWCF5(v)      pokedw(D1, CMD(1, 10), v)
#define HWCF6(v)      pokedw(D1, CMD(1, 13), v)
#define SMARR(v)      pokedw(D1, CMD(1, 21), v)
#define SMARW(v)      pokedw(D1, CMD(1, 23), v)
#define INIT1(c, v)   poke(D1, CMD(2, c), v)
#define INIT2(c, v)   poke(D2, CMD(2, c), v)
#define INIT3(c, v)   poke(D1, CMD(3, c), v)
#define INIT4(c, v)   poke(D2, CMD(3, c), v)
#define R0080(c, v)   pokedw(D0, CMD(4, c), v)   /* undocumented, cleared */
#define R00A0(c, v)   pokedw(D0, CMD(5, c), v)   /* undocumented, cleared */

/* The vendor's HWCF values 0038/0053/0007 were READ from its running driver,
 * and writing those same values straight back is what makes the card audible.
 *
 * An earlier theory read the inversions (write 0059 -> read 0058; write 0038
 * -> read 0039; write 0053 -> read 0054) as meaning the vendor's values had
 * to be pre-inverted before writing, and defaulted to 0059/0020/0004 on that
 * basis. That is wrong, and 0004 in particular does not enable audio at all.
 * The failure is silent in the worst way: zone lookup, note counts and
 * dropped-byte counters all report perfectly healthy while nothing comes out
 * of the card, so it reads as a bug anywhere but here. Bench-confirmed
 * 2026-08-26 - /H38,53,7 was audible where the built-in default was not.
 *
 * Our writes still do not read back verbatim (0038->0039, 0053->0054,
 * 0007->0013). That remains a red herring; audibility is what settles it.
 *
 * g_h2 is parsed by /H but not written here: chip_init sets HWCF2 to 0020
 * before the init arrays and the arrays leave it matching the vendor. */
static unsigned g_h1 = 0x0038, g_h2 = 0x0053, g_h3 = 0x0007;
static int g_keepref = 0;   /* 1 = leave FM refresh ch30/31 RUNNING */

static void w1cfg(void) { outp(W1 + 4, 0xC1); outp(W1 + 6, 0x03); }

static unsigned calc_pt(unsigned ip)
{
    unsigned long pt = 1UL << (ip >> 12);
    if (ip & 0x800) pt += pt * 0x102eUL / 0x2710UL;
    if (ip & 0x400) pt += pt * 0x764UL / 0x2710UL;
    if (ip & 0x200) pt += pt * 0x389UL / 0x2710UL;
    pt += pt >> 1;
    if (pt > 0xFFFFUL) pt = 0xFFFFUL;
    return (unsigned)pt;
}

static void init_fm(void)
{
    long g;
    DCYSUSV(30, 0x80); PSST(30, 0xFFFFFFE0L); CSL(30, 0x00FFFFE8L);
    PTRX(30, 0); CPF(30, 0); CCCA(30, 0x00FFFFE3L);
    DCYSUSV(31, 0x80); PSST(31, 0x00FFFFF0L); CSL(31, 0x00FFFFF8L);
    PTRX(31, 0); CPF(31, 0x8000L); CCCA(31, 0x00FFFFF3L);
    poke(D0, CMD(1, 30), 0);
    g = 0; while (!(inpw(PTRP) & 0x1000)) { if (++g > 200000L) break; }
    g = 0; while ( (inpw(PTRP) & 0x1000)) { if (++g > 200000L) break; }
    poke(D0, CMD(1, 30), 0x4828);
    outp(PTRP, 0x3C); outp(D1, 0);
}

/* send_array: each quarter of a 128-entry table goes to INIT1..INIT4 */
static void send_array(const unsigned *d)
{
    int i, k = 0;
    for (i = 0; i < 32; i++) INIT1(i, d[k++]);
    for (i = 0; i < 32; i++) INIT2(i, d[k++]);
    for (i = 0; i < 32; i++) INIT3(i, d[k++]);
    for (i = 0; i < 32; i++) INIT4(i, d[k++]);
}

/* The documented EMU8000 power-up sequence. Two steps here were missing from
 * every previous attempt, and both touch per-channel state the voice engine
 * relies on:
 *   - the init arrays into INIT1..INIT4 (register indices 2 and 3 on
 *     DATA1/DATA2), which we had been leaving at power-up values
 *   - HWCF4/5/6, which we had never written at all
 */
static void chip_init(void)
{
    int c;

    HWCF1(0x0059);
    HWCF2(0x0020);
    HWCF3(0x0000);                 /* audio off during init */
    w1cfg();

    /* init_audio: envelope engines off, then every parameter to zero */
    for (c = 0; c < 32; c++) DCYSUSV(c, 0x80);
    for (c = 0; c < 32; c++) {
        ENVVOL(c, 0); ENVVAL(c, 0); DCYSUS(c, 0);
        ATKHLDV(c, 0); LFO1VAL(c, 0); ATKHLD(c, 0); LFO2VAL(c, 0);
        IP(c, 0); IFATN(c, 0); PEFE(c, 0);
        FMMOD(c, 0); TREMFRQ(c, 0); FM2FRQ2(c, 0);
        PTRX(c, 0L); VTFT(c, 0L); PSST(c, 0L); CSL(c, 0L); CCCA(c, 0L);
    }
    for (c = 0; c < 32; c++) { CPF(c, 0L); CVCF(c, 0L); }

    /* init_dma */
    SMALR(0L); SMARR(0L); SMALW(0L); SMARW(0L);

    /* init_arrays */
    send_array(init1);
    dly(40000);                    /* wait 1024 sample clocks (~23ms) */
    send_array(init2);
    send_array(init3);
    HWCF4(0L);
    HWCF5(0x83L);
    HWCF6(0x8000L);
    send_array(init4);

    init_fm();

    for (c = 0; c < 32; c++) DCYSUSV(c, 0x807F);

    /* The vendor's running driver reads back HWCF 0038/0053/0007. HWCF2 now
     * matches after the init arrays; set HWCF1 and HWCF3 last, since writing
     * them earlier gets overwritten by the init sequence. */
    HWCF1(g_h1);
    HWCF3(g_h3);                   /* enable audio */
    w1cfg();

    if (!g_keepref) {
        IFATN(30, 0xFFFF); IFATN(31, 0xFFFF);
        DCYSUSV(30, 0x807F); DCYSUSV(31, 0x807F);
        CVCF(30, 0L); CVCF(31, 0L); VTFT(30, 0L); VTFT(31, 0L);
    } else {
        IFATN(30, 0xFFFF); IFATN(31, 0xFFFF);
    }
}

/* IP = 0xE000 + (note - root) * 4096/12.  0xE000 is root pitch and one
 * octave is 0x1000, so a semitone is 4096/12 = 341.33. All samples in this
 * ROM are 44100Hz so the sample-rate offset term is zero. */



/* ---- shared state ---- */

static SF2MAP  gmap;                       /* the preset map, built at init */
static unsigned g_apan = 0xFF, g_aaux = 0xFF;  /* LEFT and RIGHT levels */
static int      g_flat = 0;                /* 1 = ignore SoundFont envelopes */

/* Which MIDI channels sound (bit n = channel n+1).  Default: all sixteen,
 * the spec behaviour.  Two real devices differ: the Windows MIDI Mapper
 * strips 11-16 (Microsoft's own .MIDs carry a DUPLICATE base-level
 * arrangement there - CANYON's ch 16 hammers the drum rhythm as PIANO
 * notes), and an MT-32/CM-32L receives only 2-10. */
static unsigned g_chmask = 0xFFFF;

void synth_chmask(unsigned m) { g_chmask = m; }

/* ---- voices ---- */

#define NVOICE 30
static struct { int ch, note, used; unsigned long when; } voice[NVOICE];
static unsigned long vclock;
static int chprog[16];
static int chvol[16];    /* CC7  channel volume, 0-127 */
static int chpan[16];    /* CC10 channel pan, 64 = centre */
static int chexpr[16];   /* CC11 expression, 0-127 */
static int chsust[16];   /* CC64 sustain pedal */
static int vheld[NVOICE];/* note-off arrived while the pedal was down */

/* scale = the zone's scaleTuning (100 = normal; ALSA emux calc_pitch scales
 * the note-root term by it, so a 0 plays every key at the root pitch) */
static unsigned note_to_ip(int note, int root, int cents, int scale)
{
    long ip = 0xE000L + (((long)(note - root) * 4096L) / 12L) * scale / 100L
                      + ((long)cents * 4096L) / 1200L;
    if (ip < 0) ip = 0;
    if (ip > 0xFFFFL) ip = 0xFFFFL;
    return (unsigned)ip;
}

/* The voice recipe follows ALSA's emu8000 driver (sound/isa/sb/
 * emu8000_callback.c start_voice/trigger_voice + sound/synth/emux/
 * emux_synth.c setup_voice/calc_volume, GPL-2.0), which is the only
 * hardware-proven reference for this chip. Where this file used to differ -
 * volume/filter targets, the attenuation curve, filterQ - instruments came
 * out audibly wrong. */
static void play_zone(int ch, SF2ZP s, int note, int vel, int mch, int drum)
{
    unsigned ip, pt, at, acut, ft;
    int vol, pv;

    ip = note_to_ip(note, (int)s->root, s->cents, g_flat ? 100 : (int)s->scale);

    /* attenuation: emux calc_volume's native curve (CC7 x velocity via the
     * tables, plus the zone's own attenuation, corrected by CC11) */
    vol = ((voltab1[chvol[mch]] + voltab2[vel]) * 8) / 3 + (int)s->atten;
    if (vol < 0) vol = 0;      /* clamp BEFORE the expression term: 16-bit
                                * int here, and (0x100-vol)*127 only fits
                                * once vol <= 255 (ALSA clamps after, in
                                * 32-bit arithmetic) */
    if (vol > 255) vol = 255;
    vol += ((0x100 - vol) * expressiontab[chexpr[mch]]) / 128;
    if (vol > 255) vol = 255;
    at = (unsigned)vol;

    /* cutoff: velocity brightens melodic zones with a fast volume attack
     * (emux calc_volume); the FILTER TARGET stays the zone's own cutoff */
    if (!drum && (s->volatkhld & 0xFF) < 0x7D) {
        int a = vel < 70 ? 70 : vel;
        acut = (unsigned)((a * (int)s->cutoff + 0xA0) >> 7);
        if (acut > 255) acut = 255;
    } else
        acut = s->cutoff;
    ft = (unsigned)s->cutoff << 8;

    DCYSUSV(ch, 0x0080);
    VTFT(ch, 0x0000FFFFL); CVCF(ch, 0x0000FFFFL);
    PTRX(ch, 0L); CPF(ch, 0L);
    IP(ch, ip);
    ENVVAL(ch, 0x8000);            /* no mod-envelope delay */
    ATKHLD(ch, g_flat ? 0xFF7F : s->modatkhld);
    DCYSUS(ch, g_flat ? 0xFF00 : s->moddcysus);
    ENVVOL(ch, 0x8000);            /* no vol-envelope delay */
    ATKHLDV(ch, g_flat ? 0xFF7F : s->volatkhld);
    IFATN(ch, g_flat ? 0xFF00 : (unsigned)((acut << 8) | at));
    PEFE(ch, g_flat ? 0x0000 : s->pefe);
    LFO1VAL(ch, 0x8000); LFO2VAL(ch, 0x8000);
    FMMOD(ch, 0x0000); TREMFRQ(ch, 0x0000); FM2FRQ2(ch, 0x0000);
    {   /* apan (PSST 31-24) = LEFT level, aaux (PTRX 7-0) = RIGHT level */
        /* zone pan, shifted by CC10. Our byte is 0 = right, 0xFF = left,
         * while CC10 runs 0 = left .. 127 = right, so CC10 subtracts. */
        pv = g_flat ? 128 : (int)s->pan;
        pv -= (chpan[mch] - 64) * 2;
        if (pv < 0) pv = 0;
        if (pv > 255) pv = 255;
        g_apan = pan_volumes[pv];
        g_aaux = pan_volumes[255 - pv];
        PSST(ch, ((unsigned long)g_apan << 24) | (s->ls - 1L));
    }
    CSL(ch,  s->le - 1L);          /* chorus send (bits 24-31) still 0 - the
                                    * effect engine is not initialised yet */
    CCCA(ch, ((unsigned long)(g_flat ? 0 : (s->modes >> 4)) << 28)
             | (s->st - 1L));      /* filterQ in bits 28-31 */
    R0080(ch, 0L); R00A0(ch, 0L);
    /* "reset volume" (start_voice): the volume TARGET is 0 - amplitude is
     * the envelope engine's job, started by the DCYSUSV write below - and
     * the filter target is the zone's cutoff. The previous full-scale
     * targets (0xFFFF/0xFFFF) drove every voice toward wide-open bright and
     * stepped the volume, which is exactly the onset click: ALSA carries the
     * voltarget variant #if 0'd with "this leads to some clicks". */
    VTFT(ch, (unsigned long)(g_flat ? 0xFF00 : ft));
    CVCF(ch, 0x0000FF00L);
    pt = calc_pt(ip);
    PTRX(ch, ((unsigned long)pt << 16) | (unsigned long)g_aaux);
    CPF(ch, (unsigned long)pt << 16);
    DCYSUSV(ch, g_flat ? 0xFF00 : s->voldcysus);
}

static unsigned vrelease[NVOICE];

static void release_voice(int v)
{
    DCYSUSV(v, vrelease[v] ? vrelease[v] : 0x8020);
    voice[v].used = 0;
    vheld[v] = 0;
}

static void allsil(void)
{
    int c;
    for (c = 0; c < 32; c++) {
        DCYSUSV(c, 0x807F); IFATN(c, 0xFFFF);
        CVCF(c, 0L); VTFT(c, 0L);
    }
}

static int alloc_voice(void)
{
    int i, best = 0;
    unsigned long oldest = 0xFFFFFFFFUL;
    for (i = 0; i < NVOICE; i++)
        if (!voice[i].used) return i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].when < oldest) { oldest = voice[i].when; best = i; }
    return best;
}

void synth_note_off(int ch, int note)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].used && voice[i].ch == ch && voice[i].note == note) {
            if (chsust[ch]) vheld[i] = 1;   /* pedal down: hold it */
            else            release_voice(i);
        }
}

/* CC64 released: let go of everything held on this channel */
static void sustain_off(int ch)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].used && voice[i].ch == ch && vheld[i])
            release_voice(i);
}

void synth_cc(int ch, int cc, int val)
{
    switch (cc) {
    case 7:  chvol[ch] = val; break;
    case 10: chpan[ch] = val; break;
    case 11: chexpr[ch] = val; break;
    case 64:
        chsust[ch] = (val >= 64);
        if (!chsust[ch]) sustain_off(ch);
        break;
    case 120:                       /* all sound off  */
    case 123: {                     /* all notes off  */
        /* Both ignore the sustain pedal, so a voice the pedal is holding
         * goes too. An MPU-401 reset broadcasts CC123 on all sixteen
         * channels; without this case the facade's reset is silently
         * discarded and a stuck note survives it. */
        int i;
        for (i = 0; i < NVOICE; i++)
            if (voice[i].used && voice[i].ch == ch) release_voice(i);
        break;
    }
    default: break;
    }
}

#define MAXLAYER 4

void synth_note_on(int ch, int note, int vel)
{
    /* STATIC, not on the stack, and this is not a style choice.
     *
     * When the synth runs from the resident TSR's interrupt handler, __loadds
     * gives us the right DS but SS stays the CALLER's. In Watcom's small
     * model a near pointer is DS-relative while a local lives in SS, so
     * passing the address of a stack array to sf2_layers() would have it
     * written through one segment and read back through another - garbage
     * zone pointers, and a tick instead of a note.
     *
     * Keeping it in DGROUP sidesteps that. The cost is that synth_note_on is
     * no longer reentrant, which is fine: the player is single-threaded and
     * the TSR guards with a busy flag. The alternative is switching SS to our
     * own stack inside the handler so SS == DS again. */
    static SF2ZP lay[MAXLAYER];
    int n, i, v;

    if (vel == 0) { synth_note_off(ch, note); return; }
    if (!(g_chmask & (1U << ch))) return;   /* masked channel */

    /* exclusiveClass chokes (SF2 8.1.2): a new note in a nonzero class
     * rapidly terminates every sounding drum in the same class.  The kit
     * puts closed/pedal/open hi-hat in class 1 - without this the open
     * hat rings straight through the closed hits. */
    if (ch == 9) {
        int ec = gmap.dexcl[note & 0x7F];
        if (ec)
            for (i = 0; i < NVOICE; i++)
                if (voice[i].used && voice[i].ch == 9
                    && voice[i].note != note
                    && gmap.dexcl[voice[i].note & 0x7F] == ec) {
                    DCYSUSV(i, 0x807F);        /* fastest release */
                    voice[i].used = 0;
                    vheld[i] = 0;
                }
    }

    n = (ch == 9) ? sf2_layers_drum(&gmap, note, lay, MAXLAYER)
                  : sf2_layers(&gmap, chprog[ch], note, lay, MAXLAYER);
    if (n <= 0) return;

    /* one voice per layer - 49 of the 128 presets in this bank stack two or
     * three samples on a single key, and playing only the first sounds wrong */
    for (i = 0; i < n; i++) {
        v = alloc_voice();
        if (voice[v].used) release_voice(v);
        voice[v].ch = ch; voice[v].note = note; voice[v].used = 1;
        voice[v].when = ++vclock;
        vrelease[v] = g_flat ? 0x8020 : lay[i]->volrelease;
        vheld[v] = 0;
        play_zone(v, lay[i], note, vel, ch, ch == 9);
    }
}

void synth_prog(int ch, int prog) { chprog[ch] = prog; }

/* Reset all per-channel MIDI state and the voice pool.
 *
 * This MUST be called after chip_init by every user of the synth. It used to
 * live in the file player's play_file(), which meant the TSR never ran it:
 * chvol[] defaulted to 0 (attenuation 95 rather than 32, about 24 dB down)
 * and chpan[] to 0 (which shifts the pan hard left). The result was a synth
 * that could not make an audible note in any context - easily mistaken for a
 * fault in the resident plumbing.
 */
void synth_reset(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        chprog[i] = 0;
        chvol[i]  = 100;      /* GM default */
        chpan[i]  = 64;       /* centre */
        chexpr[i] = 127;      /* full expression */
        chsust[i] = 0;
    }
    for (i = 0; i < NVOICE; i++) {
        voice[i].used = 0;
        vrelease[i]   = 0;
        vheld[i]      = 0;
    }
    vclock = 0;
}
