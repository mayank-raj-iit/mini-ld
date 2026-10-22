// minild.cpp - command line for the linker.
//
//   minild start.o main.o lib.o -o prog [--map] [-e entry]
#include <iostream>

#include "linker.hpp"

int main(int argc, char** argv) {
  LinkOptions opt;
  std::vector<std::string> inputs;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if ((a == "-o" || a == "-e") && i + 1 < argc) {
      (a == "-o" ? opt.output : opt.entry) = argv[++i];
    } else if (a == "--map") {
      opt.printMap = true;
    } else if (a == "-h" || a == "--help") {
      std::cout << "usage: minild <file.o>... [-o output] [-e entry] [--map]\n";
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "minild: unknown option '" << a << "'\n";
      return 2;
    } else {
      inputs.push_back(a);
    }
  }
  try {
    Linker ld(opt);
    for (const auto& in : inputs) ld.addObject(in);
    ld.link();
    if (opt.printMap) ld.writeMap(std::cout);
  } catch (const std::exception& e) {
    std::cerr << "minild: error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
