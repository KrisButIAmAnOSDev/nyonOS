.global get_stub_table
get_stub_table:
    movabs $isr_stub_table, %rax
    ret