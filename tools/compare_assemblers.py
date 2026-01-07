#!/usr/bin/env python3
import os
import sys
import subprocess
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN_C = ROOT / 'assembler'
PY = sys.executable or 'python3'
PY_ASM = ROOT / 'assembler_py.py'
BUILD = ROOT / 'build' / 'compare'


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, p.stderr


def ensure_tools():
    if not BIN_C.exists():
        print("C assembler not found. Run 'make' first.")
        sys.exit(2)
    if not PY_ASM.exists():
        print("Python assembler 'assembler_py.py' not found.")
        sys.exit(2)


def read_bytes(path: Path) -> bytes:
    with open(path, 'rb') as f:
        return f.read()


def compare_files(a: Path, b: Path) -> bool:
    ba = read_bytes(a)
    bb = read_bytes(b)
    if ba == bb:
        return True
    # Show small diff summary
    print(f"Mismatch: {a} vs {b}")
    # Find first differing offset
    n = min(len(ba), len(bb))
    off = next((i for i in range(n) if ba[i] != bb[i]), None)
    if off is None:
        off = n
    print(f"  Lengths: {len(ba)} vs {len(bb)}; first diff at offset {off}")
    return False


def compare_case(name: str, extra_args_c, extra_args_py, src: Path, with_map=False) -> bool:
    ok = True
    out_c = BUILD / f"{name}_c.bin"
    out_p = BUILD / f"{name}_py.bin"

    cmd_c = [str(BIN_C), *extra_args_c, str(src), str(out_c)]
    rc, out, err = run(cmd_c)
    if rc != 0:
        print(f"[C FAIL] {name}: rc={rc}\n{err}")
        return False

    cmd_p = [PY, str(PY_ASM), *extra_args_py, str(src), str(out_p)]
    rc, out, err = run(cmd_p)
    if rc != 0:
        print(f"[PY FAIL] {name}: rc={rc}\n{err}")
        return False

    if not compare_files(out_c, out_p):
        ok = False

    if with_map:
        map_c = BUILD / f"{name}_c.map"
        map_p = BUILD / f"{name}_py.map"
        # Re-run to produce maps
        cmd_c_map = [str(BIN_C), *extra_args_c, '--map', str(map_c), str(src), str(out_c)]
        rc, _, err = run(cmd_c_map)
        if rc != 0:
            print(f"[C MAP FAIL] {name}: rc={rc}\n{err}")
            ok = False
        cmd_p_map = [PY, str(PY_ASM), *extra_args_py, '--map', str(map_p), str(src), str(out_p)]
        rc, _, err = run(cmd_p_map)
        if rc != 0:
            print(f"[PY MAP FAIL] {name}: rc={rc}\n{err}")
            ok = False
        if ok:
            # Compare map text exactly
            if map_c.read_text() != map_p.read_text():
                print(f"Mismatch: {map_c} vs {map_p}")
                ok = False

    print(f"[{name}] => {'OK' if ok else 'DIFF'}")
    return ok


def main() -> int:
    ensure_tools()
    BUILD.mkdir(parents=True, exist_ok=True)

    tests = []
    tests.append((
        'test_default_header',
        [],
        [],
        ROOT / 'test.asm',
        True
    ))
    tests.append((
        'test_raw',
        ['--no-prg-header'],
        ['--no-prg-header'],
        ROOT / 'test.asm',
        False
    ))
    tests.append((
        'illegal_raw',
        ['--illegal-opcodes', '--no-prg-header'],
        ['--illegal-opcodes', '--no-prg-header'],
        ROOT / 'illegal.asm',
        False
    ))
    tests.append((
        'illegal_header',
        ['--illegal-opcodes'],
        ['--illegal-opcodes'],
        ROOT / 'illegal.asm',
        False
    ))

    all_ok = True
    for name, ac, ap, src, use_map in tests:
        if not compare_case(name, ac, ap, src, with_map=use_map):
            all_ok = False

    print("\nSummary:")
    print("  All comparisons", "PASSED" if all_ok else "FAILED")
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())

