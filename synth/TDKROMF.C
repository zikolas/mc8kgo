/* TDKROMF - find and dump the DMC-9000's silicon SoundFont.
 *
 * The 9000 ships no bank file; its GM bank is a standard SF2 image embedded
 * in the card's 4MB sample ROM (words 0..0x200000). This tool locates it by
 * signature ('RIFF'...'sfbk', both byte orders) and dumps word ranges to a
 * binary file for host-side assembly.
 *
 * Reads are BLOCK-STREAMED the way the vendor driver does it: SMALR once per
 * block, PTR parked at SMLD, then a tight run of data-port reads (SMALR
 * auto-increments on reads). Per-word re-SMALR with pauses between reads
 * loses/duplicates words - the pause lets other sample-memory traffic yank
 * the pipeline. Every block is read twice and must match, so dumps are
 * self-verifying; file I/O happens only between blocks.
 *
 * Usage:  TDKROMF CAL  [addrhex]
 *         TDKROMF SCAN
 *         TDKROMF DUMP <starthex> <counthex> <outfile> [/SWAP]
 *         /IO=hex overrides the window0 probe (240h then 260h).
 */

#include <stdio.h>
#include <conio.h>
#include <stdlib.h>
#include <string.h>
#include <i86.h>

static unsigned W0 = 0;
static unsigned W1 = 0x320;

#define PTRP (W0 + 0x0E)
#define D0   (W0 + 0x04)
#define D1   (W0 + 0x08)
#define CMD(r, c) ((unsigned)(((r) << 5) | (c)))

#define ROMTOP 0x200000L
#define BLK    0x800

static void dly(unsigned n) { while (n--) (void)inp(0x80); }
static void poke(unsigned dp, unsigned cmd, unsigned val)
{ outpw(PTRP, cmd); outpw(dp, val); }
static void pokedw(unsigned dp, unsigned cmd, unsigned long val)
{ outpw(PTRP, cmd); outpw(dp, (unsigned)(val & 0xFFFF)); outpw(dp + 2, (unsigned)(val >> 16)); }
static unsigned peek(unsigned dp, unsigned cmd)
{ outpw(PTRP, cmd); return inpw(dp); }

#define DCYSUSV(c, v) poke(D1, CMD(5, c), v)
#define CPF(c, v)     pokedw(D0, CMD(0, c), v)
#define PTRX(c, v)    pokedw(D0, CMD(1, c), v)
#define CVCF(c, v)    pokedw(D0, CMD(2, c), v)
#define VTFT(c, v)    pokedw(D0, CMD(3, c), v)
#define PSST(c, v)    pokedw(D0, CMD(6, c), v)
#define CSL(c, v)     pokedw(D0, CMD(7, c), v)
#define CCCA(c, v)    pokedw(D1, CMD(0, c), v)
#define SMALR(v)      pokedw(D1, CMD(1, 20), v)
#define SMALW(v)      pokedw(D1, CMD(1, 22), v)
#define SMLD_R()      peek(D1, CMD(1, 26))
#define HWCF1(v)      poke(D1, CMD(1, 29), v)
#define HWCF2(v)      poke(D1, CMD(1, 30), v)
#define HWCF3(v)      poke(D1, CMD(1, 31), v)

static void w1cfg(void) { outp(W1 + 4, 0xC1); outp(W1 + 6, 0x03); }

static int detect(unsigned base)
{
    unsigned h1, h2;
    W0 = base;
    HWCF1(0x0059); HWCF2(0x0020); HWCF3(0x0000);
    h1 = peek(D1, CMD(1, 29));
    h2 = peek(D1, CMD(1, 30));
    return (h1 & 0x7E) == 0x58 && (h2 & 0x03) == 0x03;
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

static void dma_rchan(int ch)
{
    DCYSUSV(ch, 0x80);
    VTFT(ch, 0L); CVCF(ch, 0L);
    PSST(ch, 0L); CSL(ch, 0L);
    CCCA(ch, 0x04000000L);
    PTRX(ch, 0x40000000L);
    CPF(ch, 0x40000000L);
}

static void chip_init(void)
{
    int c;
    for (c = 0; c < 32; c++) DCYSUSV(c, 0x80);
    SMALR(0L); SMALW(0L);
    init_fm();
    for (c = 0; c < 32; c++) DCYSUSV(c, 0x807F);
    HWCF1(0x0038);
    w1cfg();
    dma_rchan(1);
    dly(20000);
    SMALR(0L); dly(2000); (void)SMLD_R(); dly(2000); (void)SMLD_R();
}

static void chip_done(void)
{
    int c;
    for (c = 0; c < 32; c++) DCYSUSV(c, 0x807F);
    HWCF3(0x0000);
}

static unsigned bufa[BLK], bufb[BLK];

/* one word: SMALR + dummy + real with interrupts masked, so nothing can
 * land between the address latch and the pipeline pops (the VXD brackets
 * its EMU access with cli/sti the same way). ~10us blind - the 16550 FIFO
 * rides it out. Streamed pops without re-SMALR outrun the refill. */
/* Sentinel-anchored read. The SMLD pipeline races: the first pop after
 * SMALR returns either the previous register content (stale) or, when the
 * prefetch wins, the target word itself - and which regime you are in
 * drifts, so an open-loop dummy+real read silently shifts one word ahead
 * for whole stretches. Anchor every read instead: v2-read a KNOWN word
 * (0x1C3 in the RIFF header), then require the target's first pop to be
 * that word's known successor ('sf'/'bk'). That proves the stale entry is
 * where it belongs, so the second pop is the target. Anything else: retry. */
#define SENT_A 0x1C3L
#define SENT_V 0x3F00
#define SENT_1 0x7366
#define SENT_2 0x626B

static unsigned rdw(unsigned long addr)
{
    unsigned p1, p2, s2;
    int t;
    p2 = 0;
    for (t = 0; t < 24; t++) {
        _disable();
        SMALR(SENT_A);
        (void)SMLD_R();
        s2 = SMLD_R();
        _enable();
        if (s2 != SENT_V) continue;
        _disable();
        SMALR(addr);
        p1 = SMLD_R();
        p2 = SMLD_R();
        _enable();
        if (p1 == SENT_1 || p1 == SENT_2) return p2;
    }
    return p2;
}

static void readblk(unsigned long start, unsigned n, unsigned *buf)
{
    unsigned i;
    for (i = 0; i < n; i++)
        buf[i] = rdw(start + i);
}

/* verified block read: two passes must agree; disagreeing words are
 * repaired one at a time (read until two consecutive agree). 1=clean */
static int readblk2(unsigned long start, unsigned n, unsigned *buf)
{
    unsigned i, a, b;
    int t, flaky;
    readblk(start, n, buf);
    readblk(start, n, bufb);
    flaky = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == bufb[i]) continue;
        a = rdw(start + i);
        for (t = 0; t < 12; t++) {
            b = rdw(start + i);
            if (a == b) break;
            a = b;
        }
        buf[i] = a;
        if (t >= 12) flaky = 1;
    }
    return !flaky;
}

static unsigned bswap(unsigned v) { return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8); }

/* full signature at w[0..5]: 0=no, 1=straight LE, 2=byte-swapped */
static int sigat(unsigned *w)
{
    if (w[0] == 0x4952 && w[1] == 0x4646 && w[4] == 0x6673 && w[5] == 0x6B62)
        return 1;
    if (w[0] == 0x5249 && w[1] == 0x4646 && w[4] == 0x7366 && w[5] == 0x626B)
        return 2;
    return 0;
}

static unsigned long peekdw(unsigned dp, unsigned cmd)
{
    unsigned lo, hi;
    outpw(PTRP, cmd);
    lo = inpw(dp);
    hi = inpw(dp + 2);
    return ((unsigned long)hi << 16) | lo;
}


static void do_cal(unsigned long base)
{
    unsigned i;
    int ok;
    ok = readblk2(base, BLK, bufa);
    printf("CAL @ %06lX: streamed double-read %s\n",
           base, ok ? "STABLE" : "UNSTABLE after 5 tries");
    printf("w[0..F]: ");
    for (i = 0; i < 16; i++) printf("%04X ", bufa[i]);
    printf("\n");
}

/* protocol shoot-out against known data (RIFF sig at 1C0h:
 * 5249 4646 xxxx xxxx 7366 626B). Which variant returns truth? */
static void do_pr2(unsigned long a)
{
    unsigned v[8];
    int i, k;
    static unsigned waits[3] = { 0, 0x20, 0x400 };

    for (k = 0; k < 3; k++) {
        _disable();
        SMALR(a);
        if (waits[k]) dly(waits[k]);
        for (i = 0; i < 6; i++) v[i] = SMLD_R();
        _enable();
        printf("burst wait %4X: ", waits[k]);
        for (i = 0; i < 6; i++) printf("%04X ", v[i]);
        printf("\n");
    }
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 6; i++) {
            _disable();
            SMALR(a + i);
            if (waits[k]) dly(waits[k]);
            v[i] = SMLD_R();
            _enable();
        }
        printf("1pop  wait %4X: ", waits[k]);
        for (i = 0; i < 6; i++) printf("%04X ", v[i]);
        printf("\n");
    }
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 6; i++) {
            _disable();
            SMALR(a + i);
            if (waits[k]) dly(waits[k]);
            (void)SMLD_R();
            v[i] = SMLD_R();
            _enable();
        }
        printf("2pop  wait %4X: ", waits[k]);
        for (i = 0; i < 6; i++) printf("%04X ", v[i]);
        printf("\n");
    }
}

/* pipeline mechanics probe: what does SMALR read back around pops? */
static void do_pipe(unsigned long base)
{
    unsigned long r;
    unsigned v;
    int k, t;
    for (t = 0; t < 3; t++) {
        _disable();
        SMALR(base);
        r = peekdw(D1, CMD(1, 20));
        _enable();
        printf("try %d: SMALR(%06lX) rb %08lX", t, base, r);
        for (k = 0; k < 6; k++) {
            _disable();
            v = SMLD_R();
            r = peekdw(D1, CMD(1, 20));
            _enable();
            printf(" | pop %04X rb %08lX", v, r);
        }
        printf("\n");
    }
}

static void do_scan(void)
{
    unsigned long blk, found[8];
    unsigned n, i, j;
    int ord, hits, dup, unstable;
    hits = 0; unstable = 0;
    printf("SCAN 0-%06lX blk %X\n", ROMTOP, BLK);
    for (blk = 0; blk < ROMTOP; blk += BLK - 8) {
        n = (unsigned)((ROMTOP - blk) > BLK ? BLK : (ROMTOP - blk));
        if ((blk & 0xFFFFL) < (BLK - 8)) printf(".");
        if (!readblk2(blk, n, bufa)) { unstable++; printf("!"); continue; }
        if (n < 6) break;
        for (i = 0; i + 6 <= n; i++) {
            ord = sigat(bufa + i);
            if (!ord) continue;
            dup = 0;
            for (j = 0; j < (unsigned)hits; j++)
                if (found[j] == blk + i) dup = 1;
            if (dup) continue;
            {
                unsigned w2 = bufa[i + 2], w3 = bufa[i + 3];
                unsigned long sz;
                if (ord == 2) { w2 = bswap(w2); w3 = bswap(w3); }
                sz = (unsigned long)w2 | ((unsigned long)w3 << 16);
                printf("\nHIT word %06lX (byte %07lX) %s RIFF size %lu\n",
                       blk + i, (blk + i) << 1,
                       ord == 1 ? "straight" : "SWAPPED", sz);
            }
            found[hits] = blk + i;
            if (++hits >= 8) { printf("hit cap\n"); return; }
        }
    }
    printf("\n%d hit(s), %d unstable block(s)\n", hits, unstable);
}

static void do_dump(unsigned long start, unsigned long count, char *fn, int swap)
{
    FILE *f;
    unsigned long done;
    unsigned n, i, v;
    int bad;
    f = fopen(fn, "wb");
    if (!f) { printf("cannot open %s\n", fn); return; }
    printf("DUMP %06lX + %lX -> %s%s\n", start, count, fn, swap ? " swapped" : "");
    bad = 0;
    for (done = 0; done < count; done += n) {
        n = (unsigned)((count - done) > BLK ? BLK : (count - done));
        if (!readblk2(start + done, n, bufa)) { bad++; printf("!"); }
        for (i = 0; i < n; i++) {
            v = bufa[i];
            if (swap) v = bswap(v);
            fputc(v & 0xFF, f);
            fputc((v >> 8) & 0xFF, f);
        }
        if ((done & 0x3FFFL) == 0) printf(".");
    }
    fclose(f);
    printf("\nwrote %lu words, %d unstable block(s)\n", count, bad);
}

int main(int argc, char **argv)
{
    unsigned io = 0;
    int i, mode = 0, swap = 0;
    char *pos[5];
    int npos = 0;

    setbuf(stdout, NULL);
    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-')) {
            if (argv[i][1] == 'I' || argv[i][1] == 'i')
                io = (unsigned)strtoul(strchr(argv[i], '=') ?
                        strchr(argv[i], '=') + 1 : argv[i] + 3, (char **)0, 16);
            if (argv[i][1] == 'S' || argv[i][1] == 's')
                swap = 1;
        } else if (npos < 5)
            pos[npos++] = argv[i];
    }
    if (npos < 1) {
        printf("usage: TDKROMF CAL [addrhex] | SCAN |\n");
        printf("       DUMP <starthex> <counthex> <file> [/SWAP]\n");
        printf("       /IO=hex window0 override\n");
        return 1;
    }
    mode = pos[0][0];

    if (io) {
        if (!detect(io)) { printf("no EMU at %03Xh\n", io); return 1; }
    } else if (!detect(0x240) && !detect(0x260)) {
        printf("no EMU at 240h or 260h - enabler loaded?\n");
        return 1;
    }
    printf("EMU window %03Xh (HWCF %04X %04X)\n",
           W0, peek(D1, CMD(1, 29)), peek(D1, CMD(1, 30)));
    chip_init();

    if (mode == 'C' || mode == 'c') {
        do_cal(npos > 1 ? strtoul(pos[1], (char **)0, 16) : 0x1000L);
    } else if (mode == 'P' || mode == 'p') {
        if (pos[0][1] == '2')
            do_pr2(npos > 1 ? strtoul(pos[1], (char **)0, 16) : 0x1C0L);
        else
            do_pipe(npos > 1 ? strtoul(pos[1], (char **)0, 16) : 0x1000L);
    } else if (mode == 'S' || mode == 's') {
        do_scan();
    } else if (mode == 'D' || mode == 'd') {
        if (npos < 4) { printf("DUMP <starthex> <counthex> <file>\n"); chip_done(); return 1; }
        do_dump(strtoul(pos[1], (char **)0, 16),
                strtoul(pos[2], (char **)0, 16),
                pos[3], swap);
    } else
        printf("unknown mode %s\n", pos[0]);

    chip_done();
    return 0;
}
