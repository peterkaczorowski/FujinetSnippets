/*
 * NET001_NTP_GetDateTime
 *
 * Raw NTP client for Atari 8-bit via FujiNet Network Device (N:).
 * Communicates over UDP with an NTP server using FujiNet SIO.
 *
 * Protocol flow:
 *   1. OPEN  N:UDP://<host>:<port>  (device 0x71, cmd 0x4F)
 *   2. WRITE 48-byte NTP request     (cmd 0x57)
 *   3. Poll  STATUS until data ready  (cmd 0x53)
 *   4. READ  48-byte NTP response     (cmd 0x52)
 *   5. CLOSE                          (cmd 0x43)
 *   6. Parse NTP timestamp and display.
 *
 * Build:  cl65 -t atari -o build/ntp_sio_fujinet.com src/ntp_sio_fujinet.c
 *
 * Requires: FujiNet (real or FujiNet-PC) attached.
 *           FujiNet-PC needs the fnUDP::available() fix
 *           (parsePacket() call) to receive UDP data.
 *
 * Author: Claude / Piotr  2026
 * License: Public domain
 */

#include <stdio.h>
#include <string.h>
#include <atari.h>
#include <peekpoke.h>

/* ------------------------------------------------------------------ */
/* NTP server configuration                                           */
/*                                                                    */
/* Change NTP_HOST and NTP_PORT to test different scenarios:           */
/*                                                                    */
/* Test A - local mock NTP server (mock_ntp_server.py):               */
/*   #define NTP_HOST "127.0.0.1"                                     */
/*   #define NTP_PORT 9123                                            */
/*                                                                    */
/* Test B - public NTP, direct IPv4 (no DNS):                         */
/*   #define NTP_HOST "162.159.200.123"  (Cloudflare Time)            */
/*   #define NTP_HOST "216.239.35.0"     (Google time1)               */
/*                                                                    */
/* Test C - public NTP, DNS name:                                     */
/*   #define NTP_HOST "pl.pool.ntp.org"                               */
/*   #define NTP_HOST "time.cloudflare.com"                           */
/*   #define NTP_HOST "time.google.com"                               */
/* ------------------------------------------------------------------ */
#define NTP_HOST "162.159.200.123"
#define NTP_PORT 123

/* NTP version: 3 (0x1B) or 4 (0x23).                                */
/* NTPv3 has widest server compatibility.                             */
#define NTP_VERSION 3

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
/* From: fujinet-firmware/include/fujiCommandID.h                     */
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
/* NTP constants                                                      */
/* ------------------------------------------------------------------ */
#define NTP_PACKET_SIZE   48
#define NTP_UNIX_DELTA    2208988800UL

/* ------------------------------------------------------------------ */
/* Retry / timeout tuning                                             */
/* ------------------------------------------------------------------ */
#define MAX_ATTEMPTS      3
#define POLLS_PER_ATTEMPT 120   /* STATUS polls per attempt           */
#define VBI_PER_POLL      6     /* ~100ms between polls               */
/* Total wait: 120 * 100ms = ~12 seconds per attempt                 */

/* ------------------------------------------------------------------ */
/* Buffers                                                            */
/* ------------------------------------------------------------------ */
static unsigned char devicespec[80];
static unsigned char ntp_packet[NTP_PACKET_SIZE];
static unsigned char status_buf[4];

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
/* SIO helper: common DCB fields                                      */
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

    call_siov();
    return PEEK(DSTATS);
}

/* ------------------------------------------------------------------ */
/* Print STATUS diagnostics                                           */
/* ------------------------------------------------------------------ */
static void print_status(void)
{
    unsigned int avail;
    avail = (unsigned int)status_buf[0] |
            ((unsigned int)status_buf[1] << 8);
    printf("  raw: $%02X $%02X $%02X $%02X\n",
           status_buf[0], status_buf[1],
           status_buf[2], status_buf[3]);
    printf("  avail=%u conn=%u err=%u",
           avail, status_buf[2], status_buf[3]);
    if (status_buf[3] == 1)
        printf(" (OK)");
    else if (status_buf[3] > 1)
        printf(" (ERR)");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Helper: read big-endian 32-bit unsigned                            */
/* ------------------------------------------------------------------ */
static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |
           ((unsigned long)p[3]);
}

/* ------------------------------------------------------------------ */
/* Days-in-month table                                                */
/* ------------------------------------------------------------------ */
static const unsigned char days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static unsigned char is_leap(unsigned int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Convert Unix timestamp to date/time components                     */
/* ------------------------------------------------------------------ */
static void unix_to_datetime(unsigned long ts,
    unsigned int *year, unsigned char *month, unsigned char *day,
    unsigned char *hour, unsigned char *min, unsigned char *sec)
{
    unsigned int y;
    unsigned char m, dim;

    *sec  = (unsigned char)(ts % 60); ts /= 60;
    *min  = (unsigned char)(ts % 60); ts /= 60;
    *hour = (unsigned char)(ts % 24); ts /= 24;

    y = 1970;
    for (;;) {
        unsigned int days_year = is_leap(y) ? 366 : 365;
        if (ts < (unsigned long)days_year) break;
        ts -= (unsigned long)days_year;
        ++y;
    }
    *year = y;

    for (m = 0; m < 12; ++m) {
        dim = days_in_month[m];
        if (m == 1 && is_leap(y)) ++dim;
        if (ts < (unsigned long)dim) break;
        ts -= (unsigned long)dim;
    }
    *month = m + 1;
    *day   = (unsigned char)ts + 1;
}

/* ------------------------------------------------------------------ */
/* Build NTP request packet                                           */
/* ------------------------------------------------------------------ */
static void build_ntp_request(void)
{
    unsigned char vn_byte;

    memset(ntp_packet, 0, NTP_PACKET_SIZE);

    /*
     * Byte 0: LI (2 bits) | VN (3 bits) | Mode (3 bits)
     *   LI=0, VN=NTP_VERSION, Mode=3 (client)
     *   NTPv3: 0b_00_011_011 = 0x1B
     *   NTPv4: 0b_00_100_011 = 0x23
     */
#if NTP_VERSION == 4
    vn_byte = 0x23;
#else
    vn_byte = 0x1B;
#endif
    ntp_packet[0] = vn_byte;

    /* Byte 2: Poll interval (log2 seconds), e.g. 6 = 64s */
    ntp_packet[2] = 6;

    /* Byte 3: Precision (signed, log2 seconds)
     * -6 = 0xFA (~15ms, typical for low-res clock) */
    ntp_packet[3] = 0xFA;

    /*
     * Bytes 40-47: Transmit Timestamp (T1)
     * Must be non-zero for some servers.
     * We use a simple pseudo-value from Atari RTCLOK.
     */
    ntp_packet[40] = 0x00;
    ntp_packet[41] = 0x00;
    ntp_packet[42] = 0x00;
    ntp_packet[43] = 0x01;  /* non-zero marker */
    ntp_packet[44] = PEEK(0x12);  /* RTCLOK byte 0 */
    ntp_packet[45] = PEEK(0x13);  /* RTCLOK byte 1 */
    ntp_packet[46] = PEEK(0x14);  /* RTCLOK byte 2 */
    ntp_packet[47] = 0x00;
}

/* ------------------------------------------------------------------ */
/* Main program                                                       */
/* ------------------------------------------------------------------ */
int main(void)
{
    unsigned char sts;
    unsigned int avail;
    unsigned char attempt, polls;
    unsigned char got_response;
    unsigned long ntp_secs, unix_secs;
    unsigned int year;
    unsigned char month, day, hour, min, sec;
    unsigned char last_poll_sts;

    printf("=== FujiNet NTP Client ===\n");
    printf("Host: " NTP_HOST "\n");
    printf("Port: %u  NTPv%u\n\n", NTP_PORT, NTP_VERSION);

    /* Build devicespec */
    sprintf((char *)devicespec,
            "N:UDP://" NTP_HOST ":%u\x9b",
            NTP_PORT);

    got_response = 0;

    for (attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {

        if (attempt > 0) {
            printf("\n--- Attempt %u/%u ---\n",
                   attempt + 1, MAX_ATTEMPTS);
            net_close();
            wait_vbi(60);
        }

        /* ---- OPEN ---- */
        printf("OPEN...");
        sts = net_open();
        if (sts != 1) {
            printf(" FAIL (SIO:%u)\n", sts);
            printf("FujiNet not responding.\n");
            continue;
        }
        printf(" OK\n");

        /* ---- WRITE NTP request ---- */
        build_ntp_request();
        printf("WRITE %uB...", NTP_PACKET_SIZE);
        sts = net_write(ntp_packet, NTP_PACKET_SIZE);
        if (sts != 1) {
            printf(" FAIL (SIO:%u)\n", sts);
            continue;
        }
        printf(" OK\n");

        /* ---- Poll STATUS ---- */
        printf("Polling STATUS");
        avail = 0;
        last_poll_sts = 1;
        for (polls = 0; polls < POLLS_PER_ATTEMPT; ++polls) {
            sts = net_status();
            last_poll_sts = sts;
            if (sts != 1) {
                printf("\nSTATUS SIO err: %u\n", sts);
                break;
            }
            avail = (unsigned int)status_buf[0] |
                    ((unsigned int)status_buf[1] << 8);
            if (avail >= NTP_PACKET_SIZE) {
                printf(" data!\n");
                break;
            }
            /* FujiNet-level error? */
            if (status_buf[3] > 1) {
                printf("\nFujiNet err: %u\n", status_buf[3]);
                break;
            }
            /* Progress dot every ~1 second */
            if ((polls % 10) == 9)
                printf(".");
            wait_vbi(VBI_PER_POLL);
        }

        /* Diagnostics */
        if (avail < NTP_PACKET_SIZE) {
            printf("\nTimeout. Last STATUS:\n");
            print_status();
        } else {
            printf("STATUS OK, avail=%u\n", avail);
        }

        if (last_poll_sts != 1)
            continue;

        /* ---- READ ---- */
        if (avail < NTP_PACKET_SIZE) {
            /*
             * avail=0 but data might exist in FujiNet's
             * internal receiveBuffer (UDP status quirk).
             * Try READ as a diagnostic probe.
             */
            printf("READ probe %uB...", NTP_PACKET_SIZE);
        } else {
            printf("READ %uB...", NTP_PACKET_SIZE);
        }

        memset(ntp_packet, 0, NTP_PACKET_SIZE);
        sts = net_read(ntp_packet, NTP_PACKET_SIZE);
        if (sts == 1) {
            printf(" OK\n");
            got_response = 1;
            break;
        }

        printf(" FAIL (SIO:%u)\n", sts);

        if (avail < NTP_PACKET_SIZE) {
            printf("No UDP data received.\n");
            printf("Possible causes:\n");
            printf("- FujiNet-PC fnUDP bug\n");
            printf("  (needs parsePacket fix)\n");
            printf("- DNS cannot resolve host\n");
            printf("- Firewall blocks UDP/%u\n",
                   NTP_PORT);
            printf("- NTP server unreachable\n");
        }
    }

    /* ---- Close ---- */
    net_close();

    if (!got_response) {
        printf("\n=== FAILED ===\n");
        printf("%u attempts, no NTP response.\n",
               MAX_ATTEMPTS);
        printf("\nDiagnostic steps:\n");
        printf("1. Apply fnUDP.cpp fix\n");
        printf("2. Rebuild FujiNet-PC\n");
        printf("3. Test with mock server\n");
        printf("4. Check UDP/%u access\n", NTP_PORT);
        printf("5. Try direct IP host\n");
        return 1;
    }

    /* ---- Parse NTP response ---- */
    ntp_secs = read_be32(&ntp_packet[40]);

    printf("\n=== NTP Response ===\n");
    printf("Byte0: $%02X (LI=%u VN=%u M=%u)\n",
           ntp_packet[0],
           (ntp_packet[0] >> 6) & 3,
           (ntp_packet[0] >> 3) & 7,
           ntp_packet[0] & 7);
    printf("Stratum: %u\n", ntp_packet[1]);
    printf("NTP secs: %lu\n", ntp_secs);

    if (ntp_secs == 0) {
        printf("\nTransmit Timestamp = 0!\n");
        printf("Kiss-of-death or bad server.\n");
        return 1;
    }

    unix_secs = ntp_secs - NTP_UNIX_DELTA;
    unix_to_datetime(unix_secs, &year, &month, &day,
                     &hour, &min, &sec);

    printf("\nDate (UTC): %u-%02u-%02u\n",
           year, month, day);
    printf("Time (UTC): %02u:%02u:%02u\n",
           hour, min, sec);
    printf("\n=== Done! ===\n");

    return 0;
}
