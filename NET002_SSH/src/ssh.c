/*
 * NET002_SSH
 *
 * SSH terminal client for Atari 8-bit via FujiNet Network Device (N:).
 * Opens an SSH connection and acts as a simple interactive console.
 * Received characters are displayed on screen, typed keys are sent
 * to the remote shell.
 *
 * Protocol flow:
 *   1. OPEN  N:SSH://user:pass@host[:port]/  (device 0x71, cmd 0x4F)
 *   2. Loop:
 *      a. Poll STATUS for incoming data       (cmd 0x53)
 *      b. READ available bytes, display them   (cmd 0x52)
 *      c. Check keyboard, WRITE typed char     (cmd 0x57)
 *   3. CLOSE on disconnect (remote close or 'exit' command) (cmd 0x43)
 *
 * Build:  cl65 -t atari -o build/ssh_terminal.com src/ssh_terminal.c
 *
 * Requires: FujiNet (real or FujiNet-PC) with SSH support (libssh).
 *           FujiNet firmware with fnUDP::available() and
 *           sio_status_channel() fixes applied.
 *
 * Controls:
 *   RETURN     - sends CR to remote
 *   BACKSPACE  - sends BS to remote
 *   CTRL+A..Z  - sends control characters
 *
 * Keyboard input uses CIO K: handler directly (not E:) to avoid
 * the screen editor's line-read-on-RETURN behaviour that would
 * inject extra characters into the input stream.
 *
 * Author: Claude / Piotr  2026
 * License: Public domain
 */

#include <stdio.h>
#include <string.h>
#include <atari.h>
#include <peekpoke.h>

/* ------------------------------------------------------------------ */
/* SSH connection settings                                            */
/* ------------------------------------------------------------------ */
#define SSH_PORT 22
#define INPUT_MAX 63
#define TERM_ROWS 24

/* Hardcoded login defaults (override with -DHARDCODED_HOST=\"...\" etc.) */
#if defined(HARDCODED_LOGIN) || defined(HARDCODED_KEYAUTH)
#ifndef HARDCODED_HOST
#define HARDCODED_HOST "192.168.1.20"
#endif
#ifndef HARDCODED_USER
#define HARDCODED_USER "test"
#endif
#endif
#ifdef HARDCODED_LOGIN
#ifndef HARDCODED_PASS
#define HARDCODED_PASS "test"
#endif
#endif

/* RMARGN ($0053) — right margin of screen editor.
   39 = 40-column mode, 79 = 80-column mode (e.g. VBXE CON 80). */
#define RMARGN 0x0053

/* ------------------------------------------------------------------ */
/* Atari SIO registers                                                */
/* ------------------------------------------------------------------ */
#define DDEVIC  0x0300
#define DUNIT   0x0301
#define DCOMND  0x0302
#define DSTATS  0x0303
#define DBUFLO  0x0304
#define DBUFHI  0x0305
#define DTIMLO  0x0306
#define DBYTLO  0x0308
#define DBYTHI  0x0309
#define DAUX1   0x030A
#define DAUX2   0x030B

/* ------------------------------------------------------------------ */
/* FujiNet Network Device constants                                   */
/* ------------------------------------------------------------------ */
#define FN_DEV_NET    0x71

#define NETCMD_OPEN   0x4F
#define NETCMD_CLOSE  0x43
#define NETCMD_READ   0x52
#define NETCMD_WRITE  0x57
#define NETCMD_STATUS 0x53

#define MODE_READWRITE 12
#define TRANS_NONE     0

#define SIO_READ  0x40
#define SIO_WRITE 0x80
#define SIO_NONE  0x00

/* ------------------------------------------------------------------ */
/* PROCEED interrupt (FujiNet signals data available via SIO PROCEED)  */
/* ------------------------------------------------------------------ */
#define VPRCED   0x0202          /* OS proceed interrupt vector */
#define PACTL    0xD302          /* PIA port A control, bit 0 = PROCEED enable */

volatile unsigned char trip;     /* set to 1 by interrupt handler */
extern void ih(void);            /* assembly handler in intr.s */

static unsigned int old_vprced;
static unsigned char old_pactl_bit;

/* ------------------------------------------------------------------ */
/* CIO constants for direct K: keyboard access                        */
/* ------------------------------------------------------------------ */
#define CIOV      0xE456
#define CIO_OPEN  0x03
#define CIO_CLOSE 0x0C
#define CIO_GETCHR 0x07

/* IOCB 6 at $03A0 (IOCB base $0340 + 6*16) */
#define IOCB6_BASE  0x03A0
#define IOCB6_COM   (IOCB6_BASE + 0x02)
#define IOCB6_BAL   (IOCB6_BASE + 0x04)
#define IOCB6_BAH   (IOCB6_BASE + 0x05)
#define IOCB6_BLL   (IOCB6_BASE + 0x08)
#define IOCB6_BLH   (IOCB6_BASE + 0x09)
#define IOCB6_AX1   (IOCB6_BASE + 0x0A)
#define IOCB6_AX2   (IOCB6_BASE + 0x0B)

/* CH register — last key pressed ($FF = none) */
#define CH 0x02FC

/* ------------------------------------------------------------------ */
/* Buffers                                                            */
/* ------------------------------------------------------------------ */
#define RXBUF_SIZE 4096

static unsigned char devicespec[256];
static unsigned char rxbuf[RXBUF_SIZE];
static unsigned char txbuf[1];
static unsigned char status_buf[4];

/* Used by keyboard_get() to retrieve CIO result */
static unsigned char kb_result;

/* Connection parameters entered by user */
static char input_host[INPUT_MAX + 1];
static char input_user[INPUT_MAX + 1];
static char input_pass[INPUT_MAX + 1];

/* ------------------------------------------------------------------ */
/* Atari screen editor registers for cursor positioning               */
/* ------------------------------------------------------------------ */
#define ROWCRS  0x0054          /* Cursor row (0-23) */
#define COLCRS  0x0055          /* Cursor column low byte */
#define COLCRH  0x0056          /* Cursor column high byte (always 0) */

/* ------------------------------------------------------------------ */
/* VT100 escape sequence parser state                                 */
/* ------------------------------------------------------------------ */
#define ANSI_NORMAL  0
#define ANSI_ESC     1
#define ANSI_CSI     2
#define ANSI_OSC     3

static unsigned char ansi_state = ANSI_NORMAL;

/* Screen width — set from RMARGN at startup */
static unsigned char screen_cols = 80;

/* CSI parameter collection */
#define CSI_MAX_PARAMS 8
static unsigned int  csi_params[CSI_MAX_PARAMS];
static unsigned char csi_nparam;
static unsigned char csi_private;    /* '?' prefix seen */
static unsigned char csi_collecting; /* currently building a number */

/* Saved cursor position for ESC 7/8 and CSI s/u */
static unsigned char saved_row = 0;
static unsigned char saved_col = 0;

/* Scroll region boundaries (0-indexed, inclusive).
   VT100 ESC[r sets these; default = full screen. */
static unsigned char scroll_top = 0;
static unsigned char scroll_bottom = TERM_ROWS - 1;  /* 23 */

/* ------------------------------------------------------------------ */
/* Pre-filled buffers for CIO block writes                            */
/* ------------------------------------------------------------------ */
static unsigned char blk_left[80];   /* 0x1E cursor left  */
static unsigned char blk_right[80];  /* 0x1F cursor right */
static unsigned char blk_up[24];     /* 0x1C cursor up    */
static unsigned char blk_down[24];   /* 0x1D cursor down  */
static unsigned char blk_space[80];  /* 0x20 space        */

/* ------------------------------------------------------------------ */
/* Wait for N VBI frames (~16.7ms NTSC, ~20ms PAL)                   */
/* ------------------------------------------------------------------ */
static void wait_vbi(unsigned int frames)
{
    unsigned char v;
    while (frames > 0) {
        v = PEEK(0x14);
        while (PEEK(0x14) == v)
            ;
        --frames;
    }
}

/* ------------------------------------------------------------------ */
/* Call SIO vector                                                    */
/* ------------------------------------------------------------------ */
static void call_siov(void)
{
    __asm__("jsr $E459");
}

/* ------------------------------------------------------------------ */
/* Direct keyboard access via CIO K: handler (bypasses E: editor)     */
/* ------------------------------------------------------------------ */
static const char kdev[] = "K:\x9B";

static void keyboard_open(void)
{
    POKE(IOCB6_COM, CIO_OPEN);
    POKE(IOCB6_BAL, (unsigned)kdev & 0xFF);
    POKE(IOCB6_BAH, ((unsigned)kdev >> 8) & 0xFF);
    POKE(IOCB6_AX1, 0x04);  /* Read mode */
    POKE(IOCB6_AX2, 0x00);
    __asm__("ldx #$60");
    __asm__("jsr $E456");
}

static void keyboard_close(void)
{
    POKE(IOCB6_COM, CIO_CLOSE);
    __asm__("ldx #$60");
    __asm__("jsr $E456");
}

/* Check if a key is available (reads CH register directly) */
static unsigned char key_available(void)
{
    return PEEK(CH) != 0xFF;
}

/* Read one ATASCII character via K: GET BYTE */
static unsigned char keyboard_get(void)
{
    POKE(IOCB6_COM, CIO_GETCHR);
    POKE(IOCB6_BLL, 0x00);  /* length=0 → GET BYTE mode */
    POKE(IOCB6_BLH, 0x00);
    __asm__("ldx #$60");
    __asm__("jsr $E456");
    __asm__("sta %v", kb_result);
    return kb_result;
}

/* ------------------------------------------------------------------ */
/* SIO helper: common DCB fields for network device                   */
/* ------------------------------------------------------------------ */
static void sio_setup_net(void)
{
    POKE(DDEVIC, FN_DEV_NET);
    POKE(DUNIT, 1);
    POKE(DTIMLO, 30);
}

/* ------------------------------------------------------------------ */
/* OPEN                                                               */
/* ------------------------------------------------------------------ */
static unsigned char net_open(void)
{
    static unsigned char open_buf[256];
    memset(open_buf, 0, sizeof(open_buf));
    memcpy(open_buf, devicespec, strlen((char *)devicespec));

    sio_setup_net();
    POKE(DCOMND, NETCMD_OPEN);
    POKE(DSTATS, SIO_WRITE);
    POKE(DBUFLO, (unsigned)open_buf & 0xFF);
    POKE(DBUFHI, ((unsigned)open_buf >> 8) & 0xFF);
    POKE(DBYTLO, 0x00);
    POKE(DBYTHI, 0x01);
    POKE(DAUX1, MODE_READWRITE);
    POKE(DAUX2, TRANS_NONE);

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* WRITE                                                              */
/* ------------------------------------------------------------------ */
static unsigned char net_write(unsigned char *buf, unsigned int len)
{
    sio_setup_net();
    POKE(DCOMND, NETCMD_WRITE);
    POKE(DSTATS, SIO_WRITE);
    POKE(DBUFLO, (unsigned)buf & 0xFF);
    POKE(DBUFHI, ((unsigned)buf >> 8) & 0xFF);
    POKE(DBYTLO, len & 0xFF);
    POKE(DBYTHI, (len >> 8) & 0xFF);
    POKE(DAUX1, len & 0xFF);
    POKE(DAUX2, (len >> 8) & 0xFF);

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* STATUS (4 bytes: avail_lo, avail_hi, conn, err)                    */
/* ------------------------------------------------------------------ */
static unsigned char net_status(void)
{
    sio_setup_net();
    POKE(DCOMND, NETCMD_STATUS);
    POKE(DSTATS, SIO_READ);
    POKE(DBUFLO, (unsigned)status_buf & 0xFF);
    POKE(DBUFHI, ((unsigned)status_buf >> 8) & 0xFF);
    POKE(DBYTLO, 4);
    POKE(DBYTHI, 0);
    POKE(DAUX1, 0);
    POKE(DAUX2, 0);

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* READ                                                               */
/* ------------------------------------------------------------------ */
static unsigned char net_read(unsigned char *buf, unsigned int len)
{
    sio_setup_net();
    POKE(DCOMND, NETCMD_READ);
    POKE(DSTATS, SIO_READ);
    POKE(DBUFLO, (unsigned)buf & 0xFF);
    POKE(DBUFHI, ((unsigned)buf >> 8) & 0xFF);
    POKE(DBYTLO, len & 0xFF);
    POKE(DBYTHI, (len >> 8) & 0xFF);
    POKE(DAUX1, len & 0xFF);
    POKE(DAUX2, (len >> 8) & 0xFF);

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* CLOSE                                                              */
/* ------------------------------------------------------------------ */
static unsigned char net_close(void)
{
    sio_setup_net();
    POKE(DCOMND, NETCMD_CLOSE);
    POKE(DSTATS, SIO_NONE);
    POKE(DBUFLO, 0);
    POKE(DBUFHI, 0);
    POKE(DBYTLO, 0);
    POKE(DBYTHI, 0);
    POKE(DAUX1, 0);
    POKE(DAUX2, 0);
    POKE(DTIMLO, 2);   /* short timeout — device may already be gone */

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* PROCEED interrupt install / remove                                 */
/* ------------------------------------------------------------------ */
static void install_proceed(void)
{
    trip = 0;
    old_vprced = PEEKW(VPRCED);
    old_pactl_bit = PEEK(PACTL) & 1;
    POKE(PACTL, PEEK(PACTL) & ~1);     /* disable PROCEED int */
    POKEW(VPRCED, (unsigned)ih);        /* install handler */
    POKE(PACTL, PEEK(PACTL) | 1);      /* enable PROCEED int */
}

static void remove_proceed(void)
{
    POKE(PACTL, PEEK(PACTL) & ~1);     /* disable PROCEED int */
    POKEW(VPRCED, old_vprced);          /* restore original */
    if (old_pactl_bit)
        POKE(PACTL, PEEK(PACTL) | 1);
    trip = 0;
}

/* ------------------------------------------------------------------ */
/* VT100 CSI helpers                                                  */
/* ------------------------------------------------------------------ */
static void csi_reset(void)
{
    unsigned char i;
    for (i = 0; i < CSI_MAX_PARAMS; ++i)
        csi_params[i] = 0;
    csi_nparam = 0;
    csi_private = 0;
    csi_collecting = 0;
}

/* Return CSI parameter idx, or defval if missing/zero */
static unsigned int csi_param(unsigned char idx, unsigned int defval)
{
    if (idx >= csi_nparam || csi_params[idx] == 0)
        return defval;
    return csi_params[idx];
}

/* Forward declaration — defined after net_write */
static void net_send_str(const char *s);

/* ------------------------------------------------------------------ */
/* CIO block write: send N bytes to E: (IOCB 0) in a single PUT      */
/* BYTES call.  Much faster than N individual putchar() calls because  */
/* only one CIO dispatch overhead instead of N.                       */
/* ------------------------------------------------------------------ */
static void eput_block(const unsigned char *buf, unsigned char len)
{
    if (len == 0) return;
    /* IOCB 0 is at $0340 */
    POKE(0x0342, 0x0B);                          /* ICCOM = PUT BYTES */
    POKE(0x0344, (unsigned)buf & 0xFF);           /* ICBAL */
    POKE(0x0345, ((unsigned)buf >> 8) & 0xFF);    /* ICBAH */
    POKE(0x0348, len);                            /* ICBLL */
    POKE(0x0349, 0);                              /* ICBLH */
    __asm__("ldx #$00");                          /* IOCB 0 */
    __asm__("jsr $E456");                         /* CIOV  */
}

/* ------------------------------------------------------------------ */
/* Move cursor to absolute position using ATASCII cursor movement.    */
/* Uses cursor up/down/left/right through E: handler — works with     */
/* any screen handler including VBXE CON 80 (POKE ROWCRS/COLCRS does  */
/* not reliably control the cursor in VBXE).                          */
/* Uses CIO block writes for speed.                                   */
/* ------------------------------------------------------------------ */
static void cursor_moveto(unsigned char target_row, unsigned char target_col)
{
    unsigned char cur, n;

    /* Vertical movement */
    cur = PEEK(ROWCRS);
    if (cur > target_row) {
        n = cur - target_row;
        eput_block(blk_up, n);
    } else if (cur < target_row) {
        n = target_row - cur;
        eput_block(blk_down, n);
    }

    /* Horizontal movement */
    cur = PEEK(COLCRS);
    if (cur > target_col) {
        n = cur - target_col;
        eput_block(blk_left, n);
    } else if (cur < target_col) {
        n = target_col - cur;
        eput_block(blk_right, n);
    }
}

/* Move cursor to column 0 on the current line */
static void cursor_col0(void)
{
    unsigned char c = PEEK(COLCRS);
    if (c > 0) eput_block(blk_left, c);
}

/* ------------------------------------------------------------------ */
/* Scroll region operations.                                           */
/* Uses a two-step delete/insert algorithm: the native Atari 0x9C      */
/* (delete line) and 0x9D (insert line) affect the full screen, so to  */
/* scroll only within [scroll_top, scroll_bottom] we delete at one     */
/* edge and insert at the other — the lines outside the region get     */
/* shifted by the first op and restored by the second.                 */
/* ------------------------------------------------------------------ */

/* Scroll region up: top line removed, blank at bottom */
static void region_scroll_up(void)
{
    cursor_moveto(scroll_top, 0);
    putchar(0x9C);                 /* delete at top */
    cursor_moveto(scroll_bottom, 0);
    putchar(0x9D);                 /* insert blank at bottom */
}

/* Scroll region down: bottom line removed, blank at top */
static void region_scroll_down(void)
{
    cursor_moveto(scroll_bottom, 0);
    putchar(0x9C);                 /* delete at bottom */
    cursor_moveto(scroll_top, 0);
    putchar(0x9D);                 /* insert blank at top */
}

/* Insert N blank lines at row R within scroll region */
static void region_insert_lines(unsigned char r, unsigned char cnt)
{
    unsigned char save_row = PEEK(ROWCRS);
    unsigned char save_col = PEEK(COLCRS);
    while (cnt > 0) {
        cursor_moveto(scroll_bottom, 0);
        putchar(0x9C);             /* delete bottom of region */
        cursor_moveto(r, 0);
        putchar(0x9D);             /* insert blank at r */
        --cnt;
    }
    cursor_moveto(save_row, save_col);
}

/* Delete N lines at row R within scroll region */
static void region_delete_lines(unsigned char r, unsigned char cnt)
{
    unsigned char save_row = PEEK(ROWCRS);
    unsigned char save_col = PEEK(COLCRS);
    while (cnt > 0) {
        cursor_moveto(r, 0);
        putchar(0x9C);             /* delete at r */
        cursor_moveto(scroll_bottom, 0);
        putchar(0x9D);             /* insert blank at bottom */
        --cnt;
    }
    cursor_moveto(save_row, save_col);
}

/* ------------------------------------------------------------------ */
/* Execute a parsed CSI sequence (ESC [ ... <cmd>)                    */
/* ------------------------------------------------------------------ */
static void csi_execute(unsigned char cmd)
{
    unsigned int n;
    unsigned char row, col, c, r;

    switch (cmd) {

    /* -- Cursor movement (block writes for speed) -------------------- */
    case 'A':  /* CUU — Cursor Up */
        n = csi_param(0, 1);
        if (n > 24) n = 24;
        eput_block(blk_up, (unsigned char)n);
        break;

    case 'B':  /* CUD — Cursor Down */
        n = csi_param(0, 1);
        if (n > 24) n = 24;
        eput_block(blk_down, (unsigned char)n);
        break;

    case 'C':  /* CUF — Cursor Forward (Right) */
        n = csi_param(0, 1);
        if (n > 80) n = 80;
        eput_block(blk_right, (unsigned char)n);
        break;

    case 'D':  /* CUB — Cursor Backward (Left) */
        n = csi_param(0, 1);
        if (n > 80) n = 80;
        eput_block(blk_left, (unsigned char)n);
        break;

    /* -- Absolute cursor positioning ------------------------------- */
    case 'H':  /* CUP — Cursor Position */
    case 'f':  /* HVP — same as CUP */
        row = (unsigned char)csi_param(0, 1) - 1;  /* 1-based → 0-based */
        col = (unsigned char)csi_param(1, 1) - 1;
        if (row >= TERM_ROWS)   row = TERM_ROWS - 1;
        if (col >= screen_cols) col = screen_cols - 1;
        cursor_moveto(row, col);
        break;

    /* -- Erase in Display ------------------------------------------ */
    /* Note: space writes stop at screen_cols-1 to avoid the Atari     */
    /* E: handler wrapping at the last column (no VT100 delayed wrap). */
    /* Uses eput_block(blk_space, ...) for speed.                      */
    case 'J':
        n = csi_param(0, 0);
        if (n == 2 || n == 3) {
            /* ESC[2J — Clear entire screen */
            putchar(0x7D);
        } else if (n == 0) {
            /* ESC[0J — Erase from cursor to end of screen */
            row = PEEK(ROWCRS);
            col = PEEK(COLCRS);
            if (row == 0 && col == 0) {
                /* At home — equivalent to full clear, use fast path */
                putchar(0x7D);
            } else {
                c = screen_cols - 1 - col;
                if (c > 0) eput_block(blk_space, c);
                for (r = row + 1; r < TERM_ROWS; ++r) {
                    cursor_moveto(r, 0);
                    eput_block(blk_space, screen_cols - 1);
                }
                cursor_moveto(row, col);
            }
        } else if (n == 1) {
            /* ESC[1J — Erase from start of screen to cursor */
            row = PEEK(ROWCRS);
            col = PEEK(COLCRS);
            for (r = 0; r < row; ++r) {
                cursor_moveto(r, 0);
                eput_block(blk_space, screen_cols - 1);
            }
            cursor_moveto(row, 0);
            if (col > 0) eput_block(blk_space, col + 1);
            cursor_moveto(row, col);
        }
        break;

    /* -- Erase in Line --------------------------------------------- */
    /* Uses eput_block(blk_space, ...) for speed.                      */
    case 'K':
        n = csi_param(0, 0);
        row = PEEK(ROWCRS);
        col = PEEK(COLCRS);
        if (n == 0) {
            /* ESC[K — Erase from cursor to end of line */
            c = screen_cols - 1 - col;
            if (c > 0) eput_block(blk_space, c);
        } else if (n == 1) {
            /* ESC[1K — Erase from start of line to cursor */
            cursor_col0();
            eput_block(blk_space, col + 1);
        } else if (n == 2) {
            /* ESC[2K — Erase entire line */
            cursor_col0();
            eput_block(blk_space, screen_cols - 1);
        }
        /* Restore cursor position using movement */
        cursor_moveto(row, col);
        break;

    /* -- Insert / Delete Lines (scroll-region aware) ------------------ */
    case 'L':  /* IL — Insert Lines at cursor within scroll region */
        n = csi_param(0, 1);
        row = PEEK(ROWCRS);
        region_insert_lines(row, (unsigned char)n);
        break;

    case 'M':  /* DL — Delete Lines at cursor within scroll region */
        n = csi_param(0, 1);
        row = PEEK(ROWCRS);
        region_delete_lines(row, (unsigned char)n);
        break;

    /* -- Delete Characters ----------------------------------------- */
    case 'P':  /* DCH — Delete Characters at cursor */
        n = csi_param(0, 1);
        for (; n > 0; --n) {
            /* Move right then backspace: deletes char at original pos */
            putchar(0x1F);
            putchar(0x7E);
        }
        break;

    /* -- Cursor Save / Restore ------------------------------------- */
    case 's':  /* SCP — Save Cursor Position */
        saved_row = PEEK(ROWCRS);
        saved_col = PEEK(COLCRS);
        break;

    case 'u':  /* RCP — Restore Cursor Position */
        cursor_moveto(saved_row, saved_col);
        break;

    /* -- Attributes (SGR) — no color on Atari, ignore -------------- */
    case 'm':
        break;

    /* -- Mode set/reset — ignore ----------------------------------- */
    case 'h':
    case 'l':
        break;

    /* -- Scroll region (DECSTBM) ----------------------------------- */
    case 'r':
        scroll_top = (unsigned char)csi_param(0, 1) - 1;
        scroll_bottom = (unsigned char)csi_param(1, TERM_ROWS) - 1;
        if (scroll_top >= TERM_ROWS) scroll_top = 0;
        if (scroll_bottom >= TERM_ROWS) scroll_bottom = TERM_ROWS - 1;
        if (scroll_top >= scroll_bottom) {
            scroll_top = 0;
            scroll_bottom = TERM_ROWS - 1;
        }
        /* VT100: cursor moves to home after setting scroll region */
        cursor_moveto(0, 0);
        break;

    /* -- Device Status Report -------------------------------------- */
    case 'n':
        /* ESC[6n — report cursor position; respond ESC[row;colR */
        if (csi_param(0, 0) == 6) {
            char resp[16];
            sprintf(resp, "\033[%u;%uR",
                    (unsigned)(PEEK(ROWCRS) + 1),
                    (unsigned)(PEEK(COLCRS) + 1));
            net_send_str(resp);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* VT100 output: process one received ASCII byte.                     */
/* Interprets ANSI/VT100 escape sequences and converts to ATASCII.    */
/* ------------------------------------------------------------------ */
static void output_char(unsigned char c)
{
    switch (ansi_state) {

    /* -- ESC state: byte after ESC --------------------------------- */
    case ANSI_ESC:
        if (c == '[') {
            csi_reset();
            ansi_state = ANSI_CSI;
            return;
        }
        if (c == ']') {
            ansi_state = ANSI_OSC;
            return;
        }
        /* VT100 ESC sequences */
        switch (c) {
        case 'D':  /* IND — Index (cursor down, scroll at bottom margin) */
            {
                unsigned char cur_row = PEEK(ROWCRS);
                unsigned char cur_col = PEEK(COLCRS);
                if (cur_row >= scroll_bottom) {
                    region_scroll_up();
                    cursor_moveto(scroll_bottom, cur_col);
                } else {
                    putchar(0x1D);
                }
            }
            ansi_state = ANSI_NORMAL;
            return;
        case 'M':  /* RI — Reverse Index (cursor up, scroll at top margin) */
            {
                unsigned char cur_row = PEEK(ROWCRS);
                unsigned char cur_col = PEEK(COLCRS);
                if (cur_row <= scroll_top) {
                    region_scroll_down();
                    cursor_moveto(scroll_top, cur_col);
                } else {
                    putchar(0x1C);
                }
            }
            ansi_state = ANSI_NORMAL;
            return;
        case '7':  /* DECSC — Save Cursor */
            saved_row = PEEK(ROWCRS);
            saved_col = PEEK(COLCRS);
            ansi_state = ANSI_NORMAL;
            return;
        case '8':  /* DECRC — Restore Cursor */
            cursor_moveto(saved_row, saved_col);
            ansi_state = ANSI_NORMAL;
            return;
        case 'c':  /* RIS — Full Reset */
            putchar(0x7D);
            scroll_top = 0;
            scroll_bottom = TERM_ROWS - 1;
            ansi_state = ANSI_NORMAL;
            return;
        }
        /* Intermediate bytes (charset selection ESC ( B etc.) */
        if (c >= 0x20 && c <= 0x2F) {
            return;  /* stay in ESC, consume next byte */
        }
        /* Final byte of 2-char sequence — ignore and return */
        if (c >= 0x40 && c <= 0x7E) {
            ansi_state = ANSI_NORMAL;
            return;
        }
        ansi_state = ANSI_NORMAL;
        break;

    /* -- CSI state: collecting parameters -------------------------- */
    case ANSI_CSI:
        /* '?' private mode prefix */
        if (c == '?' && csi_nparam == 0 && !csi_collecting) {
            csi_private = 1;
            return;
        }
        /* Digit — accumulate into current parameter */
        if (c >= '0' && c <= '9') {
            if (!csi_collecting) {
                csi_collecting = 1;
                if (csi_nparam < CSI_MAX_PARAMS)
                    csi_params[csi_nparam] = 0;
            }
            if (csi_nparam < CSI_MAX_PARAMS)
                csi_params[csi_nparam] =
                    csi_params[csi_nparam] * 10 + (c - '0');
            return;
        }
        /* Semicolon — next parameter */
        if (c == ';') {
            if (csi_nparam < CSI_MAX_PARAMS)
                ++csi_nparam;
            csi_collecting = 0;
            return;
        }
        /* Intermediate bytes — skip */
        if (c >= 0x20 && c <= 0x2F) {
            return;
        }
        /* Final byte — execute command */
        if (c >= 0x40 && c <= 0x7E) {
            if (csi_collecting && csi_nparam < CSI_MAX_PARAMS)
                ++csi_nparam;
            csi_execute(c);
            ansi_state = ANSI_NORMAL;
        }
        return;

    /* -- OSC state: skip until BEL or ST --------------------------- */
    case ANSI_OSC:
        if (c == 0x07) {
            ansi_state = ANSI_NORMAL;
        } else if (c == 0x1B) {
            ansi_state = ANSI_ESC;
        }
        return;
    }

    /* -- Normal character: ASCII → ATASCII ------------------------- */
    switch (c) {
    case 0x1B:  /* ESC — start escape sequence */
        ansi_state = ANSI_ESC;
        break;
    case 0x0A:  /* LF → index (cursor down, scroll at bottom margin) */
        {
            unsigned char cur_row = PEEK(ROWCRS);
            if (cur_row >= scroll_bottom) {
                if (scroll_top == 0 && scroll_bottom == TERM_ROWS - 1) {
                    /* Full screen — use fast native scroll */
                    putchar(0x9B);
                } else {
                    /* Restricted scroll region — manual scroll */
                    unsigned char cur_col = PEEK(COLCRS);
                    region_scroll_up();
                    cursor_moveto(scroll_bottom, cur_col);
                }
            } else {
                putchar(0x1D);  /* simple cursor down */
            }
        }
        break;
    case 0x0D:  /* CR → move cursor to column 0 */
        cursor_col0();
        break;
    case 0x08:  /* BS → cursor left (non-destructive) */
        putchar(0x1E);
        break;
    case 0x09:  /* TAB → spaces to next 8-column stop */
        {
            unsigned char col = PEEK(COLCRS);
            do {
                putchar(' ');
                ++col;
            } while (col < screen_cols && (col & 7));
        }
        break;
    case 0x07:  /* BEL → Atari bell */
        putchar(0xFD);
        break;
    default:
        if (c >= 0x20 && c <= 0x7E) {
            putchar(c);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Convert ATASCII key code to ASCII for sending over SSH             */
/* ------------------------------------------------------------------ */
static unsigned char atascii_to_ascii(unsigned char c)
{
    if (c == 0x9B) return 0x0D;  /* Atari EOL → CR */
    if (c == 0x7E) return 0x08;  /* Atari BS   → BS */
    if (c == 0x7F) return 0x09;  /* Atari TAB  → TAB */
    /* CTRL+A..Z (0x01-0x1A) and printable (0x20-0x7E) pass as-is */
    return c;
}

/* ------------------------------------------------------------------ */
/* Read a line of input from keyboard.                                */
/* If masked=1, displays '*' instead of typed characters.             */
/* Returns length of input. buf must be at least maxlen+1 bytes.      */
/* ------------------------------------------------------------------ */
static unsigned char read_input(char *buf, unsigned char maxlen,
                                unsigned char masked)
{
    unsigned char pos = 0;
    unsigned char ch;

    while (1) {
        while (!key_available())
            wait_vbi(1);
        ch = keyboard_get();

        if (ch == 0x9B) {          /* RETURN — done */
            buf[pos] = '\0';
            putchar(0x9B);
            return pos;
        }
        if (ch == 0x7E || ch == 0x08) {  /* BACKSPACE */
            if (pos > 0) {
                --pos;
                putchar(0x7E);     /* Atari backspace */
            }
            continue;
        }
        if (ch == 0x1B) {          /* ESC — ignore */
            continue;
        }
        if (ch >= 0x20 && ch <= 0x7E && pos < maxlen) {
            buf[pos++] = (char)ch;
            putchar(masked ? '*' : ch);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Send a string to the remote shell via SIO WRITE                    */
/* ------------------------------------------------------------------ */
static void net_send_str(const char *s)
{
    unsigned int len = (unsigned int)strlen(s);
    /* net_write expects unsigned char *, cast away const for SIO buf */
    net_write((unsigned char *)s, len);
}

/* ------------------------------------------------------------------ */
/* Drain and discard any pending incoming data (wait up to N frames)  */
/* ------------------------------------------------------------------ */
static void drain_incoming(unsigned int frames)
{
    unsigned int avail, readlen;
    unsigned int waited = 0;

    while (waited < frames) {
        net_status();
        avail = (unsigned int)status_buf[0] |
                ((unsigned int)status_buf[1] << 8);
        if (avail > 0) {
            while (avail > 0) {
                readlen = (avail > RXBUF_SIZE) ? RXBUF_SIZE : avail;
                net_read(rxbuf, readlen);
                avail -= readlen;
            }
            waited = 0;  /* reset — more data may come */
        } else {
            wait_vbi(1);
            ++waited;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Configure remote terminal: TERM=vt100, columns, rows               */
/* Sends shell commands and drains their output so it doesn't appear  */
/* on screen.                                                         */
/* ------------------------------------------------------------------ */
static void setup_remote_terminal(unsigned char cols)
{
    char cmd[80];

    /* Wait for initial shell prompt / MOTD to arrive */
    drain_incoming(30);

    /* Set TERM, disable PROMPT_EOL_MARK (zsh's "%" partial-line marker
       uses ESC[K + CR to overwrite, which doesn't work on Atari because
       writing at the last column causes an immediate wrap instead of
       VT100's delayed autowrap) */
    net_send_str("export TERM=vt100 PROMPT_EOL_MARK=''\r");
    drain_incoming(15);

    /* Set terminal size via stty */
    sprintf(cmd, "stty cols %u rows %u\r",
            (unsigned)cols, (unsigned)TERM_ROWS);
    net_send_str(cmd);
    drain_incoming(15);

    /* Clear screen on remote side — don't drain so the clear
       escape sequences and the new prompt flow through the main
       loop and appear on the Atari screen naturally */
    net_send_str("clear\r");
}

/* ================================================================== */
/* ZMODEM RECEIVE PROTOCOL                                             */
/*                                                                     */
/* Detects ZMODEM auto-download sequence (**\x18B) in the SSH output   */
/* stream, receives one file via the ZMODEM protocol, and saves it     */
/* to D: on the Atari.  Triggered by running "sz filename" on the      */
/* remote shell.                                                       */
/*                                                                     */
/* First prototype: CRC verification disabled (TODO), single file.     */
/* ================================================================== */

/* ---- ZMODEM frame type constants --------------------------------- */
#define ZM_ZRQINIT   0    /* Request receive init */
#define ZM_ZRINIT    1    /* Receive init */
#define ZM_ZSINIT    2    /* Send init sequence */
#define ZM_ZACK      3    /* ACK */
#define ZM_ZFILE     4    /* File name from sender */
#define ZM_ZSKIP     5    /* Skip this file */
#define ZM_ZNAK      6    /* Last packet was garbled */
#define ZM_ZABORT    7    /* Abort batch transfers */
#define ZM_ZFIN      8    /* Finish session */
#define ZM_ZRPOS     9    /* Resume data trans at this position */
#define ZM_ZDATA    10    /* Data packet(s) follow */
#define ZM_ZEOF     11    /* End of file */
#define ZM_ZFERR    12    /* Fatal read/write error */

/* ---- ZMODEM encoding markers ------------------------------------- */
#define ZPAD    0x2A       /* '*' padding */
#define ZDLE    0x18       /* Escape character */
#define ZHEX    0x42       /* 'B' hex header follows */
#define ZBIN    0x41       /* 'A' binary header, CRC-16 */
#define ZBIN32  0x43       /* 'C' binary header, CRC-32 */

/* ---- Subpacket frame-end markers --------------------------------- */
#define ZCRCE   0x68       /* 'h' end of frame, no more data */
#define ZCRCG   0x69       /* 'i' more data coming, no ACK */
#define ZCRCQ   0x6A       /* 'j' more data coming, ACK requested */
#define ZCRCW   0x6B       /* 'k' end of frame, sender waits */

/* ---- ZRINIT capability flags (ZF0 byte) -------------------------- */
#define ZM_CANFDX   0x01   /* Can full duplex */
/* Note: CANFC32 (0x20) NOT advertised — CRC-16 only */

/* ---- I/O buffer -------------------------------------------------- */
#define ZM_IOBUF_SIZE 4096

/* ---- IOCB 1 for file output ($0350) ------------------------------ */
#define IOCB1_BASE  0x0350
#define IOCB1_COM   (IOCB1_BASE + 0x02)
#define IOCB1_STA   (IOCB1_BASE + 0x03)
#define IOCB1_BAL   (IOCB1_BASE + 0x04)
#define IOCB1_BAH   (IOCB1_BASE + 0x05)
#define IOCB1_BLL   (IOCB1_BASE + 0x08)
#define IOCB1_BLH   (IOCB1_BASE + 0x09)
#define IOCB1_AX1   (IOCB1_BASE + 0x0A)
#define IOCB1_AX2   (IOCB1_BASE + 0x0B)

/* ---- ZMODEM state ------------------------------------------------ */
static unsigned char zm_iobuf[ZM_IOBUF_SIZE];
static char zm_fname[68];               /* "D:filename\x9B\0" */
static unsigned int zm_rxpos;            /* Position in rxbuf for zm_getbyte */
static unsigned int zm_rxlen;            /* Valid bytes in rxbuf */
static unsigned long zm_file_offset;     /* Bytes written to file */
static unsigned char zm_crc_bytes;       /* 2=CRC-16, 4=CRC-32 */
static unsigned char zm_detect_state;    /* Auto-detect FSM state */
static unsigned char zm_file_is_open;    /* IOCB 1 open flag */
static unsigned char zm_text_mode;       /* 1=convert LF→$9B on write */
static unsigned int zm_net_avail;        /* remaining bytes from last STATUS */

/* ---- ZMODEM mode selection (inline prompt) ----------------------- */
/* Returns 0=binary, 1=text.                                          */
static unsigned char zm_show_mode_dialog(void)
{
    unsigned char ch;

    printf("Transfer mode: 1.binary or 2.ascii ? ");
    POKE(CH, 0xFF);
    while (1) {
        ch = keyboard_get();
        if (ch == '1' || ch == '2') break;
    }
    putchar(ch);
    putchar('\n');

    return (ch == '2') ? 1 : 0;
}

/* ---- Pushback buffer (LIFO, 4 bytes max) ------------------------- */
static unsigned char zm_pb[4];
static unsigned char zm_pb_count = 0;

static void zm_ungetbyte(unsigned char b)
{
    if (zm_pb_count < 4)
        zm_pb[zm_pb_count++] = b;
}

/* ---- Refill rxbuf from network ----------------------------------- */
/* Returns number of bytes read, 0 on nothing available, -1 on error.  */
/* Skips STATUS when we know data remains from a previous STATUS call.  */
static int zm_refill(void)
{
    unsigned int avail;
    unsigned int timeout = 0;
    unsigned char sts;

    while (1) {
        if (zm_net_avail > 0) {
            /* Data remaining from previous STATUS — skip STATUS call */
            avail = zm_net_avail;
        } else {
            sts = net_status();
            if (sts != 1) return -1;   /* SIO error */
            avail = (unsigned int)status_buf[0] |
                    ((unsigned int)status_buf[1] << 8);
            if (avail == 0) {
                if (!status_buf[2]) return -1;   /* disconnected */
                if (++timeout > 30000) return -1;  /* timeout */
                continue;
            }
        }

        if (avail > RXBUF_SIZE) {
            zm_net_avail = avail - RXBUF_SIZE;
            avail = RXBUF_SIZE;
        } else {
            zm_net_avail = 0;
        }

        sts = net_read(rxbuf, avail);
        if (sts != 1) { zm_net_avail = 0; return -1; }
        zm_rxpos = 0;
        zm_rxlen = avail;
        return (int)avail;
    }
}

/* ---- Byte reader (used for headers, not bulk data) --------------- */
/* Priority: pushback (LIFO) → rxbuf remainder → network.              */
static int zm_getbyte(void)
{
    if (zm_pb_count > 0)
        return zm_pb[--zm_pb_count];

    if (zm_rxpos < zm_rxlen)
        return rxbuf[zm_rxpos++];

    if (zm_refill() < 0) return -1;
    return rxbuf[zm_rxpos++];
}

/* ---- CRC-16-CCITT (bit-by-bit, no lookup table) ------------------ */
static unsigned int zm_crc16(unsigned int crc, unsigned char b)
{
    unsigned char i;
    crc ^= (unsigned int)b << 8;
    for (i = 0; i < 8; i++) {
        if (crc & 0x8000)
            crc = (crc << 1) ^ 0x1021;
        else
            crc = crc << 1;
    }
    return crc & 0xFFFF;
}

/* ---- Consume CRC bytes (ZDLE-escaped, not verified) -------------- */
static void zm_consume_crc(void)
{
    unsigned char i;
    int b;
    /* TODO: verify CRC against accumulated data */
    for (i = 0; i < zm_crc_bytes; i++) {
        b = zm_getbyte();
        if (b < 0) return;
        if (b == ZDLE) zm_getbyte();  /* consume escaped companion */
    }
}

/* ---- Hex nibble decoder ------------------------------------------ */
static unsigned char zm_hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* ---- File I/O via CIO IOCB 1 ------------------------------------ */

static unsigned char zm_file_open(const char *fname)
{
    POKE(IOCB1_COM, CIO_OPEN);
    POKE(IOCB1_BAL, (unsigned)fname & 0xFF);
    POKE(IOCB1_BAH, ((unsigned)fname >> 8) & 0xFF);
    POKE(IOCB1_AX1, 0x08);     /* Write mode */
    POKE(IOCB1_AX2, 0x00);
    POKE(IOCB1_BLL, 0);
    POKE(IOCB1_BLH, 0);
    __asm__("ldx #$10");        /* IOCB 1 */
    __asm__("jsr $E456");
    return PEEK(IOCB1_STA);
}

static void zm_file_write_raw(const unsigned char *buf, unsigned int len)
{
    if (len == 0) return;
    POKE(IOCB1_COM, 0x0B);     /* PUT BYTES */
    POKE(IOCB1_BAL, (unsigned)buf & 0xFF);
    POKE(IOCB1_BAH, ((unsigned)buf >> 8) & 0xFF);
    POKE(IOCB1_BLL, len & 0xFF);
    POKE(IOCB1_BLH, (len >> 8) & 0xFF);
    __asm__("ldx #$10");
    __asm__("jsr $E456");
}

/* Text-mode aware file write: converts LF (0x0A) → 0x9B (Atari EOL) */
/* and strips CR (0x0D) when zm_text_mode is set.                      */
static void zm_file_write(const unsigned char *buf, unsigned int len)
{
    unsigned int i, start;

    if (!zm_text_mode) {
        zm_file_write_raw(buf, len);
        return;
    }

    /* Scan for LF/CR, write clean runs, convert LF→$9B, skip CR */
    start = 0;
    for (i = 0; i < len; ++i) {
        if (buf[i] == 0x0A) {
            /* Flush preceding bytes */
            if (i > start)
                zm_file_write_raw(buf + start, i - start);
            /* Write Atari EOL */
            zm_iobuf[0] = 0x9B;
            zm_file_write_raw(zm_iobuf, 1);
            start = i + 1;
        } else if (buf[i] == 0x0D) {
            /* Flush preceding bytes, skip CR */
            if (i > start)
                zm_file_write_raw(buf + start, i - start);
            start = i + 1;
        }
    }
    /* Flush remaining */
    if (start < len)
        zm_file_write_raw(buf + start, len - start);
}

static void zm_file_close(void)
{
    POKE(IOCB1_COM, CIO_CLOSE);
    __asm__("ldx #$10");
    __asm__("jsr $E456");
    zm_file_is_open = 0;
}

/* ---- Read hex header --------------------------------------------- */
/* Reads: type(2hex) data(8hex) crc(4hex) CR LF [XON]                  */
static int zm_read_hex_hdr(unsigned char *type, unsigned char *data)
{
    unsigned char hex[14];
    int b;
    unsigned char i;

    for (i = 0; i < 14; i++) {
        b = zm_getbyte();
        if (b < 0) return -1;
        hex[i] = (unsigned char)b;
    }

    *type   = (zm_hexval(hex[0])  << 4) | zm_hexval(hex[1]);
    data[0] = (zm_hexval(hex[2])  << 4) | zm_hexval(hex[3]);
    data[1] = (zm_hexval(hex[4])  << 4) | zm_hexval(hex[5]);
    data[2] = (zm_hexval(hex[6])  << 4) | zm_hexval(hex[7]);
    data[3] = (zm_hexval(hex[8])  << 4) | zm_hexval(hex[9]);
    /* hex[10..13] = CRC-16 — TODO: verify */

    /* Consume trailing CR, LF, optional XON */
    b = zm_getbyte();
    if (b == 0x0D) b = zm_getbyte();
    if (b == 0x0A) b = zm_getbyte();
    if (b >= 0 && b != 0x11)
        zm_ungetbyte((unsigned char)b);

    return 0;
}

/* ---- Read binary CRC-16 header ----------------------------------- */
static int zm_read_bin_hdr(unsigned char *type, unsigned char *data)
{
    int b;
    unsigned char i;

    b = zm_getbyte();
    if (b < 0) return -1;
    if (b == ZDLE) { b = zm_getbyte(); if (b < 0) return -1; b ^= 0x40; }
    *type = (unsigned char)b;

    for (i = 0; i < 4; i++) {
        b = zm_getbyte();
        if (b < 0) return -1;
        if (b == ZDLE) { b = zm_getbyte(); if (b < 0) return -1; b ^= 0x40; }
        data[i] = (unsigned char)b;
    }

    zm_crc_bytes = 2;
    zm_consume_crc();
    return 0;
}

/* ---- Read binary CRC-32 header ----------------------------------- */
static int zm_read_bin32_hdr(unsigned char *type, unsigned char *data)
{
    int b;
    unsigned char i;

    b = zm_getbyte();
    if (b < 0) return -1;
    if (b == ZDLE) { b = zm_getbyte(); if (b < 0) return -1; b ^= 0x40; }
    *type = (unsigned char)b;

    for (i = 0; i < 4; i++) {
        b = zm_getbyte();
        if (b < 0) return -1;
        if (b == ZDLE) { b = zm_getbyte(); if (b < 0) return -1; b ^= 0x40; }
        data[i] = (unsigned char)b;
    }

    zm_crc_bytes = 4;
    zm_consume_crc();
    return 0;
}

/* ---- Read any header (hex / binary-16 / binary-32) --------------- */
/* Sets zm_crc_bytes for subsequent data subpacket CRC consumption.    */
static int zm_read_header(unsigned char *type, unsigned char *data)
{
    int b;

    /* Skip until ZPAD */
    while (1) {
        b = zm_getbyte();
        if (b < 0) return -1;
        if (b == ZPAD) break;
    }
    /* Skip additional ZPADs */
    while (1) {
        b = zm_getbyte();
        if (b < 0) return -1;
        if (b != ZPAD) break;
    }
    if (b != ZDLE) return -1;

    b = zm_getbyte();
    if (b < 0) return -1;

    switch (b) {
    case ZHEX:
        zm_crc_bytes = 2;
        return zm_read_hex_hdr(type, data);
    case ZBIN:
        zm_crc_bytes = 2;
        return zm_read_bin_hdr(type, data);
    case ZBIN32:
        zm_crc_bytes = 4;
        return zm_read_bin32_hdr(type, data);
    default:
        return -1;
    }
}

/* ---- Send hex header --------------------------------------------- */
/* Parameters are in ZMODEM wire order:                                 */
/*   h[0]=zp0, h[1]=zp1, h[2]=zf1, h[3]=zf0                          */
/* For position-type frames (ZRPOS, ZACK, ZEOF, ZDATA) all 4 bytes    */
/* form a little-endian 32-bit offset.                                 */
static void zm_send_hex_hdr(unsigned char type,
                             unsigned char zp0, unsigned char zp1,
                             unsigned char zf1, unsigned char zf0)
{
    unsigned char buf[22];
    unsigned int crc = 0;

    crc = zm_crc16(crc, type);
    crc = zm_crc16(crc, zp0);
    crc = zm_crc16(crc, zp1);
    crc = zm_crc16(crc, zf1);
    crc = zm_crc16(crc, zf0);

    buf[0] = ZPAD;
    buf[1] = ZPAD;
    buf[2] = ZDLE;
    buf[3] = ZHEX;
    sprintf((char *)buf + 4, "%02x%02x%02x%02x%02x%02x%02x\r\n",
            type, zp0, zp1, zf1, zf0,
            (unsigned)((crc >> 8) & 0xFF),
            (unsigned)(crc & 0xFF));
    buf[20] = 0x11;  /* XON — required after hex header */
    net_write(buf, 21);  /* 4 prefix + 14 hex + 2 CRLF + XON */
}

/* ---- Read ZFILE filename subpacket ------------------------------- */
/* Reads into namebuf (up to NUL), then consumes the rest of the       */
/* subpacket (file metadata after the NUL).  Does NOT stream to file.  */
/* frame_end receives the frame-end marker type.                       */
static int zm_read_filename(char *namebuf, unsigned char maxlen,
                            unsigned char *frame_end)
{
    unsigned char pos = 0;
    unsigned char got_nul = 0;
    int b;

    while (1) {
        b = zm_getbyte();
        if (b < 0) return -1;

        if (b == ZDLE) {
            b = zm_getbyte();
            if (b < 0) return -1;
            /* Frame-end marker — not a data byte */
            if (b >= ZCRCE && b <= ZCRCW) {
                *frame_end = (unsigned char)b;
                zm_consume_crc();
                if (!got_nul && pos < maxlen)
                    namebuf[pos] = '\0';
                return 0;
            }
            /* Escaped data byte */
            b ^= 0x40;
        }

        if (!got_nul) {
            if (b == 0) {
                got_nul = 1;
                if (pos < maxlen) namebuf[pos] = '\0';
            } else if (pos < maxlen - 1) {
                namebuf[pos++] = (char)b;
            }
        }
        /* After NUL: keep consuming metadata until frame-end */
    }
}

/* ---- Sanitize filename for Atari DOS ----------------------------- */
/* Strips path, replaces illegal chars, prepends "D:", appends 0x9B.   */
static void zm_sanitize_filename(char *dest, const char *src,
                                  unsigned char maxlen)
{
    const char *base;
    const char *p;
    unsigned char i;
    unsigned char c;

    /* Find basename: last component after / \ or : */
    base = src;
    for (p = src; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    }

    dest[0] = 'D';
    dest[1] = ':';
    i = 2;

    for (p = base; *p && i < maxlen - 2; ++p) {
        c = (unsigned char)*p;
        if (c < 0x20 || c == ':' || c == '/' || c == '\\' || c > 0x7E)
            c = '_';
        dest[i++] = c;
    }

    dest[i++] = 0x9B;  /* CIO filename terminator */
    dest[i] = '\0';     /* C string terminator for display */
}

/* ---- Print filename (up to 0x9B or NUL) for display -------------- */
static void zm_print_fname(void)
{
    unsigned char i;
    for (i = 0; zm_fname[i] && zm_fname[i] != (char)0x9B; ++i)
        putchar(zm_fname[i]);
}

/* ---- Read data subpackets, streaming to file --------------------- */
/* Batch-optimized: scans rxbuf directly for ZDLE markers and copies    */
/* non-escaped runs via memcpy instead of per-byte function calls.      */
/* Flushes zm_iobuf to file every ZM_IOBUF_SIZE bytes.                 */
/* *frame_end receives the frame-end marker.  Returns 0 ok, -1 error.  */
static int zm_read_data_to_file(unsigned char *frame_end)
{
    unsigned int iopos = 0;
    unsigned int run_start, run_len, space;
    int b;

    while (1) {
        /* Ensure rxbuf has data */
        if (zm_rxpos >= zm_rxlen) {
            if (zm_refill() < 0) return -1;
        }

        /* Scan for next ZDLE in rxbuf */
        run_start = zm_rxpos;
        while (zm_rxpos < zm_rxlen && rxbuf[zm_rxpos] != ZDLE)
            ++zm_rxpos;
        run_len = zm_rxpos - run_start;

        /* Copy non-escaped run to iobuf, flushing as needed */
        while (run_len > 0) {
            space = ZM_IOBUF_SIZE - iopos;
            if (run_len <= space) {
                memcpy(zm_iobuf + iopos, rxbuf + run_start, run_len);
                iopos += run_len;
                run_len = 0;
            } else {
                memcpy(zm_iobuf + iopos, rxbuf + run_start, space);
                zm_file_write(zm_iobuf, ZM_IOBUF_SIZE);
                zm_file_offset += ZM_IOBUF_SIZE;
                run_start += space;
                run_len -= space;
                iopos = 0;
            }
            if (iopos >= ZM_IOBUF_SIZE) {
                zm_file_write(zm_iobuf, ZM_IOBUF_SIZE);
                zm_file_offset += ZM_IOBUF_SIZE;
                iopos = 0;
            }
        }

        /* If we stopped because rxbuf ended (not ZDLE), loop to refill */
        if (zm_rxpos >= zm_rxlen)
            continue;

        /* We hit a ZDLE — consume it and read next byte */
        ++zm_rxpos;  /* skip ZDLE */

        /* Get the byte after ZDLE */
        if (zm_rxpos >= zm_rxlen) {
            if (zm_refill() < 0) return -1;
        }
        b = rxbuf[zm_rxpos++];

        /* Frame-end marker? */
        if (b >= ZCRCE && b <= ZCRCW) {
            if (iopos > 0) {
                zm_file_write(zm_iobuf, iopos);
                zm_file_offset += iopos;
            }
            *frame_end = (unsigned char)b;
            zm_consume_crc();
            return 0;
        }

        /* Normal escaped data byte */
        zm_iobuf[iopos++] = (unsigned char)(b ^ 0x40);
        if (iopos >= ZM_IOBUF_SIZE) {
            zm_file_write(zm_iobuf, ZM_IOBUF_SIZE);
            zm_file_offset += ZM_IOBUF_SIZE;
            iopos = 0;
        }
    }
}

/* ---- Consume and discard trailing bytes after ZMODEM handshake --- */
/* sz sends "OO" after our ZFIN+OO; drain it so it doesn't leak      */
/* into the terminal display.                                         */
static void zm_drain_trailing(void)
{
    unsigned int avail;
    unsigned char tries = 0;

    while (tries < 30) {
        net_status();
        avail = (unsigned int)status_buf[0] |
                ((unsigned int)status_buf[1] << 8);
        if (avail > 0) {
            if (avail > RXBUF_SIZE) avail = RXBUF_SIZE;
            net_read(rxbuf, avail);
            tries = 0;  /* reset — more might come */
        } else {
            ++tries;
        }
    }
}

/* ---- Main ZMODEM receive function -------------------------------- */
/* Called when auto-detection has matched **\x18B and pushed them into  */
/* the LIFO pushback buffer.  zm_read_header() reads them naturally.   */
static void zmodem_receive(void)
{
    unsigned char hdr_type;
    unsigned char hdr_data[4];
    unsigned char frame_end;
    char raw_name[64];
    unsigned long progress_next;   /* next offset to print '#' */

    zm_file_is_open = 0;
    zm_file_offset = 0;
    zm_net_avail = 0;

    /* Step 1: Read ZRQINIT (from pushback + network) */
    if (zm_read_header(&hdr_type, hdr_data) < 0 ||
        hdr_type != ZM_ZRQINIT) {
        return;
    }

    /* Step 2: Send ZRINIT */
    /* zp0=0x00, zp1=0x01 (MaxDataLen=256), zf1=0x00, zf0=CANFDX */
    zm_send_hex_hdr(ZM_ZRINIT, 0x00, 0x20, 0x00, ZM_CANFDX);

    /* Step 3: Read ZFILE */
    if (zm_read_header(&hdr_type, hdr_data) < 0) {
        return;
    }
    if (hdr_type != ZM_ZFILE) {
        return;
    }

    /* Step 4: Read filename subpacket (into buffer, not file) */
    raw_name[0] = '\0';
    if (zm_read_filename(raw_name, sizeof(raw_name), &frame_end) < 0) {
        return;
    }

    zm_sanitize_filename(zm_fname, raw_name, sizeof(zm_fname));

    /* Ask user: binary or text mode */
    zm_text_mode = zm_show_mode_dialog();

    /* Display: "D:filename [......] " */
    zm_print_fname();
    putchar(' ');

    /* Step 5: Open file for writing */
    {
        unsigned char fsts = zm_file_open(zm_fname);
        if (fsts != 1) {
            printf("FAIL\n");
            zm_send_hex_hdr(ZM_ZSKIP, 0, 0, 0, 0);
            return;
        }
    }
    zm_file_is_open = 1;

    /* Step 6: Send ZRPOS 0 (start from beginning) */
    zm_send_hex_hdr(ZM_ZRPOS, 0, 0, 0, 0);

    /* Progress bar */
    putchar('[');
    progress_next = 1024;

    /* Step 7: Data reception loop */
    while (1) {
        if (zm_read_header(&hdr_type, hdr_data) < 0) {
            goto cleanup;
        }

        if (hdr_type == ZM_ZEOF) {
            break;
        }

        if (hdr_type != ZM_ZDATA) {
            goto cleanup;
        }

        /* Read data subpackets until frame-end requires new header */
        while (1) {
            if (zm_read_data_to_file(&frame_end) < 0) {
                goto cleanup;
            }

            /* Print '#' for each 1KB received */
            while (zm_file_offset >= progress_next) {
                putchar('.');
                progress_next += 1024;
            }

            if (frame_end == ZCRCG) {
                continue;   /* more data, no ACK */
            }
            if (frame_end == ZCRCQ) {
                zm_send_hex_hdr(ZM_ZACK,
                    (unsigned char)(zm_file_offset & 0xFF),
                    (unsigned char)((zm_file_offset >> 8) & 0xFF),
                    (unsigned char)((zm_file_offset >> 16) & 0xFF),
                    (unsigned char)((zm_file_offset >> 24) & 0xFF));
                continue;   /* more data after ACK */
            }
            if (frame_end == ZCRCW) {
                zm_send_hex_hdr(ZM_ZACK,
                    (unsigned char)(zm_file_offset & 0xFF),
                    (unsigned char)((zm_file_offset >> 8) & 0xFF),
                    (unsigned char)((zm_file_offset >> 16) & 0xFF),
                    (unsigned char)((zm_file_offset >> 24) & 0xFF));
                break;      /* sender waits — read next header */
            }
            break;          /* ZCRCE — end, read next header */
        }
    }

    /* Step 8: Close file, finish handshake */
    zm_file_close();
    zm_file_is_open = 0;

    printf("] %lu bytes received.", zm_file_offset);

    zm_send_hex_hdr(ZM_ZRINIT, 0x00, 0x20, 0x00, ZM_CANFDX);

    if (zm_read_header(&hdr_type, hdr_data) >= 0 &&
        hdr_type == ZM_ZFIN) {
        zm_send_hex_hdr(ZM_ZFIN, 0, 0, 0, 0);
    }

    /* Sender sends "OO" (Over and Out) — consume it */
    zm_getbyte();  /* 'O' */
    zm_getbyte();  /* 'O' */

    /* Drain any remaining protocol bytes */
    zm_drain_trailing();

    /* Send ENTER so shell prompt reappears after sz exits */
    net_send_str("\r");
    return;

cleanup:
    if (zm_file_is_open)
        zm_file_close();
    printf("] ERR\n");
    zm_drain_trailing();
}

/* ---- Auto-detection state machine -------------------------------- */
/* Watches for **\x18B in the output stream.  On match, pushes the     */
/* 4-byte sequence into the LIFO pushback buffer in reverse order      */
/* (B, ZDLE, *, *) so zm_getbyte() reads *, *, ZDLE, B — then calls   */
/* zmodem_receive() which reads the ZRQINIT via zm_read_header().      */
/*                                                                     */
/* buf_i:   index of current byte in rxbuf                             */
/* buf_len: total bytes in current rxbuf batch                         */
/* Returns 1 if ZMODEM was triggered (caller must break), 0 otherwise. */
static unsigned char zm_detect_byte(unsigned char b,
                                     unsigned int buf_i,
                                     unsigned int buf_len)
{
    switch (zm_detect_state) {
    case 0:
        if (b == ZPAD) { zm_detect_state = 1; return 0; }
        output_char(b);
        return 0;
    case 1:
        if (b == ZPAD) { zm_detect_state = 2; return 0; }
        output_char(ZPAD);
        output_char(b);
        zm_detect_state = 0;
        return 0;
    case 2:
        if (b == ZDLE) { zm_detect_state = 3; return 0; }
        output_char(ZPAD);
        output_char(ZPAD);
        output_char(b);
        zm_detect_state = 0;
        return 0;
    case 3:
        zm_detect_state = 0;
        if (b == ZHEX) {
            /* ZMODEM detected — push in LIFO reverse order */
            zm_pb_count = 0;
            zm_ungetbyte(ZHEX);   /* popped last  → read 4th */
            zm_ungetbyte(ZDLE);   /*              → read 3rd */
            zm_ungetbyte(ZPAD);   /*              → read 2nd */
            zm_ungetbyte(ZPAD);   /* popped first → read 1st */
            zm_rxpos = buf_i + 1;
            zm_rxlen = buf_len;
            remove_proceed();   /* disable PROCEED during ZMODEM */
            zmodem_receive();
            install_proceed();  /* re-enable PROCEED */
            return 1;
        }
        output_char(ZPAD);
        output_char(ZPAD);
        output_char(ZDLE);
        output_char(b);
        return 0;
    }
    return 0;
}

/* ================================================================== */
/* END OF ZMODEM RECEIVE PROTOCOL                                      */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Main program                                                       */
/* ------------------------------------------------------------------ */
int main(void)
{
    unsigned char sts, ch, ascii;
    unsigned int avail, readlen, i;
    unsigned char connected;
    unsigned char old_soundr;
    unsigned char term_cols;

    /* Silence SIO buzzer (SOUNDR at $41) */
    old_soundr = PEEK(0x41);
    POKE(0x41, 0);

    /* Initialize block-write buffers for fast cursor/erase operations */
    memset(blk_left,  0x1E, sizeof(blk_left));
    memset(blk_right, 0x1F, sizeof(blk_right));
    memset(blk_up,    0x1C, sizeof(blk_up));
    memset(blk_down,  0x1D, sizeof(blk_down));
    memset(blk_space, 0x20, sizeof(blk_space));

    /* Detect screen width from RMARGN: 39→40 cols, 79→80 cols */
    term_cols = PEEK(RMARGN) + 1;
    screen_cols = term_cols;

    /* Open K: keyboard handler on IOCB 6 (bypasses E: editor) */
    keyboard_open();

    /* Clear screen */
    putchar(0x7D);

    printf("=== FujiNet SSH Terminal ===\n");
    printf("Terminal: vt100 %ux%u\n\n",
           (unsigned)term_cols, (unsigned)TERM_ROWS);

#ifdef HARDCODED_KEYAUTH
    /* Hardcoded host/user for public-key auth testing (no password) */
    strcpy(input_host, HARDCODED_HOST);
    strcpy(input_user, HARDCODED_USER);
#elif defined(HARDCODED_LOGIN)
    /* Hardcoded credentials for password auth testing */
    strcpy(input_host, HARDCODED_HOST);
    strcpy(input_user, HARDCODED_USER);
    strcpy(input_pass, HARDCODED_PASS);
#else
    /* Prompt for host */
    printf("Host: ");
    read_input(input_host, INPUT_MAX, 0);
    if (input_host[0] == '\0') {
        printf("No host specified.\n");
        keyboard_close();
        POKE(0x41, old_soundr);
        return 1;
    }

    /* Prompt for login */
    printf("Login: ");
    read_input(input_user, INPUT_MAX, 0);
    if (input_user[0] == '\0') {
        printf("No login specified.\n");
        keyboard_close();
        POKE(0x41, old_soundr);
        return 1;
    }

    /* Prompt for password */
    printf("Password: ");
    read_input(input_pass, INPUT_MAX, 1);
#endif

    /* Build devicespec */
#ifdef HARDCODED_KEYAUTH
    /* Public-key auth: N:SSH://user@host:port/ (no password) */
    sprintf((char *)devicespec,
            "N:SSH://%s@%s:%u/\x9b",
            input_user, input_host, SSH_PORT);
#else
    /* Password auth: N:SSH://user:pass@host:port/ */
    sprintf((char *)devicespec,
            "N:SSH://%s:%s@%s:%u/\x9b",
            input_user, input_pass, input_host, SSH_PORT);
#endif

    /* OPEN — this triggers SSH handshake + auth on FujiNet */
    printf("\nConnecting to %s as %s...",
           input_host, input_user);
    POKE(DTIMLO, 60);  /* longer timeout for SSH handshake */
    sts = net_open();
    if (sts != 1) {
        printf(" FAIL (SIO:%u)\n", sts);
        printf("\nCheck:\n");
        printf("- FujiNet running?\n");
        printf("- SSH server at %s:%u?\n",
               input_host, SSH_PORT);
        printf("- Login/password correct?\n");
        net_close();
        keyboard_close();
        POKE(0x41, old_soundr);
        return 1;
    }
    printf(" OK\n");
    printf("Setting up terminal...");
    setup_remote_terminal(term_cols);
    printf(" OK\n\n");

    /* Install PROCEED interrupt handler — FujiNet signals data via
       the SIO PROCEED line.  The handler sets trip=1 and DISABLES
       the interrupt (PACTL bit 0=0) to prevent rapid-fire IRQs
       from FujiNet-PC's PROCEED oscillation.  We re-enable after
       processing data. */
    install_proceed();

    /* Main terminal loop */
    while (1) {

        /* Wait for PROCEED interrupt (trip=1) or keypress.
           No polling — purely interrupt-driven wakeup. */
        while (!trip && !key_available())
            ;

        /* ---- Poll STATUS ---- */
        sts = net_status();
        if (sts != 1) {
            printf("\n[SIO error: %u]\n", sts);
            break;
        }

        avail = (unsigned int)status_buf[0] |
                ((unsigned int)status_buf[1] << 8);
        connected = status_buf[2];

        /* Connection dropped? */
        if (!connected && avail == 0) {
            printf("\n[Connection closed]\n");
            break;
        }

        /* ---- READ and display incoming data ---- */
        while (avail > 0) {
            readlen = (avail > RXBUF_SIZE) ? RXBUF_SIZE : avail;
            sts = net_read(rxbuf, readlen);
            if (sts != 1)
                break;
            for (i = 0; i < readlen; ++i) {
                if (zm_detect_byte(rxbuf[i], i, readlen)) {
                    avail = 0;  /* ZMODEM consumed remaining bytes */
                    break;
                }
            }
            if (avail > 0) avail -= readlen;
        }

        /* ---- Check keyboard and send ---- */
        if (key_available()) {
            ch = keyboard_get();
            /* Arrow keys → VT100 escape sequences for shell history etc. */
            if (ch == 0x1C) {        /* cursor up → ESC[A */
                net_send_str("\033[A");
            } else if (ch == 0x1D) { /* cursor down → ESC[B */
                net_send_str("\033[B");
            } else if (ch == 0x1F) { /* cursor right → ESC[C */
                net_send_str("\033[C");
            } else if (ch == 0x1E) { /* cursor left → ESC[D */
                net_send_str("\033[D");
            } else {
                ascii = atascii_to_ascii(ch);
                txbuf[0] = ascii;
                net_write(txbuf, 1);
            }
        }

        /* Re-enable PROCEED interrupt — the handler disabled it
           (PACTL bit 0=0) to prevent rapid-fire IRQs. */
        trip = 0;
        POKE(PACTL, PEEK(PACTL) | 1);
    }

    /* ---- Cleanup ---- */
    remove_proceed();
    /* Keep PROCEED disabled — FujiNet may still pulse the line after
       disconnect, and the default OS handler can't cope with rapid-fire
       IRQs (each RTI allows one instruction before the next edge fires). */
    POKE(PACTL, PEEK(PACTL) & ~1);
    printf("\nClosing...");
    net_close();
    printf(" Done.\n");

    keyboard_close();
    /* Restore SIO buzzer */
    POKE(0x41, old_soundr);

    return 0;
}
