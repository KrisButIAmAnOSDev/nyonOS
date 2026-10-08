.intel_syntax noprefix

.global user_copy_bytes
.type user_copy_bytes,@function

user_copy_bytes:
    test rdx, rdx
    jz .done
    cld
.loop:
    movsb
    dec rdx
    jnz .loop
.done:
    ret

.size user_copy_bytes, . - user_copy_bytes
