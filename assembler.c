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
    int scope_depth; /* 0 = global, >0 = inside .block nesting */
    int is_label;    /* 1 if label (scoped), 0 if define (global) */
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
    DIR_TEXT,
    DIR_FILL,
    DIR_BLOCK_START,
    DIR_BLOCK_END
} dir_kind_t;

/* Globals */
static asm_line_t *lines[MAX_LINES];
static int line_count = 0;

static symbol_t symtab[MAX_SYMBOLS];
static int sym_count = 0;
/* Track lexical scope depth per source line and current scope during eval */
static int line_scope_depth[MAX_LINES];
static int current_scope_depth = 0;

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
char *trimws(char *str)
{
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }

    size_t len = strlen(str);
    if (len == 0) {
        return str;
    }

    char *end = str + len - 1;

    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return str;
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
static int macro_try_define(FILE *input_file, const char *source_path, const char *header_line_text, int *line_no)
{
    char macro_name[STRING_BUF];
    char argnames_buf[MAX_MACRO_ARGS][STRING_BUF];
    int argc = 0;
    int has_open_brace = 0;

    if (!macro_parse_header(header_line_text, macro_name, argnames_buf, &argc, &has_open_brace)) {
        return 0;
    }

    macro_def_t m;
    memset(&m, 0, sizeof(m));
    m.name = strdup(macro_name);
    m.argc = argc;

    for (int i = 0; i < argc; ++i) {
        m.argnames[i] = strdup(argnames_buf[i]);
    }

    /* If header has no opening '{', require next non-empty line to be '{' or start body until .endmacro */
    char linebuf[MAX_LINE_LEN];
    int seen_open = has_open_brace;

    while (fgets(linebuf, sizeof(linebuf), input_file)) {
        (*line_no)++;

        /* Remove trailing newline */
        size_t L = strlen(linebuf);
        if (L && (linebuf[L-1] == '\n' || linebuf[L-1] == '\r')) {
            linebuf[L-1] = '\0';
        }

        char tmp[MAX_LINE_LEN];
        strncpy(tmp, linebuf, sizeof(tmp)-1);
        tmp[sizeof(tmp)-1] = '\0';

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

    asm_error_file(source_path, *line_no, "Unterminated .macro %s", macro_name);

    return 0; /* not reached */
}

/* Replace identifiers in 'in' based on simple token table. tokens[i] -> repl[i], ntok entries. */
static char *replace_ident_tokens(const char *in_text, const char **tokens, const char **repl, int token_count)
{
    size_t cap = strlen(in_text) + 64;
    size_t len = 0;
    char *out = malloc(cap);
    const char *p = in_text;

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

            for (int i = 0; i < token_count; ++i) {
                if (strcmp(id, tokens[i]) == 0) {
                    const char *r = repl[i];
                    size_t rl = strlen(r);

                    if (len + rl + 1 > cap) {
                        cap = (len + rl + 64) * 2;
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
                    cap = (len + ilen + 64) * 2;
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
            cap = (len + 64) * 2;
            out = realloc(out, cap);
        }

        out[len++] = *p++;
        out[len] = '\0';
    }

    return out;
}

/* Attempt to match and expand a macro invocation line, emitting expanded lines into lines[]
 * Returns 1 if expanded, 0 if not a macro call. */
static int macro_try_expand_and_emit(const char *source_path, const char *original_line, int source_line)
{
    /* Work on a comment-free copy to detect calls; preserve original for parsing */
    char work[MAX_LINE_LEN];
    strncpy(work, original_line, sizeof(work)-1);
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
                char *trimmed = trimws(token);

                /* If there is nothing between parentheses, treat as zero args. */
                if (!(ac == 0 && *trimmed == '\0')) {
                    char *v = strdup(trimmed);
                    if (ac < MAX_MACRO_ARGS) {
                        argstore[ac] = v;
                        argvals[ac] = v;
                    }

                    ac++;
                }

                break;
            } else {
                depth--;
                token[tlen++] = c;
            }
        } else if (c == ',' && depth == 0) {
            token[tlen] = '\0';
            char *trimmed = trimws(token);

            /* Ignore empty tokens between commas (e.g., stray commas). */
            if (*trimmed != '\0') {
                char *v = strdup(trimmed);
                if (ac < MAX_MACRO_ARGS) {
                    argstore[ac] = v;
                    argvals[ac] = v;
                }

                ac++;
            }

            tlen = 0;
            token[0] = '\0';
        } else {
            if (tlen < (int)sizeof(token)-1) {
                token[tlen++] = c;
            }
        }
    }

    if (ac != m->argc) {
        asm_error_file(source_path, source_line, "Macro %s expects %d args, got %d", m->name, m->argc, ac);
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
        asm_line_t *ln = parse_line(repl, source_line);

        if (ln) {
            ln->filename = strdup(source_path);
            lines[line_count++] = ln;
            if (line_count >= MAX_LINES) {
                asm_error_file(source_path, source_line, "Too many lines in input (macro expansion)");
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
 * - Hex: '$1A2B' or '0x1A2B'
 * - Binary: '%1010' or '0b1010'
 * - Decimal: '1234'
 * Returns 1 on success (value in '*out'), otherwise 0.
 */
int parse_number(const char *str, int *out_value)
{
    if (!str || !*str) {
        return 0;
    }

    /* Binary: %1010 or 0b1010 */
    if (str[0] == '%') {
        char *end;
        long v = strtol(str + 1, &end, 2);

        if (*end != '\0') {
            return 0;
        }

        *out_value = (int)v;
        return 1;
    }

    if (str[0] == CHAR_DOLLAR) {
        char *end;
        long v = strtol(str + 1, &end, 16);

        if (*end != '\0') {
            return 0;
        }

        *out_value = (int)v;
        return 1;
    }

    /* Decimal fallback, allow leading 0x too */
    if (strncmp(str, "0x", 2) == 0 || strncmp(str, "0X", 2) == 0) {
        char *end;
        long v = strtol(str + 2, &end, 16);

        if (*end != '\0') {
            return 0;
        }

        *out_value = (int)v;
        return 1;
    }

    if (strncmp(str, "0b", 2) == 0 || strncmp(str, "0B", 2) == 0) {
        char *end;
        long v = strtol(str + 2, &end, 2);

        if (*end != '\0') {
            return 0;
        }

        *out_value = (int)v;
        return 1;
    }

    {
        char *end;
        long v = strtol(str, &end, 10);
        if (*end != '\0') {
            return 0;
        }

        *out_value = (int)v;
        return 1;
    }
}

/* Parse an operand string (e.g. "#$10", "$2000,X", "(zp),Y", label).
 * The goal here is to determine the addressing mode as much as possible
 * and either capture a literal value or keep the expression string to
 * resolve later in the second pass.
 */

static int set_immediate_operand(const char *text, operand_t *operand)
{
    if (*text != CHAR_HASH) {
        return 0;
    }

    const char *rest_after_hash = text + 1;

    int value;

    operand->mode = AM_IMMEDIATE;

    if (parse_number(rest_after_hash, &value)) {
        operand->value = value;
    } else {
        operand->expr = strdup(rest_after_hash);
    }

    return 1;
}

static int set_accumulator_operand(const char *text, operand_t *operand)
{
    if (!(strcmp(text, "A") == 0 || strcmp(text, "a") == 0)) {
        return 0;
    }

    operand->mode = AM_ACCUMULATOR;

    return 1;
}

static int set_indirect_operand(char *text, operand_t *operand)
{
    if (*text != CHAR_LPAREN) {
        return 0;
    }

    char *right_paren = strrchr(text, ')');
    if (!right_paren) {
        asm_error(0, "Malformed indirect operand: %s", text);
    }

    *right_paren = '\0';
    char *inside_parens = text + 1;
    char *after = trimws(right_paren + 1);

    /* (expr),Y */
    if (*after == ',') {
        char *reg_text = trimws(after + 1);

        if (toupper((unsigned char)*reg_text) == 'Y') {
            int value;
            operand->mode = AM_INDIRECT_Y;

            if (parse_number(inside_parens, &value)) {
                operand->value = value;
            } else {
                operand->expr = strdup(inside_parens);
            }

            return 1;
        }
    }

    /* (expr,X) */
    char *comma_inside = strchr(inside_parens, CHAR_COMMA);
    if (comma_inside) {
        *comma_inside = '\0';
        char *base = trimws(inside_parens);
        char *reg_text = trimws(comma_inside + 1);

        if (toupper((unsigned char)*reg_text) == 'X') {
            int value;
            operand->mode = AM_INDIRECT_X;

            if (parse_number(base, &value)) {
                operand->value = value;
            } else {
                operand->expr = strdup(base);
            }

            return 1;
        }
    }

    /* pure (expr) */
    int value;
    operand->mode = AM_INDIRECT;
    if (parse_number(inside_parens, &value)) {
        operand->value = value;
    } else {
        operand->expr = strdup(inside_parens);
    }

    return 1;
}

static int set_indexed_operand(char *text, operand_t *operand, const char *original_text)
{
    char *comma = strchr(text, CHAR_COMMA);
    if (!comma) {
        return 0;
    }

    *comma = '\0';
    char *base = trimws(text);
    char *reg_text = trimws(comma + 1);
    char reg = toupper((unsigned char)*reg_text);

    int value;

    if (parse_number(base, &value)) {
        if (reg == 'X') {
            operand->mode = (value <= BYTE_MAX) ? AM_ZEROPAGE_X : AM_ABSOLUTE_X;
            operand->value = value;
        } else if (reg == 'Y') {
            operand->mode = (value <= BYTE_MAX) ? AM_ZEROPAGE_Y : AM_ABSOLUTE_Y;
            operand->value = value;
        } else {
            asm_error(0, "Unknown suffix register %c in %s", reg, original_text);
        }
    } else {
        if (reg == 'X') {
            operand->mode = AM_ABSOLUTE_X;
        } else if (reg == 'Y') {
            operand->mode = AM_ABSOLUTE_Y;
        }

        operand->expr = strdup(base);
    }

    return 1;
}

operand_t parse_operand(const char *operand_text)
{
    operand_t operand;
    operand.mode = AM_NONE;
    operand.value = 0;
    operand.expr = NULL;

    if (!operand_text) {
        return operand;
    }

    char buf[STRING_BUF];
    strncpy(buf, operand_text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *trimmed = trimws(buf);
    if (*trimmed == '\0') {
        return operand;
    }

    if (set_immediate_operand(trimmed, &operand)) {
        return operand;
    }

    if (set_accumulator_operand(trimmed, &operand)) {
        return operand;
    }

    if (set_indirect_operand(trimmed, &operand)) {
        return operand;
    }

    if (set_indexed_operand(trimmed, &operand, operand_text)) {
        return operand;
    }

    /* Fallback: direct value or label */
    int value;
    if (parse_number(trimmed, &value)) {
        operand.mode = (value <= BYTE_MAX) ? AM_ZEROPAGE : AM_ABSOLUTE;
        operand.value = value;
    } else {
        operand.mode = AM_ABSOLUTE;
        operand.expr = strdup(trimmed);
    }

    return operand;
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
    /* Backward-compat: flat global lookup. */
    for (int i = 0; i < sym_count; i++) {
        if (strcmp(symtab[i].name, name) == 0) {
            return symtab[i].addr;
        }
    }

    return -1;
}

/* Scoped symbol lookup: prefer nearest (highest) scope <= depth; fall back to global defines */
static int sym_lookup_scoped(const char *symbol_name, int scope_depth_limit)
{
    int best_index = -1;
    int best_scope = -1;

    for (int i = 0; i < sym_count; i++) {
        if (strcmp(symtab[i].name, symbol_name) == 0 && symtab[i].is_label) {
            int symbol_scope = symtab[i].scope_depth;

            if (symbol_scope <= scope_depth_limit && symbol_scope >= 0) {
                if (symbol_scope > best_scope) {
                    best_scope = symbol_scope;
                    best_index = i;
                }
            }
        }
    }

    if (best_index >= 0) {
        return symtab[best_index].addr;
    }

    /* fall back to defines (treated as global) */
    for (int i = 0; i < sym_count; i++) {
        if (strcmp(symtab[i].name, symbol_name) == 0 && !symtab[i].is_label) {
            return symtab[i].addr;
        }
    }

    return -1;
}

/* Add a symbol with explicit scoping. For labels, duplicates are not allowed within the same scope.
 * For defines, duplicates are not allowed globally. */
static void sym_add_scoped(const char *name, int addr, int scope_depth_in, int is_label, const char *file, int lineno)
{
    if (is_label) {
        /* disallow duplicate label in same scope */
        for (int i = 0; i < sym_count; ++i) {
            if (symtab[i].is_label && symtab[i].scope_depth == scope_depth_in && strcmp(symtab[i].name, name) == 0) {
                asm_error_file(file, lineno, "Duplicate label in scope: %s", name);
            }
        }
    } else {
        /* define: must be unique by name (global semantics) */
        for (int i = 0; i < sym_count; ++i) {
            if (!symtab[i].is_label && strcmp(symtab[i].name, name) == 0) {
                asm_error_file(file, lineno, "Duplicate define: %s", name);
            }
        }
    }

    if (sym_count >= MAX_SYMBOLS) {
        asm_error_file(file, lineno, "Too many symbols");
    }

    symtab[sym_count].name = strdup(name);
    symtab[sym_count].addr = addr;
    symtab[sym_count].scope_depth = is_label ? scope_depth_in : 0;
    symtab[sym_count].is_label = is_label ? 1 : 0;
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

static const char *expr_skip_ws(const char *cursor)
{
    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    return cursor;
}

static int eval_parse_token(const char **pcur, char *token_buf, int token_bufsz)
{
    /* Parse a token that is not an operator or parenthesis. */
    const char *cursor = expr_skip_ws(*pcur);
    int out_len = 0;

    while (*cursor && !isspace((unsigned char)*cursor)) {
        char c = *cursor;
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
            break;
        }

        if (out_len < token_bufsz - 1) {
            token_buf[out_len++] = c;
        }

        cursor++;
    }

    token_buf[out_len] = '\0';
    *pcur = cursor;

    return out_len > 0;
}

static int eval_parse_factor(const char **pcur, int *out_value)
{
    const char *cursor = expr_skip_ws(*pcur);

    int sign = +1;

    if (*cursor == '+') {
        cursor++;
        cursor = expr_skip_ws(cursor);
    } else if (*cursor == '-') {
        sign = -1;
        cursor++;
        cursor = expr_skip_ws(cursor);
    }

    /* Low/high byte operators: '<expr' (lo), '>expr' (hi) */
    if (*cursor == '<' || *cursor == '>') {
        char op = *cursor++;
        int sub_value;

        if (!eval_parse_factor(&cursor, &sub_value)) {
            return 0;
        }

        int v = (op == '<') ? (sub_value & 0xFF) : ((sub_value >> 8) & 0xFF);
        *pcur = cursor;
        *out_value = sign * v;
        return 1;
    }

    if (*cursor == '(') {
        cursor++;

        if (!eval_parse_expr(&cursor, out_value)) {
            return 0;
        }

        cursor = expr_skip_ws(cursor);
        if (*cursor != ')') {
            return 0;
        }

        cursor++;
        *pcur = cursor;
        *out_value = sign * (*out_value);
        return 1;
    } else {
        /* Support current PC symbol '*' at factor position */
        if (*cursor == '*') {
            cursor++;
            *pcur = cursor;
            *out_value = sign * g_eval_pc;
            return 1;
        }

        char tok[STRING_BUF];
        if (!eval_parse_token(&cursor, tok, sizeof(tok))) {
            return 0;
        }

        int literal_value;
        if (parse_number(tok, &literal_value)) {
            *out_value = sign * literal_value;
            *pcur = cursor;
            return 1;
        }

        int symbol_addr = sym_lookup_scoped(tok, current_scope_depth);
        if (symbol_addr < 0) {
            return 0;
        }

        *out_value = sign * symbol_addr;
        *pcur = cursor;
        return 1;
    }
}

static int eval_parse_term(const char **pcur, int *out_value)
{
    if (!eval_parse_factor(pcur, out_value)) {
        return 0;
    }

    const char *cursor = expr_skip_ws(*pcur);

    while (*cursor == '*' || *cursor == '/') {
        char op = *cursor++;
        int rhs;

        if (!eval_parse_factor(&cursor, &rhs)) {
            return 0;
        }

        if (op == '*') {
            *out_value = (*out_value) * rhs;
        } else {
            if (rhs == 0) {
                return 0; /* division by zero */
            }

            *out_value = (*out_value) / rhs;
        }

        cursor = expr_skip_ws(cursor);
    }

    *pcur = cursor;

    return 1;
}

static int eval_parse_expr(const char **pcur, int *out_value)
{
    if (!eval_parse_term(pcur, out_value)) {
        return 0;
    }

    const char *cursor = expr_skip_ws(*pcur);

    while (*cursor == '+' || *cursor == '-') {
        char op = *cursor++;
        int rhs;

        if (!eval_parse_term(&cursor, &rhs)) {
            return 0;
        }

        if (op == '+') {
            *out_value = (*out_value) + rhs;
        } else {
            *out_value = (*out_value) - rhs;
        }

        cursor = expr_skip_ws(cursor);
    }

    *pcur = cursor;
    
    return 1;
}

int eval_expr(const char *expr, int *out_value) {
    const char *cursor = expr;

    if (!eval_parse_expr(&cursor, out_value)) {
        return 0;
    }

    cursor = expr_skip_ws(cursor);
    if (*cursor != '\0') {
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

static int is_fill_dir(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".fill") == 0));
}

static int is_block_start(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".block") == 0));
}

static int is_block_end(const asm_line_t *ln)
{
    return (ln->mnemonic && (strcasecmp(ln->mnemonic, ".bend") == 0));
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

    if (is_fill_dir(ln)) {
        return DIR_FILL;
    }

    if (is_block_start(ln)) {
        return DIR_BLOCK_START;
    }

    if (is_block_end(ln)) {
        return DIR_BLOCK_END;
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

static void first_handle_org(const asm_line_t *line, int *program_counter)
{
    *program_counter = parse_org_value_file(line->filename, line->extra, line->lineno, *program_counter);

    DEBUG_PRINT("ORG: 0x%04x\n", *program_counter);
}

static void first_handle_incbin(const asm_line_t *line, int *program_counter)
{
    if (!line->extra) {
        asm_error_file(line->filename, line->lineno, "Missing filename for .incbin");
    }

    char fnbuf[STRING_BUF];

    strncpy(fnbuf, line->extra, sizeof(fnbuf) - 1);
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
        asm_error_file(line->filename, line->lineno, "Cannot open .incbin file: %s", fn);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);

    DEBUG_PRINT(".incbin: %s at 0x%04x, len: %d\n", fn, *program_counter, (int)sz);

    /* Early size check to ensure included bytes will fit in output buffer. */
    if (*program_counter + (int)sz > MAX_OUTPUT) {
        asm_error_file(line->filename, line->lineno, ".incbin would exceed output buffer (size %ld at PC $%04X)", sz, *program_counter & 0xFFFF);
    }

    *program_counter += (int)sz;
}

static void first_handle_byte(const asm_line_t *line, int *program_counter)
{
    if (!line->extra) {
        asm_error_file(line->filename, line->lineno, "Missing data in .byte");
    }

    int count = count_csv_items(line->extra);
    if (*program_counter + count > MAX_OUTPUT) {
        asm_error_file(line->filename, line->lineno, ".byte would exceed output buffer (count %d at PC $%04X)", count, *program_counter & 0xFFFF);
    }

    *program_counter += count;
}

static void first_handle_word(const asm_line_t *line, int *program_counter)
{
    if (!line->extra) {
        asm_error_file(line->filename, line->lineno, "Missing data in .word");
    }

    int count = 2 * count_csv_items(line->extra);
    if (*program_counter + count > MAX_OUTPUT) {
        asm_error_file(line->filename, line->lineno, ".word would exceed output buffer (count %d at PC $%04X)", count, *program_counter & 0xFFFF);
    }

    *program_counter += count;
}

/* .fill <size>, <value> : advance PC by <size> in first pass */
static void first_handle_fill(const asm_line_t *line, int *program_counter)
{
    if (!line->extra) {
        asm_error_file(line->filename, line->lineno, "Missing arguments for .fill (expected: size, value)");
    }

    char buf[MAX_LINE_LEN];
    strncpy(buf, line->extra, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma) {
        asm_error_file(line->filename, line->lineno, "Malformed .fill, expected: .fill <size>, <value>");
    }

    *comma = '\0';
    char *size_tok = trimws(buf);
    int size_val;

    g_eval_pc = *program_counter;
    if (!(parse_number(size_tok, &size_val) || eval_expr(size_tok, &size_val))) {
        /* try symbol lookup of bare name */
        int s = sym_lookup_scoped(size_tok, current_scope_depth);
        if (s < 0) {
            asm_error_file(line->filename, line->lineno, "Unable to resolve .fill size: %s", line->extra);
        }
        size_val = s;
    }

    if (size_val < 0) {
        asm_error_file(line->filename, line->lineno, ".fill size must be non-negative: %d", size_val);
    }

    if (*program_counter + size_val > MAX_OUTPUT) {
        asm_error_file(line->filename, line->lineno, ".fill would exceed output buffer (size %d at PC $%04X)", size_val, *program_counter & 0xFFFF);
    }

    *program_counter += size_val;
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

static void first_handle_text(const asm_line_t *line, int *program_counter)
{
    char *text = extract_text_payload_alloc(line);
    *program_counter += (int)strlen(text);
    free(text);
}

static void first_handle_instruction(const asm_line_t *line, int *program_counter)
{
    const opcode_t *op = opcode_lookup(line->mnemonic, line->op.mode);

    /* Addressing mode fallback: if ZEROPAGE,Y is unsupported for this mnemonic, try ABSOLUTE,Y. */
    if (!op && line->op.mode == AM_ZEROPAGE_Y) {
        const opcode_t *alt = opcode_lookup(line->mnemonic, AM_ABSOLUTE_Y);
        if (alt) {
            /* Mutate the parsed mode so pass 2 uses the same */
            ((asm_line_t *)line)->op.mode = AM_ABSOLUTE_Y;
            op = alt;
        }
    }

    if (!op) {
        asm_error_file(line->filename, line->lineno, "%s does not support %s addressing",
                line->mnemonic, addrmode_str(line->op.mode));
    }

    *program_counter += op->length;
}

static void first_handle_define(const asm_line_t *line, int *program_counter)
{
    (void)program_counter; /* PC does not change for a define */

    if (!line->def_name || !line->def_expr) {
        asm_error_file(line->filename, line->lineno, "Malformed define");
    }

    int value;
    g_eval_pc = *program_counter;

    /* Accept an optional leading '#' as in immediate syntax (e.g., NAME = #$10) */
    const char *def_expr = line->def_expr;
    if (def_expr[0] == '#') {
        def_expr++; /* skip immediate marker */

        while (*def_expr && isspace((unsigned char)*def_expr)) {
            def_expr++;
        }
    }

    if (!(parse_number(def_expr, &value) || eval_expr(def_expr, &value))) {
        /* Try symbol lookup if it's a bare name */
        int s = sym_lookup_scoped(def_expr, current_scope_depth);
        if (s < 0) {
            asm_error_file(line->filename, line->lineno, "Bad expression in define: %s = %s",
                      line->def_name, line->def_expr);
        }
        value = s;
    }

    sym_add_scoped(line->def_name, value, 0, 0, line->filename, line->lineno);
}

/* FIRST PASS: Assign addresses (PC) and build symbol table. */
void first_pass(void)
{
    DEBUG_PRINT("First pass...\n");

    int pc = origin;
    /* reset +/- markers */
    minus_count = 0;
    plus_count = 0;
    /* reset scope tracking */
    memset(line_scope_depth, 0, sizeof(line_scope_depth));
    current_scope_depth = 0;
    int scope_depth = 0;

    for (int i = 0; i < line_count; i++) {
        asm_line_t *ln = lines[i];
        if (!ln) {
            continue;
        }

        /* Record scope depth for this source line to resolve labels consistently across passes */
        line_scope_depth[i] = scope_depth;
        current_scope_depth = scope_depth;

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
                DEBUG_PRINT("Found label: \"%s\" at 0x%04x (scope %d)\n", ln->label, pc, scope_depth);
                sym_add_scoped(ln->label, pc, scope_depth, 1, ln->filename, ln->lineno);
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

            case DIR_FILL:
                first_handle_fill(ln, &pc);
                break;

            case DIR_BLOCK_START:
                /* Enter a new lexical scope AFTER this line so labels on this line remain in the outer scope */
                scope_depth++;
                current_scope_depth = scope_depth;
                break;

            case DIR_BLOCK_END:
                if (scope_depth == 0) {
                    asm_error_file(ln->filename, ln->lineno, ".bend without matching .block");
                }

                /* Exit current lexical scope */
                scope_depth--;
                current_scope_depth = scope_depth;
                break;

            case DIR_NONE:
                first_handle_instruction(ln, &pc);
                break;
        }
    }

    /* Final sanity: ensure first pass PC never exceeded output buffer. */
    if (pc > MAX_OUTPUT) {
        asm_error(0, "Assembly size exceeds output buffer limit (%d bytes)", MAX_OUTPUT);
    }

    if (scope_depth != 0) {
        asm_error(0, "Unterminated .block (missing .bend)");
    }
}

/* Emit a single byte into the output buffer at absolute PC. */
void emit_byte(int program_counter, uint8_t byte_value)
{
    if (program_counter < 0 || program_counter >= MAX_OUTPUT) {
        asm_error(0, "Output PC out of range: %04X", program_counter);
    }

    outbuf[program_counter] = byte_value;

    if (program_counter < out_lo) {
        out_lo = program_counter;
    }

    if (program_counter > out_hi) {
        out_hi = program_counter;
    }

    DEBUG_PRINT("EMIT: 0x%04x : 0x%02x\n", program_counter, byte_value);
}

/* Emit a 16-bit little-endian word at absolute PC. */
void emit_word(int program_counter, int value)
{
    emit_byte(program_counter, LOBYTE(value));
    emit_byte(program_counter + 1, HIBYTE(value));
}

/* ---- Helpers to flatten second pass emission ---- */
static int resolve_value_from_expr(const char *expr_text,
                                   int program_counter,
                                   int line_index,
                                   const char *source_file,
                                   int source_line,
                                   int *out_value)
{
    char local_ref_kind;
    int local_ref_count;

    if (parse_local_ref(expr_text, &local_ref_kind, &local_ref_count)) {
        int resolved_addr;

        if (local_ref_kind == '-') {
            if (!resolve_minus_addr_k(line_index, local_ref_count, &resolved_addr)) {
                asm_error_file(source_file, source_line, "No matching '-' label found");
            }
        } else {
            if (!resolve_plus_addr_k(line_index, local_ref_count, &resolved_addr)) {
                asm_error_file(source_file, source_line, "No matching '+' label found");
            }
        }

        *out_value = resolved_addr;
        return 1;
    }

    g_eval_pc = program_counter;
    current_scope_depth = (line_index >= 0 && line_index < MAX_LINES) ? line_scope_depth[line_index] : 0;

    if (eval_expr(expr_text, out_value) || parse_number(expr_text, out_value)) {
        return 1;
    }

    int sym_addr = sym_lookup_scoped(expr_text, current_scope_depth);
    if (sym_addr < 0) {
        asm_error_file(source_file, source_line, "Undefined label %s", expr_text);
    }

    *out_value = sym_addr;

    return 1;
}

static int resolve_operand_value_for_line(const asm_line_t *line,
                                          int program_counter,
                                          int line_index,
                                          int *out_value)
{
    if (line->op.expr) {
        return resolve_value_from_expr(line->op.expr, program_counter, line_index, line->filename, line->lineno, out_value);
    }

    *out_value = line->op.value;

    return 1;
}

static void second_handle_org(const asm_line_t *line, int *program_counter)
{
    *program_counter = parse_org_value(line->extra, line->lineno, *program_counter);

    DEBUG_PRINT("ORG: 0x%04x\n", *program_counter);
}

static void second_handle_incbin(const asm_line_t *line, int *program_counter)
{
    char fnbuf[STRING_BUF];

    strncpy(fnbuf, line->extra ? line->extra : "", sizeof(fnbuf) - 1);
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
        asm_error_file(line->filename, line->lineno, "Cannot open .incbin file (second pass): %s", fn);
    }

    int c;

    DEBUG_PRINT(".incbin: %s at 0x%04x\n", fn, *program_counter);

    while ((c = fgetc(f)) != EOF) {
        emit_byte(*program_counter, (uint8_t)c);
        (*program_counter)++;
    }

    fclose(f);
}

static void second_handle_byte(const asm_line_t *line, int *program_counter)
{
    char *cp = strdup(line->extra ? line->extra : "");
    char *tok = strtok(cp, ",");

    while (tok) {
        char *t = trimws(tok);
        int value;

        if (!resolve_value_from_expr(t, *program_counter, current_line_index, line->filename, line->lineno, &value)) {
            asm_error_file(line->filename, line->lineno, "Failed to resolve .byte value");
        }

        if (value < 0 || value > BYTE_MAX) {
            asm_error_file(line->filename, line->lineno, ".byte value %d out of 8-bit range", value);
        }

        emit_byte(*program_counter, (uint8_t)value);
        (*program_counter)++;
        tok = strtok(NULL, ",");
    }

    free(cp);
}

static void second_handle_word(const asm_line_t *line, int *program_counter)
{
    char *cp = strdup(line->extra ? line->extra : "");
    char *tok = strtok(cp, ",");

    while (tok) {
        char *t = trimws(tok);
        int value;

        if (!resolve_value_from_expr(t, *program_counter, current_line_index, line->filename, line->lineno, &value)) {
            asm_error_file(line->filename, line->lineno, "Failed to resolve .word value");
        }

        if (value < 0 || value > WORD_MAX) {
            asm_error_file(line->filename, line->lineno, ".word value %d out of 16-bit range", value);
        }

        emit_word(*program_counter, value);
        (*program_counter) += 2;
        tok = strtok(NULL, ",");
    }

    free(cp);
}

static void second_handle_text(const asm_line_t *line, int *program_counter)
{
    char *text = extract_text_payload_alloc(line);

    for (char *p = text; *p; ++p) {
        emit_byte(*program_counter, (uint8_t)(unsigned char)(*p));
        (*program_counter)++;
    }

    free(text);
}

/* .fill <size>, <value> : emit <size> bytes of <value> */
static void second_handle_fill(const asm_line_t *line, int *program_counter)
{
    if (!line->extra) {
        asm_error_file(line->filename, line->lineno, "Missing arguments for .fill (expected: size, value)");
    }

    char buf[MAX_LINE_LEN];
    strncpy(buf, line->extra, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma) {
        asm_error_file(line->filename, line->lineno, "Malformed .fill, expected: .fill <size>, <value>");
    }

    *comma = '\0';
    char *size_tok = trimws(buf);
    char *val_tok = trimws(comma + 1);

    int size_val;
    int value;

    /* Resolve size via expression parser; require non-negative */
    g_eval_pc = *program_counter;
    if (!(parse_number(size_tok, &size_val) || eval_expr(size_tok, &size_val))) {
        if (!resolve_value_from_expr(size_tok, *program_counter, current_line_index, line->filename, line->lineno, &size_val)) {
            asm_error_file(line->filename, line->lineno, "Unable to resolve .fill size: %s", line->extra);
        }
    }

    if (size_val < 0) {
        asm_error_file(line->filename, line->lineno, ".fill size must be non-negative: %d", size_val);
    }

    /* Resolve fill byte value */
    if (!resolve_value_from_expr(val_tok, *program_counter, current_line_index, line->filename, line->lineno, &value)) {
        asm_error_file(line->filename, line->lineno, "Unable to resolve .fill value: %s", line->extra);
    }

    if (value < 0 || value > BYTE_MAX) {
        asm_error_file(line->filename, line->lineno, ".fill value %d out of 8-bit range", value);
    }

    for (int i = 0; i < size_val; ++i) {
        emit_byte(*program_counter, (uint8_t)value);
        (*program_counter)++;
    }
}

static void second_emit_instruction(const asm_line_t *line, int *program_counter)
{
    const opcode_t *op = opcode_lookup(line->mnemonic, line->op.mode);

    /* Addressing mode fallback: if ZEROPAGE,Y is unsupported for this mnemonic, try ABSOLUTE,Y. */
    if (!op && line->op.mode == AM_ZEROPAGE_Y) {
        const opcode_t *alt = opcode_lookup(line->mnemonic, AM_ABSOLUTE_Y);

        if (alt) {
            ((asm_line_t *)line)->op.mode = AM_ABSOLUTE_Y;
            op = alt;
        }
    }

    if (!op) {
        asm_error_file(line->filename, line->lineno, "%s does not support %s addressing",
                line->mnemonic, addrmode_str(line->op.mode));
    }

    emit_byte(*program_counter, op->opcode);

    switch (op->length) {
        case 1:
            (*program_counter) += 1;
            break;

        case 2: {
            int value;

            if (line->op.mode == AM_RELATIVE) {
                if (!resolve_operand_value_for_line(line, *program_counter, current_line_index, &value)) {
                    asm_error_file(line->filename, line->lineno, "Failed to resolve branch target");
                }

                int offset = value - (*program_counter + 2);
                if (offset < BRANCH_MIN || offset > BRANCH_MAX) {
                    asm_error_file(line->filename, line->lineno, "Branch offset out of range: %d", offset);
                }

                emit_byte(*program_counter + 1, (uint8_t)offset);
            } else {
                if (!resolve_operand_value_for_line(line, *program_counter, current_line_index, &value)) {
                    asm_error_file(line->filename, line->lineno, "Failed to resolve operand");
                }

                if (value < 0 || value > BYTE_MAX) {
                    asm_error_file(line->filename, line->lineno, "8-bit operand %d out of range", value);
                }

                emit_byte(*program_counter + 1, LOBYTE(value));
            }

            (*program_counter) += 2;
            break;
        }

        case 3: {
            int value;

            if (!resolve_operand_value_for_line(line, *program_counter, current_line_index, &value)) {
                asm_error_file(line->filename, line->lineno, "Failed to resolve operand");
            }

            if (value < 0 || value > WORD_MAX) {
                asm_error_file(line->filename, line->lineno, "16-bit operand %d out of range", value);
            }

            emit_word(*program_counter + 1, value);
            (*program_counter) += 3;
            break;
        }

        default:
            asm_error_file(line->filename, line->lineno, "Invalid opcode length %d", op->length);
    }
}

/* SECOND PASS: Generate final machine code */
void second_pass(void)
{
    DEBUG_PRINT("Second pass...\n");

    int program_counter = origin;

    for (int i = 0; i < line_count; i++) {
        asm_line_t *line = lines[i];
        current_line_index = i;
        current_scope_depth = (i >= 0 && i < MAX_LINES) ? line_scope_depth[i] : 0;

        if (!line || !line->mnemonic) {
            continue;
        }

        switch (classify_directive(line)) {
            case DIR_DEFINE:
                /* no-op in second pass */
                break;

            case DIR_ORG:
                second_handle_org(line, &program_counter);
                break;

            case DIR_INCBIN:
                second_handle_incbin(line, &program_counter);
                break;

            case DIR_BYTE:
                second_handle_byte(line, &program_counter);
                break;

            case DIR_WORD:
                second_handle_word(line, &program_counter);
                break;

            case DIR_TEXT:
                second_handle_text(line, &program_counter);
                break;

            case DIR_NONE:
                second_emit_instruction(line, &program_counter);
                break;

            case DIR_FILL:
                second_handle_fill(line, &program_counter);
                break;

            case DIR_BLOCK_START:
            case DIR_BLOCK_END:
                /* purely scoping; no effect in second pass */
                break;
        }
    }
}

/* ---------------- Source parsing ---------------- */

/* Helpers to parse one source line into asm_line_t */
static void strip_comments_preserving_quotes(char *line)
{
    int in_s = 0;
    int in_d = 0;

    for (char *p = line; *p; ++p) {
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

static int label_lhs_is_identifier(const char *text, size_t len)
{
    char leftbuf[STRING_BUF];

    if (len >= sizeof(leftbuf)) {
        len = sizeof(leftbuf) - 1;
    }

    memcpy(leftbuf, text, len); leftbuf[len] = '\0';

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

static char *find_valid_label_colon(char *line)
{
    int in_s = 0;
    int in_d = 0;

    for (char *p = line; *p; ++p) {
        if (*p == '\'' && !in_d) {
            in_s = !in_s;
            continue;
        }

        if (*p == '"' && !in_s) {
            in_d = !in_d;
            continue;
        }

        if (!in_s && !in_d && *p == ':') {
            if (label_lhs_is_identifier(line, (size_t)(p - line))) {
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
           (strcasecmp(t, ".fill") == 0) ||
           (strcasecmp(t, ".incbin") == 0) ||
           (strcasecmp(t, ".block") == 0) ||
           (strcasecmp(t, ".bend") == 0) ||
           (strcasecmp(t, ".include") == 0) ||
           (strcasecmp(t, ".macro") == 0) ||
           (strcasecmp(t, ".endmacro") == 0) ||
           (strcmp(t, "*") == 0);
}

static int try_parse_define_line(char *line_text, asm_line_t *line)
{
    char *eq = strchr(line_text, '=');
    if (!eq) {
        return 0;
    }

    *eq = '\0';
    char *lhs = trimws(line_text);
    char *rhs = trimws(eq + 1);

    if (strcmp(lhs, "*") == 0) {
        line->mnemonic = strdup("*");
        line->extra = strdup(rhs);
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
        line->def_name = strdup(lhs);
        line->def_expr = strdup(rhs);
        line->mnemonic = strdup("=");
        return 1;
    }

    /* not a define: restore '=' and signal no-define */
    *eq = '=';
    return 0;
}

static int maybe_parse_bare_label(char **pline_text, asm_line_t *line, int had_colon)
{
    if (had_colon) {
        return 0;
    }

    char *s = *pline_text;
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
        line->label = strdup(first_tok);

        if (is_single_token) {
            *pline_text = s + strlen(s);
            return 1;
        }

        *pline_text = rest_after;
    }

    return 0;
}

asm_line_t *parse_line(const char *input_line, int lineno)
{
    char line_buf[MAX_LINE_LEN];

    strncpy(line_buf, input_line, sizeof(line_buf) - 1);
    line_buf[sizeof(line_buf) - 1] = '\0';
    char *line_text = line_buf;

    /* Strip comments starting at ';', but ignore semicolons inside quotes */
    strip_comments_preserving_quotes(line_text);

    line_text = trimws(line_text);
    if (*line_text == '\0') {
        return NULL; /* blank or comment-only line */
    }

    asm_line_t *line = calloc(1, sizeof(asm_line_t));
    line->lineno = lineno;
    line->filename = NULL;
    line->label = NULL;
    line->mnemonic = NULL;
    line->op.mode = AM_NONE;
    line->op.expr = NULL;
    line->extra = NULL;
    line->def_name = NULL;
    line->def_expr = NULL;

    /* Label? Find a ':' that is outside quotes and precedes whitespace */
    char *colon_pos = find_valid_label_colon(line_text);

    if (colon_pos) {
        *colon_pos = '\0';
        line->label = strdup(trimws(line_text));
        line_text = trimws(colon_pos + 1);
    }

    if (*line_text == '\0') {
        /* A pure label line like "start:" */
        return line;
    }

    /* Detect constant define: NAME = EXPR (excluding "* = expr" which is .org) */
    if (try_parse_define_line(line_text, line)) {
        return line;
    }

    /* Support label without colon if the entire line is a single token that is
     * NOT a known mnemonic or directive. */
    if (!colon_pos) {
        if (maybe_parse_bare_label(&line_text, line, 0)) {
            return line; /* pure label */
        }
    }

    /* mnemonic / directive */
    char *mnemonic = strtok(line_text, " \t");
    line->mnemonic = strdup(mnemonic);

    char *rest = strtok(NULL, "");
    if (rest) {
        rest = trimws(rest);
    }

    /* directives */
    if ((strcasecmp(line->mnemonic, ".org") == 0) ||
        (strcmp(line->mnemonic, "*") == 0))
    {
        line->extra = rest ? strdup(rest) : NULL;
        return line;
    }

    if ((strcasecmp(line->mnemonic, ".incbin") == 0)) {
        line->extra = rest ? strdup(rest) : NULL;
        return line;
    }

    if ((strcasecmp(line->mnemonic, ".include") == 0))
    {
        line->extra = rest ? strdup(rest) : NULL;
        return line;
    }

    if ((strcasecmp(line->mnemonic, ".byte") == 0) ||
        (strcasecmp(line->mnemonic, ".word") == 0) ||
        (strcasecmp(line->mnemonic, ".text") == 0) ||
        (strcasecmp(line->mnemonic, ".fill") == 0))
    {
        line->extra = rest ? strdup(rest) : NULL;
        return line;
    }

    /* else: instruction */
    line->op = parse_operand(rest);

    /* If no operand provided, but the mnemonic supports accumulator addressing,
     * default to AM_ACCUMULATOR to allow forms like "ASL"/"LSR"/"ROL"/"ROR". */
    if ((!rest || *rest == '\0') && line->op.mode == AM_NONE) {
        const opcode_t *acc = opcode_lookup(line->mnemonic, AM_ACCUMULATOR);
        if (acc) {
            line->op.mode = AM_ACCUMULATOR;
        }
    }

    /* Branch mnemonics always use relative addressing */
    if (line->mnemonic &&
            (strcasecmp(line->mnemonic, "bcc") == 0 ||
             strcasecmp(line->mnemonic, "bcs") == 0 ||
             strcasecmp(line->mnemonic, "beq") == 0 ||
             strcasecmp(line->mnemonic, "bmi") == 0 ||
             strcasecmp(line->mnemonic, "bne") == 0 ||
             strcasecmp(line->mnemonic, "bpl") == 0 ||
             strcasecmp(line->mnemonic, "bvc") == 0 ||
             strcasecmp(line->mnemonic, "bvs") == 0))
    {
        line->op.mode = AM_RELATIVE;
    }

    return line;
}

/* ---------------- Include expansion loader ---------------- */

static void free_line(asm_line_t *line)
{
    if (!line) {
        return;
    }

    if (line->filename) {
        free(line->filename);
    }

    if (line->label) {
        free(line->label);
    }

    if (line->mnemonic) {
        free(line->mnemonic);
    }

    if (line->op.expr) {
        free(line->op.expr);
    }

    if (line->extra) {
        free(line->extra);
    }

    if (line->def_name) {
        free(line->def_name);
    }

    if (line->def_expr) {
        free(line->def_expr);
    }

    free(line);
}

static void expand_include(const char *including_path, const char *argument, int lineno)
{
    if (!argument) {
        asm_error_file(including_path, lineno, "Missing filename for .include");
    }

    char fnbuf[STRING_BUF];

    strncpy(fnbuf, argument, sizeof(fnbuf) - 1);
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

/* Read a source file and expand .include and macros into the global lines[]. */
static void read_file_with_includes(const char *input_path)
{
    if (!input_path) {
        return;
    }

    if (include_depth >= MAX_INCLUDE_DEPTH) {
        fprintf(stderr, "Error: include nesting too deep while opening %s\n", input_path);
        exit(1);
    }

    FILE *input_file = fopen(input_path, "r");
    if (!input_file) {
        fprintf(stderr, "Error (open %s): %s\n", input_path, strerror(errno));
        exit(1);
    }

    include_depth++;

    char line_buf[MAX_LINE_LEN];
    int line_no = 0;

    while (fgets(line_buf, sizeof(line_buf), input_file)) {
        line_no++;
        /* remove trailing newline */
        size_t L = strlen(line_buf);
        if (L && (line_buf[L-1] == '\n' || line_buf[L-1] == '\r')) {
            line_buf[L-1] = '\0';
        }

        /* Macro definition? If so, it consumes its block and does not emit a line. */
        if (macro_try_define(input_file, input_path, line_buf, &line_no)) {
            continue;
        }

        /* Macro invocation? Expand into lines immediately. */
        if (macro_try_expand_and_emit(input_path, line_buf, line_no)) {
            continue;
        }

        /* Normal line path */
        asm_line_t *ln = parse_line(line_buf, line_no);
        if (!ln) {
            continue;
        }

        /* attach source filename */
        ln->filename = strdup(input_path);

        /* Expand include directives inline */
        if (ln->mnemonic &&
            (strcasecmp(ln->mnemonic, ".include") == 0))
        {
            expand_include(input_path, ln->extra, line_no);
            free_line(ln);
            continue;
        }

        lines[line_count++] = ln;

        if (line_count >= MAX_LINES) {
            asm_error_file(input_path, line_no, "Too many lines in input");
        }
    }

    fclose(input_file);
    include_depth--;
}

/* Main */
/* Program entry: parse CLI, read file, assemble (two-pass), write output. */
/* Comparator for qsort: compare symbol indices by
 * 1) address, 2) type (labels before defines), 3) name. */
static int compare_symbol_indices_by_addr_type_name(const void *a, const void *b)
{
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    int addr_a = symtab[ia].addr & 0xFFFF;
    int addr_b = symtab[ib].addr & 0xFFFF;

    if (addr_a != addr_b) {
        return (addr_a < addr_b) ? -1 : 1;
    }
    /* At same address, order labels before defines */
    int is_label_a = symtab[ia].is_label ? 1 : 0;
    int is_label_b = symtab[ib].is_label ? 1 : 0;
    if (is_label_a != is_label_b) {
        return (is_label_a > is_label_b) ? -1 : 1; /* label (1) comes before define (0) */
    }

    return strcmp(symtab[ia].name, symtab[ib].name);
}

int main(int argc, char **argv)
{
    int write_header = 1; /* default: write 2-byte load address */
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *map_path = NULL; /* optional symbol map output */

    /* Parse flags */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--prg-header") == 0 || strcmp(a, "-H") == 0) {
            write_header = 1;
        } else if (strcmp(a, "--no-prg-header") == 0 || strcmp(a, "-N") == 0) {
            write_header = 0;
        } else if (strcmp(a, "--illegal-opcodes") == 0 || strcmp(a, "-I") == 0) {
            allow_illegal = 1;
        } else if (strcmp(a, "--map") == 0 || strcmp(a, "-M") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option %s requires a file path\n", a);
                return 1;
            }
            map_path = argv[++i];
        } else if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            fprintf(stderr,
                    "Usage: %s [--prg-header|-H] [--no-prg-header|-N] "
                    "[--illegal-opcodes|-I] [--map <file>|-M <file>] "
                    "input.asm output.bin\n",
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
                    "[--illegal-opcodes|-I] [--map <file>|-M <file>] "
                    "input.asm output.bin\n",
                    argv[0]);
            return 1;
        }
    }

    if (!in_path || !out_path) {
        fprintf(stderr,
                "Usage: %s [--prg-header|-H] [--no-prg-header|-N] "
                "[--illegal-opcodes|-I] [--map <file>|-M <file>] "
                "input.asm output.bin\n",
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

    /* Optional: emit symbol map file if requested. */
    if (map_path) {
        /* Build an index array and sort by address, then by name for stability. */
        int *sorted_indices = malloc(sizeof(int) * (size_t)sym_count);
        if (!sorted_indices) {
            perror("malloc map indices");
            return 1;
        }
        for (int sym_i = 0; sym_i < sym_count; ++sym_i) {
            sorted_indices[sym_i] = sym_i;
        }

        qsort(sorted_indices, (size_t)sym_count, sizeof(int), compare_symbol_indices_by_addr_type_name);

        FILE *map_file = fopen(map_path, "w");
        if (!map_file) {
            fprintf(stderr, "Error: cannot open map file %s: %s\n", map_path, strerror(errno));
            free(sorted_indices);
            return 1;
        }

        /* Header: describe columns */
        fprintf(map_file, "; $ADDR TYPE SCOPE NAME\n");
        /* Format: $ADDR TYPE SCOPE NAME  (TYPE is 'label' or 'define'; SCOPE is scope depth) */
        for (int out_i = 0; out_i < sym_count; ++out_i) {
            int sym_index = sorted_indices[out_i];
            const char *type_str = symtab[sym_index].is_label ? "label" : "define";
            int scope_depth_for_map = symtab[sym_index].is_label ? symtab[sym_index].scope_depth : 0;
            fprintf(map_file, "$%04X %s %d %s\n",
                    symtab[sym_index].addr & 0xFFFF,
                    type_str,
                    scope_depth_for_map,
                    symtab[sym_index].name);
        }

        fclose(map_file);
        free(sorted_indices);
        printf("Wrote symbol map: %s\n", map_path);
    }

    return 0;
}
