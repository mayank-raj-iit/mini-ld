// elfinspect.cpp - a small `readelf`: print what is inside an ELF file.
//
//   elfinspect prog.o            everything
//   elfinspect -h -S prog.o      header + sections
//   elfinspect -s prog           symbols         -r  relocations    -l  segments
//   elfinspect --cuda k.cubin    NVIDIA CUBIN: kernels and .nv.info attributes
#include <cinttypes>
#include <cstdio>
#include <map>
#include <string>

#include "elf_file.hpp"

using namespace elf;

namespace {

void printHeader(const ElfFile& f) {
  const Ehdr& h = f.header();
  std::printf("ELF Header (%s):\n", f.path().c_str());
  std::printf("  Class:                ELF64, little-endian\n");
  std::printf("  Type:                 %s\n", fileTypeName(h.type));
  std::printf("  Machine:              %s (%u)\n", machineName(h.machine), h.machine);
  std::printf("  Entry point:          0x%" PRIx64 "\n", h.entry);
  std::printf("  Flags:                0x%x\n", h.flags);
  std::printf("  Program headers:      %u (at offset %" PRIu64 ")\n", h.phnum, h.phoff);
  std::printf("  Section headers:      %u (at offset %" PRIu64 ")\n", h.shnum, h.shoff);
  std::printf("  Section names in:     section %u\n\n", h.shstrndx);
}

void printSections(const ElfFile& f) {
  std::printf("Section Headers:\n");
  std::printf("  [Nr] %-24s %-14s %-16s %-8s %-8s %-5s %s\n", "Name", "Type", "Address", "Offset", "Size", "Flags", "Align");
  for (const Section& s : f.sections())
    std::printf("  [%2d] %-24s %-14s %016" PRIx64 " %08" PRIx64 " %08" PRIx64 " %-5s %" PRIu64 "\n", s.index,
                s.name.c_str(), sectionTypeName(s.hdr.type), s.hdr.addr, s.hdr.offset, s.hdr.size,
                sectionFlagsString(s.hdr.flags).c_str(), s.hdr.addralign);
  std::printf("  Flags: W=write A=alloc X=execute M=merge S=strings I=info G=group\n\n");
}

void printSegments(const ElfFile& f) {
  if (f.segments().empty()) {
    std::printf("There are no program headers in this file.\n\n");
    return;
  }
  std::printf("Program Headers:\n");
  std::printf("  %-12s %-10s %-18s %-10s %-10s %-5s %s\n", "Type", "Offset", "VirtAddr", "FileSiz", "MemSiz", "Flg", "Align");
  for (const Phdr& p : f.segments()) {
    const char* t = p.type == PT_LOAD ? "LOAD" : p.type == PT_GNU_STACK ? "GNU_STACK" : p.type == PT_NOTE ? "NOTE"
                  : p.type == PT_PHDR ? "PHDR" : "OTHER";
    std::string flg;
    flg += (p.flags & PF_R) ? 'R' : ' ';
    flg += (p.flags & PF_W) ? 'W' : ' ';
    flg += (p.flags & PF_X) ? 'E' : ' ';
    std::printf("  %-12s 0x%08" PRIx64 " 0x%016" PRIx64 " 0x%08" PRIx64 " 0x%08" PRIx64 " %-5s 0x%" PRIx64 "\n", t,
                p.offset, p.vaddr, p.filesz, p.memsz, flg.c_str(), p.align);
  }
  std::printf("\n");
}

const char* bindName(uint8_t b) { return b == STB_LOCAL ? "LOCAL" : b == STB_GLOBAL ? "GLOBAL" : b == STB_WEAK ? "WEAK" : "OTHER"; }
const char* typeName(uint8_t t) {
  switch (t) {
    case STT_NOTYPE: return "NOTYPE";
    case STT_OBJECT: return "OBJECT";
    case STT_FUNC: return "FUNC";
    case STT_SECTION: return "SECTION";
    case STT_FILE: return "FILE";
    default: return "OTHER";
  }
}
std::string ndxName(uint16_t n) {
  if (n == SHN_UNDEF) return "UND";
  if (n == SHN_ABS) return "ABS";
  if (n == SHN_COMMON) return "COM";
  return std::to_string(n);
}

void printSymbols(const ElfFile& f) {
  std::printf("Symbol table contains %zu entries:\n", f.symbols().size());
  std::printf("   Num: %-16s %6s %-8s %-7s %4s Name\n", "Value", "Size", "Type", "Bind", "Ndx");
  for (const Symbol& s : f.symbols())
    std::printf("  %4d: %016" PRIx64 " %6" PRIu64 " %-8s %-7s %4s %s\n", s.index, s.sym.value, s.sym.size,
                typeName(s.type()), bindName(s.bind()), ndxName(s.sym.shndx).c_str(), s.name.c_str());
  std::printf("\n");
}

void printRelocations(const ElfFile& f) {
  if (f.relocations().empty()) {
    std::printf("There are no relocations in this file.\n\n");
    return;
  }
  for (const RelocSection& rs : f.relocations()) {
    const Section& s = f.sections()[rs.sectionIndex];
    std::printf("Relocation section '%s' (patches section '%s') has %zu entries:\n", s.name.c_str(),
                f.sections()[rs.targetSection].name.c_str(), rs.relocs.size());
    std::printf("  %-12s %-24s %-24s %s\n", "Offset", "Type", "Symbol", "Addend");
    for (const Relocation& r : rs.relocs) {
      const Symbol& sym = f.symbols()[r.symIndex()];
      const char* type = f.header().machine == EM_X86_64 ? x86RelocName(r.type()) : "(machine specific)";
      std::printf("  %012" PRIx64 " %-24s %-24s %+" PRId64 "\n", r.rela.offset, type, sym.name.c_str(), r.rela.addend);
    }
    std::printf("\n");
  }
}

// ---- NVIDIA CUBIN ----------------------------------------------------------
// A CUBIN is a normal ELF64 file with e_machine = EM_CUDA (190). Each kernel K
// has its machine code in ".text.K" and per-kernel metadata in ".nv.info.K".
// .nv.info is a list of records: format byte, attribute byte, then a 16-bit
// value or (for EIFMT_SVAL) a 16-bit size followed by that many bytes.
// Attribute names below are best-effort; compare with `cuobjdump -elf`.
const char* nvAttrName(uint8_t a) {
  static const std::map<uint8_t, const char*> names = {
      {0x0a, "EIATTR_PARAM_CBANK"},         {0x11, "EIATTR_FRAME_SIZE"},
      {0x12, "EIATTR_MIN_STACK_SIZE"},      {0x17, "EIATTR_KPARAM_INFO"},
      {0x19, "EIATTR_CBANK_PARAM_SIZE"},    {0x1b, "EIATTR_MAXREG_COUNT"},
      {0x1c, "EIATTR_EXIT_INSTR_OFFSETS"},  {0x23, "EIATTR_MAX_STACK_SIZE"},
      {0x2f, "EIATTR_REGCOUNT"},            {0x37, "EIATTR_CUDA_API_VERSION"}};
  auto it = names.find(a);
  return it == names.end() ? nullptr : it->second;
}

void printNvInfo(const ElfFile& f, const Section& s) {
  std::printf("  %s:\n", s.name.c_str());
  size_t off = 0;
  const uint8_t* d = s.data;
  while (d && off + 4 <= s.hdr.size) {
    uint8_t fmt = d[off], attr = d[off + 1];
    uint16_t v = (uint16_t)(d[off + 2] | (d[off + 3] << 8));
    const char* name = nvAttrName(attr);
    char unknown[32];
    std::snprintf(unknown, sizeof unknown, "attribute 0x%02x", attr);
    if (fmt == 0x04) {  // EIFMT_SVAL: v = payload size in bytes
      if (off + 4 + v > s.hdr.size) { std::printf("    (truncated record)\n"); return; }
      std::printf("    %-28s size %u:", name ? name : unknown, v);
      for (uint16_t k = 0; k + 4 <= v; k += 4) {
        uint32_t w = d[off + 4 + k] | (d[off + 5 + k] << 8) | (d[off + 6 + k] << 16) | ((uint32_t)d[off + 7 + k] << 24);
        std::printf(" 0x%x", w);
      }
      // REGCOUNT is (symbol index, register count): name the kernel
      if (attr == 0x2f && v == 8) {
        uint32_t symIdx = d[off + 4] | (d[off + 5] << 8) | (d[off + 6] << 16) | ((uint32_t)d[off + 7] << 24);
        uint32_t regs = d[off + 8] | (d[off + 9] << 8) | (d[off + 10] << 16) | ((uint32_t)d[off + 11] << 24);
        if (symIdx < f.symbols().size()) std::printf("   -> %s uses %u registers", f.symbols()[symIdx].name.c_str(), regs);
      }
      std::printf("\n");
      off += 4 + v;
    } else if (fmt == 0x01 || fmt == 0x02 || fmt == 0x03) {  // NVAL / BVAL / HVAL
      std::printf("    %-28s value 0x%x\n", name ? name : unknown, v);
      off += 4;
    } else {
      std::printf("    (unknown record format 0x%02x at offset %zu - stopping)\n", fmt, off);
      return;
    }
  }
}

void printCuda(const ElfFile& f) {
  if (f.header().machine != EM_CUDA) {
    std::printf("Not a CUBIN (machine is %s).\n\n", machineName(f.header().machine));
    return;
  }
  std::printf("NVIDIA CUBIN:\n");
  std::printf("  e_flags 0x%08x (low byte usually = SM version, e.g. 0x4b = sm_75)\n", f.header().flags);
  std::printf("  Kernels:\n");
  for (const Section& s : f.sections())
    if (s.name.rfind(".text.", 0) == 0)
      std::printf("    %-24s code: %6" PRIu64 " bytes (%" PRIu64 " SASS instructions of 16 bytes on sm_70+)\n",
                  s.name.substr(6).c_str(), s.hdr.size, s.hdr.size / 16);
  for (const Section& s : f.sections()) {
    if (s.name.rfind(".nv.shared.", 0) == 0)
      std::printf("    %-24s shared memory: %" PRIu64 " bytes\n", s.name.substr(11).c_str(), s.hdr.size);
    if (s.name.rfind(".nv.constant0.", 0) == 0)
      std::printf("    %-24s constant bank 0 (holds the kernel parameters): %" PRIu64 " bytes\n", s.name.substr(14).c_str(), s.hdr.size);
  }
  std::printf("  .nv.info records:\n");
  for (const Section& s : f.sections())
    if (s.name == ".nv.info" || s.name.rfind(".nv.info.", 0) == 0) printNvInfo(f, s);
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  bool h = false, S = false, l = false, s = false, r = false, cuda = false;
  std::string path;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h") h = true;
    else if (a == "-S") S = true;
    else if (a == "-l") l = true;
    else if (a == "-s") s = true;
    else if (a == "-r") r = true;
    else if (a == "--cuda") cuda = true;
    else if (a == "--help") {
      std::printf("usage: elfinspect [-h] [-S] [-l] [-s] [-r] [--cuda] <file>\n");
      return 0;
    } else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: elfinspect [-h] [-S] [-l] [-s] [-r] [--cuda] <file>\n");
    return 2;
  }
  try {
    ElfFile f(path);
    bool all = !(h || S || l || s || r || cuda);
    if (all || h) printHeader(f);
    if (all || S) printSections(f);
    if (all || l) printSegments(f);
    if (all || s) printSymbols(f);
    if (all || r) printRelocations(f);
    if (cuda || (all && f.header().machine == EM_CUDA)) printCuda(f);
  } catch (const ElfError& e) {
    std::fprintf(stderr, "elfinspect: error: %s\n", e.what());
    return 1;
  }
  return 0;
}
