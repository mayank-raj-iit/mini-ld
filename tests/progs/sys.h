// sys.h - tiny "standard library" with raw Linux system calls (no libc).
#pragma once
long sys_write(int fd, const void* buf, long n);
void print_str(const char* s);
void print_int(long v);
void print_hex(unsigned long v);
long str_len(const char* s);
