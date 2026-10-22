// linker.cpp - how a static linker works, in four steps:
//
//  1. collect   - take the code/data sections out of every .o file and group
//                 them into output sections (.text, .rodata, .data, .bss)
//  2. resolve   - match every symbol that is *used* (undefined) in one file
//                 with the file that *defines* it; report undefined or
//                 duplicate symbols
//  3. layout    - decide the final memory address of every section and symbol
//  4. relocate  - patch every place in the code that refers to a symbol
//                 (the compiler left a 0 there plus a relocation record),
//                 then write the ELF executable with program headers
#include "linker.hpp"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <fstream>
#include <map>
#include <sys/stat.h>

using namespace elf;

namespace {
uint64_t alignUp(uint64_t v, uint64_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }
constexpr uint64_t kPage = 0x1000;
}  // namespace

uint64_t Linker::InputSection::addr() const { return linker->outputs_[out].addr + offset; }

void Linker::addObject(const std::string& path) {
  auto f = std::make_unique<ElfFile>(path);
  if (f->header().type != ET_REL) throw LinkError(path + ": not a relocatable object file (.o)");
  if (f->header().machine != EM_X86_64) throw LinkError(path + ": not an x86-64 object file");
  files_.push_back(std::move(f));
}

// ---------------------------------------------------------------- step 1 ---
void Linker::collectSections() {
  auto make = [](const char* name, uint32_t type, uint64_t flags) {
    OutputSection o;
    o.name = name;
    o.type = type;
    o.flags = flags;
    return o;
  };
  outputs_ = {
      make(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR),
      make(".rodata", SHT_PROGBITS, SHF_ALLOC),
      make(".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE),
      make(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE),
  };
  for (const auto& fp : files_) {
    const ElfFile& f = *fp;
    for (const Section& s : f.sections()) {
      const Shdr& h = s.hdr;
      if (!(h.flags & SHF_ALLOC)) continue;  // .comment, .symtab, debug info ...: not loaded
      if (h.type == SHT_NOTE || h.type == SHT_X86_64_UNWIND || h.type == SHT_GROUP) continue;  // metadata
      if (h.type == SHT_INIT_ARRAY || h.type == SHT_FINI_ARRAY)
        throw LinkError(f.path() + ": global constructors/destructors (" + s.name + ") are not supported");
      if (h.type != SHT_PROGBITS && h.type != SHT_NOBITS)
        throw LinkError(f.path() + ": unsupported allocated section " + s.name + " (" + sectionTypeName(h.type) + ")");
      int out = h.type == SHT_NOBITS ? kBss : (h.flags & SHF_EXECINSTR) ? kText : (h.flags & SHF_WRITE) ? kData : kRodata;
      inputOf_[&f][s.index] = (int)inputs_.size();
      inputs_.push_back({&f, s.index, out, 0, this});
      outputs_[out].members.push_back((int)inputs_.size() - 1);
    }
  }
}

// ---------------------------------------------------------------- step 2 ---
void Linker::resolveSymbols() {
  std::vector<std::string> errors;
  for (const auto& fp : files_) {
    const ElfFile& f = *fp;
    for (const Symbol& s : f.symbols()) {
      if (s.index == 0 || s.bind() == STB_LOCAL) continue;   // locals are private to their file
      if (s.bind() != STB_GLOBAL && s.bind() != STB_WEAK) continue;
      Global& g = globals_[s.name];
      if (g.name.empty()) { g.name = s.name; globalOrder_.push_back(s.name); }
      if (s.isUndefined()) {
        if (g.firstReference.empty()) g.firstReference = f.path();
        if (!g.file && s.bind() == STB_GLOBAL) g.bind = STB_GLOBAL;  // a strong reference
        continue;
      }
      // a definition
      bool isCommon = s.sym.shndx == SHN_COMMON;
      bool isAbs = s.sym.shndx == SHN_ABS;
      int sec = -1;
      if (!isCommon && !isAbs) {
        auto it = inputOf_[&f].find(s.sym.shndx);
        if (it == inputOf_[&f].end()) continue;  // defined in a section we dropped (e.g. .eh_frame)
        sec = it->second;
      }
      auto take = [&]() {
        g.file = &f;
        g.section = sec;
        g.value = s.sym.value;
        g.size = s.sym.size;
        g.bind = s.bind();
        g.type = s.type();
        g.isAbsolute = isAbs;
        g.isCommon = isCommon;
        g.commonAlign = isCommon ? std::max<uint64_t>(1, s.sym.value) : 1;
      };
      if (!g.file) { take(); continue; }
      // two definitions of the same name: the rules a real linker follows
      bool oldStrong = g.bind == STB_GLOBAL && !g.isCommon;
      bool newStrong = s.bind() == STB_GLOBAL && !isCommon;
      if (oldStrong && newStrong) {
        errors.push_back("multiple definition of `" + s.name + "': first defined in " + g.file->path() + ", again in " + f.path());
      } else if (newStrong || (g.bind == STB_WEAK && !g.isCommon && isCommon)) {
        take();   // strong beats weak and common
      } else if (g.isCommon && isCommon) {
        g.size = std::max(g.size, s.sym.size);   // two tentative definitions merge
        g.commonAlign = std::max(g.commonAlign, std::max<uint64_t>(1, s.sym.value));
      }
    }
  }
  // Symbols the linker itself provides when a program asks for them (as GNU ld does).
  // _GLOBAL_OFFSET_TABLE_ is named by -fpic code; we rewrite every GOT access, so
  // no real GOT is needed and the symbol just has to exist.
  static const char* kProvided[] = {"_GLOBAL_OFFSET_TABLE_", "_etext", "etext", "_edata", "edata", "__bss_start", "_end", "end"};
  for (const char* n : kProvided) {
    auto it = globals_.find(n);
    if (it != globals_.end() && !it->second.file) it->second.linkerDefined = true;
  }
  for (const std::string& name : globalOrder_) {
    const Global& g = globals_[name];
    if (!g.file && !g.linkerDefined && g.bind != STB_WEAK)
      errors.push_back("undefined reference to `" + name + "' (used in " + g.firstReference + ")");
  }
  if (!errors.empty()) {
    std::string all;
    for (auto& e : errors) all += e + "\n";
    all.pop_back();
    throw LinkError(all);
  }
}

// ---------------------------------------------------------------- step 3 ---
void Linker::layout() {
  // COMMON symbols ("int x;" compiled with -fcommon) get space at the end of .bss
  uint64_t commonSize = 0, commonAlign = 1;
  for (const std::string& name : globalOrder_) {
    Global& g = globals_[name];
    if (!g.isCommon || !g.file) continue;
    commonSize = alignUp(commonSize, g.commonAlign) + g.size;
    commonAlign = std::max(commonAlign, g.commonAlign);
  }

  // place each input section inside its output section, respecting alignment
  for (auto& out : outputs_) {
    uint64_t off = 0;
    for (int m : out.members) {
      const Shdr& h = inputs_[m].file->sections()[inputs_[m].index].hdr;
      uint64_t a = std::max<uint64_t>(1, h.addralign);
      off = alignUp(off, a);
      inputs_[m].offset = off;
      off += h.size;
      out.align = std::max(out.align, a);
    }
    out.size = off;
  }
  uint64_t commonStart = alignUp(outputs_[kBss].size, commonAlign);
  if (commonSize) {
    outputs_[kBss].size = commonStart + commonSize;
    outputs_[kBss].align = std::max(outputs_[kBss].align, commonAlign);
  }

  // Segment 1 (read + execute): ELF header, program headers, .text, .rodata
  // Segment 2 (read + write):   .data, then .bss (zero-filled, takes no file space)
  // Each segment starts on a new 4 KiB page so it can have its own permissions.
  const uint64_t headers = sizeof(Ehdr) + 3 * sizeof(Phdr);
  uint64_t off = headers;
  for (int k : {kText, kRodata}) {
    off = alignUp(off, outputs_[k].align);
    outputs_[k].fileOffset = off;
    outputs_[k].addr = opt_.baseAddress + off;
    off += outputs_[k].size;
  }
  rxEnd_ = off;
  rwOffset_ = alignUp(off, kPage);
  off = rwOffset_;
  // vaddr and file offset must be equal modulo the page size; this keeps them equal outright
  off = alignUp(off, outputs_[kData].align);
  outputs_[kData].fileOffset = off;
  outputs_[kData].addr = opt_.baseAddress + off;
  off += outputs_[kData].size;
  rwFileEnd_ = off;
  uint64_t bssAddr = alignUp(opt_.baseAddress + off, outputs_[kBss].align);
  outputs_[kBss].fileOffset = off;
  outputs_[kBss].addr = bssAddr;

  // final symbol addresses
  uint64_t commonOff = commonStart;
  const uint64_t textEnd = outputs_[kText].addr + outputs_[kText].size;
  const uint64_t dataEnd = outputs_[kData].addr + outputs_[kData].size;
  const uint64_t bssEnd = outputs_[kBss].addr + outputs_[kBss].size;
  for (const std::string& name : globalOrder_) {
    Global& g = globals_[name];
    if (g.linkerDefined) {
      g.addr = (name == "_etext" || name == "etext") ? textEnd
             : (name == "_edata" || name == "edata") ? dataEnd
             : name == "__bss_start" ? outputs_[kBss].addr
             : (name == "_end" || name == "end") ? bssEnd
             : outputs_[kData].addr;  // _GLOBAL_OFFSET_TABLE_
      g.isAbsolute = true;
    } else if (!g.file) g.addr = 0;  // undefined weak symbol
    else if (g.isAbsolute) g.addr = g.value;
    else if (g.isCommon) {
      commonOff = alignUp(commonOff, g.commonAlign);
      g.addr = outputs_[kBss].addr + commonOff;
      commonOff += g.size;
    } else g.addr = inputs_[g.section].addr() + g.value;
  }

  auto it = globals_.find(opt_.entry);
  if (it == globals_.end() || !it->second.file) throw LinkError("entry symbol `" + opt_.entry + "' is not defined");
  entryAddr_ = it->second.addr;
}

uint64_t Linker::symbolAddress(const ElfFile& f, const Symbol& s) const {
  if (s.bind() != STB_LOCAL) return globals_.at(s.name).addr;
  if (s.sym.shndx == SHN_ABS) return s.sym.value;
  auto& map = inputOf_.at(&f);
  auto it = map.find(s.sym.shndx);
  if (it == map.end()) throw LinkError(f.path() + ": relocation against symbol in a dropped section (" + s.name + ")");
  return inputs_[it->second].addr() + s.sym.value;  // STT_SECTION symbols have value 0
}

// ---------------------------------------------------------------- step 4 ---
void Linker::applyRelocations(std::vector<uint8_t>& image) {
  for (const auto& fp : files_) {
    const ElfFile& f = *fp;
    for (const RelocSection& rs : f.relocations()) {
      auto tgt = inputOf_[&f].find(rs.targetSection);
      if (tgt == inputOf_[&f].end()) continue;  // relocations for a dropped section (.eh_frame)
      const InputSection& in = inputs_[tgt->second];
      const OutputSection& out = outputs_[in.out];
      if (out.type == SHT_NOBITS) throw LinkError(f.path() + ": relocation inside .bss");
      for (const Relocation& r : rs.relocs) {
        const Symbol& sym = f.symbols()[r.symIndex()];
        const uint64_t S = symbolAddress(f, sym);   // where the symbol ended up
        const int64_t A = r.rela.addend;            // constant the compiler wants added
        const uint64_t P = in.addr() + r.rela.offset;  // address of the bytes being patched
        const uint64_t at = out.fileOffset + in.offset + r.rela.offset;  // same place, in the file
        auto where = [&]() {
          char b[64];
          std::snprintf(b, sizeof b, "+0x%" PRIx64, r.rela.offset);
          return f.path() + ":(" + f.sections()[rs.targetSection].name + b + ")";
        };
        auto put32 = [&](int64_t v, bool isSigned) {
          if (isSigned ? (v < INT32_MIN || v > INT32_MAX) : (v < 0 || v > (int64_t)UINT32_MAX))
            throw LinkError(where() + ": relocation " + x86RelocName(r.type()) + " against `" + sym.name + "' does not fit in 32 bits");
          uint32_t u = (uint32_t)v;
          for (int k = 0; k < 4; k++) image[at + k] = (uint8_t)(u >> (8 * k));
        };
        auto put64 = [&](uint64_t v) {
          for (int k = 0; k < 8; k++) image[at + k] = (uint8_t)(v >> (8 * k));
        };
        switch (r.type()) {
          case R_X86_64_NONE: break;
          case R_X86_64_64: put64(S + A); break;                                  // absolute address
          case R_X86_64_PC32:
          case R_X86_64_PLT32: put32((int64_t)(S + A - P), true); break;          // rip-relative (calls, jumps, data)
          case R_X86_64_PC64: put64(S + A - P); break;
          case R_X86_64_32: put32((int64_t)(S + A), false); break;
          case R_X86_64_32S: put32((int64_t)(S + A), true); break;
          case R_X86_64_GOTPCRELX:
          case R_X86_64_REX_GOTPCRELX: {
            // "load the address of x from the GOT". In a static executable we know the
            // address, so like GNU ld we rewrite the instruction:  mov foo@GOTPCREL(%rip),%rax
            // (opcode 8b) becomes  lea foo(%rip),%rax  (opcode 8d), and call *foo@GOTPCREL(%rip)
            // (ff 15) becomes  addr32 call foo  (67 e8).
            if (at < 2) throw LinkError(where() + ": bad GOTPCRELX relocation");
            if (image[at - 2] == 0x8b) image[at - 2] = 0x8d;
            else if (image[at - 2] == 0xff && image[at - 1] == 0x15) { image[at - 2] = 0x67; image[at - 1] = 0xe8; }
            else throw LinkError(where() + ": cannot relax GOTPCRELX for this instruction (needs a GOT)");
            put32((int64_t)(S + A - P), true);
            break;
          }
          default:
            throw LinkError(where() + ": unsupported relocation type " + std::to_string(r.type()) + " (" +
                            x86RelocName(r.type()) + ") - compile with -fno-pic");
        }
      }
    }
  }
}

void Linker::writeExecutable() {
  // ---- loadable image: headers, then section bytes at their file offsets ----
  std::vector<uint8_t> image(rwFileEnd_, 0);
  for (const auto& in : inputs_) {
    const Section& s = in.file->sections()[in.index];
    const OutputSection& out = outputs_[in.out];
    if (out.type == SHT_NOBITS || !s.data) continue;
    std::copy(s.data, s.data + s.hdr.size, image.begin() + out.fileOffset + in.offset);
  }
  applyRelocations(image);

  // ---- symbol table, so gdb / objdump / nm can show names ----
  std::string strtab(1, '\0');
  auto addStr = [&](const std::string& n) { uint32_t o = (uint32_t)strtab.size(); strtab += n; strtab += '\0'; return o; };
  std::vector<int> outIndex(4, 0);  // output section -> section header index (0 = omitted)
  std::vector<int> present;
  for (int k = 0; k < 4; k++)
    if (outputs_[k].size > 0) { present.push_back(k); outIndex[k] = (int)present.size(); }
  std::vector<Sym> syms(1, Sym{});
  for (const auto& fp : files_)                       // local functions and variables first
    for (const Symbol& s : fp->symbols()) {
      if (s.index == 0 || s.bind() != STB_LOCAL || (s.type() != STT_FUNC && s.type() != STT_OBJECT)) continue;
      auto it = inputOf_[fp.get()].find(s.sym.shndx);
      if (it == inputOf_[fp.get()].end()) continue;
      Sym o{};
      o.name = addStr(s.name);
      o.info = s.sym.info;
      o.shndx = (uint16_t)outIndex[inputs_[it->second].out];
      o.value = inputs_[it->second].addr() + s.sym.value;
      o.size = s.sym.size;
      syms.push_back(o);
    }
  uint32_t firstGlobal = (uint32_t)syms.size();
  for (const std::string& name : globalOrder_) {
    const Global& g = globals_.at(name);
    if (!g.file && !g.linkerDefined) continue;
    Sym o{};
    o.name = addStr(name);
    o.info = symInfo(g.linkerDefined ? STB_GLOBAL : g.bind, g.type == STT_NOTYPE && g.isCommon ? STT_OBJECT : g.type);
    o.shndx = g.isAbsolute ? SHN_ABS : (uint16_t)outIndex[g.isCommon ? (int)kBss : inputs_[g.section].out];
    o.value = g.addr;
    o.size = g.size;
    syms.push_back(o);
  }

  // ---- section headers ----
  std::string shstrtab(1, '\0');
  auto addSh = [&](const std::string& n) { uint32_t o = (uint32_t)shstrtab.size(); shstrtab += n; shstrtab += '\0'; return o; };
  std::vector<Shdr> shdrs(1, Shdr{});
  for (int k : present) {
    const OutputSection& o = outputs_[k];
    Shdr h{};
    h.name = addSh(o.name);
    h.type = o.type;
    h.flags = o.flags;
    h.addr = o.addr;
    h.offset = o.fileOffset;
    h.size = o.size;
    h.addralign = o.align;
    shdrs.push_back(h);
  }
  uint64_t off = alignUp(image.size(), 8);
  Shdr symH{};
  symH.name = addSh(".symtab");
  symH.type = SHT_SYMTAB;
  symH.offset = off;
  symH.size = syms.size() * sizeof(Sym);
  symH.link = (uint32_t)shdrs.size() + 1;   // .strtab comes right after
  symH.info = firstGlobal;                  // index of the first non-local symbol
  symH.addralign = 8;
  symH.entsize = sizeof(Sym);
  shdrs.push_back(symH);
  off += symH.size;
  Shdr strH{};
  strH.name = addSh(".strtab");
  strH.type = SHT_STRTAB;
  strH.offset = off;
  strH.size = strtab.size();
  strH.addralign = 1;
  shdrs.push_back(strH);
  off += strH.size;
  Shdr shsH{};
  shsH.name = addSh(".shstrtab");
  shsH.type = SHT_STRTAB;
  shsH.addralign = 1;
  shdrs.push_back(shsH);
  shdrs.back().offset = off;
  shdrs.back().size = shstrtab.size();
  off += shstrtab.size();
  uint64_t shoff = alignUp(off, 8);

  // ---- ELF header + program headers ----
  Ehdr eh{};
  std::memcpy(eh.ident, "\x7f" "ELF", 4);
  eh.ident[4] = 2;  // 64-bit
  eh.ident[5] = 1;  // little-endian
  eh.ident[6] = 1;  // ELF version
  eh.type = ET_EXEC;
  eh.machine = EM_X86_64;
  eh.version = 1;
  eh.entry = entryAddr_;
  eh.phoff = sizeof(Ehdr);
  eh.shoff = shoff;
  eh.ehsize = sizeof(Ehdr);
  eh.phentsize = sizeof(Phdr);
  eh.phnum = 3;
  eh.shentsize = sizeof(Shdr);
  eh.shnum = (uint16_t)shdrs.size();
  eh.shstrndx = (uint16_t)shdrs.size() - 1;

  Phdr ph[3]{};
  ph[0].type = PT_LOAD;                       // code + read-only data, and the headers
  ph[0].flags = PF_R | PF_X;
  ph[0].offset = 0;
  ph[0].vaddr = ph[0].paddr = opt_.baseAddress;
  ph[0].filesz = ph[0].memsz = rxEnd_;
  ph[0].align = kPage;
  ph[1].type = PT_LOAD;                       // writable data + bss
  ph[1].flags = PF_R | PF_W;
  ph[1].offset = rwOffset_;
  ph[1].vaddr = ph[1].paddr = opt_.baseAddress + rwOffset_;
  ph[1].filesz = rwFileEnd_ - rwOffset_;
  uint64_t rwEnd = std::max(opt_.baseAddress + rwFileEnd_, outputs_[kBss].addr + outputs_[kBss].size);
  ph[1].memsz = rwEnd - ph[1].vaddr;
  ph[1].align = kPage;
  ph[2].type = PT_GNU_STACK;                  // ask for a non-executable stack
  ph[2].flags = PF_R | PF_W;
  ph[2].align = 16;
  std::memcpy(image.data(), &eh, sizeof eh);
  std::memcpy(image.data() + sizeof eh, ph, sizeof ph);

  // ---- write everything ----
  std::ofstream os(opt_.output, std::ios::binary | std::ios::trunc);
  if (!os) throw LinkError("cannot write '" + opt_.output + "'");
  os.write(reinterpret_cast<const char*>(image.data()), (std::streamsize)image.size());
  std::string pad(alignUp(image.size(), 8) - image.size(), '\0');
  os << pad;
  os.write(reinterpret_cast<const char*>(syms.data()), (std::streamsize)(syms.size() * sizeof(Sym)));
  os << strtab << shstrtab;
  os << std::string(shoff - off, '\0');
  os.write(reinterpret_cast<const char*>(shdrs.data()), (std::streamsize)(shdrs.size() * sizeof(Shdr)));
  os.close();
  ::chmod(opt_.output.c_str(), 0755);
}

void Linker::link() {
  if (files_.empty()) throw LinkError("no input files");
  collectSections();
  resolveSymbols();
  layout();
  writeExecutable();
}

void Linker::writeMap(std::ostream& os) const {
  char buf[256];
  os << "Memory map (entry " << opt_.entry << " at ";
  std::snprintf(buf, sizeof buf, "0x%" PRIx64 ")\n\n", entryAddr_);
  os << buf;
  for (const auto& out : outputs_) {
    std::snprintf(buf, sizeof buf, "%-10s 0x%08" PRIx64 "  size 0x%-6" PRIx64 " align %" PRIu64 "\n", out.name.c_str(),
                  out.addr, out.size, out.align);
    os << buf;
    for (int m : out.members) {
      const InputSection& in = inputs_[m];
      const Section& s = in.file->sections()[in.index];
      std::snprintf(buf, sizeof buf, "    0x%08" PRIx64 "  0x%-6" PRIx64 " %s(%s)\n", in.addr(), s.hdr.size,
                    in.file->path().c_str(), s.name.c_str());
      os << buf;
    }
  }
  os << "\nGlobal symbols:\n";
  std::vector<const Global*> gs;
  for (const auto& name : globalOrder_)
    if (globals_.at(name).file || globals_.at(name).linkerDefined) gs.push_back(&globals_.at(name));
  std::sort(gs.begin(), gs.end(), [](const Global* a, const Global* b) { return a->addr < b->addr; });
  for (const Global* g : gs) {
    std::snprintf(buf, sizeof buf, "    0x%08" PRIx64 "  %-6s %-24s %s\n", g->addr, g->bind == STB_WEAK ? "weak" : "",
                  g->name.c_str(), g->file ? g->file->path().c_str() : "(linker defined)");
    os << buf;
  }
}
