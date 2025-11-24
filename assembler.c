#define _DEFAULT_SOURCE

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf( stderr, __VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif

#define MAX_LINES 20000
#define MAX_SYMBOLS 5000
#define MAX_OUTPUT 65536
#define MAX_LINE_LEN 512
#define STRING_BUF 256

/* Sizes and ranges */
#define BYTE_MAX 0xFF
#define WORD_MAX 0xFFFF
#define BRANCH_MIN -128
#define BRANCH_MAX 127
#define PRG_HEADER_SIZE 2

/* Byte helpers */
#define LOBYTE(v) ((v) & 0xFF)
#define HIBYTE(v) (((v) >> 8) & 0xFF)

/* Syntax characters */
#define CHAR_HASH '#'
#define CHAR_LPAREN '('
#define CHAR_RPAREN ')'
#define CHAR_COMMA ','
#define CHAR_DOLLAR '$'

/* Addressing modes / operand kinds */
typedef enum addrmode
{
    AM_NONE,
    AM_IMMEDIATE,
    AM_ZEROPAGE,
    AM_ZEROPAGE_X,
    AM_ZEROPAGE_Y,
    AM_ABSOLUTE,
    AM_ABSOLUTE_X,
    AM_ABSOLUTE_Y,
    AM_INDIRECT,
    AM_INDIRECT_X,
    AM_INDIRECT_Y,
    AM_RELATIVE,
    AM_ACCUMULATOR
} addrmode_t;

/* Parsed operand */
typedef struct operand
{
    addrmode_t mode;
    int value;  /* literal if known */
    char *expr; /* string of label/expression if unresolved */
} operand_t;

/* One line of assembly / directive */
typedef struct asm_line
{
    int lineno;
    char *filename; /* source filename for diagnostics */
    char *label;    /* may be NULL */
    char *mnemonic; /* e.g. "lda", "jmp", or directive */
    operand_t op;   /* operand */
    char *extra;    /* extra text (filename for incbin, data list for .byte/.word) */
    /* Define support: NAME = EXPR */
    char *def_name; /* if non-NULL, this line is a constant definition */
    char *def_expr; /* expression string for the define */
} asm_line_t;

/* Symbol table */
typedef struct symbol
{
    char *name;
    int addr;
} symbol_t;

/* Opcode table entry */
typedef struct opcode_t
{
    const char *mn;
    addrmode_t mode;
    uint8_t opcode;
    int length;
} opcode_t;

/* Globals */
static asm_line_t *lines[MAX_LINES];
static int line_count = 0;

static symbol_t symtab[MAX_SYMBOLS];
static int sym_count = 0;

static uint8_t outbuf[MAX_OUTPUT];
static int out_lo = MAX_OUTPUT;
static int out_hi = 0;

static int origin = 0;
static int g_eval_pc = 0;     /* current PC for expression evaluation (supports '*') */
static int allow_illegal = 0; /* enable unofficial/illegal opcodes */

/* Include handling */
#define MAX_INCLUDE_DEPTH 64
static int include_depth = 0;

/* Forward decl: loader that expands includes into global lines[] */
static void read_file_with_includes(const char *path);
static void expand_include(const char *including_path, const char *arg, int lineno);

/* Forward declaration for opcode lookup used during parsing */
const opcode_t *opcode_lookup(const char *mn, addrmode_t mode);
/* Forward declaration: parse expression involving +, -, parentheses. */
int eval_expr(const char *expr, int *out);
static int eval_parse_expr(const char **pp, int *out); /* forward */
/* Forward decl for mnemonic check by name */
int is_mnemonic_name(const char *name);

/* Opcode table — all legal 6502 (documented) instructions */
static const opcode_t opcode_table[] = {
    /* Mnemonic, mode, opcode, length */
    {"adc", AM_IMMEDIATE, 0x69, 2},
    {"adc", AM_ZEROPAGE, 0x65, 2},
    {"adc", AM_ZEROPAGE_X, 0x75, 2},
    {"adc", AM_ABSOLUTE, 0x6D, 3},
    {"adc", AM_ABSOLUTE_X, 0x7D, 3},
    {"adc", AM_ABSOLUTE_Y, 0x79, 3},
    {"adc", AM_INDIRECT_X, 0x61, 2},
    {"adc", AM_INDIRECT_Y, 0x71, 2},

    {"and", AM_IMMEDIATE, 0x29, 2},
    {"and", AM_ZEROPAGE, 0x25, 2},
    {"and", AM_ZEROPAGE_X, 0x35, 2},
    {"and", AM_ABSOLUTE, 0x2D, 3},
    {"and", AM_ABSOLUTE_X, 0x3D, 3},
    {"and", AM_ABSOLUTE_Y, 0x39, 3},
    {"and", AM_INDIRECT_X, 0x21, 2},
    {"and", AM_INDIRECT_Y, 0x31, 2},

    {"asl", AM_ACCUMULATOR, 0x0A, 1},
    {"asl", AM_ZEROPAGE, 0x06, 2},
    {"asl", AM_ZEROPAGE_X, 0x16, 2},
    {"asl", AM_ABSOLUTE, 0x0E, 3},
    {"asl", AM_ABSOLUTE_X, 0x1E, 3},

    {"bcc", AM_RELATIVE, 0x90, 2},
    {"bcs", AM_RELATIVE, 0xB0, 2},
    {"beq", AM_RELATIVE, 0xF0, 2},
    {"bit", AM_ZEROPAGE, 0x24, 2},
    {"bit", AM_ABSOLUTE, 0x2C, 3},
    {"bmi", AM_RELATIVE, 0x30, 2},
    {"bne", AM_RELATIVE, 0xD0, 2},
    {"bpl", AM_RELATIVE, 0x10, 2},
    {"brk", AM_NONE, 0x00, 1},
    {"bvc", AM_RELATIVE, 0x50, 2},
    {"bvs", AM_RELATIVE, 0x70, 2},

    {"clc", AM_NONE, 0x18, 1},
    {"cld", AM_NONE, 0xD8, 1},
    {"cli", AM_NONE, 0x58, 1},
    {"clv", AM_NONE, 0xB8, 1},

    {"cmp", AM_IMMEDIATE, 0xC9, 2},
    {"cmp", AM_ZEROPAGE, 0xC5, 2},
    {"cmp", AM_ZEROPAGE_X, 0xD5, 2},
    {"cmp", AM_ABSOLUTE, 0xCD, 3},
    {"cmp", AM_ABSOLUTE_X, 0xDD, 3},
    {"cmp", AM_ABSOLUTE_Y, 0xD9, 3},
    {"cmp", AM_INDIRECT_X, 0xC1, 2},
    {"cmp", AM_INDIRECT_Y, 0xD1, 2},

    {"cpy", AM_IMMEDIATE, 0xC0, 2},
    {"cpy", AM_ZEROPAGE, 0xC4, 2},
    {"cpy", AM_ABSOLUTE, 0xCC, 3},

    {"cpx", AM_IMMEDIATE, 0xE0, 2},
    {"cpx", AM_ZEROPAGE, 0xE4, 2},
    {"cpx", AM_ABSOLUTE, 0xEC, 3},

    {"dec", AM_ZEROPAGE, 0xC6, 2},
    {"dec", AM_ZEROPAGE_X, 0xD6, 2},
    {"dec", AM_ABSOLUTE, 0xCE, 3},
    {"dec", AM_ABSOLUTE_X, 0xDE, 3},

    {"dex", AM_NONE, 0xCA, 1},
    {"dey", AM_NONE, 0x88, 1},

    {"eor", AM_IMMEDIATE, 0x49, 2},
    {"eor", AM_ZEROPAGE, 0x45, 2},
    {"eor", AM_ZEROPAGE_X, 0x55, 2},
    {"eor", AM_ABSOLUTE, 0x4D, 3},
    {"eor", AM_ABSOLUTE_X, 0x5D, 3},
    {"eor", AM_ABSOLUTE_Y, 0x59, 3},
    {"eor", AM_INDIRECT_X, 0x41, 2},
    {"eor", AM_INDIRECT_Y, 0x51, 2},

    {"inc", AM_ZEROPAGE, 0xE6, 2},
    {"inc", AM_ZEROPAGE_X, 0xF6, 2},
    {"inc", AM_ABSOLUTE, 0xEE, 3},
    {"inc", AM_ABSOLUTE_X, 0xFE, 3},

    {"inx", AM_NONE, 0xE8, 1},
    {"iny", AM_NONE, 0xC8, 1},

    {"jmp", AM_ABSOLUTE, 0x4C, 3},
    {"jmp", AM_INDIRECT, 0x6C, 3},

    {"jsr", AM_ABSOLUTE, 0x20, 3},

    {"lda", AM_IMMEDIATE, 0xA9, 2},
    {"lda", AM_ZEROPAGE, 0xA5, 2},
    {"lda", AM_ZEROPAGE_X, 0xB5, 2},
    {"lda", AM_ABSOLUTE, 0xAD, 3},
    {"lda", AM_ABSOLUTE_X, 0xBD, 3},
    {"lda", AM_ABSOLUTE_Y, 0xB9, 3},
    {"lda", AM_INDIRECT_X, 0xA1, 2},
    {"lda", AM_INDIRECT_Y, 0xB1, 2},

    {"ldx", AM_IMMEDIATE, 0xA2, 2},
    {"ldx", AM_ZEROPAGE, 0xA6, 2},
    {"ldx", AM_ZEROPAGE_Y, 0xB6, 2},
    {"ldx", AM_ABSOLUTE, 0xAE, 3},
    {"ldx", AM_ABSOLUTE_Y, 0xBE, 3},

    {"ldy", AM_IMMEDIATE, 0xA0, 2},
    {"ldy", AM_ZEROPAGE, 0xA4, 2},
    {"ldy", AM_ZEROPAGE_X, 0xB4, 2},
    {"ldy", AM_ABSOLUTE, 0xAC, 3},
    {"ldy", AM_ABSOLUTE_X, 0xBC, 3},

    {"lsr", AM_ACCUMULATOR, 0x4A, 1},
    {"lsr", AM_ZEROPAGE, 0x46, 2},
    {"lsr", AM_ZEROPAGE_X, 0x56, 2},
    {"lsr", AM_ABSOLUTE, 0x4E, 3},
    {"lsr", AM_ABSOLUTE_X, 0x5E, 3},

    {"ora", AM_IMMEDIATE, 0x09, 2},
    {"ora", AM_ZEROPAGE, 0x05, 2},
    {"ora", AM_ZEROPAGE_X, 0x15, 2},
    {"ora", AM_ABSOLUTE, 0x0D, 3},
    {"ora", AM_ABSOLUTE_X, 0x1D, 3},
    {"ora", AM_ABSOLUTE_Y, 0x19, 3},
    {"ora", AM_INDIRECT_X, 0x01, 2},
    {"ora", AM_INDIRECT_Y, 0x11, 2},

    {"pha", AM_NONE, 0x48, 1},
    {"php", AM_NONE, 0x08, 1},
    {"pla", AM_NONE, 0x68, 1},
    {"plp", AM_NONE, 0x28, 1},

    {"rol", AM_ACCUMULATOR, 0x2A, 1},
    {"rol", AM_ZEROPAGE, 0x26, 2},
    {"rol", AM_ZEROPAGE_X, 0x36, 2},
    {"rol", AM_ABSOLUTE, 0x2E, 3},
    {"rol", AM_ABSOLUTE_X, 0x3E, 3},

    {"ror", AM_ACCUMULATOR, 0x6A, 1},
    {"ror", AM_ZEROPAGE, 0x66, 2},
    {"ror", AM_ZEROPAGE_X, 0x76, 2},
    {"ror", AM_ABSOLUTE, 0x6E, 3},
    {"ror", AM_ABSOLUTE_X, 0x7E, 3},

    {"rti", AM_NONE, 0x40, 1},
    {"rts", AM_NONE, 0x60, 1},

    {"sbc", AM_IMMEDIATE, 0xE9, 2},
    {"sbc", AM_ZEROPAGE, 0xE5, 2},
    {"sbc", AM_ZEROPAGE_X, 0xF5, 2},
    {"sbc", AM_ABSOLUTE, 0xED, 3},
    {"sbc", AM_ABSOLUTE_X, 0xFD, 3},
    {"sbc", AM_ABSOLUTE_Y, 0xF9, 3},
    {"sbc", AM_INDIRECT_X, 0xE1, 2},
    {"sbc", AM_INDIRECT_Y, 0xF1, 2},

    {"sec", AM_NONE, 0x38, 1},
    {"sed", AM_NONE, 0xF8, 1},
    {"sei", AM_NONE, 0x78, 1},

    {"sta", AM_ZEROPAGE, 0x85, 2},
    {"sta", AM_ZEROPAGE_X, 0x95, 2},
    {"sta", AM_ABSOLUTE, 0x8D, 3},
    {"sta", AM_ABSOLUTE_X, 0x9D, 3},
    {"sta", AM_ABSOLUTE_Y, 0x99, 3},
    {"sta", AM_INDIRECT_X, 0x81, 2},
    {"sta", AM_INDIRECT_Y, 0x91, 2},

    {"stx", AM_ZEROPAGE, 0x86, 2},
    {"stx", AM_ZEROPAGE_Y, 0x96, 2},
    {"stx", AM_ABSOLUTE, 0x8E, 3},

    {"sty", AM_ZEROPAGE, 0x84, 2},
    {"sty", AM_ZEROPAGE_X, 0x94, 2},
    {"sty", AM_ABSOLUTE, 0x8C, 3},

    {"tax", AM_NONE, 0xAA, 1},
    {"tay", AM_NONE, 0xA8, 1},
    {"tsx", AM_NONE, 0xBA, 1},
    {"txa", AM_NONE, 0x8A, 1},
    {"txs", AM_NONE, 0x9A, 1},
    {"tya", AM_NONE, 0x98, 1},

    {"nop", AM_NONE, 0xEA, 1},

    {NULL, AM_NONE, 0, 0}
};

/* Unofficial/illegal opcodes (NMOS 6502) enabled with --illegal-opcodes */
static const opcode_t illegal_table[] = {
    /* LAX — load A and X */
    {"lax", AM_IMMEDIATE, 0xAB, 2},
    {"lax", AM_ZEROPAGE, 0xA7, 2},
    {"lax", AM_ZEROPAGE_Y, 0xB7, 2},
    {"lax", AM_ABSOLUTE, 0xAF, 3},
    {"lax", AM_ABSOLUTE_Y, 0xBF, 3},
    {"lax", AM_INDIRECT_X, 0xA3, 2},
    {"lax", AM_INDIRECT_Y, 0xB3, 2},

    /* SAX — store A & X */
    {"sax", AM_ZEROPAGE, 0x87, 2},
    {"sax", AM_ZEROPAGE_Y, 0x97, 2},
    {"sax", AM_ABSOLUTE, 0x8F, 3},
    {"sax", AM_INDIRECT_X, 0x83, 2},

    /* DCP — DEC then CMP */
    {"dcp", AM_ZEROPAGE, 0xC7, 2},
    {"dcp", AM_ZEROPAGE_X, 0xD7, 2},
    {"dcp", AM_ABSOLUTE, 0xCF, 3},
    {"dcp", AM_ABSOLUTE_X, 0xDF, 3},
    {"dcp", AM_ABSOLUTE_Y, 0xDB, 3},
    {"dcp", AM_INDIRECT_X, 0xC3, 2},
    {"dcp", AM_INDIRECT_Y, 0xD3, 2},

    /* ISC (aka ISB) — INC then SBC */
    {"isc", AM_ZEROPAGE, 0xE7, 2},
    {"isc", AM_ZEROPAGE_X, 0xF7, 2},
    {"isc", AM_ABSOLUTE, 0xEF, 3},
    {"isc", AM_ABSOLUTE_X, 0xFF, 3},
    {"isc", AM_ABSOLUTE_Y, 0xFB, 3},
    {"isc", AM_INDIRECT_X, 0xE3, 2},
    {"isc", AM_INDIRECT_Y, 0xF3, 2},

    /* SLO — ASL then ORA */
    {"slo", AM_ZEROPAGE, 0x07, 2},
    {"slo", AM_ZEROPAGE_X, 0x17, 2},
    {"slo", AM_ABSOLUTE, 0x0F, 3},
    {"slo", AM_ABSOLUTE_X, 0x1F, 3},
    {"slo", AM_ABSOLUTE_Y, 0x1B, 3},
    {"slo", AM_INDIRECT_X, 0x03, 2},
    {"slo", AM_INDIRECT_Y, 0x13, 2},

    /* RLA — ROL then AND */
    {"rla", AM_ZEROPAGE, 0x27, 2},
    {"rla", AM_ZEROPAGE_X, 0x37, 2},
    {"rla", AM_ABSOLUTE, 0x2F, 3},
    {"rla", AM_ABSOLUTE_X, 0x3F, 3},
    {"rla", AM_ABSOLUTE_Y, 0x3B, 3},
    {"rla", AM_INDIRECT_X, 0x23, 2},
    {"rla", AM_INDIRECT_Y, 0x33, 2},

    /* SRE — LSR then EOR */
    {"sre", AM_ZEROPAGE, 0x47, 2},
    {"sre", AM_ZEROPAGE_X, 0x57, 2},
    {"sre", AM_ABSOLUTE, 0x4F, 3},
    {"sre", AM_ABSOLUTE_X, 0x5F, 3},
    {"sre", AM_ABSOLUTE_Y, 0x5B, 3},
    {"sre", AM_INDIRECT_X, 0x43, 2},
    {"sre", AM_INDIRECT_Y, 0x53, 2},

    /* RRA — ROR then ADC */
    {"rra", AM_ZEROPAGE, 0x67, 2},
    {"rra", AM_ZEROPAGE_X, 0x77, 2},
    {"rra", AM_ABSOLUTE, 0x6F, 3},
    {"rra", AM_ABSOLUTE_X, 0x7F, 3},
    {"rra", AM_ABSOLUTE_Y, 0x7B, 3},
    {"rra", AM_INDIRECT_X, 0x63, 2},
    {"rra", AM_INDIRECT_Y, 0x73, 2},

    /* ANC — AND #imm then set C from bit7 */
    {"anc", AM_IMMEDIATE, 0x0B, 2},
    {"anc", AM_IMMEDIATE, 0x2B, 2},

    /* ALR — AND #imm then LSR */
    {"alr", AM_IMMEDIATE, 0x4B, 2},

    /* ARR — AND #imm then ROR */
    {"arr", AM_IMMEDIATE, 0x6B, 2},

    /* XAA — highly unstable; A := X & imm */
    {"xaa", AM_IMMEDIATE, 0x8B, 2},

    /* AXS/SBX — X := (A & X) - imm */
    {"axs", AM_IMMEDIATE, 0xCB, 2},
    {"sbx", AM_IMMEDIATE, 0xCB, 2},

    /* LAS (a.k.a. LAR/LAE) — A,X,S := mem & S */
    {"las", AM_ABSOLUTE_Y, 0xBB, 3},

    /* AHX/SHX/SHY/TAS — store with high-byte interactions */
    {"ahx", AM_INDIRECT_Y, 0x93, 2},
    {"ahx", AM_ABSOLUTE_Y, 0x9F, 3},
    {"shx", AM_ABSOLUTE_Y, 0x9E, 3},
    {"shy", AM_ABSOLUTE_X, 0x9C, 3},
    {"tas", AM_ABSOLUTE_Y, 0x9B, 3},

    /* JAM/KIL/HLT — CPU lock-up (no operand) */
    {"jam", AM_NONE, 0x02, 1},
    {"jam", AM_NONE, 0x12, 1},
    {"jam", AM_NONE, 0x22, 1},
    {"jam", AM_NONE, 0x32, 1},
    {"jam", AM_NONE, 0x42, 1},
    {"jam", AM_NONE, 0x52, 1},
    {"jam", AM_NONE, 0x62, 1},
    {"jam", AM_NONE, 0x72, 1},
    {"jam", AM_NONE, 0x92, 1},
    {"jam", AM_NONE, 0xB2, 1},
    {"jam", AM_NONE, 0xD2, 1},
    {"jam", AM_NONE, 0xF2, 1},

    /* Also accept synonyms */
    {"kil", AM_NONE, 0x02, 1},
    {"hlt", AM_NONE, 0x02, 1},

    {NULL, AM_NONE, 0, 0}
};

/* Utility / error reporting */
/* Print an error with a line number and abort the program. */
void asm_error(int lineno, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "Error (line %d): ", lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* Error with filename context */
void asm_error_file(const char *file, int lineno, const char *fmt, ...)
{
    va_list ap;
    if (file && *file) {
        fprintf(stderr, "Error (%s:%d): ", file, lineno);
    } else {
        fprintf(stderr, "Error (line %d): ", lineno);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* Trim whitespace (leading and trailing) in-place and return the new start. */
char *trimws(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    size_t len = strlen(s);
    if (len == 0) {
        return s;
    }

    char *e = s + len - 1;

    while (e >= s && isspace((unsigned char)*e)) {
        *e = '\0';
        e--;
    }

    return s;
}

/* Parse a number literal. Supports:
 * - Hex: `$1A2B` or `0x1A2B`
 * - Binary: `%1010` or `0b1010`
 * - Decimal: `1234`
 * Returns 1 on success (value in `*out`), otherwise 0.
 */
int parse_number(const char *s, int *out)
{
    if (!s || !*s) {
        return 0;
    }

    /* Binary: %1010 or 0b1010 */
    if (s[0] == '%') {
        char *end;
        long v = strtol(s + 1, &end, 2);

        if (*end != '\0') {
            return 0;
        }

        *out = (int)v;
        return 1;
    }

    if (s[0] == CHAR_DOLLAR) {
        char *end;
        long v = strtol(s + 1, &end, 16);

        if (*end != '\0') {
            return 0;
        }

        *out = (int)v;
        return 1;
    }

    /* Decimal fallback, allow leading 0x too */
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        char *end;
        long v = strtol(s + 2, &end, 16);

        if (*end != '\0') {
            return 0;
        }

        *out = (int)v;
        return 1;
    }

    if (strncmp(s, "0b", 2) == 0 || strncmp(s, "0B", 2) == 0) {
        char *end;
        long v = strtol(s + 2, &end, 2);

        if (*end != '\0') {
            return 0;
        }

        *out = (int)v;
        return 1;
    }

    {
        char *end;
        long v = strtol(s, &end, 10);
        if (*end != '\0') {
            return 0;
        }

        *out = (int)v;
        return 1;
    }
}

/* Parse an operand string (e.g. "#$10", "$2000,X", "(zp),Y", label).
 * The goal here is to determine the addressing mode as much as possible
 * and either capture a literal value or keep the expression string to
 * resolve later in the second pass.
 */
operand_t parse_operand(const char *s0)
{
    operand_t op;
    op.mode = AM_NONE;
    op.value = 0;
    op.expr = NULL;

    if (!s0) {
        op.mode = AM_NONE;
        return op;
    }

    char buf[STRING_BUF];
    strncpy(buf, s0, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *s = trimws(buf);

    if (*s == '\0') {
        op.mode = AM_NONE;
        return op;
    }

    /* Immediate */
    if (*s == CHAR_HASH) {
        const char *rest = s + 1;
        int v;
        op.mode = AM_IMMEDIATE;
        if (parse_number(rest, &v)) {
            op.value = v;
        } else {
            op.expr = strdup(rest);
        }
        return op;
    }

    /* Accumulator operand (explicit A) */
    if ((strcmp(s, "A") == 0) || (strcmp(s, "a") == 0)) {
        op.mode = AM_ACCUMULATOR;
        return op;
    }

    /* Indirect parentheses */
    if (*s == CHAR_LPAREN) {
        char *rparen = strrchr(s, ')');
        if (!rparen) {
            asm_error(0, "Malformed indirect operand: %s", s);
        }
        *rparen = '\0';
        char *inside = s + 1;
        char *after = rparen + 1;
        after = trimws(after);

        /* (expr),Y — allow whitespace after comma, detected after ')' */
        if (*after == ',') {
            char *p = after + 1;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }

            if (toupper((unsigned char)*p) == 'Y') {
                int v;
                op.mode = AM_INDIRECT_Y;

                if (parse_number(inside, &v)) {
                    op.value = v;
                } else {
                    op.expr = strdup(inside);
                }
                return op;
            }
        }

        /* (expr,X) — allow whitespace after comma, detected inside parentheses */
        {
            char *comma_in = strchr(inside, CHAR_COMMA);
            if (comma_in) {
                *comma_in = '\0';
                char *base = trimws(inside);
                char *p = trimws(comma_in + 1);

                if (toupper((unsigned char)*p) == 'X') {
                    int v;
                    op.mode = AM_INDIRECT_X;

                    if (parse_number(base, &v)) {
                        op.value = v;
                    } else {
                        op.expr = strdup(base);
                    }
                    return op;
                }
            }
        }

        /* pure (expr) */
        {
            int v;
            if (parse_number(inside, &v)) {
                op.mode = AM_INDIRECT;
                op.value = v;
            } else {
                op.mode = AM_INDIRECT;
                op.expr = strdup(inside);
            }
            return op;
        }
    }

    /* Check for comma suffix ,X or ,Y (allow whitespace after comma) */
    char *comma = strchr(s, CHAR_COMMA);
    if (comma) {
        *comma = '\0';
        char *base = trimws(s);
        char *p = comma + 1;
        p = trimws(p);
        char reg = toupper((unsigned char)*p);
        int v;

        if (parse_number(base, &v)) {
            if (reg == 'X') {
                if (v <= BYTE_MAX) {
                    op.mode = AM_ZEROPAGE_X;
                } else {
                    op.mode = AM_ABSOLUTE_X;
                }
                op.value = v;
            } else if (reg == 'Y') {
                if (v <= BYTE_MAX) {
                    op.mode = AM_ZEROPAGE_Y;
                } else {
                    op.mode = AM_ABSOLUTE_Y;
                }
                op.value = v;
            } else {
                asm_error(0, "Unknown suffix register %c in %s", reg, s0);
            }
        } else {
            if (reg == 'X') {
                op.mode = AM_ABSOLUTE_X;
            } else if (reg == 'Y') {
                op.mode = AM_ABSOLUTE_Y;
            }
            op.expr = strdup(base);
        }

        return op;
    }

    /* No comma, no parentheses, no #: treat as direct value or label */
    {
        int v;
        if (parse_number(s, &v)) {
            if (v <= BYTE_MAX) {
                op.mode = AM_ZEROPAGE;
            } else {
                op.mode = AM_ABSOLUTE;
            }
            op.value = v;
        } else {
            op.mode = AM_ABSOLUTE;
            op.expr = strdup(s);
        }
    }

    return op;
}

/* Parse one line of source into an asm_line_t structure.
 * Steps:
 *  - Strip comments
 *  - Extract optional label "label:"
 *  - Recognize directives vs. mnemonics
 *  - Parse operand (if any)
 */
asm_line_t *parse_line(const char *in, int lineno)
{
    char tmp[MAX_LINE_LEN];

    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *s = tmp;

    /* Strip comments starting at ';', but ignore semicolons inside quotes */
    {
        int in_s = 0, in_d = 0;
        for (char *p = s; *p; ++p) {
            if (*p == '\'' && !in_d) {
                in_s = !in_s;
                continue;
            }

            if (*p == '"' && !in_s) {
                in_d = !in_d;
                continue;
            }

            if (!in_s && !in_d && *p == ';') {
                *p = '\0';
                break;
            }
        }
    }

    s = trimws(s);
    if (*s == '\0') {
        return NULL; /* blank or comment-only line */
    }

    asm_line_t *ln = calloc(1, sizeof(asm_line_t));
    ln->lineno = lineno;
    ln->filename = NULL;
    ln->label = NULL;
    ln->mnemonic = NULL;
    ln->op.mode = AM_NONE;
    ln->op.expr = NULL;
    ln->extra = NULL;
    ln->def_name = NULL;
    ln->def_expr = NULL;

    /* Label? Find a ':' that is outside quotes and precedes whitespace */
    char *colon = NULL;
    {
        int in_s = 0, in_d = 0;
        for (char *p = s; *p; ++p) {
            if (*p == '\'' && !in_d) {
                in_s = !in_s;
                continue;
            }

            if (*p == '"' && !in_s) {
                in_d = !in_d;
                continue;
            }

            if (!in_s && !in_d && *p == ':') {
                /* Verify the left side is a single identifier (no spaces) */
                char leftbuf[STRING_BUF];
                size_t leftlen = (size_t)(p - s);
                if (leftlen >= sizeof(leftbuf)) {
                    leftlen = sizeof(leftbuf) - 1;
                }

                memcpy(leftbuf, s, leftlen);
                leftbuf[leftlen] = '\0';
                char *lhs = trimws(leftbuf);
                int ok = (*lhs != '\0');

                for (char *q = lhs; ok && *q; ++q) {
                    if (isspace((unsigned char)*q)) {
                        ok = 0;
                        break;
                    }

                    if (!(isalnum((unsigned char)*q) || *q == '_')) {
                        ok = 0;
                        break;
                    }
                }

                if (ok) {
                    colon = p;
                }
                break;
            }
        }
    }

    if (colon) {
        *colon = '\0';
        ln->label = strdup(trimws(s));
        s = trimws(colon + 1);
    }

    if (*s == '\0') {
        /* A pure label line like "start:" */
        return ln;
    }

    /* Detect constant define: NAME = EXPR (excluding "* = expr" which is .org) */
    do {
        char *eq = strchr(s, '=');
        if (!eq) {
            break;
        }

        /* Split into lhs and rhs, without destroying original s further */
        *eq = '\0';
        char *lhs = trimws(s);
        char *rhs = trimws(eq + 1);

        /* If lhs is '*' this is the origin directive (supports "*=$c000") */
        if (strcmp(lhs, "*") == 0) {
            ln->mnemonic = strdup("*");
            ln->extra = strdup(rhs);
            return ln;
        }

        /* Ensure lhs looks like an identifier (letters/underscore/digits) */
        int ok = (*lhs != '\0');
        for (char *p = lhs; ok && *p; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_' )) {
                ok = 0;
            }
        }

        /* Avoid confusing mnemonics/directives as define names */
        if (ok &&
                (is_mnemonic_name(lhs) ||
                 strcasecmp(lhs, ".org") == 0 ||
                 strcasecmp(lhs, ".byte") == 0 ||
                 strcasecmp(lhs, ".word") == 0 ||
                 strcasecmp(lhs, ".text") == 0 ||
                 strcasecmp(lhs, ".incbin") == 0 ||
                 strcasecmp(lhs, ".include") == 0))
        {
            ok = 0;
        }

        if (ok) {
            ln->def_name = strdup(lhs);
            ln->def_expr = strdup(rhs);
            /* Mark mnemonic to a sentinel to identify this as a define in passes */
            ln->mnemonic = strdup("=");
            return ln;
        }

        /* Not a define; restore '=' and continue to normal parse */
        *eq = '=';
    } while (0);

    /* Support label without colon if the entire line is a single token that is
     * NOT a known mnemonic or directive. */
    if (!colon) {
        char *ws = s;
        /* scan first token */
        while (*ws && !isspace((unsigned char)*ws)) {
            ws++;
        }

        /* Extract the first token without modifying the original buffer */
        size_t tok_len = (size_t)(ws - s);
        char first_tok[STRING_BUF];
        if (tok_len >= sizeof(first_tok)) {
            tok_len = sizeof(first_tok) - 1;
        }
        memcpy(first_tok, s, tok_len);
        first_tok[tok_len] = '\0';

        /* Compute the rest of the line after the token */
        char *rest_after = (*ws) ? trimws(ws + 1) : ws; /* points to after token */
        int is_single_token = (*rest_after == '\0');

        int is_dir = (strcasecmp(first_tok, ".org") == 0) ||
                     (strcasecmp(first_tok, ".byte") == 0) ||
                     (strcasecmp(first_tok, ".word") == 0) ||
                     (strcasecmp(first_tok, ".text") == 0) ||
                     (strcasecmp(first_tok, ".incbin") == 0) ||
                     (strcasecmp(first_tok, ".include") == 0) ||
                     (strcmp(first_tok, "*") == 0);

        int is_mn = is_mnemonic_name(first_tok);

        if (!is_mn && !is_dir) {
            ln->label = strdup(first_tok);
            if (is_single_token) {
                /* Pure label on its own line */
                return ln;
            }

            /* Label followed by instruction/directive on the same line */
            s = rest_after;
        }
    }

    /* mnemonic / directive */
    char *tok = strtok(s, " \t");
    ln->mnemonic = strdup(tok);

    char *rest = strtok(NULL, "");
    if (rest) {
        rest = trimws(rest);
    }

    /* directives */
    if ((strcasecmp(ln->mnemonic, ".org") == 0) ||
        (strcmp(ln->mnemonic, "*") == 0))
    {
        ln->extra = rest ? strdup(rest) : NULL;
        return ln;
    }

    if ((strcasecmp(ln->mnemonic, ".incbin") == 0)) {
        ln->extra = rest ? strdup(rest) : NULL;
        return ln;
    }

    if ((strcasecmp(ln->mnemonic, ".include") == 0))
    {
        ln->extra = rest ? strdup(rest) : NULL;
        return ln;
    }

    if ((strcasecmp(ln->mnemonic, ".byte") == 0) ||
        (strcasecmp(ln->mnemonic, ".word") == 0) ||
        (strcasecmp(ln->mnemonic, ".text") == 0))
    {
        ln->extra = rest ? strdup(rest) : NULL;
        return ln;
    }

    /* else: instruction */
    ln->op = parse_operand(rest);

    /* If no operand provided, but the mnemonic supports accumulator addressing,
     * default to AM_ACCUMULATOR to allow forms like "ASL"/"LSR"/"ROL"/"ROR". */
    if ((!rest || *rest == '\0') && ln->op.mode == AM_NONE) {
        const opcode_t *acc = opcode_lookup(ln->mnemonic, AM_ACCUMULATOR);
        if (acc) {
            ln->op.mode = AM_ACCUMULATOR;
        }
    }

    /* Branch mnemonics always use relative addressing */
    if (ln->mnemonic &&
            (strcasecmp(ln->mnemonic, "bcc") == 0 ||
            strcasecmp(ln->mnemonic, "bcs") == 0 ||
            strcasecmp(ln->mnemonic, "beq") == 0 ||
            strcasecmp(ln->mnemonic, "bmi") == 0 ||
            strcasecmp(ln->mnemonic, "bne") == 0 ||
            strcasecmp(ln->mnemonic, "bpl") == 0 ||
            strcasecmp(ln->mnemonic, "bvc") == 0 ||
            strcasecmp(ln->mnemonic, "bvs") == 0))
    {
        ln->op.mode = AM_RELATIVE;
    }

    return ln;
}

/* Symbol table lookup: return address for name, or -1 if not found. */
int sym_lookup(const char *name)
{
    for (int i = 0; i < sym_count; i++) {
        if (strcmp(symtab[i].name, name) == 0) {
            return symtab[i].addr;
        }
    }

    return -1;
}

/* Add a symbol to the table, erroring on duplicates. */
void sym_add(const char *name, int addr, const char *file, int lineno)
{
    if (sym_lookup(name) >= 0) {
        asm_error_file(file, lineno, "Duplicate symbol: %s", name);
    }

    symtab[sym_count].name = strdup(name);
    symtab[sym_count].addr = addr;
    sym_count++;
}

/* Expression evaluation.
 *
 * Supports:
 *   - Numbers: decimal (123), hex ($7F or 0x7F)
 *   - Symbols: names defined by labels
 *   - Operators with precedence and parentheses:
 *       factor:    [+|-] factor | number | symbol | '(' expr ')'
 *       term:      factor { ('*'|'/') factor }
 *       expr:      term   { ('+'|'-') term   }
 *
 * Returns 1 on success and stores value in *out, else 0.
 */

static const char *expr_skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    return p;
}

static int eval_parse_token(const char **pp, char *buf, int bufsz)
{
    /* Parse a token that is not an operator or parenthesis. */
    const char *p = expr_skip_ws(*pp);
    int i = 0;

    while (*p && !isspace((unsigned char)*p)) {
        char c = *p;
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
            break;
        }

        if (i < bufsz - 1) {
            buf[i++] = c;
        }

        p++;
    }

    buf[i] = '\0';
    *pp = p;

    return i > 0;
}

static int eval_parse_factor(const char **pp, int *out)
{
    const char *p = expr_skip_ws(*pp);

    int sign = +1;

    if (*p == '+') {
        p++;
        p = expr_skip_ws(p);
    } else if (*p == '-') {
        sign = -1;
        p++;
        p = expr_skip_ws(p);
    }

    /* Low/high byte operators: '<expr' (lo), '>expr' (hi) */
    if (*p == '<' || *p == '>') {
        char op = *p++;
        int vsub;

        if (!eval_parse_factor(&p, &vsub)) {
            return 0;
        }

        int v = (op == '<') ? (vsub & 0xFF) : ((vsub >> 8) & 0xFF);
        *pp = p;
        *out = sign * v;
        return 1;
    }

    if (*p == '(') {
        p++;

        if (!eval_parse_expr(&p, out)) {
            return 0;
        }

        p = expr_skip_ws(p);
        if (*p != ')') {
            return 0;
        }

        p++;
        *pp = p;
        *out = sign * (*out);
        return 1;
    } else {
        /* Support current PC symbol '*' at factor position */
        if (*p == '*') {
            p++;
            *pp = p;
            *out = sign * g_eval_pc;
            return 1;
        }

        char tok[STRING_BUF];
        if (!eval_parse_token(&p, tok, sizeof(tok))) {
            return 0;
        }

        int v;
        if (parse_number(tok, &v)) {
            *out = sign * v;
            *pp = p;
            return 1;
        }

        int s = sym_lookup(tok);
        if (s < 0) {
            return 0;
        }

        *out = sign * s;
        *pp = p;
        return 1;
    }
}

static int eval_parse_term(const char **pp, int *out)
{
    if (!eval_parse_factor(pp, out)) {
        return 0;
    }

    const char *p = expr_skip_ws(*pp);

    while (*p == '*' || *p == '/') {
        char op = *p++;
        int rhs;

        if (!eval_parse_factor(&p, &rhs)) {
            return 0;
        }

        if (op == '*') {
            *out = (*out) * rhs;
        } else {
            if (rhs == 0) {
                return 0; /* division by zero */
            }

            *out = (*out) / rhs;
        }

        p = expr_skip_ws(p);
    }

    *pp = p;

    return 1;
}

static int eval_parse_expr(const char **pp, int *out)
{
    if (!eval_parse_term(pp, out)) {
        return 0;
    }

    const char *p = expr_skip_ws(*pp);

    while (*p == '+' || *p == '-') {
        char op = *p++;
        int rhs;

        if (!eval_parse_term(&p, &rhs)) {
            return 0;
        }

        if (op == '+') {
            *out = (*out) + rhs;
        } else {
            *out = (*out) - rhs;
        }

        p = expr_skip_ws(p);
    }

    *pp = p;

    return 1;
}

int eval_expr(const char *expr, int *out) {
    const char *p = expr;

    if (!eval_parse_expr(&p, out)) {
        return 0;
    }

    p = expr_skip_ws(p);
    if (*p != '\0') {
        /* Trailing junk */
        return 0;
    }

    return 1;
}

/* Utility: check if a given token matches any known mnemonic (legal or illegal). */
int is_mnemonic_name(const char *name)
{
    if (!name || !*name) {
        return 0;
    }

    for (int i = 0; opcode_table[i].mn != NULL; i++) {
        if (strcasecmp(opcode_table[i].mn, name) == 0) {
            return 1;
        }
    }

    for (int i = 0; illegal_table[i].mn != NULL; i++) {
        if (strcasecmp(illegal_table[i].mn, name) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Lookup an opcode by mnemonic and addressing mode. */
const opcode_t *opcode_lookup(const char *mn, addrmode_t mode)
{
    for (int i = 0; opcode_table[i].mn != NULL; i++) {
        if (strcasecmp(opcode_table[i].mn, mn) == 0 && opcode_table[i].mode == mode) {
            return &opcode_table[i];
        }
    }

    if (allow_illegal) {
        for (int i = 0; illegal_table[i].mn != NULL; i++) {
            if (strcasecmp(illegal_table[i].mn, mn) == 0 && illegal_table[i].mode == mode) {
                return &illegal_table[i];
            }
        }
    }

    return NULL;
}

/* ------------ Small helpers to simplify passes ------------ */
static int is_org(const asm_line_t *ln)
{
    return (ln->mnemonic && ((strcasecmp(ln->mnemonic, ".org") == 0) ||
                (strcmp(ln->mnemonic, "*") == 0)));
}

static int is_incbin(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".incbin") == 0));
}

static int is_byte_dir(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".byte") == 0));
}

static int is_word_dir(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".word") == 0));
}

static int is_text_dir(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".text") == 0));
}

static int is_define(const asm_line_t *ln)
{
    return (ln && ln->def_name != NULL && ln->def_expr != NULL);
}

static int parse_org_value_file(const char *file, const char *extra, int lineno, int pc)
{
    if (!extra) {
        asm_error_file(file, lineno, "Missing expression in org directive");
    }

    char buf[MAX_LINE_LEN];

    strncpy(buf, extra, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *expr = trimws(buf);
    if (*expr == '=') {
        expr++;
        expr = trimws(expr);
    }

    int v;
    g_eval_pc = pc;

    if (parse_number(expr, &v) || eval_expr(expr, &v)) {
        return v;
    }

    asm_error_file(file, lineno, "Bad expression in org: %s", extra);

    return pc; /* not reached */
}

/* Backward-compat wrapper where filename is unknown */
static int parse_org_value(const char *extra, int lineno, int pc)
{
    return parse_org_value_file(NULL, extra, lineno, pc);
}

static int count_csv_items(const char *s)
{
    int cnt = 0;
    char *dup = strdup(s ? s : "");
    char *tok = strtok(dup, ",");

    while (tok) {
        cnt++;
        tok = strtok(NULL, ",");
    }

    free(dup);

    return cnt;
}

static void first_handle_org(const asm_line_t *ln, int *pc)
{
    *pc = parse_org_value_file(ln->filename, ln->extra, ln->lineno, *pc);

    DEBUG_PRINT("ORG: 0x%04x\n", *pc);
}

static void first_handle_incbin(const asm_line_t *ln, int *pc)
{
    if (!ln->extra) {
        asm_error_file(ln->filename, ln->lineno, "Missing filename for .incbin");
    }

    char fnbuf[STRING_BUF];

    strncpy(fnbuf, ln->extra, sizeof(fnbuf) - 1);
    fnbuf[sizeof(fnbuf) - 1] = '\0';

    char *fn = trimws(fnbuf);
    if (*fn == '"' || *fn == '\'') {
        fn++;
        char *qe = strrchr(fn, fn[-1]);
        if (qe) {
            *qe = '\0';
        }
    }

    FILE *f = fopen(fn, "rb");
    if (!f) {
        asm_error_file(ln->filename, ln->lineno, "Cannot open .incbin file: %s", fn);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);

    DEBUG_PRINT(".incbin: %s at 0x%04x, len: %d\n", fn, *pc, (int)sz);

    *pc += (int)sz;
}

static void first_handle_byte(const asm_line_t *ln, int *pc)
{
    if (!ln->extra) {
        asm_error_file(ln->filename, ln->lineno, "Missing data in .byte");
    }

    *pc += count_csv_items(ln->extra);
}

static void first_handle_word(const asm_line_t *ln, int *pc)
{
    if (!ln->extra) {
        asm_error_file(ln->filename, ln->lineno, "Missing data in .word");
    }

    *pc += 2 * count_csv_items(ln->extra);
}

static void first_handle_text(const asm_line_t *ln, int *pc)
{
    if (!ln->extra) {
        asm_error_file(ln->filename, ln->lineno, "Missing string in .text");
    }

    char buf[STRING_BUF];
    strncpy(buf, ln->extra, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t = trimws(buf);
    if (*t != '"' && *t != '\'') {
        asm_error_file(ln->filename, ln->lineno, "Expected quoted string for .text");
    }

    char quote = *t++;
    char *end = strrchr(t, quote);
    if (!end) {
        asm_error_file(ln->filename, ln->lineno, "Unterminated string in .text");
    }
    *end = '\0';

    *pc += (int)strlen(t);
}

static void first_handle_instruction(const asm_line_t *ln, int *pc)
{
    const opcode_t *op = opcode_lookup(ln->mnemonic, ln->op.mode);
    /* Fallback: if ZEROPAGE,Y isn't supported for this mnemonic, try ABSOLUTE,Y. */
    if (!op && ln->op.mode == AM_ZEROPAGE_Y) {
        const opcode_t *alt = opcode_lookup(ln->mnemonic, AM_ABSOLUTE_Y);
        if (alt) {
            /* Mutate the parsed mode so pass 2 uses the same */
            ((asm_line_t *)ln)->op.mode = AM_ABSOLUTE_Y;
            op = alt;
        }
    }
    if (!op) {
        asm_error_file(ln->filename, ln->lineno, "Invalid opcode or addressing mode: %s %s",
                ln->mnemonic, (ln->op.expr ? ln->op.expr : "(no operand)"));
    }

    *pc += op->length;
}

static void first_handle_define(const asm_line_t *ln, int *pc)
{
    (void)pc; /* PC does not change for a define */
    if (!ln->def_name || !ln->def_expr) {
        asm_error_file(ln->filename, ln->lineno, "Malformed define");
    }

    int v;
    g_eval_pc = *pc;

    /* Accept an optional leading '#' as in immediate syntax (e.g., NAME = #$10) */
    const char *def_expr = ln->def_expr;
    if (def_expr[0] == '#') {
        def_expr++; /* skip immediate marker */

        while (*def_expr && isspace((unsigned char)*def_expr)) {
            def_expr++;
        }
    }

    if (!(parse_number(def_expr, &v) || eval_expr(def_expr, &v))) {
        /* Try symbol lookup if it's a bare name */
        int s = sym_lookup(def_expr);
        if (s < 0) {
            asm_error_file(ln->filename, ln->lineno, "Bad expression in define: %s = %s",
                      ln->def_name, ln->def_expr);
        }
        v = s;
    }

    sym_add(ln->def_name, v, ln->filename, ln->lineno);
}

/* FIRST PASS: Assign addresses (PC) and build symbol table. */
void first_pass(void)
{
    DEBUG_PRINT("First pass...\n");

    int pc = origin;

    for (int i = 0; i < line_count; i++) {
        asm_line_t *ln = lines[i];
        if (!ln) {
            continue;
        }

        if (ln->label) {
            DEBUG_PRINT("Found label: \"%s\" at 0x%04x\n", ln->label, pc);
            sym_add(ln->label, pc, ln->filename, ln->lineno);
        }

        if (!ln->mnemonic) {
            continue;
        }

        if (is_define(ln)) {
            first_handle_define(ln, &pc);
            continue;
        }

        if (is_org(ln)) {
            first_handle_org(ln, &pc);
            continue;
        }

        if (is_incbin(ln)) {
            first_handle_incbin(ln, &pc);
            continue;
        }

        if (is_byte_dir(ln)) {
            first_handle_byte(ln, &pc);
            continue;
        }

        if (is_word_dir(ln)) {
            first_handle_word(ln, &pc);
            continue;
        }

        if (is_text_dir(ln)) {
            first_handle_text(ln, &pc);
            continue;
        }

        first_handle_instruction(ln, &pc);
    }
}

/* Emit a single byte into the output buffer at absolute PC. */
void emit_byte(int pc, uint8_t b)
{
    if (pc < 0 || pc >= MAX_OUTPUT) {
        asm_error(0, "Output PC out of range: %04X", pc);
    }

    outbuf[pc] = b;

    if (pc < out_lo) {
        out_lo = pc;
    }

    if (pc > out_hi) {
        out_hi = pc;
    }

    DEBUG_PRINT("EMIT: 0x%04x : 0x%02x\n", pc, b);
}

/* Emit a 16-bit little-endian word at absolute PC. */
void emit_word(int pc, int v)
{
    emit_byte(pc, LOBYTE(v));
    emit_byte(pc + 1, HIBYTE(v));
}

static void second_handle_org(const asm_line_t *ln, int *pc)
{
    *pc = parse_org_value(ln->extra, ln->lineno, *pc);

    DEBUG_PRINT("ORG: 0x%04x\n", *pc);
}

static void second_handle_incbin(const asm_line_t *ln, int *pc)
{
    char fnbuf[STRING_BUF];

    strncpy(fnbuf, ln->extra ? ln->extra : "", sizeof(fnbuf) - 1);
    fnbuf[sizeof(fnbuf) - 1] = '\0';

    char *fn = trimws(fnbuf);
    if (*fn == '"' || *fn == '\'') {
        fn++;
        char *qe = strrchr(fn, fn[-1]);
        if (qe) {
            *qe = '\0';
        }
    }

    FILE *f = fopen(fn, "rb");
    if (!f) {
        asm_error_file(ln->filename, ln->lineno, "Cannot open .incbin file (second pass): %s", fn);
    }

    int c;

    DEBUG_PRINT(".incbin: %s at 0x%04x\n", fn, *pc);

    while ((c = fgetc(f)) != EOF) {
        emit_byte(*pc, (uint8_t)c);
        (*pc)++;
    }

    fclose(f);
}

static void second_handle_byte(const asm_line_t *ln, int *pc)
{
    char *cp = strdup(ln->extra ? ln->extra : "");
    char *tok = strtok(cp, ",");

    while (tok) {
        char *t = trimws(tok);
        int v;
        g_eval_pc = *pc;

        if (parse_number(t, &v) || eval_expr(t, &v)) {
            if (v < 0 || v > BYTE_MAX) {
                asm_error_file(ln->filename, ln->lineno, ".byte value %d out of 8-bit range", v);
            }

            emit_byte(*pc, (uint8_t)v);
        } else {
            int s = sym_lookup(t);
            if (s < 0) {
                asm_error_file(ln->filename, ln->lineno, "Undefined symbol %s in .byte", t);
            }

            if (s < 0 || s > BYTE_MAX) {
                asm_error_file(ln->filename, ln->lineno, ".byte symbol %s = %d out of 8-bit range",
                        t, s);
            }

            emit_byte(*pc, (uint8_t)s);
        }

        (*pc)++;
        tok = strtok(NULL, ",");
    }

    free(cp);
}

static void second_handle_word(const asm_line_t *ln, int *pc)
{
    char *cp = strdup(ln->extra ? ln->extra : "");
    char *tok = strtok(cp, ",");

    while (tok) {
        char *t = trimws(tok);
        int v;
        g_eval_pc = *pc;

        if (parse_number(t, &v) || eval_expr(t, &v)) {
            if (v < 0 || v > WORD_MAX) {
                asm_error_file(ln->filename, ln->lineno, ".word value %d out of 16-bit range", v);
            }

            emit_word(*pc, v);
        } else {
            int s = sym_lookup(t);
            if (s < 0) {
                asm_error_file(ln->filename, ln->lineno, "Undefined symbol %s in .word", t);
            }

            if (s < 0 || s > WORD_MAX) {
                asm_error_file(ln->filename, ln->lineno,
                        ".word symbol %s = %d out of 16-bit range", t, s);
            }

            emit_word(*pc, s);
        }

        (*pc) += 2;
        tok = strtok(NULL, ",");
    }

    free(cp);
}

static void second_handle_text(const asm_line_t *ln, int *pc)
{
    char buf[STRING_BUF];
    strncpy(buf, ln->extra ? ln->extra : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t = trimws(buf);
    if (*t != '"' && *t != '\'') {
        asm_error_file(ln->filename, ln->lineno, "Expected quoted string for .text");
    }

    char quote = *t++;
    char *end = strrchr(t, quote);
    if (!end) {
        asm_error_file(ln->filename, ln->lineno, "Unterminated string in .text");
    }
    *end = '\0';

    for (char *p = t; *p; ++p) {
        emit_byte(*pc, (uint8_t)(unsigned char)(*p));
        (*pc)++;
    }
}

static void second_emit_instruction(const asm_line_t *ln, int *pc)
{
    const opcode_t *op = opcode_lookup(ln->mnemonic, ln->op.mode);
    /* Fallback: if ZEROPAGE,Y isn't supported for this mnemonic, try ABSOLUTE,Y. */
    if (!op && ln->op.mode == AM_ZEROPAGE_Y) {
        const opcode_t *alt = opcode_lookup(ln->mnemonic, AM_ABSOLUTE_Y);
        if (alt) {
            ((asm_line_t *)ln)->op.mode = AM_ABSOLUTE_Y;
            op = alt;
        }
    }
    if (!op) {
        asm_error_file(ln->filename, ln->lineno, "Cannot assemble %s with mode %d", ln->mnemonic,
                ln->op.mode);
    }

    emit_byte(*pc, op->opcode);

    switch (op->length) {
        case 1:
            (*pc) += 1;
            break;

        case 2: {
                    int v;
                    if (ln->op.mode == AM_RELATIVE) {
                        int target;

                        if (ln->op.expr) {
                            int tmp;
                            g_eval_pc = *pc;

                            if (eval_expr(ln->op.expr, &tmp) || parse_number(ln->op.expr, &tmp)) {
                                target = tmp;
                            } else {
                                target = sym_lookup(ln->op.expr);
                                if (target < 0) {
                                    asm_error_file(ln->filename, ln->lineno, "Undefined label %s for branch",
                                            ln->op.expr);
                                }
                            }
                        } else {
                            v = ln->op.value;
                            target = v;
                        }

                        int offset = target - (*pc + 2);
                        if (offset < BRANCH_MIN || offset > BRANCH_MAX) {
                            asm_error_file(ln->filename, ln->lineno, "Branch offset out of range: %d", offset);
                        }

                        emit_byte(*pc + 1, (uint8_t)offset);
                    } else {
                        if (ln->op.expr) {
                            g_eval_pc = *pc;

                            if (eval_expr(ln->op.expr, &v) || parse_number(ln->op.expr, &v)) {
                                /* ok */
                            } else {
                                int s = sym_lookup(ln->op.expr);
                                if (s < 0) {
                                    asm_error_file(ln->filename, ln->lineno, "Undefined label %s",
                                            ln->op.expr);
                                }

                                v = s;
                            }
                        } else {
                            v = ln->op.value;
                        }

                        if (v < 0 || v > BYTE_MAX) {
                            asm_error_file(ln->filename, ln->lineno, "8-bit operand %d out of range", v);
                        }

                        emit_byte(*pc + 1, LOBYTE(v));
                    }

                    (*pc) += 2;
                    break;
                }

        case 3: {
                    int v;

                    if (ln->op.expr) {
                        g_eval_pc = *pc;

                        if (eval_expr(ln->op.expr, &v) || parse_number(ln->op.expr, &v)) {
                            /* ok */
                        } else {
                            int s = sym_lookup(ln->op.expr);
                            if (s < 0) {
                                asm_error_file(ln->filename, ln->lineno, "Undefined label %s", ln->op.expr);
                            }
                            v = s;
                        }
                    } else {
                        v = ln->op.value;
                    }

                    if (v < 0 || v > WORD_MAX) {
                        asm_error_file(ln->filename, ln->lineno, "16-bit operand %d out of range", v);
                    }

                    emit_word(*pc + 1, v);
                    (*pc) += 3;

                    break;
                }

        default:
                asm_error_file(ln->filename, ln->lineno, "Invalid opcode length %d", op->length);
    }
}

/* SECOND PASS: Generate final machine code */
void second_pass(void)
{
    DEBUG_PRINT("Second pass...\n");

    int pc = origin;

    for (int i = 0; i < line_count; i++) {
        asm_line_t *ln = lines[i];

        if (!ln || !ln->mnemonic) {
            continue;
        }

        if (is_define(ln)) {
            /* Defines do not emit bytes or change PC in pass 2 */
            continue;
        }

        if (is_org(ln)) {
            second_handle_org(ln, &pc);
            continue;
        }

        if (is_incbin(ln)) {
            second_handle_incbin(ln, &pc);
            continue;
        }

        if (is_byte_dir(ln)) {
            second_handle_byte(ln, &pc);
            continue;
        }

        if (is_word_dir(ln)) {
            second_handle_word(ln, &pc);
            continue;
        }

        if (is_text_dir(ln)) {
            second_handle_text(ln, &pc);
            continue;
        }

        second_emit_instruction(ln, &pc);
    }
}

/* ---------------- Include expansion loader ---------------- */

static void free_line(asm_line_t *ln)
{
    if (!ln) {
        return;
    }

    if (ln->filename) {
        free(ln->filename);
    }

    if (ln->label) {
        free(ln->label);
    }

    if (ln->mnemonic) {
        free(ln->mnemonic);
    }

    if (ln->op.expr) {
        free(ln->op.expr);
    }

    if (ln->extra) {
        free(ln->extra);
    }

    if (ln->def_name) {
        free(ln->def_name);
    }

    if (ln->def_expr) {
        free(ln->def_expr);
    }

    free(ln);
}

static void expand_include(const char *including_path, const char *arg, int lineno)
{
    if (!arg) {
        asm_error_file(including_path, lineno, "Missing filename for .include");
    }

    char fnbuf[STRING_BUF];

    strncpy(fnbuf, arg, sizeof(fnbuf) - 1);
    fnbuf[sizeof(fnbuf) - 1] = '\0';

    char *fn = trimws(fnbuf);

    if (*fn == '"' || *fn == '\'') {
        char quote = *fn++;
        char *qe = strrchr(fn, quote);
        if (qe) {
            *qe = '\0';
        }
    }

    char resolved[PATH_MAX];
    /* Absolute path? */
    if (fn[0] == '/') {
        strncpy(resolved, fn, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    } else {
        /* Build path relative to directory of including file */
        const char *base = including_path ? including_path : "";
        const char *slash = strrchr(base, '/');
        if (slash) {
            size_t dirlen = (size_t)(slash - base);

            if (dirlen >= sizeof(resolved)) {
                dirlen = sizeof(resolved) - 1;
            }

            memcpy(resolved, base, dirlen);
            resolved[dirlen] = '\0';
            strncat(resolved, "/", sizeof(resolved) - strlen(resolved) - 1);
            strncat(resolved, fn, sizeof(resolved) - strlen(resolved) - 1);
        } else {
            /* No directory in including path; use relative as-is */
            strncpy(resolved, fn, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
        }
    }

    read_file_with_includes(resolved);
}

static void read_file_with_includes(const char *path)
{
    if (!path) return;

    if (include_depth >= MAX_INCLUDE_DEPTH) {
        fprintf(stderr, "Error: include nesting too deep while opening %s\n", path);
        exit(1);
    }

    FILE *fin = fopen(path, "r");
    if (!fin) {
        fprintf(stderr, "Error (open %s): %s\n", path, strerror(errno));
        exit(1);
    }

    include_depth++;

    char linebuf[MAX_LINE_LEN];
    int lineno = 0;

    while (fgets(linebuf, sizeof(linebuf), fin)) {
        lineno++;
        asm_line_t *ln = parse_line(linebuf, lineno);
        if (!ln) {
            continue;
        }

        /* attach source filename */
        ln->filename = strdup(path);

        /* Expand include directives inline */
        if (ln->mnemonic &&
            (strcasecmp(ln->mnemonic, ".include") == 0))
        {
            expand_include(path, ln->extra, lineno);
            free_line(ln);
            continue;
        }

        lines[line_count++] = ln;

        if (line_count >= MAX_LINES) {
            asm_error_file(path, lineno, "Too many lines in input");
        }
    }

    fclose(fin);
    include_depth--;
}

/* Main */
/* Program entry: parse CLI, read file, assemble (two-pass), write output. */
int main(int argc, char **argv)
{
    int write_header = 1; /* default: write 2-byte load address */
    const char *in_path = NULL;
    const char *out_path = NULL;

    /* Parse flags */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--prg-header") == 0 || strcmp(a, "-H") == 0) {
            write_header = 1;
        } else if (strcmp(a, "--no-prg-header") == 0 || strcmp(a, "-N") == 0) {
            write_header = 0;
        } else if (strcmp(a, "--illegal-opcodes") == 0 || strcmp(a, "-I") == 0) {
            allow_illegal = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            fprintf(stderr,
                    "Usage: %s [--prg-header|-H] [--no-prg-header|-N] "
                    "[--illegal-opcodes|-I] input.asm output.bin\n",
                    argv[0]);
            return 1;
        } else if (!in_path) {
            in_path = a;
        } else if (!out_path) {
            out_path = a;
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", a);
            fprintf(stderr,
                    "Usage: %s [--prg-header|-H] [--no-prg-header|-N] "
                    "[--illegal-opcodes|-I] input.asm output.bin\n",
                    argv[0]);
            return 1;
        }
    }

    if (!in_path || !out_path) {
        fprintf(stderr,
                "Usage: %s [--prg-header|-H] [--no-prg-header|-N] "
                "[--illegal-opcodes|-I] input.asm output.bin\n",
                argv[0]);
        return 1;
    }

    read_file_with_includes(in_path);

    /* Reset output buffer so regions between .org segments are zero-filled */
    memset(outbuf, 0, sizeof(outbuf));
    out_lo = MAX_OUTPUT;
    out_hi = 0;

    origin = 0;
    first_pass();
    second_pass();

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        perror("fopen output");
        return 1;
    }

    int len = out_hi - out_lo + 1;
    if (len > 0) {
        if (write_header) {
            /* Write load address header (little endian) */
            fputc(LOBYTE(out_lo), fout);
            fputc(HIBYTE(out_lo), fout);
        }

        /* Then write program bytes */
        fwrite(&outbuf[out_lo], 1, len, fout);
    }

    fclose(fout);

    if (write_header) {
        printf("Assembled %d bytes from $%04X to $%04X (%d-byte load address)\n",
                len, out_lo, out_hi, PRG_HEADER_SIZE);
    } else {
        printf("Assembled %d bytes from $%04X to $%04X (raw)\n", len,
                out_lo, out_hi);
    }

    return 0;
}
