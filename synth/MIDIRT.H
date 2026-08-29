/* midirt - live MIDI byte-stream handling for the TDK DMC-8000 synth.
 *
 * This is the path the MPU-401 TSR will use. MPUSHIM traps 330h/331h and its
 * data path today is "OUT 330h -> forward to UART THR"; we replace that with
 * midi_push(). Everything else in its facade (the reset/UART ACK queue and the
 * 0x80 idle status that stops games' drain loops spinning) stays as it is.
 *
 * WHY A RING BUFFER: synthesising a note costs roughly 25 port writes per
 * voice, and with layering up to four voices per note - about 100 I/O
 * operations. That is far too much to do inside a V86 port-trap handler, and
 * on this machine that is exactly the kind of stall that starves the serial
 * ISR. So the trap handler only enqueues, and returns immediately; the
 * synthesis happens later in ordinary context via midi_poll().
 *
 * The file player uses this same path, so it doubles as the test harness for
 * the TSR rather than being a parallel implementation.
 */
#ifndef MIDIRT_H
#define MIDIRT_H

void midi_reset(void);          /* clear parser + ring */
void midi_push(unsigned char b);/* trap context: enqueue only, never blocks */
int  midi_poll(void);           /* ordinary context: drain and synthesise */
unsigned midi_dropped(void);    /* bytes lost to ring overflow */

/* SysEx traffic we discard. Non-zero means the title is uploading its own
 * timbres, which we cannot reproduce - see the note in MIDIRT.C. */
unsigned midi_sysex_bytes(void);
unsigned midi_sysex_msgs(void);

/* provided by the synth */
void synth_note_on(int ch, int note, int vel);
void synth_note_off(int ch, int note);
void synth_cc(int ch, int cc, int val);
void synth_prog(int ch, int prog);

#endif
