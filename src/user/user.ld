ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0;

    .text : ALIGN(4096) {
        *(.text.start)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
    } :rodata

    .data : {
        *(.data .data.*)
    } :data

    .got.plt : {
        __start_got = .;
        *(.got.plt)
        *(.got)
        __stop_got = .;
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(16);
    _end = .;
}
