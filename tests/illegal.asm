; Test for unofficial/illegal opcodes

.org $0800

start:
    ; Define helpers
ZP0 = $00
ZP1 = $10
ZP2 = $20
ABS = $1234

    ; LAX — load A and X
    LAX #$01
    LAX ZP1
    LAX $10,Y
    LAX ABS
    LAX ABS,Y
    LAX (ZP2,X)
    LAX (ZP2),Y

    ; SAX — store A & X
    SAX ZP1
    SAX $10,Y
    SAX ABS
    SAX (ZP2,X)

    ; DCP — DEC then CMP
    DCP ZP1
    DCP ZP1,X
    DCP ABS
    DCP ABS,X
    DCP ABS,Y
    DCP (ZP2,X)
    DCP (ZP2),Y

    ; ISC (aka ISB) — INC then SBC
    ISC ZP1
    ISC ZP1,X
    ISC ABS
    ISC ABS,X
    ISC ABS,Y
    ISC (ZP2,X)
    ISC (ZP2),Y

    ; SLO — ASL then ORA
    SLO ZP1
    SLO ZP1,X
    SLO ABS
    SLO ABS,X
    SLO ABS,Y
    SLO (ZP2,X)
    SLO (ZP2),Y

    ; RLA — ROL then AND
    RLA ZP1
    RLA ZP1,X
    RLA ABS
    RLA ABS,X
    RLA ABS,Y
    RLA (ZP2,X)
    RLA (ZP2),Y

    ; SRE — LSR then EOR
    SRE ZP1
    SRE ZP1,X
    SRE ABS
    SRE ABS,X
    SRE ABS,Y
    SRE (ZP2,X)
    SRE (ZP2),Y

    ; RRA — ROR then ADC
    RRA ZP1
    RRA ZP1,X
    RRA ABS
    RRA ABS,X
    RRA ABS,Y
    RRA (ZP2,X)
    RRA (ZP2),Y

    ; ANC — AND #imm then set C from bit7
    ANC #$80

    ; ALR — AND #imm then LSR
    ALR #$0F

    ; ARR — AND #imm then ROR
    ARR #$FF

    ; XAA — unstable; A := X & imm
    XAA #$FF

    ; AXS/SBX — X := (A & X) - imm
    AXS #$10
    SBX #$10

    ; LAS — A,X,S := mem & S
    LAS ABS,Y

    ; AHX/SHX/SHY/TAS — store with high-byte interactions
    AHX (ZP2),Y
    AHX ABS,Y
    SHX ABS,Y
    SHY ABS,X
    TAS ABS,Y

    ; JAM/KIL/HLT — CPU lock-up (no operand)
    JAM
    KIL
    HLT
