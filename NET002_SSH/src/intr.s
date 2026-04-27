; PROCEED interrupt handler for FujiNet
; Sets trip flag when FujiNet signals data available via SIO PROCEED line.
; Installed at VPRCED ($0202) after successful SSH connection.
;
; IMPORTANT: Disables PROCEED interrupt (PACTL bit 0) before returning.
; FujiNet-PC pulses PROCEED rapidly (-_-_-_-_), which would cause
; continuous IRQ re-entry if the interrupt stayed enabled.  The main
; loop re-enables it after processing (PIA.pactl |= 1).

PACTL   = $D302

        .export _ih
        .import _trip

_ih:    LDA     PACTL
        AND     #$FE        ; clear bit 0 — disable PROCEED IRQ
        STA     PACTL
        LDA     #$01
        STA     _trip
        PLA
        RTI
