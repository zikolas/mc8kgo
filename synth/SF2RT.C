/* sf2rt - runtime SoundFont 2 preset-map reader.
 *
 * Reads the user's own SoundFont at startup and builds the GM zone table in
 * memory, so nothing derived from it is embedded in this program or shipped
 * with it. The SoundFont 2.0 format is publicly documented; this parser is
 * original work.
 *
 * The file is 246KB (MC-8000) or 180KB (DMC-9000) and will not fit in a
 * small-model data segment, but it does not need to: every record is fixed
 * size, so we seek per record and keep only the resulting zone table
 * resident. After startup there is no further disk access.
 *
 * The bank's sdta chunk is empty and every sample is type 0x8001 (ROM-mono),
 * so what we extract is purely a map into the card's own sample ROM: key
 * range, root key, tuning and loop points.
 *
 * Two tables come out of it, in one DOS block: the zones, and a shared table
 * of sample addresses the zones index into. A preset's key-range zones nearly
 * all quote the same sample, so that indirection removes most of the table -
 * on the 9000's own bank, 1281 zones reference 448 distinct samples.
 */

#include <string.h>
#include <stdlib.h>
#ifdef __WATCOMC__
#include <dos.h>
#endif
#include "SF2RT.H"
#include "DOSIO.H"

/* ---- chunk locations, found by walking the RIFF tree ----
 * NOTE: locating chunks by searching for the 4-byte id does NOT work - the id
 * 'pgen' matches 4 bytes early elsewhere in the file and yields the chunk's
 * own size as the first generator, producing a wrong but plausible-looking
 * instrument map. The tree must be walked properly. */

typedef struct { long off; unsigned long size; } CHK;

static CHK c_phdr, c_pbag, c_pgen, c_inst, c_ibag, c_igen, c_shdr;

static unsigned long rd32(void)
{
    unsigned char b[4];
    if (io_read(b, 4) != 4) return 0;
    return (unsigned long)b[0] | ((unsigned long)b[1] << 8)
         | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
}

static unsigned rd16(void)
{
    unsigned char b[2];
    if (io_read(b, 2) != 2) return 0;
    return (unsigned)b[0] | ((unsigned)b[1] << 8);
}

static void note_chunk(char *id, long off, unsigned long sz)
{
    if      (!memcmp(id, "phdr", 4)) { c_phdr.off = off; c_phdr.size = sz; }
    else if (!memcmp(id, "pbag", 4)) { c_pbag.off = off; c_pbag.size = sz; }
    else if (!memcmp(id, "pgen", 4)) { c_pgen.off = off; c_pgen.size = sz; }
    else if (!memcmp(id, "inst", 4)) { c_inst.off = off; c_inst.size = sz; }
    else if (!memcmp(id, "ibag", 4)) { c_ibag.off = off; c_ibag.size = sz; }
    else if (!memcmp(id, "igen", 4)) { c_igen.off = off; c_igen.size = sz; }
    else if (!memcmp(id, "shdr", 4)) { c_shdr.off = off; c_shdr.size = sz; }
}

static int walk(long off, long end)
{
    char id[4], typ[4];
    unsigned long sz;

    while (off + 8 <= end) {
        if (io_seek(off)) return 0;
        if (io_read(id, 4) != 4) return 0;
        sz = rd32();
        if (!memcmp(id, "RIFF", 4) || !memcmp(id, "LIST", 4)) {
            if (io_read(typ, 4) != 4) return 0;
            if (!walk(off + 12, off + 8 + (long)sz)) return 0;
        } else {
            note_chunk(id, off + 8, sz);
        }
        off += 8 + (long)sz + (long)(sz & 1);
    }
    return 1;
}

/* ---- record accessors: every record is fixed size, so seek to it ---- */

static void pbag_at(unsigned i, unsigned *gen)
{
    io_seek(c_pbag.off + (long)i * 4);
    *gen = rd16();
}

static void ibag_at(unsigned i, unsigned *gen)
{
    io_seek(c_ibag.off + (long)i * 4);
    *gen = rd16();
}

static void shdr_at(unsigned i, SF2SMP *s)
{
    io_seek(c_shdr.off + (long)i * 46 + 20);
    s->st = rd32(); s->en = rd32(); s->ls = rd32(); s->le = rd32();
    (void)rd32();                       /* sample rate */
    io_seek(c_shdr.off + (long)i * 46 + 40);
    s->root = (unsigned char)io_getc();
    s->pc   = (int)(signed char)io_getc();   /* pitchCorrection, cents */
}

/* Collect generators for one zone into a small sparse list. We only care
 * about a handful of operators, so a fixed array keyed by oper is enough. */
#define G_KEYRANGE   43
#define G_INSTRUMENT 41
#define G_SAMPLEID   53
#define G_SAMPLEMODE 54
#define G_ROOTKEY    58
#define G_COARSE     51
#define G_FINE       52
#define G_FILTERFC    8
#define G_FILTERQ     9
#define G_SCALETUNE  56
#define G_ATTEN      48
#define G_PAN        17
#define G_DELAYVOL   33
#define G_ATTACKVOL  34
#define G_HOLDVOL    35
#define G_DECAYVOL   36
#define G_SUSTAINVOL 37
#define G_RELEASEVOL 38
#define G_MODTOPITCH  7
#define G_MODTOFILT  11
#define G_ATTACKMOD  26
#define G_HOLDMOD    27
#define G_DECAYMOD   28
#define G_SUSTAINMOD 29

/* ---- SF2 generator -> EMU8000 register conversions ----
 * These follow the integer ("adip") formulations, which matter here: the
 * floating-point variants would need a maths library this build does not have
 * and the target has no FPU. */

/* The banks sf2_load draws from; see sf2_mode() in SF2RT.H for why. */
static unsigned g_melbank  = 0;       /* melodic bank                     */
static unsigned g_drumprog = 0;       /* which kit, as a program in bank 128 */

void sf2_mode(int mt32)
{
    g_melbank  = mt32 ? 127u : 0u;    /* MT-32 Group A timbres, in MT-32 order */
    g_drumprog = mt32 ? 127u : 0u;    /* 127 = "CM-64/32 Set", 0 = "Standard"  */
}

/* Fastest attack we will use; 127 is instantaneous. The bank omits
 * attackVolEnv on most zones so it defaults to exactly that, and the samples do
 * not begin at a zero crossing, so an instant attack snaps the voice on and
 * clicks. Backing off a few steps gives a ramp of a couple of milliseconds -
 * inaudible as an envelope, but enough to kill the click.
 *
 * This was tried once before, measured as making "no audible difference" and
 * reverted. It could not have worked then: play_zone was writing CVCF (the
 * CURRENT volume) at full, equal to the target, so there was no ramp for any
 * attack RATE to shape. That is fixed, so the clamp can finally do something.
 * Runtime-settable so it can be tuned by ear without a rebuild - /A=127
 * restores the old instantaneous behaviour for comparison. */
static int g_atkmax = 120;

void sf2_attack_max(int v)
{
    g_atkmax = v < 1 ? 1 : (v > 127 ? 127 : v);
}

static int cv_attack(int tc)          /* timecents -> attack index */
{
    int a, t1;
    if (tc >= 4300) return 1;
    if (tc > -600) {
        a = (tc + 500) / 150;         /* 0..32 for the valid range */
        t1 = ((~a) & 7) | 8;
        a >>= 3;                      /* the reference also adds (a>>16)&7,
                                       * which is always 0 here - and int is
                                       * 16-bit on this target, so that shift
                                       * would be undefined. Omitted. */
        if (a > 15) a = 15;
        t1 >>= a;
        if (t1 < 1) t1 = 1;
        return t1 > g_atkmax ? g_atkmax : t1;
    }
    a = (37 - tc) / 75 + 8;
    if (a >= 128) a = 127;
    return a > g_atkmax ? g_atkmax : a;
}

static int cv_hold(int tc)            /* timecents -> hold index */
{
    long hold, temp;
    int sh;
    if (tc < -5368) return 127;
    hold = ((long)tc + 4130L) << 16;
    hold /= 1200L;
    temp = 0x10000L | (hold & 0xFFFFL);
    hold >>= 16;
    sh = 16 - (int)(hold & 0xFFL);    /* guard: the shift is data-dependent
                                       * and can fall outside 0..31 */
    if (sh < 0) sh = 0;
    if (sh > 31) return 127;
    temp >>= sh;
    temp = 127L - temp;
    if (temp < 0) return 0;
    if (temp > 127) return 127;
    return (int)temp;
}

static int cv_decay(int tc)           /* timecents -> decay/release index */
{
    int d, t1, t2;
    if (tc > 1800) {
        d = (tc - 1800) / 150;        /* 0..206 for the valid range */
        t1 = ((~d) & 7) | 8;
        t2 = d >> 3;
        if (t2 > 15) return 1;
        t1 >>= t2;
        return t1 < 1 ? 1 : t1;
    }
    d = (37 - tc) / 75 + 39;
    return d >= 128 ? 127 : d;
}

static int cv_sustain(int cb)         /* centibels -> sustain level 1..127 */
{
    /* awe_parm.c calc_sustain_adip: LINEAR, 1000 cB = silence.  The old
     * sloped form here dropped awesfx's ">= 1000 returns 1" clamp, so the
     * 45 drum zones that ask to decay to SILENCE (sustainVolEnv = 1000)
     * held a ringing tail at level 52 instead - and 36 of them are LOOPED
     * samples (hats, toms), which then rang forever.  THE percussion bug.
     * (The old comment's "this bank uses 1000 throughout, the linear form
     * silences every note" was measured false on 2026-08-26: most melodic
     * zones use 13/23/0.  The July all-silent symptom came from the broken
     * volume-target programming, fixed since.)
     * Floor of 1, never 0 - awesfx: "sustain level must be greater than
     * zero to be audible", and -94.5 dB is silence for every purpose.
     * long: cb*127 overflows a 16-bit int past cb=258. */
    int up;
    if (cb <= 0) return 127;
    up = 127 - (int)(((long)cb * 127L) / 1000L);
    if (up < 1) up = 1;
    if (up > 127) up = 127;
    return up;
}

/* modEnv -> filter cutoff. 0x80 corresponds to 6 octaves for the mod envelope.
 * long arithmetic: cents*128 reaches ~560000. */
static int cv_cutoff_shift(int cents)
{
    long v = ((long)cents * 128L) / (6L * 1200L);
    if (v < -128L) v = -128L;
    if (v >  127L) v =  127L;
    return (int)(v < 0 ? 0x100L + v : v);
}

/* modEnv -> pitch. 0x80 corresponds to one octave. */
static int cv_pitch_shift(int cents)
{
    long v = ((long)cents * 128L) / 1200L;
    if (v < -128L) v = -128L;
    if (v >  127L) v =  127L;
    return (int)(v < 0 ? 0x100L + v : v);
}

static int cv_cutoff(int abscents)    /* abs cents -> filter cutoff 0-255 */
{
    int c = (abscents + 0xF) / 0x1D - 0x99;
    if (c < 0) return 0;
    if (c > 255) return 255;
    return c;
}

static int cv_atten(int cb)           /* centibels -> attenuation 0-255 */
{
    /* The "adip" formulation awesfx and ALSA use by default (awe_parm.c
     * calc_attenuation_adip). The traditional cb*8/30 attenuates about 2.6x
     * harder than the chip intends, which buries quiet parts and wrecks the
     * balance between layered zones. */
    int a;
    if (cb < 0) return 0;
    a = ((cb + 12) / 24) * 8 / 3;
    if (a > 255) a = 255;
    return a;
}

/* filterQ: centibels of resonance -> the 4-bit Q field (CCCA bits 28-31) */
static int cv_q(int cb)
{
    int q = cb / 12;
    if (q < 0) q = 0;
    if (q > 15) q = 15;
    return q;
}

/* SF2 pan is -500 (full LEFT) .. +500 (full RIGHT).
 * The EMU8000 byte runs the other way: 0 = right, 0xFF = left. Invert. */
static int cv_pan(int p)
{
    int v;
    if (p < -500) p = -500;
    if (p >  500) p =  500;
    v = (int)(((long)(500 - p) * 255L) / 1000L);   /* long: reaches 255000 */
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

typedef struct {
    int have[64];
    int val[64];
} GENS;

/* Allocate the zone table as its own DOS block, UMBs FIRST: with an upper
 * memory provider loaded and linked, the ~34-53KB table costs no
 * conventional memory at all, and either way it stays out of the resident
 * image (the TSR keeps only code + a small DGROUP).  The strategy and the
 * UMB link are restored around the call. */
#ifdef __WATCOMC__
static unsigned zalloc(unsigned paras, unsigned *seg)
{
    union REGS r;
    unsigned rc, strat, link;
    r.x.ax = 0x5800; int86(0x21, &r, &r); strat = r.x.ax;
    r.x.ax = 0x5802; int86(0x21, &r, &r); link = r.h.al;
    r.x.ax = 0x5803; r.x.bx = 1; int86(0x21, &r, &r);
    r.x.ax = 0x5801; r.x.bx = 0x0080;    /* first fit, high THEN low -
                                          * 0x40 is high ONLY and fails
                                          * outright without a big UMB */
    int86(0x21, &r, &r);
    rc = _dos_allocmem(paras, seg);
    r.x.ax = 0x5801; r.x.bx = strat; int86(0x21, &r, &r);
    r.x.ax = 0x5803; r.x.bx = link;  int86(0x21, &r, &r);
    return rc;
}
#endif

static void read_gens(long base, unsigned g0, unsigned g1, GENS *g)
{
    unsigned i, op, am;
    memset(g->have, 0, sizeof(g->have));
    io_seek(base + (long)g0 * 4);
    for (i = g0; i < g1; i++) {
        op = rd16();
        am = rd16();
        /* store SIGNED: generator amounts are 16-bit shorts and get summed
         * across levels below. Range ops (keyRange, sampleID) stay positive. */
        if (op < 64) { g->have[op] = 1; g->val[op] = (int)(short)am; }
    }
}

/* ---- effective generator values across the two SF2 levels ----
 * awesfx (awelib/parsesf.c, add_item_to_table) is the reference:
 *   value generators   (L_INHRT): preset level ADDS to instrument level,
 *                                 the instrument level falling back to the
 *                                 spec default when unset;
 *   selectors          (L_OVWRT): preset level, when set, OVERRIDES;
 *   keyRange           (L_RANGE): the two levels INTERSECT, and an empty
 *                                 intersection drops the zone.
 * Within one level the zone's own generator beats its global zone's. */

static GENS *e_iz, *e_ig, *e_pz, *e_pg;

static int eff(int op, int dflt)
{
    int v = e_iz->have[op] ? e_iz->val[op]
          : (e_ig->have[op] ? e_ig->val[op] : dflt);
    if      (e_pz->have[op]) v += e_pz->val[op];
    else if (e_pg->have[op]) v += e_pg->val[op];
    return v;
}

static int effc(int op, int dflt, int lo, int hi)
{
    int v = eff(op, dflt);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int effo(int op, int dflt)
{
    if (e_pz->have[op]) return e_pz->val[op];
    if (e_pg->have[op]) return e_pg->val[op];
    if (e_iz->have[op]) return e_iz->val[op];
    if (e_ig->have[op]) return e_ig->val[op];
    return dflt;
}

/* the transient layout: zones, then the sample table */
#define SF2_SAMPBASE  (SF2_MAXZONE * (long)sizeof(SF2ZONE))
#define SF2_ARENA     (SF2_SAMPBASE + SF2_MAXZONE * (long)sizeof(SF2SMPE))

/* Intern one sample triple, returning its index. Zones arrive grouped by
 * instrument, so the entry just added is nearly always the right one -
 * checking it first turns this from a scan into a comparison. */
static unsigned samp_intern(SF2MAP *m, unsigned long st,
                            unsigned long ls, unsigned long le)
{
    unsigned i;
    if (m->nsamp) {
        SF2SMPE __far *e = &m->samp[m->nsamp - 1];
        if (e->st == st && e->ls == ls && e->le == le) return m->nsamp - 1;
    }
    for (i = 0; i < m->nsamp; i++)
        if (m->samp[i].st == st && m->samp[i].ls == ls && m->samp[i].le == le)
            return i;
    if (m->nsamp >= SF2_MAXZONE) return 0;      /* cannot happen: one entry
                                                 * per zone at worst */
    m->samp[m->nsamp].st = st;
    m->samp[m->nsamp].ls = ls;
    m->samp[m->nsamp].le = le;
    return m->nsamp++;
}

int sf2_load(char *path, SF2MAP *m)
{
    long fsz;
    unsigned nphdr, ninst, k, i;
    unsigned pbag0, pbag1, ibag0, ibag1, b, w;
    unsigned bank, prog, bagndx, nextbag;
    unsigned instndx, sid;
    GENS pz, pglob, iz, iglob;
    SF2SMP s;
    int kr, cents;

    m->nzone = 0;
    m->dfirst = 0;
    m->dcount = 0;
    memset(m->first, 0, sizeof(m->first));
    memset(m->count, 0, sizeof(m->count));
    memset(m->dexcl, 0, sizeof(m->dexcl));

    /* One block holds both tables: zones from the start, and the sample
     * table parked at SAMPBASE while parsing - the final zone count is
     * not known until the end, so the samples are shuffled down against
     * the zones afterwards and the block resized to the pair. */
    if (m->zseg == 0) {
#ifdef __WATCOMC__
        if (zalloc((unsigned)((SF2_ARENA + 15L) / 16), &m->zseg))
            return SF2_ENOMEM;
        m->zone = (SF2ZP)MK_FP(m->zseg, 0);
#else
        m->zone = (SF2ZP)malloc(SF2_ARENA);
        if (!m->zone) return SF2_ENOMEM;
        m->zseg = 1;
#endif
    }
    m->samp  = (SF2SMPE __far *)((char __far *)m->zone + SF2_SAMPBASE);
    m->nsamp = 0;

    if (io_open(path)) return SF2_ENOFILE;

    fsz = io_size();
    memset(&c_phdr, 0, sizeof(c_phdr));
    c_pbag.off = c_pgen.off = c_inst.off = 0;
    c_ibag.off = c_igen.off = c_shdr.off = 0;
    if (!walk(0, fsz)) { io_close(); return SF2_EPARSE; }
    if (!c_phdr.off || !c_pgen.off || !c_igen.off || !c_shdr.off) {
        io_close(); return SF2_EPARSE;
    }

    nphdr = (unsigned)(c_phdr.size / 38);
    ninst = (unsigned)(c_inst.size / 22);
    if (nphdr < 2) { io_close(); return SF2_EPARSE; }

    for (k = 0; k + 1 < nphdr; k++) {
        io_seek(c_phdr.off + (long)k * 38 + 20);
        prog = rd16();
        bank = rd16();
        bagndx = rd16();
        io_seek(c_phdr.off + (long)(k + 1) * 38 + 24);
        nextbag = rd16();

        if (bank == 128 && prog == g_drumprog) {
            m->dfirst = (unsigned short)m->nzone;
            m->dcount = 0;
        } else if (bank == g_melbank && prog <= 127) {
            m->first[prog] = (unsigned short)m->nzone;
            m->count[prog] = 0;
        } else {
            continue;
        }

        memset(pglob.have, 0, sizeof(pglob.have));
        for (b = bagndx; b < nextbag; b++) {
            pbag_at(b, &pbag0);
            if (b + 1 < c_pbag.size / 4) pbag_at(b + 1, &pbag1);
            else pbag1 = (unsigned)(c_pgen.size / 4);
            read_gens(c_pgen.off, pbag0, pbag1, &pz);

            if (!pz.have[G_INSTRUMENT]) {
                if (b == bagndx) pglob = pz;
                continue;
            }
            instndx = (unsigned)pz.val[G_INSTRUMENT];
            if (instndx + 1 > ninst) continue;

            io_seek(c_inst.off + (long)instndx * 22 + 20);
            ibag0 = rd16();
            io_seek(c_inst.off + (long)(instndx + 1) * 22 + 20);
            ibag1 = rd16();

            memset(iglob.have, 0, sizeof(iglob.have));
            for (w = ibag0; w < ibag1; w++) {
                unsigned ig0, ig1;
                ibag_at(w, &ig0);
                if (w + 1 < c_ibag.size / 4) ibag_at(w + 1, &ig1);
                else ig1 = (unsigned)(c_igen.size / 4);
                read_gens(c_igen.off, ig0, ig1, &iz);

                if (!iz.have[G_SAMPLEID]) {
                    if (w == ibag0) iglob = iz;
                    continue;
                }
                if (m->nzone >= SF2_MAXZONE) break;

                sid = (unsigned)iz.val[G_SAMPLEID];
                shdr_at(sid, &s);
                if (s.en <= s.st) continue;

                e_iz = &iz; e_ig = &iglob; e_pz = &pz; e_pg = &pglob;

                /* keyRange: intersect the two levels; empty drops the zone */
                kr = iz.have[G_KEYRANGE] ? iz.val[G_KEYRANGE]
                   : (iglob.have[G_KEYRANGE] ? iglob.val[G_KEYRANGE] : 0x7F00);
                {
                    int kp = pz.have[G_KEYRANGE] ? pz.val[G_KEYRANGE]
                           : (pglob.have[G_KEYRANGE] ? pglob.val[G_KEYRANGE] : 0x7F00);
                    int lo = kr & 0xFF, hi = (kr >> 8) & 0xFF;
                    if ((kp & 0xFF) > lo)        lo = kp & 0xFF;
                    if (((kp >> 8) & 0xFF) < hi) hi = (kp >> 8) & 0xFF;
                    if (lo > hi) continue;
                    kr = lo | (hi << 8);
                }

                cents = effc(G_COARSE, 0, -120, 120) * 100
                      + effc(G_FINE, 0, -99, 99) + s.pc;

                m->zone[m->nzone].lo    = (unsigned char)(kr & 0xFF);
                m->zone[m->nzone].hi    = (unsigned char)((kr >> 8) & 0xFF);
                m->zone[m->nzone].root  = (unsigned char)effo(G_ROOTKEY, s.root);
                {
                    int sm = effo(G_SAMPLEMODE, 0) & 0x0F;
                    m->zone[m->nzone].modes = (unsigned char)
                        (sm | (cv_q(effc(G_FILTERQ, 0, 0, 960)) << 4));
                    if ((sm & 3) == 0) {
                        /* ONE-SHOT (sampleModes 0): the shdr loop points
                         * are authoring leftovers spanning nearly the whole
                         * sample and MUST be ignored - honouring them turns
                         * every unlooped drum into a machine-gun loop (the
                         * kick's covers 40 ms = a 25 Hz buzz).  The EMU has
                         * no no-loop mode; the E-mu convention is to loop
                         * the interpolation pad just past the sample end.
                         * Measured off the vendor driver: its one-shot
                         * voices park at end+7..9, so the pad is real. */
                        s.ls = s.en + 4;
                        s.le = s.en + 8;
                    }
                }
                m->zone[m->nzone].sid   = samp_intern(m, s.st, s.ls, s.le);
                m->zone[m->nzone].cents = cents;
                {
                    int sc = effc(G_SCALETUNE, 100, 0, 1200);
                    if (sc > 200) sc = 200;
                    m->zone[m->nzone].scale = (unsigned char)sc;
                }
                m->zone[m->nzone].atten  =
                    (unsigned char)cv_atten(effc(G_ATTEN, 0, 0, 1440));
                m->zone[m->nzone].cutoff =
                    (unsigned char)cv_cutoff(effc(G_FILTERFC, 13500, 1500, 13500));
                m->zone[m->nzone].pan =
                    (unsigned char)cv_pan(eff(G_PAN, 0));
                m->zone[m->nzone].volatkhld = (unsigned)
                    ((cv_hold(effc(G_HOLDVOL, -12000, -12000, 5000)) << 8)
                     | cv_attack(effc(G_ATTACKVOL, -12000, -12000, 5000)));
                m->zone[m->nzone].voldcysus = (unsigned)
                    ((cv_sustain(effc(G_SUSTAINVOL, 0, 0, 1440)) << 8)
                     | cv_decay(effc(G_DECAYVOL, -12000, -12000, 5000)));
                m->zone[m->nzone].volrelease = (unsigned)
                    (0x8000 | cv_decay(effc(G_RELEASEVOL, -12000, -12000, 5000)));
                /* Modulation envelope. Without this the filter never opens and
                 * everything sounds muffled: the bank sets a low initial
                 * cutoff and relies on modEnvToFilterFc to sweep it up. */
                m->zone[m->nzone].pefe = (unsigned)
                    ((cv_pitch_shift(effc(G_MODTOPITCH, 0, -12000, 12000)) << 8)
                     | cv_cutoff_shift(effc(G_MODTOFILT, 0, -12000, 12000)));
                m->zone[m->nzone].modatkhld = (unsigned)
                    ((cv_hold(effc(G_HOLDMOD, -12000, -12000, 5000)) << 8)
                     | cv_attack(effc(G_ATTACKMOD, -12000, -12000, 5000)));
                m->zone[m->nzone].moddcysus = (unsigned)
                    ((cv_sustain(effc(G_SUSTAINMOD, 0, 0, 1000)) << 8)
                     | cv_decay(effc(G_DECAYMOD, -12000, -12000, 5000)));
                m->nzone++;
                if (bank == 128) {
                    /* exclusiveClass (L_OVWRT): note -> choke group, used
                     * by synth_note_on to cut open hats etc. (the kit puts
                     * closed/pedal/open hat in class 1) */
                    int ec = effo(57, 0) & 0x7F;
                    if (ec) {
                        int k;
                        for (k = kr & 0xFF; k <= ((kr >> 8) & 0xFF); k++)
                            m->dexcl[k] = (unsigned char)ec;
                    }
                    m->dcount++;
                } else
                    m->count[prog]++;
            }
        }
    }

    io_close();
    if (m->nzone) {
        /* close the gap: the sample table moves down against the zones.
         * dest < src always, so a forward copy is safe. */
        char __far *dst = (char __far *)m->zone
                        + m->nzone * (long)sizeof(SF2ZONE);
        char __far *src = (char __far *)m->samp;
        unsigned long n = m->nsamp * (unsigned long)sizeof(SF2SMPE), k;
        for (k = 0; k < n; k++) dst[k] = src[k];
        m->samp = (SF2SMPE __far *)dst;
#ifdef __WATCOMC__
        {
            unsigned mx;              /* shrink the block to what is used */
            _dos_setblock((unsigned)(((dst - (char __far *)m->zone) + n + 15L)
                                     / 16), m->zseg, &mx);
        }
#endif
        return SF2_OK;
    }
    return SF2_EPARSE;
}

/* Drum kit: the note number selects the instrument, so every zone has a narrow
 * key range and we simply find the one covering this note. */
SF2ZP sf2_pick_drum(SF2MAP *m, int note)
{
    unsigned i;
    for (i = 0; i < m->dcount; i++)
        if (note >= (int)m->zone[m->dfirst+i].lo &&
            note <= (int)m->zone[m->dfirst+i].hi)
            return &m->zone[m->dfirst+i];
    return m->dcount ? &m->zone[m->dfirst] : (SF2ZP)0;
}

SF2ZP sf2_pick(SF2MAP *m, int prog, int note)
{
    unsigned i, f, n;
    if (prog < 0 || prog > 127) return (SF2ZP)0;
    f = m->first[prog];
    n = m->count[prog];
    for (i = 0; i < n; i++)
        if (note >= (int)m->zone[f+i].lo && note <= (int)m->zone[f+i].hi)
            return &m->zone[f+i];
    return n ? &m->zone[f] : (SF2ZP)0;
}

int sf2_layers(SF2MAP *m, int prog, int note, SF2ZP *out, int max)
{
    unsigned i, f, n;
    int k = 0;
    if (prog < 0 || prog > 127) return 0;
    f = m->first[prog];
    n = m->count[prog];
    for (i = 0; i < n && k < max; i++)
        if (note >= (int)m->zone[f+i].lo && note <= (int)m->zone[f+i].hi)
            out[k++] = &m->zone[f+i];
    if (!k && n) out[k++] = &m->zone[f];
    return k;
}

int sf2_layers_drum(SF2MAP *m, int note, SF2ZP *out, int max)
{
    unsigned i;
    int k = 0;
    for (i = 0; i < m->dcount && k < max; i++)
        if (note >= (int)m->zone[m->dfirst+i].lo &&
            note <= (int)m->zone[m->dfirst+i].hi)
            out[k++] = &m->zone[m->dfirst+i];
    return k;
}
