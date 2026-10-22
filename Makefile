# Build:  make        Test:  make test
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

all: minild elfinspect

minild: build/minild.o build/linker.o build/elf_file.o
	$(CXX) $(CXXFLAGS) -o $@ $^

elfinspect: build/elfinspect.o build/elf_file.o
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp $(wildcard src/*.hpp)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	python3 tests/run_tests.py

clean:
	rm -rf build minild elfinspect tests/out

.PHONY: all test clean
