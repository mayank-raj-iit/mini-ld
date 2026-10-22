// elf_file.hpp - read an ELF64 file into memory and give easy access to it.
#pragma once
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "elf.hpp"

struct ElfError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Section {
  int index = 0;
  std::string name;
  elf::Shdr hdr{};
  const uint8_t* data = nullptr;  // nullptr for SHT_NOBITS (.bss has no bytes in the file)
};

struct Symbol {
  int index = 0;
  std::string name;
  elf::Sym sym{};
  uint8_t bind() const { return elf::symBind(sym.info); }
  uint8_t type() const { return elf::symType(sym.info); }
  bool isUndefined() const { return sym.shndx == elf::SHN_UNDEF; }
};

struct Relocation {
  elf::Rela rela{};
  uint32_t symIndex() const { return elf::relSym(rela.info); }
  uint32_t type() const { return elf::relType(rela.info); }
};

struct RelocSection {
  int sectionIndex = 0;           // the .rela.* section itself
  int targetSection = 0;          // the section these relocations patch (sh_info)
  std::vector<Relocation> relocs;
};

class ElfFile {
 public:
  explicit ElfFile(const std::string& path);  // throws ElfError with a clear message

  const std::string& path() const { return path_; }
  const elf::Ehdr& header() const { return ehdr_; }
  const std::vector<Section>& sections() const { return sections_; }
  const std::vector<elf::Phdr>& segments() const { return phdrs_; }
  const std::vector<Symbol>& symbols() const { return symbols_; }
  const std::vector<RelocSection>& relocations() const { return relocs_; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  const Section* findSection(const std::string& name) const;

 private:
  std::string path_;
  std::vector<uint8_t> bytes_;
  elf::Ehdr ehdr_{};
  std::vector<Section> sections_;
  std::vector<elf::Phdr> phdrs_;
  std::vector<Symbol> symbols_;
  std::vector<RelocSection> relocs_;

  // Copy a struct out of the byte buffer after checking it is inside the file.
  template <class T>
  T read(uint64_t offset, const char* what) const {
    if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset)
      throw ElfError(path_ + ": truncated file while reading " + what);
    T v;
    std::memcpy(&v, bytes_.data() + offset, sizeof(T));
    return v;
  }
  std::string cstring(const Section& strtab, uint32_t offset) const;
};

// Human-readable names for the inspector.
const char* sectionTypeName(uint32_t type);
const char* machineName(uint16_t m);
const char* fileTypeName(uint16_t t);
const char* x86RelocName(uint32_t t);
std::string sectionFlagsString(uint64_t flags);
