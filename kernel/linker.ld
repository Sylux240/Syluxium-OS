/* Syluxium kernel linker script (x86_64, higher half, loaded by Limine) */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

/* Separate segments so Limine maps each with the right permissions:
 * text = R-X, rodata = R--, data = RW- */
PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* Top 2 GiB of the address space, required by -mcmodel=kernel. */
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    /* module_init() functions of Linux drivers (LinuxKPI). */
    .lkpi_initcalls : {
        __lkpi_initcalls_start = .;
        KEEP(*(SORT(.lkpi_initcalls*)))
        __lkpi_initcalls_end = .;
    } :data

    /* module_param()s of Linux drivers (set from the command line). */
    .lkpi_params : {
        __lkpi_params_start = .;
        KEEP(*(.lkpi_params))
        __lkpi_params_end = .;
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data
    __data_end = .;

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
