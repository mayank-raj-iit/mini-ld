#include "sys.h"

long sys_write(int fd, const void* buf, long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(1L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return ret;
}

long str_len(const char* s) {
    long n = 0;
    while (s[n]) n++;
    return n;
}

void print_str(const char* s) { sys_write(1, s, str_len(s)); }

void print_int(long v) {
    char buf[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
    buf[i] = '\0';
    do { buf[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    print_str(buf + i);
}

static const char hexdigits[] = "0123456789abcdef";   /* .rodata, used through a local symbol */

void print_hex(unsigned long v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int k = 0; k < 16; k++) buf[2 + k] = hexdigits[(v >> (60 - 4 * k)) & 0xf];
    buf[18] = '\0';
    print_str(buf);
}
