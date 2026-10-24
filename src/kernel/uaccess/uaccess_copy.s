.intel_syntax noprefix

.global user_copy_bytes
.type user_copy_bytes,@function
.global user_copy_bytes_lo
user_copy_bytes_lo:

#include "percpu_off.h"

user_copy_bytes:
    test rdx, rdx
    jz .done
    cld
.loop:
    mov r8, gs:PERCPU_COPY_ABORT_OFF
    test r8, r8
    jnz .done
    movsb
    dec rdx
    jnz .loop
.done:
    ret
.global user_copy_bytes_hi
user_copy_bytes_hi:

.size user_copy_bytes, . - user_copy_bytes

.global user_copy_str
.type user_copy_str,@function
.global user_copy_str_lo
user_copy_str_lo:

user_copy_str:
    test rdx, rdx
    jz .done2
    cld
.loop2:
    mov r8, gs:PERCPU_COPY_ABORT_OFF
    test r8, r8
    jnz .done2
    lodsb
    stosb
    test al, al
    je .done2
    dec rdx
    jnz .loop2
.done2:
    ret
.global user_copy_str_hi
user_copy_str_hi:

.size user_copy_str, . - user_copy_str