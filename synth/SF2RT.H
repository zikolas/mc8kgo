/* sf2rt - runtime SoundFont 2 preset-map reader (see SF2RT.C) */
#ifndef SF2RT_H
#define SF2RT_H

/* Enough for the largest mode. Measured from the MC-8000's own SoundFont:
 * General MIDI needs 884 zones (819 melodic + 65 drums), but the MT-32 map at
 * bank 127 needs 1360 (1276 + an 84-zone CM-64/32 kit). At 900 the MT-32 mode
 * silently truncated - bank 128 precedes bank 127 in the file, so the kit
 * loaded whole and roughly 460 melodic zones fell off the end, leaving the
 * higher timbres thin or missing with nothing to say so.
 *
 * The table lives in its own DOS block (sf2_load allocates it UMB-first and
 * resizes it to the parsed count), so this cap costs nothing resident - it
 * only bounds the transient allocation. */
#define SF2_MAXZONE 1400

#ifndef __WATCOMC__
#define __far                 /* host-side validation builds have no far */
#endif

#define SF2_OK       0
#define SF2_ENOFILE -1
#define SF2_EPARSE  -2
#define SF2_ENOMEM  -3

/* Where the sample sits in the card's ROM. Zones quote these rather than
 * carrying them: a preset's key-range zones nearly all share one sample,
 * so on the DMC-9000's own bank 1281 zones reference just 448 distinct
 * entries. 'en' is not here because nothing reads it - the EMU loops
 * between ls and le and the envelope ends the note. */
typedef struct {
    unsigned long st, ls, le;
} SF2SMPE;

typedef struct {
    unsigned char lo, hi, root, modes;   /* modes: sample flags in bits 0-1,
                                          * filterQ (0-15) in bits 4-7 - the
                                          * zone table cannot afford a byte */
    unsigned      sid;                   /* index into SF2MAP.samp */
    int  cents;                   /* coarse*100 + fine + pitchCorrection */
    unsigned char atten;          /* IFATN low byte  */
    unsigned char cutoff;         /* IFATN high byte */
    unsigned char pan;            /* PSST bits 24-31 */
    unsigned char scale;          /* scaleTuning, 100 = normal (stored 0-200) */
    unsigned      volatkhld;      /* ATKHLDV */
    unsigned      voldcysus;      /* DCYSUSV at trigger */
    unsigned      volrelease;     /* DCYSUSV at note-off */
    unsigned      pefe;           /* PEFE: modEnv -> pitch / filter cutoff */
    unsigned      modatkhld;      /* ATKHLD  (modulation envelope) */
    unsigned      moddcysus;      /* DCYSUS  (modulation envelope) */
} SF2ZONE;

typedef struct {
    unsigned long st, en, ls, le;
    unsigned char root;
    int pc;                       /* pitchCorrection, signed cents */
} SF2SMP;

typedef SF2ZONE __far *SF2ZP;

/* Where a zone's sample lives. Read through these, never through the
 * fields: the addresses are about to move into a shared table (many
 * zones quote the same sample) and only these three call sites should
 * have to know. */
#define SF2_ST(m, z)  ((m)->samp[(z)->sid].st)
#define SF2_LS(m, z)  ((m)->samp[(z)->sid].ls)
#define SF2_LE(m, z)  ((m)->samp[(z)->sid].le)

typedef struct {
    unsigned short first[128], count[128];   /* the melodic bank        */
    unsigned short dfirst, dcount;           /* the chosen drum kit     */
    unsigned char  dexcl[128];               /* drum note -> exclusiveClass
                                              * choke group (0 = none)  */
    unsigned       nzone;
    unsigned       nsamp;                    /* entries in samp[]       */
    unsigned       zseg;                     /* the zone table's own DOS
                                              * block (UMB when one is
                                              * linked), resized to nzone
                                              * after the parse - NOT in
                                              * DGROUP, NOT in the TSR's
                                              * kept image               */
    SF2ZP          zone;
    SF2SMPE __far *samp;                     /* the shared sample table,
                                              * inside the same block,
                                              * immediately after zone[] */
} SF2MAP;

/* Which banks sf2_load builds the map from.
 *
 * GM is melodic bank 0 with the "Standard" kit at bank 128 program 0. But a
 * game set to MT-32 output sends MT-32 TIMBRE numbers, not GM program numbers,
 * so resolving them against bank 0 gives the wrong instrument for every single
 * program no matter how good the rest of the synthesis is. The MC-8000's
 * SoundFont carries the MT-32 map as a second full 128-preset set at bank 127,
 * in exact MT-32 order, with the matching rhythm set at bank 128 program 127.
 *
 * Same file, different banks - so this is a mode, not a different bank file. */
void     sf2_mode(int mt32);            /* 0 = General MIDI, 1 = MT-32 */

/* Cap the volume-envelope attack index (127 = instantaneous, the SoundFont
 * default for most of this bank). A few steps below that gives a couple of
 * milliseconds of ramp and stops the onset clicking. Call before sf2_load. */
void     sf2_attack_max(int v);

int      sf2_load(char *path, SF2MAP *m);
SF2ZP    sf2_pick(SF2MAP *m, int prog, int note);
SF2ZP    sf2_pick_drum(SF2MAP *m, int note);

/* A preset may LAYER several zones on the same key - 49 of the 128 presets in
 * the MC-8000's bank do. Playing only the first match loses most of the
 * sound, so collect them all and give each its own voice. */
int sf2_layers(SF2MAP *m, int prog, int note, SF2ZP *out, int max);
int sf2_layers_drum(SF2MAP *m, int note, SF2ZP *out, int max);

#endif
