# mini-ld — a static linker and ELF inspector for x86-64 Linux

Two tools written in C++17 with no dependencies (~1,100 lines):

* **`minild`** links ELF relocatable object files (`.o`) into a runnable static
  executable, doing the same core job as GNU `ld`.
* **`elfinspect`** is a small `readelf`: header, sections, segments, symbols and
  relocations of any ELF64 file, and kernels / `.nv.info` records of NVIDIA
  **CUBIN** files (GPU code produced by `ptxas`, which is also ELF).

```
 start.o ─┐                                    ┌─► .text   (R X)  0x400000
 main.o  ─┼─► collect ─► resolve ─► layout ─► relocate ─► .rodata
 lib.o   ─┘   sections    symbols   addresses   patch code  .data/.bss (R W)
                                                            + ELF/program headers + .symtab
```

## What the linker does

1. **Collect** – every allocated section of every input goes into one of four
   output sections by its flags: executable → `.text`, read-only → `.rodata`,
   writable → `.data`, `NOBITS` → `.bss`. Unwind tables (`.eh_frame`), notes and
   `.comment` are dropped together with their relocations.
2. **Resolve symbols** – global, weak, local, absolute and COMMON symbols, with
   the same rules as `ld`: a strong definition beats weak and COMMON ones,
   COMMON definitions merge to the largest size, two strong definitions are a
   `multiple definition` error and a missing one is an `undefined reference` error
   naming the file that used it. `_end`, `_etext`, `_edata`, `__bss_start` and
   `_GLOBAL_OFFSET_TABLE_` are provided when a program asks for them.
3. **Layout** – sections are placed with their alignment into two page-aligned
   `PT_LOAD` segments (read+execute for code, read+write for data, `.bss`
   zero-filled at load), plus `PT_GNU_STACK` for a non-executable stack.
4. **Relocate** – patches `R_X86_64_64`, `PC32`, `PLT32`, `32`, `32S`, `PC64`, and
   relaxes `GOTPCRELX`/`REX_GOTPCRELX` (`mov foo@GOTPCREL(%rip)` → `lea foo(%rip)`,
   `call *foo@GOTPCREL(%rip)` → `addr32 call foo`), so `-fpic` objects link without a GOT.
   Every 32-bit field is overflow-checked.

The output has section headers and a symbol table, so `gdb`, `objdump -d` and
`nm` show function names. `--map` prints a link map.

## Build and use

```bash
make                                        # builds ./minild and ./elfinspect
gcc -c -ffreestanding -fno-builtin -fno-stack-protector -O2 \
    tests/progs/start.S tests/progs/sys.c tests/progs/mathlib.c tests/progs/main.c
./minild start.o sys.o mathlib.o main.o -o prog --map
./prog; echo "exit code $?"
./elfinspect -r main.o           # the relocations the linker has to patch
./elfinspect -l -S prog          # segments and sections of the result
gdb -batch -ex 'break fib' -ex run -ex bt ./prog
```

The test programs use no C library: `start.S` is the entry point (`_start`
calls `main`, then the `exit` system call) and `sys.c` prints with the raw
`write` system call.

## Testing

`make test` compiles the test programs with five flag sets (`-O0`/`-O1`/`-O2`/`-Os`,
non-PIC, PIE and PIC code generation, with and without `.eh_frame`), links each
with **minild and with GNU ld**, runs both and requires identical output and
exit codes. It also checks the executable with the system `readelf` (no
warnings), compares `elfinspect`'s symbol table with `readelf`'s for every
object file, checks `objdump` sees function names, and checks the error
messages for undefined, duplicate and missing-entry symbols. 23 checks.

### On Colab: inspect a real CUBIN

```bash
../minicuda/minicuda ../minicuda/tests/kernels/matmul_tiled.cu -o k.ptx
ptxas -arch=sm_75 -v k.ptx -o k.cubin      # -v prints registers used
./elfinspect --cuda k.cubin                 # kernels, code size, .nv.info records
cuobjdump -elf k.cubin | less               # NVIDIA's view, to compare against
```

The `.nv.info` attribute names are best-effort (the format is not officially
documented); the unit test uses a synthetic CUBIN (`tests/make_fake_cubin.py`).

## Limitations

Static linking only (no shared libraries, no dynamic loader, no TLS), x86-64
only, no linker scripts, no `.init_array` constructors, no garbage collection
of unused sections.
