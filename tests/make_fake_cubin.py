#!/usr/bin/env python3
"""Write a small *synthetic* CUBIN-like ELF (e_machine = EM_CUDA) for testing
elfinspect --cuda without a GPU toolchain. Real CUBINs come from
`ptxas -arch=sm_75 k.ptx -o k.cubin`; check against `cuobjdump -elf k.cubin`."""
import struct, sys

def build(path):
    names = [b"", b".shstrtab", b".strtab", b".symtab", b".nv.info", b".nv.info.saxpy",
             b".nv.constant0.saxpy", b".nv.shared.saxpy", b".text.saxpy"]
    shstr = b"\0".join(names[1:])
    shstr = b"\0" + shstr + b"\0"
    off = {n: shstr.index(b"\0" + n + b"\0") + 1 for n in names[1:]}
    strtab = b"\0saxpy\0"
    # symbols: null, section symbol for .text.saxpy (index 8), the kernel (FUNC GLOBAL in section 8)
    sym = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    sym += struct.pack("<IBBHQQ", 0, 0x03, 0, 8, 0, 0)                  # STT_SECTION
    sym += struct.pack("<IBBHQQ", 1, (1 << 4) | 2, 0x10, 8, 0, 128)     # saxpy, GLOBAL FUNC
    # .nv.info: EIATTR_REGCOUNT (SVAL, 8 bytes: symbol 2, 10 registers), FRAME_SIZE (SVAL, 8 bytes)
    nvinfo = struct.pack("<BBHII", 0x04, 0x2f, 8, 2, 10) + struct.pack("<BBHII", 0x04, 0x11, 8, 2, 0)
    # .nv.info.saxpy: CBANK_PARAM_SIZE as HVAL (24 bytes of params), MAXREG_COUNT HVAL
    nvk = struct.pack("<BBH", 0x03, 0x19, 24) + struct.pack("<BBH", 0x03, 0x1b, 255)
    text = bytes(128)                       # 8 SASS instructions of 16 bytes (all zero here)
    const0 = bytes(0x160 + 24)
    blobs = [b"", shstr, strtab, sym, nvinfo, nvk, const0, b"", text]
    types = [0, 3, 3, 2, 0x70000000, 0x70000000, 1, 8, 1]
    flags = [0, 0, 0, 0, 0, 0x40, 0x42, 0x3, 0x6]
    data, offsets, pos = b"", [], 64
    for b in blobs:
        offsets.append(pos + len(data)); data += b
        while len(data) % 8: data += b"\0"
    shoff = 64 + len(data)
    shdrs = b""
    for i, b in enumerate(blobs):
        if i == 0:
            shdrs += bytes(64); continue
        size = 4 if i == 7 else len(b)      # .nv.shared: NOBITS with 4 bytes
        link = 2 if i == 3 else (3 if i in (4, 5) else 0)
        info = 1 if i == 3 else (8 if i == 5 else 0)
        entsize = 24 if i == 3 else 0
        shdrs += struct.pack("<IIQQQQIIQQ", off[names[i]], types[i], flags[i], 0, offsets[i], size, link, info, 4, entsize)
    ident = b"\x7fELF" + bytes([2, 1, 1, 0x33, 7]) + bytes(7)
    ehdr = ident + struct.pack("<HHIQQQIHHHHHH", 2, 190, 1, 0, 0, shoff, 0x4b004b, 64, 56, 0, 64, len(blobs), 1)
    open(path, "wb").write(ehdr + data + shdrs)

build(sys.argv[1])
