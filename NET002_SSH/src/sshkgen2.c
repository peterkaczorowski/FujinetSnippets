/*
 * sshkgen2 — SSH key generator for FujiNet (overwrite mode)
 *
 * Sends N:SSH.KEYGEN://ed25519/?overwrite=1 to FujiNet, reads the
 * result, prints it, and exits.  Replaces existing keys if present.
 *
 * Build:  cl65 -t atari -o build/sshkgen2.com src/sshkgen2.c
 *
 * Requires: FujiNet firmware with SSH.KEYGEN protocol support.
 *
 * Author: Claude / Piotr  2026
 * License: Public domain
 */

#include <stdio.h>
#include <string.h>
#include <atari.h>
#include <peekpoke.h>

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
#define NETCMD_STATUS 0x53

#define MODE_READWRITE 12
#define TRANS_NONE     0

#define SIO_READ  0x40
#define SIO_WRITE 0x80
#define SIO_NONE  0x00

/* ------------------------------------------------------------------ */
/* Buffers                                                            */
/* ------------------------------------------------------------------ */
static unsigned char devicespec[] = "N:SSH.KEYGEN://ed25519/?overwrite=1\x9b";
static unsigned char status_buf[4];
static unsigned char rxbuf[256];

/* ------------------------------------------------------------------ */
/* Call SIO vector                                                    */
/* ------------------------------------------------------------------ */
static void call_siov(void)
{
    __asm__("jsr $E459");
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
    POKE(DTIMLO, 2);

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    unsigned char sts;
    unsigned int avail, readlen;
    unsigned char old_soundr;

    /* Silence SIO buzzer */
    old_soundr = PEEK(0x41);
    POKE(0x41, 0);

    /* Clear screen */
    putchar(0x7D);

    printf("=== FujiNet SSH Key Generator ===\n");
    printf("    (overwrite mode)\n\n");
    printf("Generating Ed25519 key pair...\n");

    /* OPEN — triggers key generation on FujiNet */
    POKE(DTIMLO, 60);  /* longer timeout for key generation */
    sts = net_open();
    if (sts != 1) {
        printf("\nOPEN failed (SIO:%u)\n", sts);
        printf("Check FujiNet firmware.\n");
        net_close();
        POKE(0x41, old_soundr);
        return 1;
    }

    /* STATUS — check for response data */
    sts = net_status();
    if (sts != 1) {
        printf("\nSTATUS failed (SIO:%u)\n", sts);
        net_close();
        POKE(0x41, old_soundr);
        return 1;
    }

    avail = (unsigned int)status_buf[0] |
            ((unsigned int)status_buf[1] << 8);

    /* READ and print response */
    printf("\n");
    while (avail > 0) {
        readlen = (avail > sizeof(rxbuf) - 1) ? sizeof(rxbuf) - 1 : avail;
        sts = net_read(rxbuf, readlen);
        if (sts != 1) {
            printf("[READ error: %u]\n", sts);
            break;
        }
        rxbuf[readlen] = '\0';
        printf("%s", (char *)rxbuf);
        avail -= readlen;
    }

    printf("\n");

    /* CLOSE */
    net_close();

    /* Restore SIO buzzer */
    POKE(0x41, old_soundr);

    return 0;
}
