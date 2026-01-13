.include ../c64/c64_defines.asm

BasicStart()

jmp start

* = $c000

start:
    sei                  ; set interrupt bit, make the cpu ignore interrupt requests
    lda #$7f             ; switch off interrupt signals from cia-1
    sta $dc0d

    and $d011            ; clear most significant bit of vic's raster register
    sta $d011

    sta $dc0d            ; acknowledge pending interrupts from cia-1
    sta $dd0d            ; acknowledge pending interrupts from cia-2

    lda #210             ; set rasterline where interrupt shall occur
    sta $d012

    lda #<irq            ; set interrupt vectors, pointing to interrupt service routine below
    sta $0314
    lda #>irq
    sta $0315

    lda #$01             ; enable raster interrupt signals from vic
    sta $d01a

    cli                  ; clear interrupt flag, allowing the cpu to respond to interrupt requests
    rts


irq:
    lda #$7
    sta $d020            ; change border colour to yellow

    ldx #$90             ; empty loop to do nothing for just under half a millisecond
pause:
    dex
    bne pause

    lda #$0
    sta $d020            ; change border colour to black

    asl $d019            ; acknowledge the interrupt by clearing the vic's interrupt flag

    jmp $ea31            ; jump into kernal's standard interrupt service routine to handle keyboard scan, cursor display etc.



.org $c100

Init       SEI                  ; set interrupt bit, make the CPU ignore interrupt requests
           LDA #%01111111       ; switch off interrupt signals from CIA-1
           STA $DC0D

           AND $D011            ; clear most significant bit of VIC's raster register
           STA $D011

           STA $DC0D            ; acknowledge pending interrupts from CIA-1
           STA $DD0D            ; acknowledge pending interrupts from CIA-2

           LDA #210             ; set rasterline where interrupt shall occur
           STA $D012

           LDA #<Irq            ; set interrupt vectors, pointing to interrupt service routine below
           STA $0314
           LDA #>Irq
           STA $0315

           LDA #%00000001       ; enable raster interrupt signals from VIC
           STA $D01A

           CLI                  ; clear interrupt flag, allowing the CPU to respond to interrupt requests
           RTS

Irq        LDA #$7
           STA $D020            ; change border colour to yellow

           LDX #$90             ; empty loop to do nothing for just under half a millisecond
Pause      DEX
           BNE Pause

           LDA #$0
           STA $D020            ; change border colour to black

           ASL $D019            ; acknowledge the interrupt by clearing the VIC's interrupt flag

           JMP $EA31            ; jump into KERNAL's standard interrupt service routine to handle keyboard scan, cursor display etc.
