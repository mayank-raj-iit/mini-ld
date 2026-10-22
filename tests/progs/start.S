# start.S - program entry point. The kernel jumps here with the stack holding
# argc, argv... We align the stack, call main(argc, argv), then exit(main's result).
    .text
    .globl _start
    .type _start, @function
_start:
    xor     %ebp, %ebp          # mark the outermost stack frame
    mov     (%rsp), %rdi        # argc
    lea     8(%rsp), %rsi       # argv
    and     $-16, %rsp          # the ABI wants a 16-byte aligned stack before a call
    call    main
    mov     %eax, %edi          # exit code = main's return value
    mov     $60, %eax           # syscall number 60 = exit
    syscall
    .section .note.GNU-stack,"",@progbits
