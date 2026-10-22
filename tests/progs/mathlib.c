// mathlib.c - functions and data used from main.c
int counter = 5;                         // .data  (initialised global)
int scratch[64];                         // .bss   (zero-initialised global)
const int squares[8] = {0, 1, 4, 9, 16, 25, 36, 49};   // .rodata
long big_table[4] = {1L << 40, -3, 7, 0x1234567890L};  // .data, 8-byte values

static int helper(int x) { return x * 3; }   // local function: not visible to other files

int add(int a, int b) { counter++; return a + b; }
int mul(int a, int b) { counter++; return helper(a) * b / 3; }

long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// weak default: main.c may override it
__attribute__((weak)) const char* greeting(void) { return "hello from the weak default"; }
// weak default that nobody overrides
__attribute__((weak)) int answer(void) { return 42; }

int fill_scratch(int n) {
    int sum = 0;
    for (int i = 0; i < n && i < 64; i++) { scratch[i] = i * i; sum += scratch[i]; }
    return sum;
}
