#!/usr/bin/env python3
"""mini-ld test suite.

For several compiler-flag combinations it compiles the test programs in
tests/progs, links them twice - with minild and with the system linker
(gcc -nostdlib -static, i.e. GNU ld) - runs both executables and checks that
they print exactly the same output and return the same exit code.
It also checks our executable with the system's readelf, compares
elfinspect's symbol listing with readelf's, and checks the linker's error
messages for undefined/duplicate symbols.

    python3 tests/run_tests.py
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGS = os.path.join(ROOT, "tests", "progs")
OUT = os.path.join(ROOT, "tests", "out")
MINILD = os.path.join(ROOT, "minild")
INSPECT = os.path.join(ROOT, "elfinspect")
CC = os.environ.get("CC", "gcc")
BASE = ["-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdlib", "-c"]

CONFIGS = [
    ("O0 no-pie", ["-O0", "-fno-pie", "-fno-asynchronous-unwind-tables"]),
    ("O2 no-pie", ["-O2", "-fno-pie", "-fno-asynchronous-unwind-tables"]),
    ("O2 pie codegen", ["-O2", "-fpie", "-fno-asynchronous-unwind-tables"]),
    ("O1 pic (GOTPCRELX relaxation)", ["-O1", "-fpic", "-fno-asynchronous-unwind-tables"]),
    ("Os with .eh_frame", ["-Os", "-fno-pie"]),
]

passed = failed = 0


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"PASS  {name}")
    else:
        failed += 1
        print(f"FAIL  {name}\n        {detail}")


def compile_all(sources, flags, tag):
    objs = []
    for src in sources:
        obj = os.path.join(OUT, f"{tag}_{os.path.splitext(src)[0]}.o")
        r = run([CC] + BASE + flags + ["-o", obj, os.path.join(PROGS, src)])
        if r.returncode != 0:
            raise RuntimeError(f"compile {src} failed:\n{r.stderr}")
        objs.append(obj)
    return objs


def link_and_compare(name, sources, flags, tag, args=("x",)):
    objs = compile_all(sources, flags, tag)
    ours, ref = os.path.join(OUT, tag + ".minild"), os.path.join(OUT, tag + ".gnuld")
    r = run([MINILD] + objs + ["-o", ours])
    if r.returncode != 0:
        check(name, False, "minild failed: " + r.stderr.strip())
        return None
    r = run([CC, "-nostdlib", "-static", "-no-pie", "-o", ref] + objs)
    if r.returncode != 0:
        check(name, False, "reference link failed: " + r.stderr.strip())
        return None
    a = run([ours] + list(args))
    b = run([ref] + list(args))
    same = a.stdout == b.stdout and a.returncode == b.returncode and a.stdout != ""
    check(name, same, f"minild: rc={a.returncode} out={a.stdout!r}\n        gnu ld: rc={b.returncode} out={b.stdout!r}")
    return ours, objs


def readelf_symbols(path):
    r = run(["readelf", "-sW", path])
    syms = set()
    for line in r.stdout.splitlines():
        m = re.match(r"\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+\w+\s+(\w+)\s+(\S+)$", line)
        if m and m.group(3) in ("FUNC", "OBJECT", "NOTYPE") and m.group(6):
            syms.add((m.group(6), int(m.group(1), 16)))
    return syms


def inspect_symbols(path):
    r = run([INSPECT, "-s", path])
    syms = set()
    for line in r.stdout.splitlines():
        m = re.match(r"\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\S+)$", line)
        if m and m.group(3) in ("FUNC", "OBJECT", "NOTYPE"):
            syms.add((m.group(6), int(m.group(1), 16)))
    return syms


def main():
    if not (os.path.exists(MINILD) and os.path.exists(INSPECT)):
        sys.exit("build first: make")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    main_prog = ["start.S", "sys.c", "mathlib.c", "main.c"]

    for i, (label, flags) in enumerate(CONFIGS):
        res = link_and_compare(f"same output as GNU ld: {label}", main_prog, flags, f"c{i}")
        if not res:
            continue
        exe, objs = res
        r = run(["readelf", "-aW", exe])
        check(f"readelf accepts our executable without warnings: {label}",
              r.returncode == 0 and "Warning" not in r.stderr and "Error" not in r.stderr, r.stderr.strip())
        if i == 0:
            want = readelf_symbols(exe)
            got = inspect_symbols(exe)
            check("elfinspect symbols == readelf symbols (executable)", want == got and len(want) > 10,
                  f"only in readelf: {sorted(want - got)[:5]}  only in elfinspect: {sorted(got - want)[:5]}")
            for o in objs:
                want, got = readelf_symbols(o), inspect_symbols(o)
                check(f"elfinspect symbols == readelf symbols ({os.path.basename(o)})", want == got,
                      f"only in readelf: {sorted(want - got)[:5]}  only in elfinspect: {sorted(got - want)[:5]}")
            r = run(["objdump", "-d", exe])
            check("objdump disassembles our executable and sees function names",
                  r.returncode == 0 and "<fib>:" in r.stdout and "<_start>:" in r.stdout, r.stderr.strip())
            r = run([MINILD] + objs + ["-o", os.path.join(OUT, "map.exe"), "--map"])
            check("link map lists sections and symbols",
                  r.returncode == 0 and ".text" in r.stdout and "fib" in r.stdout, r.stderr.strip())

    link_and_compare("COMMON symbols (-fcommon) merged and placed in .bss",
                     ["start.S", "sys.c", "common.c", "common_user.c"], ["-O1", "-fno-pie", "-fcommon"], "common")

    # error cases
    def expect_error(name, sources, msg, extra=()):
        objs = compile_all(sources, ["-O1", "-fno-pie"], "err")
        r = run([MINILD] + objs + list(extra) + ["-o", os.path.join(OUT, "err.exe")])
        check(name, r.returncode != 0 and msg in r.stderr, f"rc={r.returncode} stderr={r.stderr.strip()!r}")

    expect_error("error: undefined reference", ["start.S", "undef.c"], "undefined reference to `missing_function'")
    expect_error("error: multiple definition", ["start.S", "dup_a.c", "dup_b.c"], "multiple definition of `value'")
    expect_error("error: missing entry point", ["dup_b.c"], "entry symbol `_start' is not defined")
    r = run([MINILD, os.path.join(PROGS, "main.c"), "-o", os.path.join(OUT, "x")])
    check("error: input is not an ELF file", r.returncode != 0 and "not an ELF file" in r.stderr, r.stderr)

    # CUBIN decoding on a synthetic EM_CUDA file (real ones: see README, "On Colab")
    fake = os.path.join(OUT, "fake.cubin")
    run([sys.executable, os.path.join(ROOT, "tests", "make_fake_cubin.py"), fake])
    r = run([INSPECT, "--cuda", fake])
    check("elfinspect --cuda: kernels, code size, register count from .nv.info",
          r.returncode == 0 and "saxpy" in r.stdout and "128 bytes" in r.stdout and "uses 10 registers" in r.stdout,
          r.stdout + r.stderr)

    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
