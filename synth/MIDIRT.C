#include "MIDIRT.H"

#define RING 512                    /* power of two */
static volatile unsigned char ring[RING];
static volatile unsigned rhead, rtail;
static unsigned ndrop;

/* parser state */
/* SysEx we discard. MT-32-scored games (LucasArts titles especially) upload
 * their own timbres as a large SysEx dump at startup and then select them with
 * ordinary program changes. We drop all of it and play the stock bank's timbre
 * N instead, which is why such a game can produce percussion where it wanted a
 * melodic patch. Counting the traffic tells us whether a given title does this
 * before anyone tries to explain its sound. */
static unsigned nsx;                /* SysEx bytes discarded */
static unsigned nsxmsg;             /* SysEx messages seen   */

static unsigned char run;           /* running status */
static unsigned char d1;            /* first data byte held */
static int need;                    /* data bytes still wanted */
static int have;
static int insysex;

void midi_reset(void)
{
    rhead = rtail = 0;
    ndrop = 0;
    nsx = nsxmsg = 0;
    run = 0; need = have = 0; insysex = 0;
}

unsigned midi_dropped(void) { return ndrop; }
unsigned midi_sysex_bytes(void) { return nsx; }
unsigned midi_sysex_msgs(void)  { return nsxmsg; }

/* Called from the trap handler. Must do the minimum possible. */
void midi_push(unsigned char b)
{
    unsigned nh = (rhead + 1) & (RING - 1);
    if (nh == rtail) { ndrop++; return; }   /* full: drop, never block */
    ring[rhead] = b;
    rhead = nh;
}

static void dispatch(unsigned char st, unsigned char a, unsigned char b)
{
    int ch = st & 0x0F;
    switch (st & 0xF0) {
    case 0x80: synth_note_off(ch, a);            break;
    case 0x90: if (b) synth_note_on(ch, a, b);
               else   synth_note_off(ch, a);     break;
    case 0xB0: synth_cc(ch, a, b);               break;
    case 0xC0: synth_prog(ch, a);                break;
    case 0xA0: case 0xD0: case 0xE0:             break;  /* not yet */
    default:                                     break;
    }
}

/* how many data bytes a channel status takes */
static int datalen(unsigned char st)
{
    switch (st & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    default:              return 2;
    }
}

static void feed(unsigned char b)
{
    /* System real-time: may appear ANYWHERE, even between the data bytes of
     * another message, and must not disturb running status. Active sensing
     * (0xFE) in particular is constant traffic from real gear. */
    if (b >= 0xF8) return;

    if (insysex) {
        nsx++;
        if (b == 0xF7) insysex = 0;
        else if (b & 0x80) { insysex = 0; }   /* malformed: fall through */
        else return;
        if (!(b & 0x80)) return;
    }

    if (b & 0x80) {
        if (b == 0xF0) {
            insysex = 1; nsxmsg++; nsx++;
            run = 0; need = have = 0; return;
        }
        if (b >= 0xF1 && b <= 0xF7) {         /* system common cancels running */
            run = 0; need = have = 0;
            return;
        }
        run  = b;
        need = datalen(b);
        have = 0;
        return;
    }

    if (!run) return;                          /* data with no status */
    if (have == 0 && need >= 1) {
        d1 = b;
        have = 1;
        if (need == 1) { dispatch(run, d1, 0); have = 0; }
        return;
    }
    if (have == 1) {
        dispatch(run, d1, b);
        have = 0;                              /* running status persists */
    }
}

/* Ordinary context. Drains what the trap handler queued. */
int midi_poll(void)
{
    int n = 0;
    while (rtail != rhead) {
        unsigned char b = ring[rtail];
        rtail = (rtail + 1) & (RING - 1);
        feed(b);
        n++;
    }
    return n;
}
