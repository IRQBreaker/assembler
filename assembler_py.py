#!/usr/bin/env python3

"""
Python reimplementation of the simple two-pass 6502 assembler

Features (parity with the C version):
- Two-pass assembly with symbol table and expression resolution
- Directives: .org / * =, .incbin, .byte, .word, .text, .fill, .include
- Labels and constant defines (NAME = expr), lexical scoping via .block/.bend
- Local temporary labels: '-' backward, '+' forward with run-count selection
- Macros: .macro Name(args) { ... } ... } with argument substitution and
  automatic renaming of labels local to the macro body per invocation
- Addressing modes incl. immediate, zeropage/absolute + X/Y, (ind), (ind,X), (ind),Y
- Branch relatives with range checks
- Optional illegal (undocumented) opcodes enabled via --illegal-opcodes
- Default 2-byte PRG header (load address), toggle with --no-prg-header
- Optional symbol map output via --map <file>
"""

from __future__ import annotations

import argparse
import dataclasses
import errno
import os
import re
import sys
from typing import List, Optional, Tuple, Dict, Callable


# ---------------- Constants / limits ----------------

MAX_LINES = 20000
MAX_SYMBOLS = 5000
MAX_OUTPUT = 65536
MAX_INCLUDE_DEPTH = 64

BYTE_MAX = 0xFF
WORD_MAX = 0xFFFF
BRANCH_MIN = -128
BRANCH_MAX = 127
PRG_HEADER_SIZE = 2


# ---------------- Addressing modes ----------------


class AddrMode:
    NONE = "NONE"
    IMMEDIATE = "IMMEDIATE"
    ZEROPAGE = "ZEROPAGE"
    ZEROPAGE_X = "ZEROPAGE_X"
    ZEROPAGE_Y = "ZEROPAGE_Y"
    ABSOLUTE = "ABSOLUTE"
    ABSOLUTE_X = "ABSOLUTE_X"
    ABSOLUTE_Y = "ABSOLUTE_Y"
    INDIRECT = "INDIRECT"
    INDIRECT_X = "INDIRECT_X"
    INDIRECT_Y = "INDIRECT_Y"
    RELATIVE = "RELATIVE"
    ACCUMULATOR = "ACCUMULATOR"


def addrmode_str(m: str) -> str:
    return {
        AddrMode.NONE: "implicit",
        AddrMode.IMMEDIATE: "immediate",
        AddrMode.ZEROPAGE: "zeropage",
        AddrMode.ZEROPAGE_X: "zeropage,X",
        AddrMode.ZEROPAGE_Y: "zeropage,Y",
        AddrMode.ABSOLUTE: "absolute",
        AddrMode.ABSOLUTE_X: "absolute,X",
        AddrMode.ABSOLUTE_Y: "absolute,Y",
        AddrMode.INDIRECT: "(indirect)",
        AddrMode.INDIRECT_X: "(indirect,X)",
        AddrMode.INDIRECT_Y: "(indirect),Y",
        AddrMode.RELATIVE: "relative",
        AddrMode.ACCUMULATOR: "accumulator",
    }.get(m, "unknown")


# ---------------- Data structures ----------------


@dataclasses.dataclass
class Operand:
    mode: str = AddrMode.NONE
    value: int = 0
    expr: Optional[str] = None


@dataclasses.dataclass
class AsmLine:
    lineno: int
    filename: Optional[str]
    label: Optional[str]
    mnemonic: Optional[str]
    op: Operand
    extra: Optional[str]
    def_name: Optional[str]
    def_expr: Optional[str]


@dataclasses.dataclass
class Symbol:
    name: str
    addr: int
    scope_depth: int
    is_label: bool  # labels are scoped; defines are global (scope_depth=0)


@dataclasses.dataclass
class MacroDef:
    name: str
    argc: int
    argnames: List[str]
    body: List[str]
    local_labels: List[str]


@dataclasses.dataclass
class Opcode:
    mn: str
    mode: str
    opcode: int
    length: int


# ---------------- Globals (kept module-local) ----------------

lines: List[Optional[AsmLine]] = []
symtab: List[Symbol] = []
line_scope_depth: List[int] = []
current_scope_depth: int = 0

outbuf = bytearray(MAX_OUTPUT)
out_lo: int = MAX_OUTPUT
out_hi: int = 0

origin: int = 0
g_eval_pc: int = 0  # current PC for expression evaluation ('*')
allow_illegal: bool = False

# Local temporary labels ('-' and '+') bookkeeping
minus_idx: List[int] = []
minus_pc: List[int] = []
plus_idx: List[int] = []
plus_pc: List[int] = []
current_line_index: int = -1

include_depth = 0

macros: Dict[str, MacroDef] = {}
macro_expand_counter = 0


# ---------------- Error helpers ----------------


def asm_error_file(file: Optional[str], lineno: int, msg: str, *args) -> None:
    prefix = f"Error ({file}:{lineno}): " if file else f"Error (line {lineno}): "
    sys.stderr.write(prefix + (msg % args) + "\n")
    raise SystemExit(1)


def asm_error(lineno: int, msg: str, *args) -> None:
    asm_error_file(None, lineno, msg, *args)


def debug(_msg: str, *_args) -> None:
    # Uncomment for debugging
    # sys.stderr.write(_msg % _args + "\n")
    pass


# ---------------- Utility: identifiers and trimming ----------------


_re_trim = re.compile(r"^\s+|\s+$")


def trim(s: str) -> str:
    return _re_trim.sub("", s)


def is_ident_start(ch: str) -> bool:
    return ("a" <= ch <= "z") or ("A" <= ch <= "Z") or (ch == "_")


def is_ident_char(ch: str) -> bool:
    return is_ident_start(ch) or ("0" <= ch <= "9")


# ---------------- Number parsing ----------------


def parse_number(text: str) -> Optional[int]:
    s = text.strip()
    if not s:
        return None

    # Binary: %1010 or 0b1010
    if s.startswith("%"):
        try:
            return int(s[1:], 2)
        except ValueError:
            return None

    if s.startswith("$"):
        try:
            return int(s[1:], 16)
        except ValueError:
            return None

    if s.lower().startswith("0x"):
        try:
            return int(s[2:], 16)
        except ValueError:
            return None

    if s.lower().startswith("0b"):
        try:
            return int(s[2:], 2)
        except ValueError:
            return None

    try:
        return int(s, 10)
    except ValueError:
        return None


# ---------------- Symbol table and scoped lookup ----------------


def sym_lookup(name: str) -> int:
    for s in symtab:
        if s.name == name:
            return s.addr
    return -1


def sym_lookup_scoped(name: str, scope_depth_limit: int) -> int:
    best_idx = -1
    best_scope = -1
    for i, s in enumerate(symtab):
        if s.name == name and s.is_label:
            if 0 <= s.scope_depth <= scope_depth_limit and s.scope_depth > best_scope:
                best_scope = s.scope_depth
                best_idx = i
    if best_idx >= 0:
        return symtab[best_idx].addr
    # fallback to defines (global)
    for s in symtab:
        if s.name == name and not s.is_label:
            return s.addr
    return -1


def sym_add_scoped(
    name: str,
    addr: int,
    scope_depth_in: int,
    is_label: bool,
    file: Optional[str],
    lineno: int,
) -> None:
    if is_label:
        for s in symtab:
            if s.is_label and s.scope_depth == scope_depth_in and s.name == name:
                asm_error_file(file, lineno, "Duplicate label in scope: %s", name)
    else:
        for s in symtab:
            if not s.is_label and s.name == name:
                asm_error_file(file, lineno, "Duplicate define: %s", name)
    if len(symtab) >= MAX_SYMBOLS:
        asm_error_file(file, lineno, "Too many symbols")
    symtab.append(
        Symbol(
            name=name,
            addr=addr,
            scope_depth=(scope_depth_in if is_label else 0),
            is_label=is_label,
        )
    )


# ---------------- Expression evaluation ----------------


def _expr_skip_ws(s: str, i: int) -> int:
    n = len(s)
    while i < n and s[i].isspace():
        i += 1
    return i


def _expr_parse_token(s: str, i: int) -> Tuple[Optional[str], int]:
    i = _expr_skip_ws(s, i)
    n = len(s)
    start = i
    while i < n and not s[i].isspace() and s[i] not in "+-*/()":
        i += 1
    if i == start:
        return None, start
    return s[start:i], i


def _eval_parse_factor(s: str, i: int) -> Tuple[Optional[int], int]:
    global g_eval_pc, current_scope_depth
    i = _expr_skip_ws(s, i)
    sign = 1
    if i < len(s) and s[i] == '+':
        i += 1
        i = _expr_skip_ws(s, i)
    elif i < len(s) and s[i] == '-':
        sign = -1
        i += 1
        i = _expr_skip_ws(s, i)

    if i < len(s) and s[i] in '<>':
        op = s[i]
        val, j = _eval_parse_factor(s, i + 1)
        if val is None:
            return None, i
        v = (val & 0xFF) if op == '<' else ((val >> 8) & 0xFF)
        return sign * v, j

    if i < len(s) and s[i] == '(':
        v, j = _eval_parse_expr(s, i + 1)
        if v is None:
            return None, i
        j = _expr_skip_ws(s, j)
        if j >= len(s) or s[j] != ')':
            return None, i
        return sign * v, j + 1

    if i < len(s) and s[i] == '*':
        return sign * g_eval_pc, i + 1

    tok, j = _expr_parse_token(s, i)
    if tok is None:
        return None, i
    lit = parse_number(tok)
    if lit is not None:
        return sign * lit, j
    addr = sym_lookup_scoped(tok, current_scope_depth)
    if addr < 0:
        return None, i
    return sign * addr, j


def _eval_parse_term(s: str, i: int) -> Tuple[Optional[int], int]:
    lhs, i = _eval_parse_factor(s, i)
    if lhs is None:
        return None, i
    i = _expr_skip_ws(s, i)
    while i < len(s) and s[i] in "*/":
        op = s[i]
        rhs, j = _eval_parse_factor(s, i + 1)
        if rhs is None:
            return None, i
        if op == '*':
            lhs = lhs * rhs
        else:
            if rhs == 0:
                return None, i
            lhs = int(lhs / rhs)
        i = _expr_skip_ws(s, j)
    return lhs, i


def _eval_parse_expr(s: str, i: int) -> Tuple[Optional[int], int]:
    lhs, i = _eval_parse_term(s, i)
    if lhs is None:
        return None, i
    i = _expr_skip_ws(s, i)
    while i < len(s) and s[i] in "+-":
        op = s[i]
        rhs, j = _eval_parse_term(s, i + 1)
        if rhs is None:
            return None, i
        lhs = lhs + rhs if op == '+' else lhs - rhs
        i = _expr_skip_ws(s, j)
    return lhs, i


def eval_expr(expr: str) -> Optional[int]:
    v, i = _eval_parse_expr(expr, 0)
    if v is None:
        return None
    i = _expr_skip_ws(expr, i)
    if i != len(expr):
        return None
    return v


# ---------------- Opcode tables ----------------


def _op(mn: str, mode: str, opcode: int, length: int) -> Opcode:
    return Opcode(mn, mode, opcode, length)


opcode_table: List[Opcode] = [
    _op("adc", AddrMode.IMMEDIATE, 0x69, 2),
    _op("adc", AddrMode.ZEROPAGE, 0x65, 2),
    _op("adc", AddrMode.ZEROPAGE_X, 0x75, 2),
    _op("adc", AddrMode.ABSOLUTE, 0x6D, 3),
    _op("adc", AddrMode.ABSOLUTE_X, 0x7D, 3),
    _op("adc", AddrMode.ABSOLUTE_Y, 0x79, 3),
    _op("adc", AddrMode.INDIRECT_X, 0x61, 2),
    _op("adc", AddrMode.INDIRECT_Y, 0x71, 2),

    _op("and", AddrMode.IMMEDIATE, 0x29, 2),
    _op("and", AddrMode.ZEROPAGE, 0x25, 2),
    _op("and", AddrMode.ZEROPAGE_X, 0x35, 2),
    _op("and", AddrMode.ABSOLUTE, 0x2D, 3),
    _op("and", AddrMode.ABSOLUTE_X, 0x3D, 3),
    _op("and", AddrMode.ABSOLUTE_Y, 0x39, 3),
    _op("and", AddrMode.INDIRECT_X, 0x21, 2),
    _op("and", AddrMode.INDIRECT_Y, 0x31, 2),

    _op("asl", AddrMode.ACCUMULATOR, 0x0A, 1),
    _op("asl", AddrMode.ZEROPAGE, 0x06, 2),
    _op("asl", AddrMode.ZEROPAGE_X, 0x16, 2),
    _op("asl", AddrMode.ABSOLUTE, 0x0E, 3),
    _op("asl", AddrMode.ABSOLUTE_X, 0x1E, 3),

    _op("bcc", AddrMode.RELATIVE, 0x90, 2),
    _op("bcs", AddrMode.RELATIVE, 0xB0, 2),
    _op("beq", AddrMode.RELATIVE, 0xF0, 2),
    _op("bit", AddrMode.ZEROPAGE, 0x24, 2),
    _op("bit", AddrMode.ABSOLUTE, 0x2C, 3),
    _op("bmi", AddrMode.RELATIVE, 0x30, 2),
    _op("bne", AddrMode.RELATIVE, 0xD0, 2),
    _op("bpl", AddrMode.RELATIVE, 0x10, 2),
    _op("brk", AddrMode.NONE, 0x00, 1),
    _op("bvc", AddrMode.RELATIVE, 0x50, 2),
    _op("bvs", AddrMode.RELATIVE, 0x70, 2),

    _op("clc", AddrMode.NONE, 0x18, 1),
    _op("cld", AddrMode.NONE, 0xD8, 1),
    _op("cli", AddrMode.NONE, 0x58, 1),
    _op("clv", AddrMode.NONE, 0xB8, 1),

    _op("cmp", AddrMode.IMMEDIATE, 0xC9, 2),
    _op("cmp", AddrMode.ZEROPAGE, 0xC5, 2),
    _op("cmp", AddrMode.ZEROPAGE_X, 0xD5, 2),
    _op("cmp", AddrMode.ABSOLUTE, 0xCD, 3),
    _op("cmp", AddrMode.ABSOLUTE_X, 0xDD, 3),
    _op("cmp", AddrMode.ABSOLUTE_Y, 0xD9, 3),
    _op("cmp", AddrMode.INDIRECT_X, 0xC1, 2),
    _op("cmp", AddrMode.INDIRECT_Y, 0xD1, 2),

    _op("cpy", AddrMode.IMMEDIATE, 0xC0, 2),
    _op("cpy", AddrMode.ZEROPAGE, 0xC4, 2),
    _op("cpy", AddrMode.ABSOLUTE, 0xCC, 3),

    _op("cpx", AddrMode.IMMEDIATE, 0xE0, 2),
    _op("cpx", AddrMode.ZEROPAGE, 0xE4, 2),
    _op("cpx", AddrMode.ABSOLUTE, 0xEC, 3),

    _op("dec", AddrMode.ZEROPAGE, 0xC6, 2),
    _op("dec", AddrMode.ZEROPAGE_X, 0xD6, 2),
    _op("dec", AddrMode.ABSOLUTE, 0xCE, 3),
    _op("dec", AddrMode.ABSOLUTE_X, 0xDE, 3),

    _op("dex", AddrMode.NONE, 0xCA, 1),
    _op("dey", AddrMode.NONE, 0x88, 1),

    _op("eor", AddrMode.IMMEDIATE, 0x49, 2),
    _op("eor", AddrMode.ZEROPAGE, 0x45, 2),
    _op("eor", AddrMode.ZEROPAGE_X, 0x55, 2),
    _op("eor", AddrMode.ABSOLUTE, 0x4D, 3),
    _op("eor", AddrMode.ABSOLUTE_X, 0x5D, 3),
    _op("eor", AddrMode.ABSOLUTE_Y, 0x59, 3),
    _op("eor", AddrMode.INDIRECT_X, 0x41, 2),
    _op("eor", AddrMode.INDIRECT_Y, 0x51, 2),

    _op("inc", AddrMode.ZEROPAGE, 0xE6, 2),
    _op("inc", AddrMode.ZEROPAGE_X, 0xF6, 2),
    _op("inc", AddrMode.ABSOLUTE, 0xEE, 3),
    _op("inc", AddrMode.ABSOLUTE_X, 0xFE, 3),

    _op("inx", AddrMode.NONE, 0xE8, 1),
    _op("iny", AddrMode.NONE, 0xC8, 1),

    _op("jmp", AddrMode.ABSOLUTE, 0x4C, 3),
    _op("jmp", AddrMode.INDIRECT, 0x6C, 3),

    _op("jsr", AddrMode.ABSOLUTE, 0x20, 3),

    _op("lda", AddrMode.IMMEDIATE, 0xA9, 2),
    _op("lda", AddrMode.ZEROPAGE, 0xA5, 2),
    _op("lda", AddrMode.ZEROPAGE_X, 0xB5, 2),
    _op("lda", AddrMode.ABSOLUTE, 0xAD, 3),
    _op("lda", AddrMode.ABSOLUTE_X, 0xBD, 3),
    _op("lda", AddrMode.ABSOLUTE_Y, 0xB9, 3),
    _op("lda", AddrMode.INDIRECT_X, 0xA1, 2),
    _op("lda", AddrMode.INDIRECT_Y, 0xB1, 2),

    _op("ldx", AddrMode.IMMEDIATE, 0xA2, 2),
    _op("ldx", AddrMode.ZEROPAGE, 0xA6, 2),
    _op("ldx", AddrMode.ZEROPAGE_Y, 0xB6, 2),
    _op("ldx", AddrMode.ABSOLUTE, 0xAE, 3),
    _op("ldx", AddrMode.ABSOLUTE_Y, 0xBE, 3),

    _op("ldy", AddrMode.IMMEDIATE, 0xA0, 2),
    _op("ldy", AddrMode.ZEROPAGE, 0xA4, 2),
    _op("ldy", AddrMode.ZEROPAGE_X, 0xB4, 2),
    _op("ldy", AddrMode.ABSOLUTE, 0xAC, 3),
    _op("ldy", AddrMode.ABSOLUTE_X, 0xBC, 3),

    _op("lsr", AddrMode.ACCUMULATOR, 0x4A, 1),
    _op("lsr", AddrMode.ZEROPAGE, 0x46, 2),
    _op("lsr", AddrMode.ZEROPAGE_X, 0x56, 2),
    _op("lsr", AddrMode.ABSOLUTE, 0x4E, 3),
    _op("lsr", AddrMode.ABSOLUTE_X, 0x5E, 3),

    _op("ora", AddrMode.IMMEDIATE, 0x09, 2),
    _op("ora", AddrMode.ZEROPAGE, 0x05, 2),
    _op("ora", AddrMode.ZEROPAGE_X, 0x15, 2),
    _op("ora", AddrMode.ABSOLUTE, 0x0D, 3),
    _op("ora", AddrMode.ABSOLUTE_X, 0x1D, 3),
    _op("ora", AddrMode.ABSOLUTE_Y, 0x19, 3),
    _op("ora", AddrMode.INDIRECT_X, 0x01, 2),
    _op("ora", AddrMode.INDIRECT_Y, 0x11, 2),

    _op("pha", AddrMode.NONE, 0x48, 1),
    _op("php", AddrMode.NONE, 0x08, 1),
    _op("pla", AddrMode.NONE, 0x68, 1),
    _op("plp", AddrMode.NONE, 0x28, 1),

    _op("rol", AddrMode.ACCUMULATOR, 0x2A, 1),
    _op("rol", AddrMode.ZEROPAGE, 0x26, 2),
    _op("rol", AddrMode.ZEROPAGE_X, 0x36, 2),
    _op("rol", AddrMode.ABSOLUTE, 0x2E, 3),
    _op("rol", AddrMode.ABSOLUTE_X, 0x3E, 3),

    _op("ror", AddrMode.ACCUMULATOR, 0x6A, 1),
    _op("ror", AddrMode.ZEROPAGE, 0x66, 2),
    _op("ror", AddrMode.ZEROPAGE_X, 0x76, 2),
    _op("ror", AddrMode.ABSOLUTE, 0x6E, 3),
    _op("ror", AddrMode.ABSOLUTE_X, 0x7E, 3),

    _op("rti", AddrMode.NONE, 0x40, 1),
    _op("rts", AddrMode.NONE, 0x60, 1),

    _op("sbc", AddrMode.IMMEDIATE, 0xE9, 2),
    _op("sbc", AddrMode.ZEROPAGE, 0xE5, 2),
    _op("sbc", AddrMode.ZEROPAGE_X, 0xF5, 2),
    _op("sbc", AddrMode.ABSOLUTE, 0xED, 3),
    _op("sbc", AddrMode.ABSOLUTE_X, 0xFD, 3),
    _op("sbc", AddrMode.ABSOLUTE_Y, 0xF9, 3),
    _op("sbc", AddrMode.INDIRECT_X, 0xE1, 2),
    _op("sbc", AddrMode.INDIRECT_Y, 0xF1, 2),

    _op("sec", AddrMode.NONE, 0x38, 1),
    _op("sed", AddrMode.NONE, 0xF8, 1),
    _op("sei", AddrMode.NONE, 0x78, 1),

    _op("sta", AddrMode.ZEROPAGE, 0x85, 2),
    _op("sta", AddrMode.ZEROPAGE_X, 0x95, 2),
    _op("sta", AddrMode.ABSOLUTE, 0x8D, 3),
    _op("sta", AddrMode.ABSOLUTE_X, 0x9D, 3),
    _op("sta", AddrMode.ABSOLUTE_Y, 0x99, 3),
    _op("sta", AddrMode.INDIRECT_X, 0x81, 2),
    _op("sta", AddrMode.INDIRECT_Y, 0x91, 2),

    _op("stx", AddrMode.ZEROPAGE, 0x86, 2),
    _op("stx", AddrMode.ZEROPAGE_Y, 0x96, 2),
    _op("stx", AddrMode.ABSOLUTE, 0x8E, 3),

    _op("sty", AddrMode.ZEROPAGE, 0x84, 2),
    _op("sty", AddrMode.ZEROPAGE_X, 0x94, 2),
    _op("sty", AddrMode.ABSOLUTE, 0x8C, 3),

    _op("tax", AddrMode.NONE, 0xAA, 1),
    _op("tay", AddrMode.NONE, 0xA8, 1),
    _op("tsx", AddrMode.NONE, 0xBA, 1),
    _op("txa", AddrMode.NONE, 0x8A, 1),
    _op("txs", AddrMode.NONE, 0x9A, 1),
    _op("tya", AddrMode.NONE, 0x98, 1),

    _op("nop", AddrMode.NONE, 0xEA, 1),
]


illegal_table: List[Opcode] = [
    # LAX — load A and X
    _op("lax", AddrMode.IMMEDIATE, 0xAB, 2),
    _op("lax", AddrMode.ZEROPAGE, 0xA7, 2),
    _op("lax", AddrMode.ZEROPAGE_Y, 0xB7, 2),
    _op("lax", AddrMode.ABSOLUTE, 0xAF, 3),
    _op("lax", AddrMode.ABSOLUTE_Y, 0xBF, 3),
    _op("lax", AddrMode.INDIRECT_X, 0xA3, 2),
    _op("lax", AddrMode.INDIRECT_Y, 0xB3, 2),
    # SAX — store A & X
    _op("sax", AddrMode.ZEROPAGE, 0x87, 2),
    _op("sax", AddrMode.ZEROPAGE_Y, 0x97, 2),
    _op("sax", AddrMode.ABSOLUTE, 0x8F, 3),
    _op("sax", AddrMode.INDIRECT_X, 0x83, 2),
    # DCP — DEC then CMP
    _op("dcp", AddrMode.ZEROPAGE, 0xC7, 2),
    _op("dcp", AddrMode.ZEROPAGE_X, 0xD7, 2),
    _op("dcp", AddrMode.ABSOLUTE, 0xCF, 3),
    _op("dcp", AddrMode.ABSOLUTE_X, 0xDF, 3),
    _op("dcp", AddrMode.ABSOLUTE_Y, 0xDB, 3),
    _op("dcp", AddrMode.INDIRECT_X, 0xC3, 2),
    _op("dcp", AddrMode.INDIRECT_Y, 0xD3, 2),
    # ISC — INC then SBC
    _op("isc", AddrMode.ZEROPAGE, 0xE7, 2),
    _op("isc", AddrMode.ZEROPAGE_X, 0xF7, 2),
    _op("isc", AddrMode.ABSOLUTE, 0xEF, 3),
    _op("isc", AddrMode.ABSOLUTE_X, 0xFF, 3),
    _op("isc", AddrMode.ABSOLUTE_Y, 0xFB, 3),
    _op("isc", AddrMode.INDIRECT_X, 0xE3, 2),
    _op("isc", AddrMode.INDIRECT_Y, 0xF3, 2),
    # SLO — ASL then ORA
    _op("slo", AddrMode.ZEROPAGE, 0x07, 2),
    _op("slo", AddrMode.ZEROPAGE_X, 0x17, 2),
    _op("slo", AddrMode.ABSOLUTE, 0x0F, 3),
    _op("slo", AddrMode.ABSOLUTE_X, 0x1F, 3),
    _op("slo", AddrMode.ABSOLUTE_Y, 0x1B, 3),
    _op("slo", AddrMode.INDIRECT_X, 0x03, 2),
    _op("slo", AddrMode.INDIRECT_Y, 0x13, 2),
    # RLA — ROL then AND
    _op("rla", AddrMode.ZEROPAGE, 0x27, 2),
    _op("rla", AddrMode.ZEROPAGE_X, 0x37, 2),
    _op("rla", AddrMode.ABSOLUTE, 0x2F, 3),
    _op("rla", AddrMode.ABSOLUTE_X, 0x3F, 3),
    _op("rla", AddrMode.ABSOLUTE_Y, 0x3B, 3),
    _op("rla", AddrMode.INDIRECT_X, 0x23, 2),
    _op("rla", AddrMode.INDIRECT_Y, 0x33, 2),
    # SRE — LSR then EOR
    _op("sre", AddrMode.ZEROPAGE, 0x47, 2),
    _op("sre", AddrMode.ZEROPAGE_X, 0x57, 2),
    _op("sre", AddrMode.ABSOLUTE, 0x4F, 3),
    _op("sre", AddrMode.ABSOLUTE_X, 0x5F, 3),
    _op("sre", AddrMode.ABSOLUTE_Y, 0x5B, 3),
    _op("sre", AddrMode.INDIRECT_X, 0x43, 2),
    _op("sre", AddrMode.INDIRECT_Y, 0x53, 2),
    # RRA — ROR then ADC
    _op("rra", AddrMode.ZEROPAGE, 0x67, 2),
    _op("rra", AddrMode.ZEROPAGE_X, 0x77, 2),
    _op("rra", AddrMode.ABSOLUTE, 0x6F, 3),
    _op("rra", AddrMode.ABSOLUTE_X, 0x7F, 3),
    _op("rra", AddrMode.ABSOLUTE_Y, 0x7B, 3),
    _op("rra", AddrMode.INDIRECT_X, 0x63, 2),
    _op("rra", AddrMode.INDIRECT_Y, 0x73, 2),
    # ANC
    _op("anc", AddrMode.IMMEDIATE, 0x0B, 2),
    _op("anc", AddrMode.IMMEDIATE, 0x2B, 2),
    # ALR
    _op("alr", AddrMode.IMMEDIATE, 0x4B, 2),
    # ARR
    _op("arr", AddrMode.IMMEDIATE, 0x6B, 2),
    # XAA
    _op("xaa", AddrMode.IMMEDIATE, 0x8B, 2),
    # AXS/SBX
    _op("axs", AddrMode.IMMEDIATE, 0xCB, 2),
    _op("sbx", AddrMode.IMMEDIATE, 0xCB, 2),
    # LAS
    _op("las", AddrMode.ABSOLUTE_Y, 0xBB, 3),
    # AHX/SHX/SHY/TAS
    _op("ahx", AddrMode.INDIRECT_Y, 0x93, 2),
    _op("ahx", AddrMode.ABSOLUTE_Y, 0x9F, 3),
    _op("shx", AddrMode.ABSOLUTE_Y, 0x9E, 3),
    _op("shy", AddrMode.ABSOLUTE_X, 0x9C, 3),
    _op("tas", AddrMode.ABSOLUTE_Y, 0x9B, 3),
    # JAM/KIL/HLT
    _op("jam", AddrMode.NONE, 0x02, 1),
    _op("jam", AddrMode.NONE, 0x12, 1),
    _op("jam", AddrMode.NONE, 0x22, 1),
    _op("jam", AddrMode.NONE, 0x32, 1),
    _op("jam", AddrMode.NONE, 0x42, 1),
    _op("jam", AddrMode.NONE, 0x52, 1),
    _op("jam", AddrMode.NONE, 0x62, 1),
    _op("jam", AddrMode.NONE, 0x72, 1),
    _op("jam", AddrMode.NONE, 0x92, 1),
    _op("jam", AddrMode.NONE, 0xB2, 1),
    _op("jam", AddrMode.NONE, 0xD2, 1),
    _op("jam", AddrMode.NONE, 0xF2, 1),
    _op("kil", AddrMode.NONE, 0x02, 1),
    _op("hlt", AddrMode.NONE, 0x02, 1),
]


def is_mnemonic_name(name: str) -> bool:
    n = name.lower()
    for o in opcode_table:
        if o.mn == n:
            return True
    for o in illegal_table:
        if o.mn == n:
            return True
    return False


def opcode_lookup(mn: str, mode: str) -> Optional[Opcode]:
    m = mn.lower()
    for o in opcode_table:
        if o.mn == m and o.mode == mode:
            return o
    if allow_illegal:
        for o in illegal_table:
            if o.mn == m and o.mode == mode:
                return o
    return None


# ---------------- Operands ----------------


def _set_immediate_operand(text: str, operand: Operand) -> bool:
    if not text or text[0] != '#':
        return False
    rest = text[1:].strip()
    operand.mode = AddrMode.IMMEDIATE
    lit = parse_number(rest)
    if lit is not None:
        operand.value = lit
    else:
        operand.expr = rest
    return True


def _set_accumulator_operand(text: str, operand: Operand) -> bool:
    if text in ("A", "a"):
        operand.mode = AddrMode.ACCUMULATOR
        return True
    return False


def _set_indirect_operand(text: str, operand: Operand) -> bool:
    # Accept forms starting with '(' and handle optional ",Y" suffix after ')'
    if not text or text[0] != '(':
        return False
    # Find closing paren
    close = text.find(')')
    if close < 0:
        return False
    inside = text[1:close].strip()
    suffix = text[close + 1 :].strip()
    # Handle (expr),Y with arbitrary whitespace between ) and , Y
    if suffix.startswith(','):
        reg = suffix[1:].strip().upper()
        if reg.startswith('Y'):
            operand.mode = AddrMode.INDIRECT_Y
            lit = parse_number(inside)
            if lit is not None:
                operand.value = lit
            else:
                operand.expr = inside
            return True
        # No (expr),X addressing on 6502; fall through if not Y
        return False
    # (expr,X) must be inside parentheses
    if inside.upper().endswith(',X'):
        base = inside[:-2].strip()
        operand.mode = AddrMode.INDIRECT_X
        lit = parse_number(base)
        if lit is not None:
            operand.value = lit
        else:
            operand.expr = base
        return True
    # Plain (expr)
    operand.mode = AddrMode.INDIRECT
    lit = parse_number(inside)
    if lit is not None:
        operand.value = lit
    else:
        operand.expr = inside
    return True


def _set_indexed_operand(text: str, operand: Operand, original: str) -> bool:
    if ',' not in text:
        return False
    base, reg = text.split(',', 1)
    base = base.strip()
    reg = reg.strip().upper()
    lit = parse_number(base)
    if lit is not None:
        if reg == 'X':
            operand.mode = (
                AddrMode.ZEROPAGE_X if lit <= BYTE_MAX else AddrMode.ABSOLUTE_X
            )
            operand.value = lit
        elif reg == 'Y':
            operand.mode = (
                AddrMode.ZEROPAGE_Y if lit <= BYTE_MAX else AddrMode.ABSOLUTE_Y
            )
            operand.value = lit
        else:
            asm_error(0, f"Unknown suffix register {reg} in {original}")
    else:
        if reg == 'X':
            operand.mode = AddrMode.ABSOLUTE_X
        elif reg == 'Y':
            operand.mode = AddrMode.ABSOLUTE_Y
        operand.expr = base
    return True


def parse_operand(text: Optional[str]) -> Operand:
    op = Operand()
    if not text:
        return op
    buf = text.strip()
    if not buf:
        return op
    if _set_immediate_operand(buf, op):
        return op
    if _set_accumulator_operand(buf, op):
        return op
    if _set_indirect_operand(buf, op):
        return op
    if _set_indexed_operand(buf, op, text):
        return op
    lit = parse_number(buf)
    if lit is not None:
        op.mode = AddrMode.ZEROPAGE if lit <= BYTE_MAX else AddrMode.ABSOLUTE
        op.value = lit
    else:
        op.mode = AddrMode.ABSOLUTE
        op.expr = buf
    return op


# ---------------- Parsing one line ----------------


def strip_comments_preserving_quotes(line: str) -> str:
    in_s = False
    in_d = False
    out = []
    for ch in line:
        if ch == "'" and not in_d:
            in_s = not in_s
            out.append(ch)
            continue
        if ch == '"' and not in_s:
            in_d = not in_d
            out.append(ch)
            continue
        if ch == ';' and not in_s and not in_d:
            break
        out.append(ch)
    return ''.join(out)


def _label_lhs_is_identifier(text: str) -> bool:
    lhs = trim(text)
    if not lhs:
        return False
    for ch in lhs:
        if ch.isspace():
            return False
        if not (ch.isalnum() or ch == '_'):
            return False
    return True


def _find_valid_label_colon(line: str) -> Optional[int]:
    in_s = False
    in_d = False
    for idx, ch in enumerate(line):
        if ch == "'" and not in_d:
            in_s = not in_s
            continue
        if ch == '"' and not in_s:
            in_d = not in_d
            continue
        if ch == ':' and not in_s and not in_d:
            if _label_lhs_is_identifier(line[:idx]):
                return idx
            break
    return None


def _is_directive_name(t: str) -> bool:
    tl = t.lower()
    return (
        tl
        in {
            ".org",
            ".byte",
            ".word",
            ".text",
            ".fill",
            ".incbin",
            ".block",
            ".bend",
            ".include",
            ".macro",
            ".endmacro",
        }
        or t == "*"
    )


def _try_parse_define_line(line_text: str, out: AsmLine) -> bool:
    if '=' not in line_text:
        return False
    lhs, rhs = line_text.split('=', 1)
    lhs = trim(lhs)
    rhs = trim(rhs)
    if lhs == '*':
        out.mnemonic = '*'
        out.extra = rhs
        return True
    if not lhs:
        return False
    ok = all((c.isalnum() or c == '_') for c in lhs)
    if ok and (is_mnemonic_name(lhs) or _is_directive_name(lhs)):
        ok = False
    if ok:
        out.def_name = lhs
        out.def_expr = rhs
        out.mnemonic = '='
        return True
    return False


def _maybe_parse_bare_label(
    line_text: str, line_obj: AsmLine, had_colon: bool
) -> Tuple[bool, str]:
    if had_colon:
        return False, line_text
    s = line_text
    ws = 0
    while ws < len(s) and not s[ws].isspace():
        ws += 1
    first_tok = s[:ws]
    rest_after = trim(s[ws + 1 :]) if ws < len(s) and s[ws].isspace() else s[ws:]
    is_single = rest_after == ""
    is_dir = _is_directive_name(first_tok)
    is_mn = is_mnemonic_name(first_tok)
    if not is_mn and not is_dir and first_tok:
        line_obj.label = first_tok
        if is_single:
            return True, s[len(s) :]
        return False, rest_after
    return False, line_text


def parse_line(input_line: str, lineno: int) -> Optional[AsmLine]:
    line_text = strip_comments_preserving_quotes(input_line)
    line_text = trim(line_text)
    if not line_text:
        return None
    line = AsmLine(
        lineno=lineno,
        filename=None,
        label=None,
        mnemonic=None,
        op=Operand(),
        extra=None,
        def_name=None,
        def_expr=None,
    )

    colon_pos = _find_valid_label_colon(line_text)
    if colon_pos is not None:
        line.label = trim(line_text[:colon_pos])
        line_text = trim(line_text[colon_pos + 1 :])
    if not line_text:
        return line
    if _try_parse_define_line(line_text, line):
        return line
    if colon_pos is None:
        took, rest_after = _maybe_parse_bare_label(line_text, line, False)
        if took and not rest_after:
            return line
        if line.label is not None and rest_after:
            line_text = rest_after

    # mnemonic / directive
    parts = line_text.split()
    mnemonic = parts[0]
    rest = line_text[len(mnemonic) :].strip() if len(parts) > 1 else None
    line.mnemonic = mnemonic

    # directives
    if line.mnemonic.lower() in (".org",) or line.mnemonic == "*":
        line.extra = rest
        return line
    if line.mnemonic.lower() in (".incbin", ".include"):
        line.extra = rest
        return line
    if line.mnemonic.lower() in (".byte", ".word", ".text", ".fill"):
        line.extra = rest
        return line

    # instruction
    line.op = parse_operand(rest)
    if (not rest or not rest.strip()) and line.op.mode == AddrMode.NONE:
        if opcode_lookup(line.mnemonic, AddrMode.ACCUMULATOR):
            line.op.mode = AddrMode.ACCUMULATOR
    if line.mnemonic and line.mnemonic.lower() in (
        "bcc",
        "bcs",
        "beq",
        "bmi",
        "bne",
        "bpl",
        "bvc",
        "bvs",
    ):
        line.op.mode = AddrMode.RELATIVE
    return line


# ---------------- Macro system ----------------


def macro_init() -> None:
    macros.clear()


def _macro_collect_local_label(line: str, out: List[str]) -> None:
    s = line.lstrip()
    if not s or not is_ident_start(s[0]):
        return
    i = 1
    while i < len(s) and is_ident_char(s[i]):
        i += 1
    if i < len(s) and s[i] == ':':
        name = s[:i]
        if name not in out:
            out.append(name)


def _macro_parse_header(line: str) -> Optional[Tuple[str, List[str], bool]]:
    s = trim(line)
    if not s.lower().startswith(".macro"):
        return None
    s = trim(s[6:])
    if not s or not is_ident_start(s[0]):
        return None
    i = 1
    while i < len(s) and is_ident_char(s[i]):
        i += 1
    name = s[:i]
    rest = trim(s[i:])
    if not rest or rest[0] != '(':
        return None
    rest = rest[1:]
    # parse arg names until ')'
    args: List[str] = []
    tok = ''
    depth = 0
    j = 0
    while j < len(rest):
        c = rest[j]
        if c == '(':
            depth += 1
            tok += c
        elif c == ')':
            if depth == 0:
                if tok.strip():
                    args.append(tok.strip())
                j += 1
                break
            depth -= 1
            tok += c
        elif c == ',':
            if depth == 0:
                if tok.strip():
                    args.append(tok.strip())
                tok = ''
            else:
                tok += c
        else:
            tok += c
        j += 1
    after = trim(rest[j:])
    has_open_brace = after.startswith('{')
    return name, args, has_open_brace


def macro_try_define(
    fin, source_path: str, header_line_text: str, line_no_ref: List[int]
) -> bool:
    hdr = _macro_parse_header(header_line_text)
    if not hdr:
        return False
    name, argnames, has_open_brace = hdr
    body: List[str] = []
    local_labels: List[str] = []
    seen_open = has_open_brace
    for raw in fin:
        line_no_ref[0] += 1
        line = raw.rstrip('\r\n')
        work = trim(line)
        if not seen_open:
            if work.startswith('{'):
                seen_open = True
                continue
            seen_open = True  # start body now (brace-less style)
        if work == '':
            body.append('')
            continue
        lw = work.lower()
        if lw == '}' or lw == '.endmacro':
            macros[name] = MacroDef(
                name=name,
                argc=len(argnames),
                argnames=argnames,
                body=body,
                local_labels=local_labels,
            )
            return True
        body.append(line)
        _macro_collect_local_label(line, local_labels)
    asm_error_file(source_path, line_no_ref[0], f"Unterminated .macro {name}")
    return False


def _replace_ident_tokens(in_text: str, tokens: List[str], repl: List[str]) -> str:
    # Tokenize by identifier vs non-identifier characters for simple replacement
    out = []
    i = 0
    n = len(in_text)
    while i < n:
        ch = in_text[i]
        if is_ident_start(ch):
            j = i + 1
            while j < n and is_ident_char(in_text[j]):
                j += 1
            ident = in_text[i:j]
            replaced = False
            for k, tok in enumerate(tokens):
                if ident == tok:
                    out.append(repl[k])
                    replaced = True
                    break
            if not replaced:
                out.append(ident)
            i = j
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def macro_try_expand_and_emit(
    source_path: str, original_line: str, source_line: int
) -> bool:
    # Detect macro call "Name(args)" with no leading label
    work = strip_comments_preserving_quotes(original_line)
    s = trim(work)
    if not s or not is_ident_start(s[0]):
        return False
    i = 1
    while i < len(s) and is_ident_char(s[i]):
        i += 1
    name = s[:i]
    m = macros.get(name)
    if not m:
        return False
    rest = trim(s[i:])
    if not rest.startswith('('):
        return False
    # parse arg values CSV respecting parentheses
    p = rest[1:]
    args: List[str] = []
    tok = ''
    depth = 0
    for c in p:
        if c == '(':
            depth += 1
            tok += c
        elif c == ')':
            if depth == 0:
                if tok.strip():
                    args.append(tok.strip())
                break
            depth -= 1
            tok += c
        elif c == ',' and depth == 0:
            if tok.strip():
                args.append(tok.strip())
            tok = ''
        else:
            tok += c
    if len(args) != m.argc:
        asm_error_file(
            source_path,
            source_line,
            "Macro %s expects %d args, got %d",
            m.name,
            m.argc,
            len(args),
        )

    # Create per-invocation replacements: local label renames and arg substitutions
    global macro_expand_counter
    macro_expand_counter += 1
    uid = macro_expand_counter
    tokens: List[str] = []
    repl: List[str] = []
    # local labels
    for ll in m.local_labels:
        tokens.append(ll)
        # Match C tool's local label rename: __m<uid>_<label>
        repl.append(f"__m{uid}_{ll}")
    # args
    for i, an in enumerate(m.argnames):
        tokens.append(an)
        rv = args[i] if i < len(args) else ''
        repl.append(rv)

    # Emit expanded lines by parsing each replacement line and appending to global lines
    for body_line in m.body:
        replaced = _replace_ident_tokens(body_line, tokens, repl)
        ln = parse_line(replaced, source_line)
        if ln:
            ln.filename = source_path
            lines.append(ln)
            if len(lines) >= MAX_LINES:
                asm_error_file(
                    source_path,
                    source_line,
                    "Too many lines in input (macro expansion)",
                )
    return True


# ---------------- Include expansion loader ----------------


def expand_include(including_path: str, argument: Optional[str], lineno: int) -> None:
    if not argument:
        asm_error_file(including_path, lineno, "Missing filename for .include")
    fn = trim(argument)
    if fn and fn[0] in ('"', "'"):
        q = fn[0]
        fn = fn[1:]
        qpos = fn.rfind(q)
        if qpos >= 0:
            fn = fn[:qpos]
    # resolve path relative to including_path directory
    if os.path.isabs(fn):
        resolved = fn
    else:
        base = including_path or ''
        d = os.path.dirname(base)
        resolved = os.path.join(d, fn)
    read_file_with_includes(resolved)


def read_file_with_includes(input_path: str) -> None:
    global include_depth
    if not input_path:
        return
    if include_depth >= MAX_INCLUDE_DEPTH:
        sys.stderr.write(
            f"Error: include nesting too deep while opening {input_path}\n"
        )
        raise SystemExit(1)
    try:
        f = open(input_path, 'r')
    except OSError as e:
        sys.stderr.write(f"Error (open {input_path}): {e.strerror}\n")
        raise SystemExit(1)
    include_depth += 1
    line_no_box = [0]
    for raw in f:
        line_no_box[0] += 1
        line_raw = raw.rstrip('\r\n')
        # Macro definition? If so, it consumes its block
        if macro_try_define(f, input_path, line_raw, line_no_box):
            continue
        # Macro invocation? Expand now
        if macro_try_expand_and_emit(input_path, line_raw, line_no_box[0]):
            continue
        ln = parse_line(line_raw, line_no_box[0])
        if not ln:
            continue
        ln.filename = input_path
        if ln.mnemonic and ln.mnemonic.lower() == ".include":
            expand_include(input_path, ln.extra, line_no_box[0])
            continue
        lines.append(ln)
        if len(lines) >= MAX_LINES:
            asm_error_file(input_path, line_no_box[0], "Too many lines in input")
    f.close()
    include_depth -= 1


# ---------------- First pass ----------------


def count_csv_items(s: Optional[str]) -> int:
    if not s:
        return 0
    # split by commas, ignore empty tokens
    return sum(1 for tok in s.split(',') if tok.strip() != '')


def _extract_text_payload_alloc(ln: AsmLine) -> str:
    if not ln.extra:
        asm_error_file(ln.filename, ln.lineno, "Missing string in .text")
    t = trim(ln.extra)
    if not t or t[0] not in ('"', "'"):
        asm_error_file(ln.filename, ln.lineno, "Expected quoted string for .text")
    quote = t[0]
    end = t.rfind(quote)
    if end < 1:
        asm_error_file(ln.filename, ln.lineno, "Unterminated string in .text")
    return t[1:end]


def parse_org_value_file(
    file: Optional[str], extra: Optional[str], lineno: int, pc: int
) -> int:
    if not extra:
        asm_error_file(file, lineno, "Missing expression in org directive")
    expr = trim(extra)
    if expr.startswith('='):
        expr = trim(expr[1:])
    global g_eval_pc
    g_eval_pc = pc
    v = parse_number(expr)
    if v is None:
        v = eval_expr(expr)
    if v is None:
        asm_error_file(file, lineno, "Bad expression in org: %s", extra)
    if v < 0 or v > WORD_MAX:
        asm_error_file(file, lineno, "Origin out of 16-bit range: %d", v)
    if v < pc:
        sys.stderr.write(
            f"Warning ({file or '<input>'}:{lineno}): .org moving PC backwards from ${pc & 0xFFFF:04X} to ${v & 0xFFFF:04X}; bytes previously assembled beyond new origin will remain in output.\n"
        )
    return v


def first_handle_org(ln: AsmLine, pc: int) -> int:
    return parse_org_value_file(ln.filename, ln.extra, ln.lineno, pc)


def first_handle_incbin(ln: AsmLine, pc: int) -> int:
    if not ln.extra:
        asm_error_file(ln.filename, ln.lineno, "Missing filename in .incbin")
    fn = trim(ln.extra)
    if fn and fn[0] in ('"', "'"):
        q = fn[0]
        fn = fn[1:]
        qpos = fn.rfind(q)
        if qpos >= 0:
            fn = fn[:qpos]
    try:
        size = os.path.getsize(fn)
    except OSError as e:
        asm_error_file(ln.filename, ln.lineno, "Cannot open .incbin file: %s", fn)
    return pc + int(size)


def first_handle_text(ln: AsmLine, pc: int) -> int:
    text = _extract_text_payload_alloc(ln)
    return pc + len(text)


def first_handle_byte(ln: AsmLine, pc: int) -> int:
    return pc + count_csv_items(ln.extra)


def first_handle_word(ln: AsmLine, pc: int) -> int:
    return pc + count_csv_items(ln.extra) * 2


def first_handle_fill(ln: AsmLine, pc: int) -> int:
    if not ln.extra or ',' not in ln.extra:
        asm_error_file(
            ln.filename, ln.lineno, "Malformed .fill, expected: .fill <size>, <value>"
        )
    size_tok, _val_tok = ln.extra.split(',', 1)
    size_tok = trim(size_tok)
    global g_eval_pc
    g_eval_pc = pc
    size_val = parse_number(size_tok)
    if size_val is None:
        size_val = eval_expr(size_tok)
    if size_val is None:
        asm_error_file(
            ln.filename, ln.lineno, "Unable to resolve .fill size: %s", ln.extra
        )
    if size_val < 0:
        asm_error_file(
            ln.filename, ln.lineno, ".fill size must be non-negative: %d", size_val
        )
    return pc + size_val


def first_handle_define(ln: AsmLine, pc: int) -> int:
    if not ln.def_name or not ln.def_expr:
        asm_error_file(ln.filename, ln.lineno, "Malformed define")
    global g_eval_pc
    g_eval_pc = pc
    expr = ln.def_expr
    if expr and expr.startswith('#'):
        expr = expr[1:].lstrip()
    v = parse_number(expr)
    if v is None:
        v = eval_expr(expr)
    if v is None:
        # try bare symbol
        saddr = sym_lookup_scoped(expr, current_scope_depth)
        if saddr < 0:
            asm_error_file(
                ln.filename,
                ln.lineno,
                "Bad expression in define: %s = %s",
                ln.def_name,
                ln.def_expr,
            )
        v = saddr
    sym_add_scoped(ln.def_name, v, 0, False, ln.filename, ln.lineno)
    return pc


def first_handle_instruction(ln: AsmLine, pc: int) -> int:
    op = opcode_lookup(ln.mnemonic, ln.op.mode)
    if not op and ln.op.mode == AddrMode.ZEROPAGE_Y:
        alt = opcode_lookup(ln.mnemonic, AddrMode.ABSOLUTE_Y)
        if alt:
            ln.op.mode = AddrMode.ABSOLUTE_Y
            op = alt
    if not op:
        asm_error_file(
            ln.filename,
            ln.lineno,
            "%s does not support %s addressing",
            ln.mnemonic,
            addrmode_str(ln.op.mode),
        )
    return pc + op.length


def first_pass() -> None:
    global minus_idx, minus_pc, plus_idx, plus_pc, line_scope_depth, current_scope_depth
    debug("First pass...")
    pc = origin
    minus_idx, minus_pc, plus_idx, plus_pc = [], [], [], []
    line_scope_depth = [0] * len(lines)
    current_scope_depth = 0
    scope_depth = 0

    for i, ln in enumerate(lines):
        if not ln:
            continue
        line_scope_depth[i] = scope_depth
        current_scope_depth = scope_depth
        if ln.label:
            all_minus = all(c == '-' for c in ln.label)
            all_plus = all(c == '+' for c in ln.label)
            if all_minus and ln.label:
                minus_idx.append(i)
                minus_pc.append(pc)
            elif all_plus and ln.label:
                plus_idx.append(i)
                plus_pc.append(pc)
            else:
                sym_add_scoped(ln.label, pc, scope_depth, True, ln.filename, ln.lineno)
        if not ln.mnemonic:
            continue
        low = ln.mnemonic.lower()
        if ln.def_name is not None and ln.def_expr is not None:
            pc = first_handle_define(ln, pc)
        elif low in (".org",) or ln.mnemonic == "*":
            pc = first_handle_org(ln, pc)
        elif low == ".incbin":
            pc = first_handle_incbin(ln, pc)
        elif low == ".byte":
            pc = first_handle_byte(ln, pc)
        elif low == ".word":
            pc = first_handle_word(ln, pc)
        elif low == ".text":
            pc = first_handle_text(ln, pc)
        elif low == ".fill":
            pc = first_handle_fill(ln, pc)
        elif low == ".block":
            scope_depth += 1
            current_scope_depth = scope_depth
        elif low == ".bend":
            if scope_depth == 0:
                asm_error_file(ln.filename, ln.lineno, ".bend without matching .block")
            scope_depth -= 1
            current_scope_depth = scope_depth
        else:
            pc = first_handle_instruction(ln, pc)

    if pc > MAX_OUTPUT:
        asm_error(0, "Assembly size exceeds output buffer limit (%d bytes)", MAX_OUTPUT)
    if scope_depth != 0:
        asm_error(0, "Unterminated .block (missing .bend)")


# ---------------- Second pass and emission ----------------


def emit_byte(pc: int, v: int) -> None:
    global out_lo, out_hi
    if not (0 <= pc < MAX_OUTPUT):
        asm_error(0, "Output PC out of range: %04X", pc)
    outbuf[pc] = v & 0xFF
    if pc < out_lo:
        out_lo = pc
    if pc > out_hi:
        out_hi = pc


def emit_word(pc: int, v: int) -> None:
    emit_byte(pc, v & 0xFF)
    emit_byte(pc + 1, (v >> 8) & 0xFF)


def parse_local_ref(s: str) -> Optional[Tuple[str, int]]:
    if not s:
        return None
    c = s[0]
    if c not in ('-', '+'):
        return None
    if not all(ch == c for ch in s):
        return None
    return c, len(s)


def resolve_minus_addr_k(line_index: int, k: int) -> Optional[int]:
    for m in range(len(minus_idx) - 1, -1, -1):
        if minus_idx[m] < line_index:
            k -= 1
            if k == 0:
                return minus_pc[m]
    return None


def resolve_plus_addr_k(line_index: int, k: int) -> Optional[int]:
    for p in range(0, len(plus_idx)):
        if plus_idx[p] > line_index:
            k -= 1
            if k == 0:
                return plus_pc[p]
    return None


def resolve_value_from_expr(
    expr_text: str,
    program_counter: int,
    line_index: int,
    source_file: Optional[str],
    source_line: int,
) -> int:
    lr = parse_local_ref(expr_text)
    if lr:
        kind, count = lr
        if kind == '-':
            addr = resolve_minus_addr_k(line_index, count)
            if addr is None:
                asm_error_file(source_file, source_line, "No matching '-' label found")
        else:
            addr = resolve_plus_addr_k(line_index, count)
            if addr is None:
                asm_error_file(source_file, source_line, "No matching '+' label found")
        return addr
    global g_eval_pc, current_scope_depth
    g_eval_pc = program_counter
    current_scope_depth = (
        line_scope_depth[line_index] if 0 <= line_index < len(line_scope_depth) else 0
    )
    v = eval_expr(expr_text)
    if v is not None:
        return v
    lit = parse_number(expr_text)
    if lit is not None:
        return lit
    sym_addr = sym_lookup_scoped(expr_text, current_scope_depth)
    if sym_addr < 0:
        asm_error_file(source_file, source_line, "Undefined label %s", expr_text)
    return sym_addr


def resolve_operand_value_for_line(
    line: AsmLine, program_counter: int, line_index: int
) -> int:
    if line.op.expr is not None:
        return resolve_value_from_expr(
            line.op.expr, program_counter, line_index, line.filename, line.lineno
        )
    return line.op.value


def second_handle_org(ln: AsmLine, pc: int) -> int:
    return parse_org_value_file(None, ln.extra, ln.lineno, pc)


def second_handle_incbin(ln: AsmLine, pc: int) -> int:
    if not ln.extra:
        asm_error_file(ln.filename, ln.lineno, "Missing filename in .incbin")
    fnbuf = trim(ln.extra)
    if fnbuf and fnbuf[0] in ('"', "'"):
        q = fnbuf[0]
        fnbuf = fnbuf[1:]
        qpos = fnbuf.rfind(q)
        if qpos >= 0:
            fnbuf = fnbuf[:qpos]
    try:
        with open(fnbuf, 'rb') as f:
            data = f.read()
    except OSError:
        asm_error_file(
            ln.filename, ln.lineno, "Cannot open .incbin file (second pass): %s", fnbuf
        )
    for b in data:
        emit_byte(pc, b)
        pc += 1
    return pc


def second_handle_byte(ln: AsmLine, pc: int, line_index: int) -> int:
    extra = ln.extra or ''
    for tok in extra.split(','):
        t = trim(tok)
        if not t:
            continue
        v = resolve_value_from_expr(t, pc, line_index, ln.filename, ln.lineno)
        if v < 0 or v > BYTE_MAX:
            asm_error_file(
                ln.filename, ln.lineno, ".byte value %d out of 8-bit range", v
            )
        emit_byte(pc, v)
        pc += 1
    return pc


def second_handle_word(ln: AsmLine, pc: int, line_index: int) -> int:
    extra = ln.extra or ''
    for tok in extra.split(','):
        t = trim(tok)
        if not t:
            continue
        v = resolve_value_from_expr(t, pc, line_index, ln.filename, ln.lineno)
        if v < 0 or v > WORD_MAX:
            asm_error_file(
                ln.filename, ln.lineno, ".word value %d out of 16-bit range", v
            )
        emit_word(pc, v)
        pc += 2
    return pc


def second_handle_text(ln: AsmLine, pc: int) -> int:
    text = _extract_text_payload_alloc(ln)
    for ch in text:
        emit_byte(pc, ord(ch) & 0xFF)
        pc += 1
    return pc


def second_handle_fill(ln: AsmLine, pc: int, line_index: int) -> int:
    if not ln.extra or ',' not in ln.extra:
        asm_error_file(
            ln.filename,
            ln.lineno,
            "Missing arguments for .fill (expected: size, value)",
        )
    size_tok, val_tok = ln.extra.split(',', 1)
    size_tok = trim(size_tok)
    val_tok = trim(val_tok)
    global g_eval_pc
    g_eval_pc = pc
    size_val = parse_number(size_tok)
    if size_val is None:
        size_val = eval_expr(size_tok)
    if size_val is None:
        size_val = resolve_value_from_expr(
            size_tok, pc, line_index, ln.filename, ln.lineno
        )
    if size_val < 0:
        asm_error_file(
            ln.filename, ln.lineno, ".fill size must be non-negative: %d", size_val
        )
    v = resolve_value_from_expr(val_tok, pc, line_index, ln.filename, ln.lineno)
    if v < 0 or v > BYTE_MAX:
        asm_error_file(ln.filename, ln.lineno, ".fill value %d out of 8-bit range", v)
    for _ in range(size_val):
        emit_byte(pc, v)
        pc += 1
    return pc


def second_emit_instruction(ln: AsmLine, pc: int, line_index: int) -> int:
    op = opcode_lookup(ln.mnemonic, ln.op.mode)
    if not op and ln.op.mode == AddrMode.ZEROPAGE_Y:
        alt = opcode_lookup(ln.mnemonic, AddrMode.ABSOLUTE_Y)
        if alt:
            ln.op.mode = AddrMode.ABSOLUTE_Y
            op = alt
    if not op:
        asm_error_file(
            ln.filename,
            ln.lineno,
            "%s does not support %s addressing",
            ln.mnemonic,
            addrmode_str(ln.op.mode),
        )
    emit_byte(pc, op.opcode)
    if op.length == 1:
        return pc + 1
    if op.length == 2:
        if ln.op.mode == AddrMode.RELATIVE:
            value = resolve_operand_value_for_line(ln, pc, line_index)
            offset = value - (pc + 2)
            if offset < BRANCH_MIN or offset > BRANCH_MAX:
                asm_error_file(
                    ln.filename, ln.lineno, "Branch offset out of range: %d", offset
                )
            emit_byte(pc + 1, offset & 0xFF)
        else:
            value = resolve_operand_value_for_line(ln, pc, line_index)
            if value < 0 or value > BYTE_MAX:
                asm_error_file(
                    ln.filename, ln.lineno, "8-bit operand %d out of range", value
                )
            emit_byte(pc + 1, value & 0xFF)
        return pc + 2
    if op.length == 3:
        value = resolve_operand_value_for_line(ln, pc, line_index)
        if value < 0 or value > WORD_MAX:
            asm_error_file(
                ln.filename, ln.lineno, "16-bit operand %d out of range", value
            )
        emit_word(pc + 1, value)
        return pc + 3
    asm_error_file(ln.filename, ln.lineno, "Invalid opcode length %d", op.length)
    return pc


def second_pass() -> None:
    debug("Second pass...")
    pc = origin
    global current_line_index, current_scope_depth
    for i, ln in enumerate(lines):
        current_line_index = i
        current_scope_depth = (
            line_scope_depth[i] if 0 <= i < len(line_scope_depth) else 0
        )
        if not ln or not ln.mnemonic:
            continue
        low = ln.mnemonic.lower()
        if ln.def_name is not None and ln.def_expr is not None:
            continue
        if low in (".org",) or ln.mnemonic == "*":
            pc = second_handle_org(ln, pc)
        elif low == ".incbin":
            pc = second_handle_incbin(ln, pc)
        elif low == ".byte":
            pc = second_handle_byte(ln, pc, i)
        elif low == ".word":
            pc = second_handle_word(ln, pc, i)
        elif low == ".text":
            pc = second_handle_text(ln, pc)
        elif low == ".fill":
            pc = second_handle_fill(ln, pc, i)
        elif low in (".block", ".bend"):
            pass
        else:
            pc = second_emit_instruction(ln, pc, i)


# ---------------- CLI and main ----------------


def write_symbol_map(map_path: str) -> None:
    # Sort: by address, then labels before defines, then by name
    idxs = list(range(len(symtab)))

    def key_fn(i: int):
        s = symtab[i]
        return (s.addr & 0xFFFF, 0 if s.is_label else 1, s.name)

    idxs.sort(key=key_fn)
    try:
        with open(map_path, 'w') as mf:
            mf.write("; $ADDR TYPE SCOPE NAME\n")
            for i in idxs:
                s = symtab[i]
                type_str = "label" if s.is_label else "define"
                scope_depth = s.scope_depth if s.is_label else 0
                mf.write(f"${s.addr & 0xFFFF:04X} {type_str} {scope_depth} {s.name}\n")
    except OSError as e:
        sys.stderr.write(f"Error: cannot open map file {map_path}: {e.strerror}\n")
        raise SystemExit(1)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--prg-header', '-H', action='store_true', default=True)
    parser.add_argument('--no-prg-header', '-N', action='store_true', default=False)
    parser.add_argument('--illegal-opcodes', '-I', action='store_true', default=False)
    parser.add_argument('--map', '-M', dest='map_path')
    parser.add_argument('input', nargs='?')
    parser.add_argument('output', nargs='?')
    args, unknown = parser.parse_known_args(argv)

    if args.input is None or args.output is None:
        sys.stderr.write(
            f"Usage: {os.path.basename(sys.argv[0])} [--prg-header|-H] [--no-prg-header|-N] "
            f"[--illegal-opcodes|-I] [--map <file>|-M <file>] input.asm output.bin\n"
        )
        return 1

    global allow_illegal
    allow_illegal = args.illegal_opcodes

    macro_init()
    read_file_with_includes(args.input)

    # Reset output buffer to zeros so gaps remain zero-filled
    for i in range(MAX_OUTPUT):
        outbuf[i] = 0
    global out_lo, out_hi, origin
    out_lo = MAX_OUTPUT
    out_hi = 0
    origin = 0

    first_pass()
    second_pass()

    try:
        with open(args.output, 'wb') as fout:
            length = 0 if out_hi < out_lo else (out_hi - out_lo + 1)
            if length > 0:
                write_header = not args.no_prg_header
                if write_header:
                    fout.write(bytes([out_lo & 0xFF, (out_lo >> 8) & 0xFF]))
                fout.write(outbuf[out_lo : out_lo + length])
            # Print short summary
            if not args.no_prg_header:
                print(
                    f"Assembled {length} bytes from ${out_lo:04X} to ${out_hi:04X} ({PRG_HEADER_SIZE}-byte load address)"
                )
            else:
                print(
                    f"Assembled {length} bytes from ${out_lo:04X} to ${out_hi:04X} (raw)"
                )
    except OSError as e:
        sys.stderr.write(f"Error writing output: {e.strerror}\n")
        return 1

    if args.map_path:
        write_symbol_map(args.map_path)
        print(f"Wrote symbol map: {args.map_path}")

    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
