.intel_syntax noprefix

.global user_copy_bytes
.type user_copy_bytes,@function

.extern copy_abort

user_copy_bytes:
    test rdx, rdx
    jz .done
    cld
.loop:
    mov r8, [rip + copy_abort]
    test r8, r8
    jnz .done
    movsb
    dec rdx
    jnz .loop
.done:
    ret

.size user_copy_bytes, . - user_copy_bytes
