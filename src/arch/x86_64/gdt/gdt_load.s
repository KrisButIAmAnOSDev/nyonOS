.intel_syntax noprefix

.extern gdt_tss_desc

.global gdt_load
.type gdt_load,@function
gdt_load:
    cli
    lgdt [rdi]

    push 0x08
    lea rax, [rip + .reload_cs]
    push rax
    retfq

.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rax, offset gdt_tss_desc
    and byte ptr [rax + 5], 0xfd
    mov ax, 0x28
    ltr ax
    ret
