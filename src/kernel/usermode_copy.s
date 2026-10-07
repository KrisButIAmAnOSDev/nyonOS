.intel_syntax noprefix

.global user_copy_bytes
.type user_copy_bytes,@function

# user_copy_bytes(rdi = dst, rsi = src, rdx = len)
# byte at a time so a fault always lands on exactly one 1-byte instruction,
# which is what lets the exception handler skip it instead of looping.
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
