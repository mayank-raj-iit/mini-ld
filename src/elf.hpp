// elf.hpp - ELF64 data structures and constants (little-endian, as on x86-64).
//
// ELF (Executable and Linkable Format) is the file format of Linux object
// files (.o), executables, shared libraries (.so) and also NVIDIA CUBIN files.
// We define the structs ourselves (instead of including <elf.h>) so every
// field is visible and the code also builds on Windows/macOS.
#pragma once
#include <cstdint>

namespace elf {

struct Ehdr {                 // ELF header: always at offset 0, 64 bytes
  uint8_t ident[16];          // "\x7f" "ELF", class, data encoding, version ...
  uint16_t type;              // ET_REL / ET_EXEC / ET_DYN
  uint16_t machine;           // EM_X86_64 / EM_CUDA ...
  uint32_t version;
  uint64_t entry;             // address of the first instruction to run
  uint64_t phoff;             // file offset of the program header table
  uint64_t shoff;             // file offset of the section header table
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize, phnum;
  uint16_t shentsize, shnum;
  uint16_t shstrndx;          // index of the section holding section names
};

struct Shdr {                 // section header, 64 bytes each
  uint32_t name;              // offset into the section-name string table
  uint32_t type;              // SHT_PROGBITS, SHT_SYMTAB, ...
  uint64_t flags;             // SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR ...
  uint64_t addr;              // address in memory (0 in .o files)
  uint64_t offset;            // where the bytes are in the file
  uint64_t size;
  uint32_t link, info;        // meaning depends on type
  uint64_t addralign;
  uint64_t entsize;           // size of one entry for tables
};

struct Phdr {                 // program header ("segment"), 56 bytes each
  uint32_t type;              // PT_LOAD ...
  uint32_t flags;             // PF_R | PF_W | PF_X
  uint64_t offset, vaddr, paddr;
  uint64_t filesz, memsz;     // memsz > filesz -> the rest is zero-filled (.bss)
  uint64_t align;
};

struct Sym {                  // symbol table entry, 24 bytes
  uint32_t name;
  uint8_t info;               // binding (high 4 bits) and type (low 4 bits)
  uint8_t other;              // visibility
  uint16_t shndx;             // section the symbol lives in, or SHN_UNDEF/ABS/COMMON
  uint64_t value;             // offset inside the section (in .o files)
  uint64_t size;
};

struct Rela {                 // relocation with explicit addend, 24 bytes
  uint64_t offset;            // where to patch, inside the target section
  uint64_t info;              // symbol index (high 32 bits) and type (low 32 bits)
  int64_t addend;
};

static_assert(sizeof(Ehdr) == 64 && sizeof(Shdr) == 64 && sizeof(Phdr) == 56, "ELF struct size");
static_assert(sizeof(Sym) == 24 && sizeof(Rela) == 24, "ELF struct size");

inline uint8_t symBind(uint8_t info) { return info >> 4; }
inline uint8_t symType(uint8_t info) { return info & 0xf; }
inline uint8_t symInfo(uint8_t bind, uint8_t type) { return (uint8_t)((bind << 4) | (type & 0xf)); }
inline uint32_t relSym(uint64_t info) { return (uint32_t)(info >> 32); }
inline uint32_t relType(uint64_t info) { return (uint32_t)info; }

// e_type
constexpr uint16_t ET_REL = 1, ET_EXEC = 2, ET_DYN = 3;
// e_machine
constexpr uint16_t EM_X86_64 = 62, EM_AARCH64 = 183, EM_CUDA = 190;
// section types
constexpr uint32_t SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4,
                   SHT_HASH = 5, SHT_DYNAMIC = 6, SHT_NOTE = 7, SHT_NOBITS = 8, SHT_REL = 9,
                   SHT_DYNSYM = 11, SHT_INIT_ARRAY = 14, SHT_FINI_ARRAY = 15, SHT_GROUP = 17,
                   SHT_X86_64_UNWIND = 0x70000001;
// section flags
constexpr uint64_t SHF_WRITE = 0x1, SHF_ALLOC = 0x2, SHF_EXECINSTR = 0x4, SHF_MERGE = 0x10,
                   SHF_STRINGS = 0x20, SHF_INFO_LINK = 0x40, SHF_GROUP = 0x200;
// special section indexes
constexpr uint16_t SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2, SHN_XINDEX = 0xffff;
// symbol binding / type
constexpr uint8_t STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;
constexpr uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4;
// segments
constexpr uint32_t PT_NULL = 0, PT_LOAD = 1, PT_NOTE = 4, PT_PHDR = 6, PT_GNU_STACK = 0x6474e551;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
// x86-64 relocation types (System V ABI)
constexpr uint32_t R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4,
                   R_X86_64_GOTPCREL = 9, R_X86_64_32 = 10, R_X86_64_32S = 11, R_X86_64_PC64 = 24,
                   R_X86_64_GOTPCRELX = 41, R_X86_64_REX_GOTPCRELX = 42;

}  // namespace elf
