#include "sys.h"
int shared_counter;              // second tentative definition: merged with common.c's
extern int shared_array[10];
int main(void) {
    for (int i = 0; i < 10; i++) { shared_array[i] = i; shared_counter += shared_array[i]; }
    print_str("shared_counter = "); print_int(shared_counter); print_str("\n");
    return shared_counter;   // 45
}
