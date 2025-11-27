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

/* Central directive classifier used by both passes */
typedef enum dir_kind
{
    DIR_NONE = 0,
    DIR_DEFINE,
    DIR_ORG,
    DIR_INCBIN,
    DIR_BYTE,
    DIR_WORD,
    DIR_TEXT
} dir_kind_t;

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

/* Local temporary labels support ("-" backward and "+" forward) */
static int minus_idx[MAX_LINES];
static int minus_pc[MAX_LINES];
static int minus_count = 0;
static int plus_idx[MAX_LINES];
static int plus_pc[MAX_LINES];
static int plus_count = 0;
static int current_line_index = -1; /* used during second pass to resolve +/- */

/* Include handling */
#define MAX_INCLUDE_DEPTH 64
static int include_depth = 0;

/* ------------- Macro system ------------- */
#define MAX_MACROS 256
#define MAX_MACRO_ARGS 16

typedef struct macro_def
{
    char *name;
    int argc;
    char *argnames[MAX_MACRO_ARGS];
    /* Raw body lines as read (without trailing newlines) */
    char **body;
    int body_count;
    int body_cap;
    /* Collected local labels defined in the body (tokens ending with ':') */
    char **local_labels;
    int local_count;
    int local_cap;
} macro_def_t;

static macro_def_t macros[MAX_MACROS];
static int macro_count = 0;
static int macro_expand_counter = 0;

/* Forward decl: loader that expands includes into global lines[] */
static void read_file_with_includes(const char *path);
static void expand_include(const char *including_path, const char *arg, int lineno);
/* Macro support: forward decls */
static void macro_init(void);
static int  macro_try_define(FILE *fin, const char *path, const char *header_line, int *lineno);
static int  macro_try_expand_and_emit(const char *path, const char *line, int lineno);
/* Parser helper used by macro matcher */
static void strip_comments_preserving_quotes(char *s);
/* Forward declaration for opcode lookup used during parsing */
const opcode_t *opcode_lookup(const char *mn, addrmode_t mode);
/* Forward declaration: parse expression involving +, -, parentheses. */
int eval_expr(const char *expr, int *out);
static int eval_parse_expr(const char **pp, int *out); /* forward */
/* Forward decl for mnemonic check by name */
int is_mnemonic_name(const char *name);
/* Forward decl: line parser used by macro expansion */
struct asm_line;
struct asm_line *parse_line(const char *in, int lineno);

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

/* ------------- Macro system ------------- */

static void macro_init(void)
{
    /* nothing to init beyond globals right now */
}

static macro_def_t *macro_find(const char *name)
{
    for (int i = 0; i < macro_count; ++i) {
        if (strcasecmp(macros[i].name, name) == 0) {
            return &macros[i];
        }
    }

    return NULL;
}

static int is_ident_start(char c)
{
    return (isalpha((unsigned char)c) || c == '_');
}

static int is_ident_char(char c)
{
    return (isalnum((unsigned char)c) || c == '_');
}

static void macro_collect_local_label(macro_def_t *m, const char *line)
{
    /* A local label is: optional ws, then identifier, then ':' */
    const char *p = line;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    const char *start = p;

    if (!is_ident_start(*p)) {
        return;
    }

    while (*p && is_ident_char(*p)) {
        p++;
    }

    if (*p != ':') {
        return;
    }

    size_t len = (size_t)(p - start);
    if (len == 0) {
        return;
    }

    char tmp[STRING_BUF];

    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }

    memcpy(tmp, start, len); tmp[len] = '\0';

    /* Append to local label list if not present */
    for (int i = 0; i < m->local_count; ++i) {
        if (strcmp(m->local_labels[i], tmp) == 0) {
            return;
        }
    }

    if (m->local_count >= m->local_cap) {
        m->local_cap = m->local_cap ? m->local_cap * 2 : 8;
        m->local_labels = realloc(m->local_labels, m->local_cap * sizeof(char*));
    }

    m->local_labels[m->local_count++] = strdup(tmp);
}

static void macro_body_push_line(macro_def_t *m, const char *line)
{
    if (m->body_count >= m->body_cap) {
        m->body_cap = m->body_cap ? m->body_cap * 2 : 16;
        m->body = realloc(m->body, m->body_cap * sizeof(char*));
    }

    m->body[m->body_count++] = strdup(line);
    macro_collect_local_label(m, line);
}

static void macro_register(const macro_def_t *src)
{
    if (macro_find(src->name)) {
        asm_error(0, "Duplicate macro: %s", src->name);
    }

    if (macro_count >= MAX_MACROS) {
        asm_error(0, "Too many macros defined");
    }

    macros[macro_count] = *src; /* shallow copy of owned pointers */
    macro_count++;
}

/* Parse a .macro header line. Example:
 *   .macro ClearScreen(screen,clearByte) {
 * Accepts optional opening '{' at end. Returns 1 if header parsed, else 0. */
static int macro_parse_header(const char *line, char *name_out, char argnames[][STRING_BUF], int *argc_out, int *has_open_brace)
{
    *argc_out = 0;
    *has_open_brace = 0;
    name_out[0] = '\0';

    char buf[MAX_LINE_LEN];

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *s = trimws(buf);

    if (strncasecmp(s, ".macro", 6) != 0) {
        return 0;
    }

    s += 6;
    s = trimws(s);

    if (!is_ident_start(*s)) {
        return 0;
    }

    /* name */
    char *p = s; int nlen = 0;

    while (*p && is_ident_char(*p)) {
        if (nlen < STRING_BUF-1) {
            name_out[nlen++] = *p;
        }

        p++;
    }

    name_out[nlen] = '\0';

    s = trimws(p);
    if (*s != '(') {
        return 0;
    }

    s++;

    /* args until ')' */
    int argc = 0;

    while (1) {
        s = trimws(s);

        if (*s == ')') {
            s++;
            break;
        }

        if (!is_ident_start(*s)) {
            return 0;
        }

        int alen = 0;

        while (*s && is_ident_char(*s)) {
            if (alen < STRING_BUF-1) {
                argnames[argc][alen++] = *s;
            }

            s++;
        }

        argnames[argc][alen] = '\0';
        argc++;

        if (argc > MAX_MACRO_ARGS) {
            asm_error(0, "Too many macro arguments in %s", name_out);
        }

        s = trimws(s);

        if (*s == ',') {
            s++;
            continue;
        }

        if (*s == ')') {
            s++;
            break;
        }

        return 0;
    }

    s = trimws(s);
    if (*s == '{') {
        *has_open_brace = 1;
    }

    *argc_out = argc;

    return 1;
}

/* Try to parse and define a macro given the header line already read.
 * Consumes lines from 'fin' until '}' or '.endmacro'.
 * Returns 1 if a macro was defined, 0 if header_line isn't a macro. */
static int macro_try_define(FILE *fin, const char *path, const char *header_line, int *lineno)
{
    char name[STRING_BUF];
    char args_buf[MAX_MACRO_ARGS][STRING_BUF];
    int argc = 0;
    int has_brace = 0;

    if (!macro_parse_header(header_line, name, args_buf, &argc, &has_brace)) {
        return 0;
    }

    macro_def_t m;
    memset(&m, 0, sizeof(m));
    m.name = strdup(name);
    m.argc = argc;

    for (int i = 0; i < argc; ++i) {
        m.argnames[i] = strdup(args_buf[i]);
    }

    /* If header has no opening '{', require next non-empty line to be '{' or start body until .endmacro */
    char linebuf[MAX_LINE_LEN];
    int seen_open = has_brace;

    while (fgets(linebuf, sizeof(linebuf), fin)) {
        (*lineno)++;

        /* Remove trailing newline */
        size_t L = strlen(linebuf);
        if (L && (linebuf[L-1] == '\n' || linebuf[L-1] == '\r')) {
            linebuf[L-1] = '\0';
        }

        char tmp[MAX_LINE_LEN];
        strncpy(tmp, linebuf, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        char *t = trimws(tmp);

        /* strip comments for control tokens but keep original in body */
        if (!seen_open) {
            if (*t == '{') {
                seen_open = 1;
                continue;
            }

            /* allow immediate body line if using .macro ... (no brace style) */
            seen_open = 1; /* start body now */
        }

        if (*t == '\0') {
            /* keep empty line */
            macro_body_push_line(&m, "");
            continue;
        }

        if (*t == '}' || strcasecmp(t, ".endmacro") == 0) {
            macro_register(&m);
            return 1;
        }

        macro_body_push_line(&m, linebuf);
    }

    asm_error_file(path, *lineno, "Unterminated .macro %s", name);

    return 0; /* not reached */
}

/* Replace identifiers in 'in' based on simple token table. tokens[i] -> repl[i], ntok entries. */
static char *replace_ident_tokens(const char *in, const char **tokens, const char **repl, int ntok)
{
    size_t cap = strlen(in) + 64;
    size_t len = 0;
    char *out = malloc(cap);
    const char *p = in;

    while (*p) {
        if (is_ident_start(*p)) {
            char id[STRING_BUF];
            int ilen = 0;
            const char *q = p;

            while (*q && is_ident_char(*q)) {
                if (ilen < (int)sizeof(id)-1) {
                    id[ilen++] = *q;
                }
                q++;
            }

            id[ilen] = '\0';
            int replaced = 0;

            for (int i = 0; i < ntok; ++i) {
                if (strcmp(id, tokens[i]) == 0) {
                    const char *r = repl[i];
                    size_t rl = strlen(r);

                    if (len + rl + 1 > cap) {
                        cap = (len + rl + 64)*2;
                        out = realloc(out, cap);
                    }

                    memcpy(out + len, r, rl);
                    len += rl;
                    out[len] = '\0';
                    p = q;
                    replaced = 1;
                    break;
                }
            }

            if (!replaced) {
                if (len + (size_t)ilen + 1 > cap) {
                    cap = (len + ilen + 64)*2;
                    out = realloc(out, cap);
                }

                memcpy(out + len, id, (size_t)ilen);
                len += (size_t)ilen;
                out[len] = '\0';
                p = q;
            }

            continue;
        }

        /* copy single char */
        if (len + 2 > cap) {
            cap = (len + 64)*2;
            out = realloc(out, cap);
        }

        out[len++] = *p++;
        out[len] = '\0';
    }

    return out;
}

/* Attempt to match and expand a macro invocation line, emitting expanded lines into lines[]
 * Returns 1 if expanded, 0 if not a macro call. */
static int macro_try_expand_and_emit(const char *path, const char *orig_line, int lineno)
{
    /* Work on a comment-free copy to detect calls; preserve original for parsing */
    char work[MAX_LINE_LEN];
    strncpy(work, orig_line, sizeof(work)-1);
    work[sizeof(work)-1] = '\0';

    strip_comments_preserving_quotes(work);

    char *s = trimws(work);

    if (!is_ident_start(*s)) {
        return 0;
    }

    /* name */
    char name[STRING_BUF];
    int nlen = 0;
    const char *p = s;

    while (*p && is_ident_char(*p)) {
        if (nlen < STRING_BUF-1) {
            name[nlen++] = *p;
        }
        p++;
    }

    name[nlen] = '\0';

    const macro_def_t *m = macro_find(name);
    if (!m) {
        return 0;
    }

    p = (const char *)trimws((char*)p);
    if (*p != '(') {
        return 0;
    }

    p++;

    /* parse args CSV respecting parentheses nesting (simple, no strings) */
    const char *argvals[MAX_MACRO_ARGS];
    char *argstore[MAX_MACRO_ARGS];
    int ac = 0;
    char token[MAX_LINE_LEN];
    int tlen = 0;
    int depth = 0;

    while (*p) {
        char c = *p++;

        if (c == '(') {
            depth++;
            token[tlen++] = c;
        } else if (c == ')') {
            if (depth == 0) {
                /* end of args */
                token[tlen] = '\0';
                char *v = strdup(trimws(token));

                if (ac < MAX_MACRO_ARGS) {
                    argstore[ac] = v;
                    argvals[ac] = v;
                }

                ac++;
                break;
            } else {
                depth--;
                token[tlen++] = c;
            }
        } else if (c == ',' && depth == 0) {
            token[tlen] = '\0';

            char *v = strdup(trimws(token));

            if (ac < MAX_MACRO_ARGS) {
                argstore[ac] = v;
                argvals[ac] = v;
            }

            ac++;
            tlen = 0;
            token[0] = '\0';
        } else {
            if (tlen < (int)sizeof(token)-1) {
                token[tlen++] = c;
            }
        }
    }

    if (ac != m->argc) {
        asm_error_file(path, lineno, "Macro %s expects %d args, got %d", m->name, m->argc, ac);
    }

    /* Build replacement tables: first local label rename, then arg substitution. */
    int uid = ++macro_expand_counter;
    const char *tok_keys[MAX_MACRO_ARGS + 64];
    const char *tok_vals[MAX_MACRO_ARGS + 64];
    int ntok = 0;

    /* local labels */
    char **ll_new = NULL;
    int ll_cnt = 0;

    if (m->local_count > 0) {
        ll_cnt = m->local_count;
        ll_new = malloc((size_t)ll_cnt * sizeof(char*));

        for (int i = 0; i < ll_cnt; ++i) {
            tok_keys[ntok] = m->local_labels[i];
            char buf[STRING_BUF];
            snprintf(buf, sizeof(buf), "__m%d_%s", uid, m->local_labels[i]);
            ll_new[i] = strdup(buf);
            tok_vals[ntok] = ll_new[i];
            ntok++;
        }
    }

    /* params */
    for (int i = 0; i < m->argc; ++i) {
        tok_keys[ntok] = m->argnames[i];
        tok_vals[ntok] = argvals[i < MAX_MACRO_ARGS ? i : 0];
        ntok++;
    }

    /* For each body line: apply replacements; then parse and push. */
    for (int i = 0; i < m->body_count; ++i) {
        char *repl = replace_ident_tokens(m->body[i], tok_keys, tok_vals, ntok);
        asm_line_t *ln = parse_line(repl, lineno);

        if (ln) {
            ln->filename = strdup(path);
            lines[line_count++] = ln;
            if (line_count >= MAX_LINES) {
                asm_error_file(path, lineno, "Too many lines in input (macro expansion)");
            }
        }

        free(repl);
    }

    /* cleanup */
    for (int i = 0; i < ll_cnt; ++i) {
        free(ll_new[i]);
    }

    free(ll_new);

    for (int i = 0; i < ac && i < MAX_MACRO_ARGS; ++i) {
        free(argstore[i]);
    }

    return 1;
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
/* ---- Helpers to flatten parse_operand ---- */
static int set_immediate_operand(const char *s, operand_t *op)
{
    if (*s != CHAR_HASH) {
        return 0;
    }

    const char *rest = s + 1;

    int v;

    op->mode = AM_IMMEDIATE;

    if (parse_number(rest, &v)) {
        op->value = v;
    } else {
        op->expr = strdup(rest);
    }

    return 1;
}

static int set_accumulator_operand(const char *s, operand_t *op)
{
    if (!(strcmp(s, "A") == 0 || strcmp(s, "a") == 0)) {
        return 0;
    }

    op->mode = AM_ACCUMULATOR;

    return 1;
}

static int set_indirect_operand(char *s, operand_t *op)
{
    if (*s != CHAR_LPAREN) {
        return 0;
    }

    char *rparen = strrchr(s, ')');
    if (!rparen) {
        asm_error(0, "Malformed indirect operand: %s", s);
    }

    *rparen = '\0';
    char *inside = s + 1;
    char *after = trimws(rparen + 1);

    /* (expr),Y */
    if (*after == ',') {
        char *p = trimws(after + 1);

        if (toupper((unsigned char)*p) == 'Y') {
            int v;
            op->mode = AM_INDIRECT_Y;

            if (parse_number(inside, &v)) {
                op->value = v;
            } else {
                op->expr = strdup(inside);
            }

            return 1;
        }
    }

    /* (expr,X) */
    char *comma_in = strchr(inside, CHAR_COMMA);
    if (comma_in) {
        *comma_in = '\0';
        char *base = trimws(inside);
        char *p = trimws(comma_in + 1);

        if (toupper((unsigned char)*p) == 'X') {
            int v;
            op->mode = AM_INDIRECT_X;

            if (parse_number(base, &v)) {
                op->value = v;
            } else {
                op->expr = strdup(base);
            }

            return 1;
        }
    }

    /* pure (expr) */
    int v;
    op->mode = AM_INDIRECT;
    if (parse_number(inside, &v)) {
        op->value = v;
    } else {
        op->expr = strdup(inside);
    }

    return 1;
}

static int set_indexed_operand(char *s, operand_t *op, const char *orig)
{
    char *comma = strchr(s, CHAR_COMMA);
    if (!comma) {
        return 0;
    }

    *comma = '\0';
    char *base = trimws(s);
    char *p = trimws(comma + 1);
    char reg = toupper((unsigned char)*p);

    int v;

    if (parse_number(base, &v)) {
        if (reg == 'X') {
            op->mode = (v <= BYTE_MAX) ? AM_ZEROPAGE_X : AM_ABSOLUTE_X;
            op->value = v;
        } else if (reg == 'Y') {
            op->mode = (v <= BYTE_MAX) ? AM_ZEROPAGE_Y : AM_ABSOLUTE_Y;
            op->value = v;
        } else {
            asm_error(0, "Unknown suffix register %c in %s", reg, orig);
        }
    } else {
        if (reg == 'X') {
            op->mode = AM_ABSOLUTE_X;
        } else if (reg == 'Y') {
            op->mode = AM_ABSOLUTE_Y;
        }

        op->expr = strdup(base);
    }

    return 1;
}

operand_t parse_operand(const char *s0)
{
    operand_t op;
    op.mode = AM_NONE;
    op.value = 0;
    op.expr = NULL;

    if (!s0) {
        return op;
    }

    char buf[STRING_BUF];
    strncpy(buf, s0, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *s = trimws(buf);
    if (*s == '\0') {
        return op;
    }

    if (set_immediate_operand(s, &op)) {
        return op;
    }

    if (set_accumulator_operand(s, &op)) {
        return op;
    }

    if (set_indirect_operand(s, &op)) {
        return op;
    }

    if (set_indexed_operand(s, &op, s0)) {
        return op;
    }

    /* Fallback: direct value or label */
    int v;
    if (parse_number(s, &v)) {
        op.mode = (v <= BYTE_MAX) ? AM_ZEROPAGE : AM_ABSOLUTE;
        op.value = v;
    } else {
        op.mode = AM_ABSOLUTE;
        op.expr = strdup(s);
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

/* Human-readable addressing mode for diagnostics */
static const char *addrmode_str(addrmode_t m)
{
    switch (m) {
        case AM_NONE:
            return "implicit";

        case AM_IMMEDIATE:
            return "immediate";

        case AM_ZEROPAGE:
            return "zeropage";

        case AM_ZEROPAGE_X:
            return "zeropage,X";

        case AM_ZEROPAGE_Y:
            return "zeropage,Y";

        case AM_ABSOLUTE:
            return "absolute";

        case AM_ABSOLUTE_X:
            return "absolute,X";

        case AM_ABSOLUTE_Y:
            return "absolute,Y";

        case AM_INDIRECT:
            return "(indirect)";

        case AM_INDIRECT_X:
            return "(indirect,X)";

        case AM_INDIRECT_Y:
            return "(indirect),Y";

        case AM_RELATIVE:
            return "relative";

        case AM_ACCUMULATOR:
            return "accumulator";

        default:
            return "unknown";
    }
}

/* ------------ Small helpers to simplify passes ------------ */
/* Resolve local temporary labels '-' (previous) and '+' (next) relative to a line index. */
/* s = "-", "--", ... or "+", "++", ... detection */
static int parse_local_ref(const char *s, char *kind, int *count)
{
    if (!s || !*s) {
        return 0;
    }

    char c = s[0];
    if (c != '-' && c != '+') {
        return 0;
    }

    int n = 0;

    for (const char *p = s; *p; ++p) {
        if (*p != c) {
            return 0;
        }
        n++;
    }

    if (n <= 0) {
        return 0;
    }

    if (kind) {
        *kind = c;
    }

    if (count) {
        *count = n;
    }

    return 1;
}

static int resolve_minus_addr_k(int line_index, int k, int *out_addr)
{
    for (int m = minus_count - 1; m >= 0; --m) {
        if (minus_idx[m] < line_index) {
            k--;

            if (k == 0) {
                if (out_addr) {
                    *out_addr = minus_pc[m];
                }

                return 1;
            }
        }
    }

    return 0; /* not enough previous '-' labels */
}

static int resolve_plus_addr_k(int line_index, int k, int *out_addr)
{
    for (int p = 0; p < plus_count; ++p) {
        if (plus_idx[p] > line_index) {
            k--;

            if (k == 0) {
                if (out_addr) {
                    *out_addr = plus_pc[p];
                }

                return 1;
            }
        }
    }

    return 0; /* not enough next '+' labels */
}

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

static dir_kind_t classify_directive(const asm_line_t *ln)
{
    if (!ln || !ln->mnemonic) {
        return DIR_NONE;
    }

    if (is_define(ln)) {
        return DIR_DEFINE;
    }

    if (is_org(ln)) {
        return DIR_ORG;
    }

    if (is_incbin(ln)) {
        return DIR_INCBIN;
    }

    if (is_byte_dir(ln)) {
        return DIR_BYTE;
    }

    if (is_word_dir(ln)) {
        return DIR_WORD;
    }

    if (is_text_dir(ln)) {
        return DIR_TEXT;
    }

    return DIR_NONE;
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
        if (v < 0 || v > WORD_MAX) {
            asm_error_file(file, lineno, "Origin out of 16-bit range: %d", v);
        }

        /* Warn if origin moves backwards, as this can overwrite prior bytes
         * and leave trailing data beyond the new segment end. */
        if (v < pc) {
            fprintf(stderr,
                    "Warning (%s:%d): .org moving PC backwards from $%04X to $%04X; bytes previously assembled beyond new origin will remain in output.\n",
                    file ? file : "<input>", lineno, pc & 0xFFFF, v & 0xFFFF);
        }

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

/* Extracts the payload of a .text directive as a newly allocated C string.
 * Caller must free the returned pointer. */
static char *extract_text_payload_alloc(const asm_line_t *ln)
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

    return strdup(t);
}

static void first_handle_text(const asm_line_t *ln, int *pc)
{
    char *text = extract_text_payload_alloc(ln);
    *pc += (int)strlen(text);
    free(text);
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
        asm_error_file(ln->filename, ln->lineno, "%s does not support %s addressing",
                ln->mnemonic, addrmode_str(ln->op.mode));
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
    /* reset +/- markers */
    minus_count = 0;
    plus_count = 0;

    for (int i = 0; i < line_count; i++) {
        asm_line_t *ln = lines[i];
        if (!ln) {
            continue;
        }

        if (ln->label) {
            /* Accept sequences of '-' or '+' as local temp labels too */
            int all_minus = 1;
            int all_plus = 1;
            for (const char *p = ln->label; *p; ++p) {
                if (*p != '-') {
                    all_minus = 0;
                }

                if (*p != '+') {
                    all_plus = 0;
                }
            }

            if (all_minus && *ln->label) {
                if (minus_count < MAX_LINES) {
                    minus_idx[minus_count] = i;
                    minus_pc[minus_count] = pc;
                    minus_count++;
                }
            } else if (all_plus && *ln->label) {
                if (plus_count < MAX_LINES) {
                    plus_idx[plus_count] = i;
                    plus_pc[plus_count] = pc;
                    plus_count++;
                }
            } else {
                DEBUG_PRINT("Found label: \"%s\" at 0x%04x\n", ln->label, pc);
                sym_add(ln->label, pc, ln->filename, ln->lineno);
            }
        }

        if (!ln->mnemonic) {
            continue;
        }

        switch (classify_directive(ln)) {
            case DIR_DEFINE:
                first_handle_define(ln, &pc);
                break;

            case DIR_ORG:
                first_handle_org(ln, &pc);
                break;

            case DIR_INCBIN:
                first_handle_incbin(ln, &pc);
                break;

            case DIR_BYTE:
                first_handle_byte(ln, &pc);
                break;

            case DIR_WORD:
                first_handle_word(ln, &pc);
                break;

            case DIR_TEXT:
                first_handle_text(ln, &pc);
                break;

            case DIR_NONE:
                first_handle_instruction(ln, &pc);
                break;
        }
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

/* ---- Helpers to flatten second pass emission ---- */
static int resolve_value_from_expr(const char *expr,
                                   int pc,
                                   int line_index,
                                   const char *file,
                                   int lineno,
                                   int *out)
{
    char kind;
    int n;

    if (parse_local_ref(expr, &kind, &n)) {
        int addr;

        if (kind == '-') {
            if (!resolve_minus_addr_k(line_index, n, &addr)) {
                asm_error_file(file, lineno, "No matching '-' label found");
            }
        } else {
            if (!resolve_plus_addr_k(line_index, n, &addr)) {
                asm_error_file(file, lineno, "No matching '+' label found");
            }
        }

        *out = addr;
        return 1;
    }

    g_eval_pc = pc;

    if (eval_expr(expr, out) || parse_number(expr, out)) {
        return 1;
    }

    int s = sym_lookup(expr);
    if (s < 0) {
        asm_error_file(file, lineno, "Undefined label %s", expr);
    }

    *out = s;

    return 1;
}

static int resolve_operand_value_for_line(const asm_line_t *ln,
                                          int pc,
                                          int line_index,
                                          int *out)
{
    if (ln->op.expr) {
        return resolve_value_from_expr(ln->op.expr, pc, line_index, ln->filename, ln->lineno, out);
    }

    *out = ln->op.value;

    return 1;
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

        if (!resolve_value_from_expr(t, *pc, current_line_index, ln->filename, ln->lineno, &v)) {
            asm_error_file(ln->filename, ln->lineno, "Failed to resolve .byte value");
        }

        if (v < 0 || v > BYTE_MAX) {
            asm_error_file(ln->filename, ln->lineno, ".byte value %d out of 8-bit range", v);
        }

        emit_byte(*pc, (uint8_t)v);
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

        if (!resolve_value_from_expr(t, *pc, current_line_index, ln->filename, ln->lineno, &v)) {
            asm_error_file(ln->filename, ln->lineno, "Failed to resolve .word value");
        }

        if (v < 0 || v > WORD_MAX) {
            asm_error_file(ln->filename, ln->lineno, ".word value %d out of 16-bit range", v);
        }

        emit_word(*pc, v);
        (*pc) += 2;
        tok = strtok(NULL, ",");
    }

    free(cp);
}

static void second_handle_text(const asm_line_t *ln, int *pc)
{
    char *text = extract_text_payload_alloc(ln);

    for (char *p = text; *p; ++p) {
        emit_byte(*pc, (uint8_t)(unsigned char)(*p));
        (*pc)++;
    }

    free(text);
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
        asm_error_file(ln->filename, ln->lineno, "%s does not support %s addressing",
                ln->mnemonic, addrmode_str(ln->op.mode));
    }

    emit_byte(*pc, op->opcode);

    switch (op->length) {
        case 1:
            (*pc) += 1;
            break;

        case 2: {
            int v;

            if (ln->op.mode == AM_RELATIVE) {
                if (!resolve_operand_value_for_line(ln, *pc, current_line_index, &v)) {
                    asm_error_file(ln->filename, ln->lineno, "Failed to resolve branch target");
                }

                int offset = v - (*pc + 2);
                if (offset < BRANCH_MIN || offset > BRANCH_MAX) {
                    asm_error_file(ln->filename, ln->lineno, "Branch offset out of range: %d", offset);
                }

                emit_byte(*pc + 1, (uint8_t)offset);
            } else {
                if (!resolve_operand_value_for_line(ln, *pc, current_line_index, &v)) {
                    asm_error_file(ln->filename, ln->lineno, "Failed to resolve operand");
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

            if (!resolve_operand_value_for_line(ln, *pc, current_line_index, &v)) {
                asm_error_file(ln->filename, ln->lineno, "Failed to resolve operand");
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
        current_line_index = i;

        if (!ln || !ln->mnemonic) {
            continue;
        }

        switch (classify_directive(ln)) {
            case DIR_DEFINE:
                /* no-op in second pass */
                break;

            case DIR_ORG:
                second_handle_org(ln, &pc);
                break;

            case DIR_INCBIN:
                second_handle_incbin(ln, &pc);
                break;

            case DIR_BYTE:
                second_handle_byte(ln, &pc);
                break;

            case DIR_WORD:
                second_handle_word(ln, &pc);
                break;

            case DIR_TEXT:
                second_handle_text(ln, &pc);
                break;

            case DIR_NONE:
                second_emit_instruction(ln, &pc);
                break;
        }
    }
}

/* ---------------- Source parsing ---------------- */

/* Helpers to parse one source line into asm_line_t */
static void strip_comments_preserving_quotes(char *s)
{
    int in_s = 0;
    int in_d = 0;

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

static int label_lhs_is_identifier(const char *s, size_t len)
{
    char leftbuf[STRING_BUF];

    if (len >= sizeof(leftbuf)) {
        len = sizeof(leftbuf) - 1;
    }

    memcpy(leftbuf, s, len); leftbuf[len] = '\0';

    char *lhs = trimws(leftbuf);
    if (*lhs == '\0') {
        return 0;
    }

    for (char *q = lhs; *q; ++q) {
        if (isspace((unsigned char)*q)) {
            return 0;
        }

        if (!(isalnum((unsigned char)*q) || *q == '_')) {
            return 0;
        }

    }

    return 1;
}

static char *find_valid_label_colon(char *s)
{
    int in_s = 0;
    int in_d = 0;

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
            if (label_lhs_is_identifier(s, (size_t)(p - s))) {
                return p;
            }

            break;
        }
    }

    return NULL;
}

static int is_directive_name(const char *t)
{
    return (strcasecmp(t, ".org") == 0) ||
           (strcasecmp(t, ".byte") == 0) ||
           (strcasecmp(t, ".word") == 0) ||
           (strcasecmp(t, ".text") == 0) ||
           (strcasecmp(t, ".incbin") == 0) ||
           (strcasecmp(t, ".include") == 0) ||
           (strcasecmp(t, ".macro") == 0) ||
           (strcasecmp(t, ".endmacro") == 0) ||
           (strcmp(t, "*") == 0);
}

static int try_parse_define_line(char *s, asm_line_t *ln)
{
    char *eq = strchr(s, '=');
    if (!eq) {
        return 0;
    }

    *eq = '\0';
    char *lhs = trimws(s);
    char *rhs = trimws(eq + 1);

    if (strcmp(lhs, "*") == 0) {
        ln->mnemonic = strdup("*");
        ln->extra = strdup(rhs);
        return 1;
    }

    int ok = (*lhs != '\0');

    for (char *p = lhs; ok && *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' )) {
            ok = 0;
        }
    }

    if (ok && (is_mnemonic_name(lhs) || is_directive_name(lhs))) {
        ok = 0;
    }

    if (ok) {
        ln->def_name = strdup(lhs);
        ln->def_expr = strdup(rhs);
        ln->mnemonic = strdup("=");
        return 1;
    }

    /* not a define: restore '=' and signal no-define */
    *eq = '=';
    return 0;
}

static int maybe_parse_bare_label(char **ps, asm_line_t *ln, int had_colon)
{
    if (had_colon) {
        return 0;
    }

    char *s = *ps;
    char *ws = s;

    while (*ws && !isspace((unsigned char)*ws)) {
        ws++;
    }

    size_t tok_len = (size_t)(ws - s);
    char first_tok[STRING_BUF];

    if (tok_len >= sizeof(first_tok)) {
        tok_len = sizeof(first_tok) - 1;
    }

    memcpy(first_tok, s, tok_len); first_tok[tok_len] = '\0';

    char *rest_after = (*ws) ? trimws(ws + 1) : ws;

    int is_single_token = (*rest_after == '\0');
    int is_dir = is_directive_name(first_tok);
    int is_mn = is_mnemonic_name(first_tok);

    if (!is_mn && !is_dir) {
        ln->label = strdup(first_tok);

        if (is_single_token) {
            *ps = s + strlen(s);
            return 1;
        }

        *ps = rest_after;
    }

    return 0;
}

asm_line_t *parse_line(const char *in, int lineno)
{
    char tmp[MAX_LINE_LEN];

    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *s = tmp;

    /* Strip comments starting at ';', but ignore semicolons inside quotes */
    strip_comments_preserving_quotes(s);

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
    char *colon = find_valid_label_colon(s);

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
    if (try_parse_define_line(s, ln)) {
        return ln;
    }

    /* Support label without colon if the entire line is a single token that is
     * NOT a known mnemonic or directive. */
    if (!colon) {
        if (maybe_parse_bare_label(&s, ln, 0)) {
            return ln; /* pure label */
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
    if (!path) {
        return;
    }

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
        /* remove trailing newline */
        size_t L = strlen(linebuf); if (L && (linebuf[L-1] == '\n' || linebuf[L-1] == '\r')) linebuf[L-1] = '\0';

        /* Macro definition? If so, it consumes its block and does not emit a line. */
        if (macro_try_define(fin, path, linebuf, &lineno)) {
            continue;
        }

        /* Macro invocation? Expand into lines immediately. */
        if (macro_try_expand_and_emit(path, linebuf, lineno)) {
            continue;
        }

        /* Normal line path */
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

    macro_init();
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
