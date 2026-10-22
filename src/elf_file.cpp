// elf_file.cpp - parse an ELF64 little-endian file.
#include "elf_file.hpp"

#include <fstream>
#include <iterator>

using namespace elf;

ElfFile::ElfFile(const std::string& path) : path_(path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ElfError("cannot open '" + path + "'");
  bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

  // --- ELF header ---
  if (bytes_.size() < 4 || std::memcmp(bytes_.data(), "\x7f" "ELF", 4) != 0)
    throw ElfError(path + ": not an ELF file (bad magic number)");
  ehdr_ = read<Ehdr>(0, "ELF header");
  if (ehdr_.ident[4] != 2) throw ElfError(path + ": only 64-bit ELF (ELFCLASS64) is supported");
  if (ehdr_.ident[5] != 1) throw ElfError(path + ": only little-endian ELF is supported");
  if (ehdr_.shnum && ehdr_.shentsize != sizeof(Shdr)) throw ElfError(path + ": unexpected section header size");
  if (ehdr_.phnum && ehdr_.phentsize != sizeof(Phdr)) throw ElfError(path + ": unexpected program header size");

  // --- program headers (segments), present in executables ---
  for (int i = 0; i < ehdr_.phnum; i++) phdrs_.push_back(read<Phdr>(ehdr_.phoff + i * sizeof(Phdr), "program header"));

  // --- section headers ---
  for (int i = 0; i < ehdr_.shnum; i++) {
    Section s;
    s.index = i;
    s.hdr = read<Shdr>(ehdr_.shoff + i * sizeof(Shdr), "section header");
    if (s.hdr.type != SHT_NOBITS && s.hdr.type != SHT_NULL) {
      if (s.hdr.offset > bytes_.size() || s.hdr.size > bytes_.size() - s.hdr.offset)
        throw ElfError(path + ": section " + std::to_string(i) + " lies outside the file");
      s.data = bytes_.data() + s.hdr.offset;
    }
    sections_.push_back(s);
  }
  // section names live in the section-name string table (.shstrtab)
  if (ehdr_.shnum) {
    if (ehdr_.shstrndx >= sections_.size()) throw ElfError(path + ": bad section name table index");
    const Section& names = sections_[ehdr_.shstrndx];
    for (auto& s : sections_) s.name = cstring(names, s.hdr.name);
  }

  // --- symbol table ---
  for (const auto& s : sections_) {
    if (s.hdr.type != SHT_SYMTAB) continue;
    if (s.hdr.link >= sections_.size()) throw ElfError(path + ": symbol table has a bad string table link");
    const Section& strtab = sections_[s.hdr.link];
    size_t n = s.hdr.size / sizeof(Sym);
    for (size_t i = 0; i < n; i++) {
      Symbol sym;
      sym.index = (int)i;
      sym.sym = read<Sym>(s.hdr.offset + i * sizeof(Sym), "symbol");
      sym.name = cstring(strtab, sym.sym.name);
      if (sym.type() == STT_SECTION && sym.sym.shndx < sections_.size()) sym.name = sections_[sym.sym.shndx].name;
      symbols_.push_back(sym);
    }
    break;  // an object file has a single .symtab
  }

  // --- relocation sections (.rela.text, .rela.data, ...) ---
  for (const auto& s : sections_) {
    if (s.hdr.type == SHT_REL) throw ElfError(path + ": SHT_REL relocations (without addend) are not supported");
    if (s.hdr.type != SHT_RELA) continue;
    RelocSection rs;
    rs.sectionIndex = s.index;
    rs.targetSection = (int)s.hdr.info;
    size_t n = s.hdr.size / sizeof(Rela);
    for (size_t i = 0; i < n; i++) {
      Relocation r;
      r.rela = read<Rela>(s.hdr.offset + i * sizeof(Rela), "relocation");
      if (r.symIndex() >= symbols_.size()) throw ElfError(path + ": relocation refers to a missing symbol");
      rs.relocs.push_back(r);
    }
    relocs_.push_back(std::move(rs));
  }
}

std::string ElfFile::cstring(const Section& strtab, uint32_t offset) const {
  if (!strtab.data || offset >= strtab.hdr.size) return "";
  const char* p = reinterpret_cast<const char*>(strtab.data) + offset;
  size_t maxLen = strtab.hdr.size - offset;
  size_t len = 0;
  while (len < maxLen && p[len] != '\0') len++;  // stay inside the section even if it lacks a final '\0'
  return std::string(p, len);
}

const Section* ElfFile::findSection(const std::string& name) const {
  for (const auto& s : sections_)
    if (s.name == name) return &s;
  return nullptr;
}

const char* sectionTypeName(uint32_t t) {
  switch (t) {
    case SHT_NULL: return "NULL";
    case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB: return "SYMTAB";
    case SHT_STRTAB: return "STRTAB";
    case SHT_RELA: return "RELA";
    case SHT_HASH: return "HASH";
    case SHT_DYNAMIC: return "DYNAMIC";
    case SHT_NOTE: return "NOTE";
    case SHT_NOBITS: return "NOBITS";
    case SHT_REL: return "REL";
    case SHT_DYNSYM: return "DYNSYM";
    case SHT_INIT_ARRAY: return "INIT_ARRAY";
    case SHT_FINI_ARRAY: return "FINI_ARRAY";
    case SHT_GROUP: return "GROUP";
    case SHT_X86_64_UNWIND: return "X86_64_UNWIND";
    default: return t >= 0x70000000 ? "LOPROC+" : "UNKNOWN";
  }
}

const char* machineName(uint16_t m) {
  switch (m) {
    case EM_X86_64: return "x86-64";
    case EM_AARCH64: return "AArch64";
    case EM_CUDA: return "NVIDIA CUDA";
    default: return "other";
  }
}

const char* fileTypeName(uint16_t t) {
  switch (t) {
    case ET_REL: return "REL (relocatable object file)";
    case ET_EXEC: return "EXEC (executable)";
    case ET_DYN: return "DYN (shared object / PIE executable)";
    default: return "unknown";
  }
}

const char* x86RelocName(uint32_t t) {
  switch (t) {
    case R_X86_64_NONE: return "R_X86_64_NONE";
    case R_X86_64_64: return "R_X86_64_64";
    case R_X86_64_PC32: return "R_X86_64_PC32";
    case R_X86_64_PLT32: return "R_X86_64_PLT32";
    case R_X86_64_GOTPCREL: return "R_X86_64_GOTPCREL";
    case R_X86_64_32: return "R_X86_64_32";
    case R_X86_64_32S: return "R_X86_64_32S";
    case R_X86_64_PC64: return "R_X86_64_PC64";
    case R_X86_64_GOTPCRELX: return "R_X86_64_GOTPCRELX";
    case R_X86_64_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
    default: return "R_X86_64_<other>";
  }
}

std::string sectionFlagsString(uint64_t f) {
  std::string s;
  if (f & SHF_WRITE) s += 'W';
  if (f & SHF_ALLOC) s += 'A';
  if (f & SHF_EXECINSTR) s += 'X';
  if (f & SHF_MERGE) s += 'M';
  if (f & SHF_STRINGS) s += 'S';
  if (f & SHF_INFO_LINK) s += 'I';
  if (f & SHF_GROUP) s += 'G';
  return s;
}
