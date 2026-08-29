#include "SYNTH.C"
#include "MIDIRT.C"

/* ---- timing ---- */

static unsigned long tick_base;

static unsigned long now_us(void)
{
    unsigned long ticks;
    unsigned cnt;
    unsigned char lo, hi;

    _disable();
    ticks = *(unsigned long far *)MK_FP(0x40, 0x6C);
    outp(0x43, 0x00);                 /* latch counter 0 */
    lo = (unsigned char)inp(0x40);
    hi = (unsigned char)inp(0x40);
    _enable();
    cnt = ((unsigned)hi << 8) | lo;   /* counts DOWN from 65536 */

    /* one BIOS tick = 65536 counts = 54925 us */
    return ticks * 54925UL + (unsigned long)(65536U - cnt) * 54925UL / 65536UL;
}

static void wait_until(unsigned long target)
{
    while ((long)(now_us() - target) < 0L)
        ;
}

/* ---- MIDI file ---- */

static unsigned char far *mid;
static unsigned long midlen;

typedef struct {
    unsigned long pos, end;
    unsigned long next;        /* absolute tick of the next event */
    unsigned char running;
    int done;
} TRACK;

#define MAXTRK 32
static TRACK trk[MAXTRK];
static int ntrk;
static unsigned division;

static unsigned long rd_vlq(unsigned long *p)
{
    unsigned long v = 0;
    unsigned char c;
    do {
        c = mid[(*p)++];
        v = (v << 7) | (c & 0x7F);
    } while (c & 0x80);
    return v;
}

static unsigned long be32(unsigned long p)
{
    return ((unsigned long)mid[p] << 24) | ((unsigned long)mid[p+1] << 16)
         | ((unsigned long)mid[p+2] << 8) | mid[p+3];
}

static int load_mid(char *path)
{
    FILE *f;
    unsigned long p;
    unsigned n;
    int i;

    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    midlen = ftell(f);
    fseek(f, 0, SEEK_SET);
    mid = (unsigned char far *)_fmalloc(midlen + 16);
    if (!mid) { fclose(f); printf("out of memory for %lu bytes\n", midlen); return 0; }
    /* fread into far memory in chunks */
    { unsigned long got = 0; unsigned char buf[512]; size_t k;
      while (got < midlen && (k = fread(buf, 1, sizeof(buf), f)) > 0) {
          unsigned j; for (j = 0; j < k; j++) mid[got + j] = buf[j];
          got += k;
      } }
    fclose(f);

    if (mid[0] != 'M' || mid[1] != 'T' || mid[2] != 'h' || mid[3] != 'd') {
        printf("not a MIDI file\n"); return 0;
    }
    division = ((unsigned)mid[12] << 8) | mid[13];
    n        = ((unsigned)mid[10] << 8) | mid[11];
    p = 8 + be32(4);

    ntrk = 0;
    for (i = 0; i < (int)n && p + 8 <= midlen && ntrk < MAXTRK; i++) {
        unsigned long len = be32(p + 4);
        if (mid[p] == 'M' && mid[p+1] == 'T' && mid[p+2] == 'r' && mid[p+3] == 'k') {
            trk[ntrk].pos = p + 8;
            trk[ntrk].end = p + 8 + len;
            trk[ntrk].running = 0;
            trk[ntrk].done = 0;
            trk[ntrk].next = rd_vlq(&trk[ntrk].pos);
            ntrk++;
        }
        p += 8 + len;
    }
    printf("format %u, %d tracks, division %u\n",
           ((unsigned)mid[8] << 8) | mid[9], ntrk, division);
    return ntrk > 0;
}

/* ---- the event loop ---- */

static void play_file(void)
{
    unsigned long tempo = 500000UL;      /* default 120bpm, us per quarter */
    unsigned long nowtick = 0, t0;
    unsigned long notes = 0;
    int i;

    for (i = 0; i < 16; i++) {
        chprog[i] = 0; chvol[i] = 100; chpan[i] = 64; chsust[i] = 0;
    }
    for (i = 0; i < NVOICE; i++) { voice[i].used = 0; vrelease[i] = 0; vheld[i] = 0; }
    midi_reset();

    t0 = now_us();
    printf("playing (Esc or any key to stop)...\n");

    for (;;) {
        unsigned long best = 0xFFFFFFFFUL;
        int bt = -1, alive = 0;
        unsigned long us;

        for (i = 0; i < ntrk; i++) {
            if (trk[i].done) continue;
            alive = 1;
            if (trk[i].next < best) { best = trk[i].next; bt = i; }
        }
        if (!alive || bt < 0) break;
        if (kbhit()) { getch(); printf("stopped\n"); break; }

        /* wait until this event is due */
        us = t0 + (best * (tempo / division));
        wait_until(us);
        nowtick = best;

        /* consume every event at this tick on this track */
        {
            TRACK *tk = &trk[bt];
            unsigned char st, d1, d2;

            st = mid[tk->pos];
            if (st & 0x80) { tk->pos++; tk->running = st; }
            else           { st = tk->running; }

            if (st == 0xFF) {                       /* meta */
                unsigned char type = mid[tk->pos++];
                unsigned long len = rd_vlq(&tk->pos);
                if (type == 0x51 && len == 3)
                    tempo = ((unsigned long)mid[tk->pos] << 16)
                          | ((unsigned long)mid[tk->pos+1] << 8)
                          | mid[tk->pos+2];
                if (type == 0x2F) tk->done = 1;
                tk->pos += len;
            } else if (st == 0xF0 || st == 0xF7) {  /* sysex - skip */
                unsigned long len = rd_vlq(&tk->pos);
                tk->pos += len;
            } else {
                int cmd = st & 0xF0, ch = st & 0x0F;
                d1 = mid[tk->pos++];
                switch (cmd) {
                /* Emit the raw MIDI bytes through midi_push/midi_poll - the
                 * exact path the MPU-401 TSR will use - rather than calling
                 * the synth directly. This file player is therefore also the
                 * test harness for the TSR. */
                case 0x80: d2 = mid[tk->pos++];
                           midi_push(st); midi_push(d1); midi_push(d2); break;
                case 0x90: d2 = mid[tk->pos++];
                           midi_push(st); midi_push(d1); midi_push(d2);
                           notes++; break;
                case 0xA0: d2 = mid[tk->pos++]; break;
                case 0xB0: d2 = mid[tk->pos++];
                           midi_push(st); midi_push(d1); midi_push(d2); break;
                case 0xC0: midi_push(st); midi_push(d1); break;
                case 0xD0: break;
                case 0xE0: tk->pos++; break;        /* pitch bend: TODO */
                default:   break;
                }
            }

            midi_poll();               /* drain and synthesise */

            if (tk->pos >= tk->end) tk->done = 1;
            else tk->next = best + rd_vlq(&tk->pos);
        }
    }
    printf("%lu notes, final tick %lu, %u bytes dropped\n",
           notes, nowtick, midi_dropped());
}

int main(int argc, char **argv)
{
    static char *deflt = "C:\\WINDOWS\\SYSTEM\\SYNTH2GM.SF2";
    char *sfpath = deflt, *midpath = "C:\\WINDOWS\\CANYON.MID";
    int i, rc;

    setbuf(stdout, NULL);
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '/' || argv[i][0] == '-') {
            if (argv[i][1] == 'h' || argv[i][1] == 'H')
                sscanf(argv[i] + 2, "%x,%x,%x", &g_h1, &g_h2, &g_h3);
            else if (argv[i][1] == 'f' || argv[i][1] == 'F') sfpath = argv[i] + 2;
            else if (argv[i][1] == 'p' || argv[i][1] == 'P') g_flat = 1;
        } else midpath = argv[i];
    }

    printf("TDKPLAY - Standard MIDI File through the card's ROM wavetable\n");
    printf("SoundFont: %s\nMIDI file: %s\n", sfpath, midpath);

    rc = sf2_load(sfpath, &gmap);
    if (rc != SF2_OK) { printf("SoundFont load failed (%d)\n", rc); return 1; }
    printf("preset map: %u zones, drum kit %u zones\n", gmap.nzone, gmap.dcount);

    if (!load_mid(midpath)) return 1;

    chip_init();
    play_file();
    allsil();
    HWCF3(0x0000);
    printf("\ndone\n");
    return 0;
}
