6502 Assembler

Overview
- Two-pass assembler for the 6502 ISA with a small, clear codebase.
- Supports labels, expressions (+, -, *, /, parentheses, unary ±), and common directives.
- Emits a contiguous binary image; by default prefixes a 2‑byte little‑endian load address (PRG header).

Quick Start
- Build: 'make'
- Assemble: './assembler input.asm output.bin'
- With PRG header (default): first two bytes are the load address.
- Without header: './assembler --no-prg-header input.asm output.bin'

Directives
- '.org expr' and '* = expr': set program counter to 'expr'.
- '.byte expr[, expr ...]': emit bytes (each 'expr' must be 0..255).
- '.word expr[, expr ...]': emit 16-bit little‑endian words (each 'expr' 0..65535).
- '.text "string"' (or with single quotes): emit ASCII bytes for the quoted string as-is; no terminator is appended and escape sequences are not interpreted.
- 'NAME = expr': define a constant symbol with the value of 'expr' (usable later like a label).
- '.incbin "file"': include bytes from an external file (single or double quotes accepted).
- '.include "file.asm"': inline another assembly source file at this point. Paths are resolved relative to the including file; nested includes are supported.

Expressions
- Use labels and numbers in expressions with '+ - * /' and parentheses.
- Number formats: hex '$7F'/'0x7F', binary '%1010'/'0b1010', decimal '127'.
- Current PC symbol: '*' may be used inside expressions (e.g., '.byte * - $1000').
- Low/High byte operators: '<expr' yields the low byte, '>expr' yields the high byte (e.g., 'LDA #<label', 'LDA #>label').
- Range checking: values outside 8‑bit/16‑bit ranges cause errors (no truncation).

Local Temporary Labels
- Use '-' to mark a backward target and '+' to mark a forward target.
- Use '--' to refer to the second previous '-' and '++' to refer to the second next '+'. Longer runs work similarly (nth previous/next).
- Reuse them multiple times; references pick by proximity and count.
- Example:
  ldx #$00
 - inx
   bne -

  ldx #$00
 - lda $c000,x
   beq +
   inx
   bne -
 + rts

CLI Options
- '--prg-header | -H': write 2‑byte load address header (default).
- '--no-prg-header | -N': disable load address header.
- '--illegal-opcodes | -I': enable unofficial ("illegal") NMOS 6502 opcodes (see below).

Unofficial 6502 Opcodes (when '-I' is enabled)
- Combos (read‑modify‑write):
  - 'slo' (ASL+ORA), 'rla' (ROL+AND), 'sre' (LSR+EOR), 'rra' (ROR+ADC),
    'dcp' (DEC+CMP), 'isc' (INC+SBC) - with zp, zp,x, abs, abs,x, abs,y, (ind,x), (ind,y) where applicable.
- Load/Store combos:
  - 'lax' (A:=M, X:=M) - imm (0xAB), zp, zp,y, abs, abs,y, (ind,x), (ind,y)
  - 'sax' (M:=A&X) - zp, zp,y, abs, (ind,x)
- Immediate specials:
  - 'anc' (AND #imm, set C from bit7) - imm 0x0B and 0x2B
  - 'alr' (AND #imm then LSR) - imm 0x4B
  - 'arr' (AND #imm then ROR) - imm 0x6B
  - 'axs'/'sbx' (X := (A&X) - imm) - imm 0xCB
  - 'xaa' (A := X & imm) - imm 0x8B (unstable)
- Stack/high‑byte interactions:
  - 'las' (A,X,S := M & S) - abs,y 0xBB
  - 'ahx' — (ind,y) 0x93, abs,y 0x9F
  - 'shx' - abs,y 0x9E, 'shy' - abs,x 0x9C, 'tas' - abs,y 0x9B
- CPU lock:
  - 'jam' (synonyms: 'kil', 'hlt') - all known KIL opcodes (0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2)

Examples
- Standard build and test: 'make && make test'
- Illegal opcode test: 'make test-illegal' (assembles 'illegal.asm' with '-I').

'.text' examples
- Labelled string: 'hello: .text "hello world"' (emits 11 bytes at 'hello').
- Single quotes: '.text 'ABC xyz'' (emits 7 bytes).
- Note: Only a single quoted string is accepted per directive; use multiple '.text' lines to concatenate.
