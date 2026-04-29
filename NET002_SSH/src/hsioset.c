/*
 * hsioset — Install high-speed SIO for FujiNet
 *
 * Negotiates HSIO with FujiNet (device $71) via $3F command,
 * then patches the OS ROM in RAM so all subsequent SIOV calls
 * use the high-speed AUDF3 divisor (~112 kbit/s instead of
 * ~19200 baud).
 *
 * Works with: OSXL 65c816, standard OSXL, Altirra built-in kernel.
 *
 * Build:  cl65 -t atari -o build/hsioset.com src/hsioset.c src/hsio.s
 *
 * Author: Claude / Piotr  2026
 * License: Public domain
 */

#include <stdio.h>
#include <peekpoke.h>

/* SIO registers */
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

#define FN_DEV_NET  0x71
#define SIO_READ    0x40

/* ROM copy + disable (hsio.s) */
extern void copy_rom_disable(void);

static void call_siov(void)
{
    __asm__("jsr $E459");
}

/* Send $3F to FujiNet — returns AUDF3 divisor or 0 */
static unsigned char net_get_hsio_index(void)
{
    static unsigned char hsio_buf;
    hsio_buf = 0;

    POKE(DDEVIC, FN_DEV_NET);
    POKE(DUNIT, 1);
    POKE(DTIMLO, 5);
    POKE(DCOMND, 0x3F);
    POKE(DSTATS, SIO_READ);
    POKE(DBUFLO, (unsigned)&hsio_buf & 0xFF);
    POKE(DBUFHI, ((unsigned)&hsio_buf >> 8) & 0xFF);
    POKE(DBYTLO, 1);
    POKE(DBYTHI, 0);
    POKE(DAUX1, 0);
    POKE(DAUX2, 0);

    call_siov();
    if (PEEK(DSTATS) == 1)
        return hsio_buf;
    return 0;
}

/* Which scan strategy matched (1=OSXL device-check, 2=OSXL init, 3=Altirra) */
static unsigned char patch_strategy;
/* Strategy 1: address of the LDA opcode at the BNE target */
static unsigned osxl_lda_addr;

/* Scan OS ROM/RAM for the AUDF3 byte to patch.
   Three strategies for different OS kernels. */
static unsigned find_sio_speed_byte(void)
{
    unsigned a;
    signed char offset;
    unsigned target;

    /* Strategy 1: OSXL device-check — follow BNE to LDA #xx */
    for (a = 0xD800; a <= 0xFEF6; ++a) {
        if (PEEK(a)   == 0xAD && PEEK(a+1) == 0x00 &&
            PEEK(a+2) == 0x03 && PEEK(a+3) == 0x29 &&
            PEEK(a+4) == 0x70 && PEEK(a+5) == 0xC9 &&
            PEEK(a+6) == 0x30 && PEEK(a+7) == 0xD0) {
            offset = (signed char)PEEK(a+8);
            target = (unsigned)((int)(a + 9) + (int)offset);
            if (PEEK(target) == 0xA9) {
                patch_strategy = 1;
                osxl_lda_addr = target;
                return target + 1;
            }
        }
    }

    /* Strategy 2: OSXL init — A9 xx 85 3F */
    for (a = 0xD800; a <= 0xFEFC; ++a) {
        if (PEEK(a)   == 0xA9 &&
            PEEK(a+2) == 0x85 && PEEK(a+3) == 0x3F) {
            patch_strategy = 2;
            return a + 1;
        }
    }

    /* Strategy 3: Altirra kernel regdata_normal */
    for (a = 0xD800; a <= 0xFEF7; ++a) {
        if (PEEK(a)   == 0x00 && PEEK(a+1) == 0xA0 &&
            PEEK(a+2) == 0x00 && PEEK(a+3) == 0xA0 &&
            PEEK(a+5) == 0xA0 &&
            PEEK(a+6) == 0x00 && PEEK(a+7) == 0xA0) {
            patch_strategy = 3;
            return a + 4;
        }
    }

    return 0;
}

/* Install a JMP trampoline at $FFBD that separates non-disk (fast)
   from disk-fallback (standard $28) traffic.  This avoids breaking
   disk $3F HSIO negotiation which shares the same default path.
   lda_addr = address of LDA #xx; BRA xx (4 bytes to replace). */
static void install_osxl_trampoline(unsigned lda_addr, unsigned char audf3)
{
    unsigned store_addr;
    signed char bra_off;
    unsigned i;

    /* Compute BRA target (store epilogue) from original code */
    if (PEEK(lda_addr + 2) != 0x80)
        return;
    bra_off = (signed char)PEEK(lda_addr + 3);
    store_addr = (unsigned)((int)(lda_addr + 4) + (int)bra_off);

    /* Verify $FFBD-$FFCF are free (all zeros) */
    for (i = 0xFFBD; i <= 0xFFCF; ++i) {
        if (PEEK(i) != 0x00)
            return;
    }

    /* Write 19-byte trampoline at $FFBD */
    POKE(0xFFBD, 0xAD); POKE(0xFFBE, 0x00); POKE(0xFFBF, 0x03);
    POKE(0xFFC0, 0x29); POKE(0xFFC1, 0x70);
    POKE(0xFFC2, 0xC9); POKE(0xFFC3, 0x30);
    POKE(0xFFC4, 0xF0); POKE(0xFFC5, 0x05);
    POKE(0xFFC6, 0xA9); POKE(0xFFC7, audf3);
    POKE(0xFFC8, 0x4C); POKE(0xFFC9, store_addr & 0xFF);
    POKE(0xFFCA, (store_addr >> 8) & 0xFF);
    POKE(0xFFCB, 0xA9); POKE(0xFFCC, 0x28);
    POKE(0xFFCD, 0x4C); POKE(0xFFCE, store_addr & 0xFF);
    POKE(0xFFCF, (store_addr >> 8) & 0xFF);

    /* Replace LDA #xx; BRA xx with JMP $FFBD; NOP */
    POKE(lda_addr,     0x4C);
    POKE(lda_addr + 1, 0xBD);
    POKE(lda_addr + 2, 0xFF);
    POKE(lda_addr + 3, 0xEA);
}

int main(void)
{
    unsigned char audf3;
    unsigned patch_addr;
    unsigned a;

    audf3 = net_get_hsio_index();
    if (audf3 == 0 || audf3 >= 0x28) {
        printf("HSIO not supported.\n");
        return 1;
    }

    patch_addr = find_sio_speed_byte();
    if (patch_addr == 0) {
        printf("HSIO patch not found.\n");
        return 1;
    }

    /* Copy ROM to RAM and disable ROM (skip if already done) */
    if (PEEK(0xD301) & 0x01)
        copy_rom_disable();

    /* Strategy 1 (OSXL device-check): install JMP trampoline at $FFBD
       to separate non-disk (fast) from disk (standard $28) paths.
       Without this, disk $3F HSIO negotiation would also run at high speed. */
    if (patch_strategy == 1) {
        install_osxl_trampoline(osxl_lda_addr, audf3);
    } else {
        /* Strategy 2/3: patch operand directly */
        POKE(patch_addr, audf3);
    }

    /* Update trampoline speed byte on repeat runs */
    if (PEEK(0xFFBD) == 0xAD)
        POKE(0xFFC7, audf3);

    /* Patch OSXL init sequences: A9 xx 85 3F */
    for (a = 0xD800; a <= 0xFEFC; ++a) {
        if (PEEK(a)   == 0xA9 &&
            PEEK(a+2) == 0x85 && PEEK(a+3) == 0x3F)
            POKE(a+1, audf3);
    }

    /* Patch Altirra kernel regdata_normal table */
    for (a = 0xD800; a <= 0xFEF7; ++a) {
        if (PEEK(a)   == 0x00 && PEEK(a+1) == 0xA0 &&
            PEEK(a+2) == 0x00 && PEEK(a+3) == 0xA0 &&
            PEEK(a+5) == 0xA0 &&
            PEEK(a+6) == 0x00 && PEEK(a+7) == 0xA0)
            POKE(a+4, audf3);
    }

    /* Set ZP $3F/$40 for OSXL */
    POKE(0x3F, audf3);
    POKE(0x40, audf3);

    printf("HSIO Installed.\n\n");
    return 0;
}
