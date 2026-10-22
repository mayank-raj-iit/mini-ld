// linker.hpp - a static linker for x86-64 Linux ELF object files.
#pragma once
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "elf_file.hpp"

struct LinkError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct LinkOptions {
  std::string output = "a.out";
  std::string entry = "_start";
  uint64_t baseAddress = 0x400000;  // where the executable is loaded (classic non-PIE address)
  bool printMap = false;            // print a link map (like ld -Map)
};

class Linker {
 public:
  explicit Linker(LinkOptions opt) : opt_(std::move(opt)) {}
  void addObject(const std::string& path);   // throws ElfError / LinkError
  void link();                               // throws LinkError
  void writeMap(std::ostream& os) const;

 private:
  // One section from one input file that ends up in the output.
  struct InputSection {
    const ElfFile* file;
    int index;                  // section index inside that file
    int out;                    // output section it goes into
    uint64_t offset = 0;        // offset inside the output section
    uint64_t addr() const;      // final virtual address (needs outputs_ laid out)
    const Linker* linker;
  };
  // An output section: all input sections of one kind, glued together.
  struct OutputSection {
    std::string name;
    uint32_t type;
    uint64_t flags;
    uint64_t align = 1;
    uint64_t size = 0;
    uint64_t addr = 0;
    uint64_t fileOffset = 0;
    std::vector<int> members;   // indexes into inputs_
  };
  // A resolved global (or weak) symbol.
  struct Global {
    std::string name;
    const ElfFile* file = nullptr;  // defining file (nullptr = still undefined)
    int section = -1;               // input-section index into inputs_, -1 for absolute/common
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t bind = 0, type = 0;
    bool isAbsolute = false, isCommon = false;
    bool linkerDefined = false;     // _end, _etext, _GLOBAL_OFFSET_TABLE_ ... (provided by the linker)
    uint64_t commonAlign = 1;
    uint64_t addr = 0;              // final address
    std::string firstReference;     // a file that used it (for error messages)
  };

  LinkOptions opt_;
  std::vector<std::unique_ptr<ElfFile>> files_;
  std::vector<InputSection> inputs_;
  std::vector<OutputSection> outputs_;
  std::unordered_map<const ElfFile*, std::unordered_map<int, int>> inputOf_;  // (file, section) -> inputs_ index
  std::unordered_map<std::string, Global> globals_;
  std::vector<std::string> globalOrder_;
  uint64_t entryAddr_ = 0;
  uint64_t rxEnd_ = 0, rwFileEnd_ = 0, rwOffset_ = 0;

  enum { kText, kRodata, kData, kBss };
  void collectSections();
  void resolveSymbols();
  void layout();
  uint64_t symbolAddress(const ElfFile& f, const Symbol& s) const;
  void applyRelocations(std::vector<uint8_t>& image);
  void writeExecutable();
};
