; hsio.s — Copy OS ROM to RAM, then disable ROM.
;
; void copy_rom_disable(void);
;
; Altirra's memory manager discards writes to ROM addresses (dummy handler).
; On real XL hardware, writes pass through ROM to RAM, but not in Altirra.
; So we toggle ROM on/off for each page:
;   1. ROM on  -> read page to temp buffer (reads ROM)
;   2. ROM off -> write temp buffer to RAM (now exposed)
;   3. ROM on  -> repeat for next page
; After all pages: leave ROM disabled permanently.

        .export _copy_rom_disable

PORTB   = $D301
NMIEN   = $D40E

ptr     = $F0
ptrh    = $F1

        .segment "BSS"
rombuf: .res 256                ; temp buffer for one ROM page

        .segment "CODE"

_copy_rom_disable:
        sei
        lda     #$00
        sta     NMIEN           ; disable NMI
        sta     ptr             ; ptr low byte = 0

        ; === Copy $C000-$CFFF (16 pages) ===
        lda     #$C0
        sta     ptrh
@lp1:   jsr     copy_one_page
        inc     ptrh
        lda     ptrh
        cmp     #$D0
        bne     @lp1

        ; === Copy $D800-$FFFF (40 pages) ===
        lda     #$D8
        sta     ptrh
@lp2:   jsr     copy_one_page
        inc     ptrh
        bne     @lp2            ; wraps $FF->$00, Z set -> exit

        ; === Disable ROM permanently ===
        lda     PORTB
        and     #$FE
        sta     PORTB

        ; === Re-enable interrupts ===
        lda     #$40
        sta     NMIEN
        cli
        rts

; ---- Copy one 256-byte page: ROM -> buffer -> RAM ----
; Input: ptr/ptrh = page address
; Trashes: A, Y
copy_one_page:
        ; Step 1: ROM is enabled — read page into buffer
        ldy     #$00
@rd:    lda     (ptr),y
        sta     rombuf,y
        iny
        bne     @rd

        ; Step 2: Disable ROM to expose underlying RAM
        lda     PORTB
        and     #$FE
        sta     PORTB

        ; Step 3: Write buffer contents to RAM
        ldy     #$00
@wr:    lda     rombuf,y
        sta     (ptr),y
        iny
        bne     @wr

        ; Step 4: Re-enable ROM for next page
        lda     PORTB
        ora     #$01
        sta     PORTB
        rts
