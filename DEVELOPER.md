Developer Notes: 6502 Assembler

Overview
- Two-pass assembler that expands includes and macros into a flat list of parsed lines, then runs a first pass to compute addresses and a second pass to emit bytes.

Core Data Structures
- `asm_line_t`: Parsed source line with optional `label`, `mnemonic`/directive, `operand` (mode/value or expression), and `extra` text for directives. Also carries `filename` and `lineno` for diagnostics. Defines use `def_name` and `def_expr`.
- `operand_t`: Addressing `mode`, immediate `value` if literal, or `expr` string to resolve in pass 2.
- `symbol_t`: Symbol table entry with `name`, `addr`, `scope_depth`, and `is_label` flag (defines are global, labels may be scoped by `.block`).
- `macro_def_t`: Macro definition with `name`, parameters, raw body lines, and detected local labels for safe renaming.

Pipeline
1) Load: `read_file_with_includes()` reads the main source, inlines `.include`, captures `.macro ... }` definitions, and expands macro invocations immediately into the global `lines[]` vector.
2) Parse: `parse_line()` strips comments, extracts optional `label:`, distinguishes directives from mnemonics, and parses the operand into an addressing mode or a deferred expression.
3) First Pass: `first_pass()` walks `lines[]` to compute the program counter (PC), sizes of directives, and instruction lengths, building the symbol table. It also records `line_scope_depth[]` and positions of temporary labels `-` and `+`.
4) Second Pass: `second_pass()` emits machine code. It resolves expressions and labels (respecting scope and local `-`/`+` references), enforces range checks, and writes bytes into `outbuf`.

Expressions
- `eval_expr()` supports decimal/hex/binary numbers, labels, current PC `*`, unary `<`/`>` for low/high byte, and `+ - * /` with parentheses. Scope resolution prefers the nearest label at or above the current `line_scope_depth`.

Scoping
- `.block` / `.bend` push/pop a lexical scope. Labels defined inside a block are looked up preferentially within the most nested visible scope; global defines remain visible everywhere.

Temporary Labels
- Sequences of only `-` or `+` on a label line mark addresses used by later references consisting solely of runs of `-` or `+`. Counts select the nth previous/next occurrence.

Directives
- `.org` / `* =`: Set PC (warn when moving backwards). `.incbin`: include file bytes. `.byte`/`.word`/`.text`/`.fill`: emit data. `.include`: inline source file. `NAME = expr`: define constant.

Illegal Opcodes
- Optional via `--illegal-opcodes` CLI flag; lookup is guarded inside `opcode_lookup()`.

Notes for Contributors
- Prefer descriptive local and parameter names (e.g., `program_counter`, `source_path`, `line_text`) to ease understanding.
- Keep pass 1 side-effect free regarding emission—only compute sizes and symbols. Restrict byte emission to pass 2.
- When extending directives, add classification in `classify_directive()`, pass 1 size accounting, and pass 2 emission.

