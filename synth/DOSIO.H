/* dosio - the handful of DOS calls this synth needs, done directly.
 *
 * TDKSYN stays resident, so every byte the C library links in stays resident
 * with it. stdio was the single largest tenant: printf's formatter, sscanf's
 * parser and the FILE layer together dwarf the synthesis code they were
 * supporting. None of it is needed - the parser reads fixed-size records by
 * absolute offset (INT 21h AH=3F/42) and the messages are plain text with a
 * few integers in them.
 *
 * So: file access on a raw DOS handle, output through one write, and the two
 * number parsers the command line wants. Host builds map the same names onto
 * stdio so the parser can still be run and diffed off-target.
 */
#ifndef DOSIO_H
#define DOSIO_H

#ifdef __WATCOMC__

/* every program including this uses some of these and not others */
#pragma off (unreferenced)

#include <dos.h>

static int io_h = -1;

static int io_open(char *path)          /* 0 = open, -1 = no such file */
{
    int h;
    if (_dos_open(path, 0, &h)) return -1;   /* 0 = DOS open mode read-only */
    io_h = h;
    return 0;
}

static unsigned io_read(void *buf, unsigned n)
{
    unsigned got;
    if (io_h < 0) return 0;
    if (_dos_read((unsigned)io_h, buf, n, &got)) return 0;
    return got;
}

/* absolute seek; returns 0 on success, so callers read like fseek's */
static int io_seek(long off)
{
    union REGS r;
    if (io_h < 0) return -1;
    r.h.ah = 0x42; r.h.al = 0x00;
    r.x.bx = (unsigned)io_h;
    r.x.cx = (unsigned)(off >> 16);
    r.x.dx = (unsigned)(off & 0xFFFFL);
    int86(0x21, &r, &r);
    return r.x.cflag ? -1 : 0;
}

static long io_size(void)
{
    union REGS r;
    if (io_h < 0) return 0L;
    r.h.ah = 0x42; r.h.al = 0x02;       /* seek from end */
    r.x.bx = (unsigned)io_h;
    r.x.cx = 0; r.x.dx = 0;
    int86(0x21, &r, &r);
    if (r.x.cflag) return 0L;
    return ((long)r.x.dx << 16) | r.x.ax;
}

static void io_close(void)
{
    if (io_h >= 0) _dos_close((unsigned)io_h);
    io_h = -1;
}

static void o_str(char *s)
{
    unsigned n = 0, put;
    while (s[n]) n++;
    if (n) _dos_write(1, s, n, &put);
}

#else   /* host validation build */

#include <stdio.h>
static FILE *io_f;
static int  io_open(char *path) { io_f = fopen(path, "rb"); return io_f ? 0 : -1; }
static unsigned io_read(void *b, unsigned n) { return (unsigned)fread(b, 1, n, io_f); }
static int  io_seek(long off) { return fseek(io_f, off, SEEK_SET); }
static long io_size(void) { long e; fseek(io_f, 0, SEEK_END); e = ftell(io_f); return e; }
static void io_close(void) { if (io_f) fclose(io_f); io_f = 0; }
static void o_str(char *s) { fputs(s, stdout); }

#endif

static int io_getc(void)
{
    unsigned char c;
    return io_read(&c, 1) == 1 ? (int)c : -1;
}

/* ---- the formatting the messages actually use ---- */

static void o_u(unsigned v)             /* unsigned decimal */
{
    char b[8];
    int i = 7;
    b[i] = '\0';
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    o_str(b + i);
}

static void o_d(int v)                  /* signed decimal */
{
    if (v < 0) { o_str("-"); v = -v; }
    o_u((unsigned)v);
}

static void o_x(unsigned v, int digits) /* zero-padded uppercase hex */
{
    char b[8];
    int i = digits;
    b[i] = '\0';
    while (i > 0) {
        int d = (int)(v & 0x0F);
        b[--i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    o_str(b);
}

/* ---- and the two the command line wants ---- */

static unsigned p_hex(char *s)
{
    unsigned v = 0;
    for (;;) {
        char c = *s++;
        if      (c >= '0' && c <= '9') v = (v << 4) | (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
        else return v;
    }
}

static int p_dec(char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

/* /H<a,b,c> - three hex values, comma separated. A field that is absent or
 * empty leaves its variable alone, the way the scanf this replaced did:
 * /H,,7 sets only the third. */
static void p_hex3(char *s, unsigned *a, unsigned *b, unsigned *c)
{
    if (is_hex(*s)) *a = p_hex(s);
    while (*s && *s != ',') s++;
    if (!*s++) return;
    if (is_hex(*s)) *b = p_hex(s);
    while (*s && *s != ',') s++;
    if (!*s++) return;
    if (is_hex(*s)) *c = p_hex(s);
}

#endif
