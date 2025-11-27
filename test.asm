; Test file for 6502 assembler — expression support

; Test .org with an expression that equals $1000
.org $0800 + $0800

DATA = $0200

start:
    ; Immediate expression (division)
    LDX #$10/2           ; -> #$08
    INX
    INY

loop
    ; Immediate expression with precedence and parentheses
    LDA #(1 + 2*3)       ; -> #$07
    ; Absolute,X with expression
    STA DATA + 4,X      ; -> $0204,X
    INX
    ; Another immediate expression
    CPX #($08 + 8)       ; -> #$10
    ; Relative branch to expression (no-op +0)
    BNE loop + 0

    ; Data directives using expressions
    .byte 1+2, $10/2, (3*4)-5
    .word start+1, loop + 2*2, $1000 + 0x20

table:
    .byte $AA, $BB, $CC, $DD

end
    ; Indirect JMP with expression
    JMP (table + 2)

; New segment at $2000 — should zero-fill gap from end to $2000
.org $2000
.byte $01

; Accept spaces after commas in operands
    LDA $10 , X
    LDA ($10) , Y

; Support "* = expr" syntax for origin
* = $2100
.byte $02

; Test current PC symbol '*' in expressions and overflow warnings
.org $3000
.byte * - $3000         ; expect 0
.word * - 1             ; at $3001, expect $3000
.byte 255               ; max in range
.word 65535             ; max in range

; Low/high byte operators for address constants
.org $3100
    LDA #<table2
    STA $02
    LDA #>table2
    STA $03
    LDY #$0
    LDA ($02),Y
    STA $2000
    RTS

table2
    .byte $01,$02,$03,$04

; Binary literal tests
.org $3200
    ; Immediate binary with '%'
    LDA #%01111111       ; -> #$7F
    STA $2001
    ; Immediate binary with '0b'
    LDA #0b00001111      ; -> #$0F
    STA $2002
    ; Data bytes with binary forms
    .byte %10101010, 0b01010101

; .text directive tests
.org $3300
hello:
    .text "hello world"   ; 11 bytes, no terminator
singleq:
    .text 'ABC xyz'        ; single quotes also accepted

; Local temporary labels: '-', '--', '+' and '++'
.org $3400
; Backward references with '-' and '--'
    LDX #$00
-   INX                 ; first '-'
-   DEX                 ; second '-'
    BNE --              ; branch to first '-' (INX)
    NOP

; Forward references with '+' and '++'
    LDX #$00
    BEQ ++              ; skip to second '+' (RTS)
+   NOP                 ; first '+'
+   RTS                 ; second '+'

; -----------------------------------------------------------------------------
; Exhaustive tests for all legal opcodes and addressing modes
; -----------------------------------------------------------------------------
.org $3500

; Constants for operands
ZP0 = $00
ZP1 = $10
ZP2 = $20
ABS = $1234

legal_opcodes:
; ADC
    ADC #$01
    ADC $10
    ADC $10,X
    ADC ABS
    ADC ABS,X
    ADC ABS,Y
    ADC (ZP2,X)
    ADC (ZP2),Y
; AND
    AND #$02
    AND $10
    AND $10,X
    AND ABS
    AND ABS,X
    AND ABS,Y
    AND (ZP2,X)
    AND (ZP2),Y
; ASL
    ASL A
    ASL $10
    ASL $10,X
    ASL ABS
    ASL ABS,X
; Branches (relative)
    BCC *
    BCS *
    BEQ *
    BMI *
    BNE *
    BPL *
    BVC *
    BVS *
; BIT
    BIT $10
    BIT ABS
; BRK
    BRK
; Flag clears
    CLC
    CLD
    CLI
    CLV
; CMP
    CMP #$03
    CMP $10
    CMP $10,X
    CMP ABS
    CMP ABS,X
    CMP ABS,Y
    CMP (ZP2,X)
    CMP (ZP2),Y
; CPY
    CPY #$04
    CPY $10
    CPY ABS
; CPX
    CPX #$05
    CPX $10
    CPX ABS
; DEC
    DEC $10
    DEC $10,X
    DEC ABS
    DEC ABS,X
; DEX/DEY
    DEX
    DEY
; EOR
    EOR #$06
    EOR $10
    EOR $10,X
    EOR ABS
    EOR ABS,X
    EOR ABS,Y
    EOR (ZP2,X)
    EOR (ZP2),Y
; INC
    INC $10
    INC $10,X
    INC ABS
    INC ABS,X
; INX/INY
    INX
    INY
; JMP/JSR
    JMP ABS
    JMP (ABS)
    JSR ABS
; LDA
    LDA #$07
    LDA $10
    LDA $10,X
    LDA ABS
    LDA ABS,X
    LDA ABS,Y
    LDA (ZP2,X)
    LDA (ZP2),Y
; LDX
    LDX #$08
    LDX $10
    LDX $10,Y
    LDX ABS
    LDX ABS,Y
; LDY
    LDY #$09
    LDY $10
    LDY $10,X
    LDY ABS
    LDY ABS,X
; LSR
    LSR A
    LSR $10
    LSR $10,X
    LSR ABS
    LSR ABS,X
; ORA
    ORA #$0A
    ORA $10
    ORA $10,X
    ORA ABS
    ORA ABS,X
    ORA ABS,Y
    ORA (ZP2,X)
    ORA (ZP2),Y
; Stack ops
    PHA
    PHP
    PLA
    PLP
; ROL
    ROL A
    ROL $10
    ROL $10,X
    ROL ABS
    ROL ABS,X
; ROR
    ROR A
    ROR $10
    ROR $10,X
    ROR ABS
    ROR ABS,X
; RTI/RTS
    RTI
    RTS
; SBC
    SBC #$0B
    SBC $10
    SBC $10,X
    SBC ABS
    SBC ABS,X
    SBC ABS,Y
    SBC (ZP2,X)
    SBC (ZP2),Y
; Flag sets
    SEC
    SED
    SEI
; STA
    STA $10
    STA $10,X
    STA ABS
    STA ABS,X
    STA ABS,Y
    STA (ZP2,X)
    STA (ZP2),Y
; STX
    STX $10
    STX $10,Y
    STX ABS
; STY
    STY $10
    STY $10,X
    STY ABS
; Transfers and NOP
    TAX
    TAY
    TSX
    TXA
    TXS
    TYA
    NOP

; -----------------------------------------------------------------------------
; Macro tests
; -----------------------------------------------------------------------------
.org $3700

; Define a macro with two parameters and an internal loop label.
.macro ClearScreen(screen,clearByte) {
    lda #clearByte
    ldx #0
Loop:
    sta screen,x
    sta screen+$100,x
    sta screen+$200,x
    sta screen+$300,x
    inx
    bne Loop
}

; Invoke macro twice; internal label should not collide between calls.
ClearScreen($0400,$20)
ClearScreen($4400,$20)

; Another small macro to test argument substitution in expressions and immediate.
.macro AddImm(val) {
    clc
    adc #val
}

    lda #$10
    AddImm(5)        ; becomes: clc / adc #5

.macro PushRegs() {
    pha
    txa
    pha
    tya
    pha
}

.macro PopRegs() {
    pla
    tay
    pla
    tax
    pla
}

    PushRegs()
    PopRegs()
    rts

; FILL
.org $3800

.fill 256, 1

foobar .fill 256, 2
