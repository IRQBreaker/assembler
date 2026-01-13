; Commodore 64 defines (VIC-II, SID, CIA, memory map)
; Use with: .include "c64_defines.asm"

; -----------------------------------------------------------------------------
; Utility macros
; -----------------------------------------------------------------------------
.macro BasicStart() {
    * = $0801
    .byte $0b, $08, $0a, $00, $9e, $32, $30, $36, $31, $00, $00, $00
}

; -----------------------------------------------------------------------------
; Memory map (common locations)
; -----------------------------------------------------------------------------
C64_SCREEN          = $0400
C64_COLOR           = $D800
C64_BASIC_ROM       = $A000
C64_KERNAL_ROM      = $E000
C64_CHAR_ROM        = $D000        ; char ROM when I/O is banked out
C64_IO              = $D000        ; I/O area when enabled

; Vectors (system)
C64_VEC_IRQ         = $0314
C64_VEC_NMI         = $0318
C64_VEC_RESET       = $FFFC
C64_VEC_NMI_HW      = $FFFA
C64_VEC_IRQ_HW      = $FFFE

; -----------------------------------------------------------------------------
; VIC-II ($D000-$D02E)
; -----------------------------------------------------------------------------
VIC_BASE            = $D000

VIC_SPR0_X          = $D000
VIC_SPR0_Y          = $D001
VIC_SPR1_X          = $D002
VIC_SPR1_Y          = $D003
VIC_SPR2_X          = $D004
VIC_SPR2_Y          = $D005
VIC_SPR3_X          = $D006
VIC_SPR3_Y          = $D007
VIC_SPR4_X          = $D008
VIC_SPR4_Y          = $D009
VIC_SPR5_X          = $D00A
VIC_SPR5_Y          = $D00B
VIC_SPR6_X          = $D00C
VIC_SPR6_Y          = $D00D
VIC_SPR7_X          = $D00E
VIC_SPR7_Y          = $D00F

VIC_SPR_X_MSB       = $D010
VIC_CTRL1           = $D011
VIC_RASTER          = $D012
VIC_LIGHTPEN_X      = $D013
VIC_LIGHTPEN_Y      = $D014
VIC_SPR_ENABLE      = $D015
VIC_CTRL2           = $D016
VIC_SPR_Y_EXPAND    = $D017
VIC_MEMPTR          = $D018
VIC_IRQ_STATUS      = $D019
VIC_IRQ_ENABLE      = $D01A
VIC_SPR_PRIORITY    = $D01B
VIC_SPR_MULTICOLOR  = $D01C
VIC_SPR_X_EXPAND    = $D01D
VIC_SPR_SPR_COLL    = $D01E
VIC_SPR_BG_COLL     = $D01F

VIC_BORDER_COLOR    = $D020
VIC_BG_COLOR0       = $D021
VIC_BG_COLOR1       = $D022
VIC_BG_COLOR2       = $D023
VIC_BG_COLOR3       = $D024
VIC_SPR_MC0         = $D025
VIC_SPR_MC1         = $D026
VIC_SPR0_COLOR      = $D027
VIC_SPR1_COLOR      = $D028
VIC_SPR2_COLOR      = $D029
VIC_SPR3_COLOR      = $D02A
VIC_SPR4_COLOR      = $D02B
VIC_SPR5_COLOR      = $D02C
VIC_SPR6_COLOR      = $D02D
VIC_SPR7_COLOR      = $D02E

; C64 color constants (0-15)
C64_COLOR_BLACK     = $00
C64_COLOR_WHITE     = $01
C64_COLOR_RED       = $02
C64_COLOR_CYAN      = $03
C64_COLOR_PURPLE    = $04
C64_COLOR_GREEN     = $05
C64_COLOR_BLUE      = $06
C64_COLOR_YELLOW    = $07
C64_COLOR_ORANGE    = $08
C64_COLOR_BROWN     = $09
C64_COLOR_LTRED     = $0A
C64_COLOR_DKGRAY    = $0B
C64_COLOR_MDGRAY    = $0C
C64_COLOR_LTGREEN   = $0D
C64_COLOR_LTBLUE    = $0E
C64_COLOR_LTGRAY    = $0F

; VIC_CTRL1 bits
VIC_CTRL1_YSCROLL   = $07
VIC_CTRL1_RSEL      = $08
VIC_CTRL1_DEN       = $10
VIC_CTRL1_BMM       = $20
VIC_CTRL1_ECM       = $40
VIC_CTRL1_RASTER8   = $80

; VIC_CTRL2 bits
VIC_CTRL2_XSCROLL   = $07
VIC_CTRL2_CSEL      = $08
VIC_CTRL2_MCM       = $10
VIC_CTRL2_RES       = $20

; VIC_IRQ bits
VIC_IRQ_RASTER      = $01
VIC_IRQ_SPR_BG      = $02
VIC_IRQ_SPR_SPR     = $04
VIC_IRQ_LIGHTPEN    = $08

; VIC sprite enable bits
VIC_SPR0_BIT        = $01
VIC_SPR1_BIT        = $02
VIC_SPR2_BIT        = $04
VIC_SPR3_BIT        = $08
VIC_SPR4_BIT        = $10
VIC_SPR5_BIT        = $20
VIC_SPR6_BIT        = $40
VIC_SPR7_BIT        = $80

; VIC memory pointer ($D018) bits
VIC_MEMPTR_SCREEN_0000 = $00      ; screen at $0000
VIC_MEMPTR_SCREEN_0400 = $10
VIC_MEMPTR_SCREEN_0800 = $20
VIC_MEMPTR_SCREEN_0C00 = $30
VIC_MEMPTR_SCREEN_1000 = $40
VIC_MEMPTR_SCREEN_1400 = $50
VIC_MEMPTR_SCREEN_1800 = $60
VIC_MEMPTR_SCREEN_1C00 = $70
VIC_MEMPTR_CHAR_0000   = $00      ; char at $0000
VIC_MEMPTR_CHAR_0800   = $02
VIC_MEMPTR_CHAR_1000   = $04
VIC_MEMPTR_CHAR_1800   = $06
VIC_MEMPTR_CHAR_2000   = $08
VIC_MEMPTR_CHAR_2800   = $0A
VIC_MEMPTR_CHAR_3000   = $0C
VIC_MEMPTR_CHAR_3800   = $0E

; VIC sprite multicolor bits in $D01C
VIC_SPR_MC0_BIT     = $01
VIC_SPR_MC1_BIT     = $02
VIC_SPR_MC2_BIT     = $04
VIC_SPR_MC3_BIT     = $08
VIC_SPR_MC4_BIT     = $10
VIC_SPR_MC5_BIT     = $20
VIC_SPR_MC6_BIT     = $40
VIC_SPR_MC7_BIT     = $80

; -----------------------------------------------------------------------------
; SID ($D400-$D418)
; -----------------------------------------------------------------------------
SID_BASE            = $D400

SID_V1_FREQ_LO      = $D400
SID_V1_FREQ_HI      = $D401
SID_V1_PW_LO        = $D402
SID_V1_PW_HI        = $D403
SID_V1_CTRL         = $D404
SID_V1_AD           = $D405
SID_V1_SR           = $D406

SID_V2_FREQ_LO      = $D407
SID_V2_FREQ_HI      = $D408
SID_V2_PW_LO        = $D409
SID_V2_PW_HI        = $D40A
SID_V2_CTRL         = $D40B
SID_V2_AD           = $D40C
SID_V2_SR           = $D40D

SID_V3_FREQ_LO      = $D40E
SID_V3_FREQ_HI      = $D40F
SID_V3_PW_LO        = $D410
SID_V3_PW_HI        = $D411
SID_V3_CTRL         = $D412
SID_V3_AD           = $D413
SID_V3_SR           = $D414

SID_FILT_CUTOFF_LO  = $D415
SID_FILT_CUTOFF_HI  = $D416
SID_FILT_RES        = $D417
SID_VOL_FILT        = $D418

; SID voice control bits
SID_CTRL_GATE       = $01
SID_CTRL_SYNC       = $02
SID_CTRL_RING       = $04
SID_CTRL_TEST       = $08
SID_CTRL_TRI        = $10
SID_CTRL_SAW        = $20
SID_CTRL_PULSE      = $40
SID_CTRL_NOISE      = $80

; SID filter routing/resonance ($D417)
SID_FILT_V1         = $01
SID_FILT_V2         = $02
SID_FILT_V3         = $04
SID_FILT_EXT        = $08
SID_RESON_MASK      = $F0

; SID filter mode/volume ($D418)
SID_MODE_LOPASS     = $10
SID_MODE_BANDPASS   = $20
SID_MODE_HIGHPASS   = $40
SID_MODE_V3OFF      = $80

; SID volume bits (lower 4 bits of $D418)
SID_VOL_MASK        = $0F

; -----------------------------------------------------------------------------
; CIA #1 ($DC00-$DC0F) - keyboard/joystick, timers
; -----------------------------------------------------------------------------
CIA1_BASE           = $DC00

CIA1_PRA            = $DC00
CIA1_PRB            = $DC01
CIA1_DDRA           = $DC02
CIA1_DDRB           = $DC03
CIA1_TA_LO          = $DC04
CIA1_TA_HI          = $DC05
CIA1_TB_LO          = $DC06
CIA1_TB_HI          = $DC07
CIA1_TOD_10TH       = $DC08
CIA1_TOD_SEC        = $DC09
CIA1_TOD_MIN        = $DC0A
CIA1_TOD_HR         = $DC0B
CIA1_SDR            = $DC0C
CIA1_ICR            = $DC0D
CIA1_CRA            = $DC0E
CIA1_CRB            = $DC0F

; CIA1 joystick bits (active low)
JOY_PORT2           = $DC00       ; CIA1_PRA
JOY_PORT1           = $DC01       ; CIA1_PRB
JOY_UP              = $01
JOY_DOWN            = $02
JOY_LEFT            = $04
JOY_RIGHT           = $08
JOY_FIRE            = $10

; Key matrix (CIA1 PRA/PRB). Drive rows low on PRB, read columns on PRA.
KBD_PORT_COL        = $DC00       ; CIA1_PRA
KBD_PORT_ROW        = $DC01       ; CIA1_PRB
KBD_ROW0            = $01
KBD_ROW1            = $02
KBD_ROW2            = $04
KBD_ROW3            = $08
KBD_ROW4            = $10
KBD_ROW5            = $20
KBD_ROW6            = $40
KBD_ROW7            = $80

KBD_COL0            = $01
KBD_COL1            = $02
KBD_COL2            = $04
KBD_COL3            = $08
KBD_COL4            = $10
KBD_COL5            = $20
KBD_COL6            = $40
KBD_COL7            = $80

; -----------------------------------------------------------------------------
; CIA #2 ($DD00-$DD0F) - IEC, user port, VIC bank select
; -----------------------------------------------------------------------------
CIA2_BASE           = $DD00

CIA2_PRA            = $DD00
CIA2_PRB            = $DD01
CIA2_DDRA           = $DD02
CIA2_DDRB           = $DD03
CIA2_TA_LO          = $DD04
CIA2_TA_HI          = $DD05
CIA2_TB_LO          = $DD06
CIA2_TB_HI          = $DD07
CIA2_TOD_10TH       = $DD08
CIA2_TOD_SEC        = $DD09
CIA2_TOD_MIN        = $DD0A
CIA2_TOD_HR         = $DD0B
CIA2_SDR            = $DD0C
CIA2_ICR            = $DD0D
CIA2_CRA            = $DD0E
CIA2_CRB            = $DD0F

; CIA2 Port A bits (VIC bank select)
CIA2_VIC_BANK_MASK  = $03
CIA2_VIC_BANK_0     = $03         ; $0000-$3FFF
CIA2_VIC_BANK_1     = $02         ; $4000-$7FFF
CIA2_VIC_BANK_2     = $01         ; $8000-$BFFF
CIA2_VIC_BANK_3     = $00         ; $C000-$FFFF

; -----------------------------------------------------------------------------
; KERNAL jump table (routine vectors) $FF81-$FFF3
; -----------------------------------------------------------------------------
KERNAL_CINT         = $FF81
KERNAL_IOINIT       = $FF84
KERNAL_RAMTAS       = $FF87
KERNAL_RESTOR       = $FF8A
KERNAL_VECTOR       = $FF8D
KERNAL_SETMSG       = $FF90
KERNAL_SECOND       = $FF93
KERNAL_TKSA         = $FF96
KERNAL_MEMTOP       = $FF99
KERNAL_MEMBOT       = $FF9C
KERNAL_SCNKEY       = $FF9F
KERNAL_SETTMO       = $FFA2
KERNAL_ACPTR        = $FFA5
KERNAL_CIOUT        = $FFA8
KERNAL_UNTLK        = $FFAB
KERNAL_UNLSN        = $FFAE
KERNAL_LISTEN       = $FFB1
KERNAL_TALK         = $FFB4
KERNAL_READST       = $FFB7
KERNAL_SETLFS       = $FFBA
KERNAL_SETNAM       = $FFBD
KERNAL_OPEN         = $FFC0
KERNAL_CLOSE        = $FFC3
KERNAL_CHKIN        = $FFC6
KERNAL_CKOUT        = $FFC9
KERNAL_CLRCHN       = $FFCC
KERNAL_CHRIN        = $FFCF
KERNAL_CHROUT       = $FFD2
KERNAL_LOAD         = $FFD5
KERNAL_SAVE         = $FFD8
KERNAL_SETTIM       = $FFDB
KERNAL_RDTIM        = $FFDE
KERNAL_STOP         = $FFE1
KERNAL_GETIN        = $FFE4
KERNAL_CLALL        = $FFE7
KERNAL_UDTIM        = $FFEA
KERNAL_SCREEN       = $FFED
KERNAL_PLOT         = $FFF0
KERNAL_IOBASE       = $FFF3

; -----------------------------------------------------------------------------
; BASIC entry points (common)
; -----------------------------------------------------------------------------
BASIC_START         = $A000
BASIC_WARM_START    = $A7AE
BASIC_READY         = $A7AE
