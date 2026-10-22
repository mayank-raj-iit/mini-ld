// main.c - exercises everything the linker has to get right.
#include "sys.h"

extern int counter;              // defined in mathlib.c (.data)
extern int scratch[64];          // defined in mathlib.c (.bss)
extern const int squares[8];     // defined in mathlib.c (.rodata)
extern long big_table[4];
int add(int a, int b);
int mul(int a, int b);
long fib(int n);
const char* greeting(void);
int answer(void);
int fill_scratch(int n);

// strong definition overrides the weak one in mathlib.c
const char* greeting(void) { return "hello from main.c (strong)"; }

typedef int (*binop)(int, int);
binop ops[2] = {add, mul};       // function pointers in data: needs absolute (R_X86_64_64) relocations
const char* names[2] = {"add", "mul"};

static int calls;                // local .bss variable
int next_id(void) { static int id = 100; return id++ + (calls++ * 0); }   // static local in .data

int main(int argc, char** argv) {
    (void)argv;
    print_str(greeting()); print_str("\n");
    for (int i = 0; i < 2; i++) {
        print_str(names[i]); print_str("(6, 7) = "); print_int(ops[i](6, 7)); print_str("\n");
    }
    print_str("fib(20) = "); print_int(fib(20)); print_str("\n");
    print_str("squares[7] = "); print_int(squares[7]); print_str("\n");
    print_str("sum of scratch = "); print_int(fill_scratch(10)); print_str("\n");
    print_str("scratch[9] = "); print_int(scratch[9]); print_str("\n");
    print_str("big_table[0] = "); print_int(big_table[0]); print_str("\n");
    print_str("big_table[3] = "); print_hex((unsigned long)big_table[3]); print_str("\n");
    print_str("answer() = "); print_int(answer()); print_str("\n");
    print_str("ids: "); print_int(next_id()); print_str(" "); print_int(next_id()); print_str("\n");
    print_str("counter = "); print_int(counter); print_str("\n");
    print_str("argc = "); print_int(argc); print_str("\n");
    return counter + 30;   // exit code 37
}
