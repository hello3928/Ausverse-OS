ENTRY(_start)

SECTIONS
{
    /* Load at 1 MiB — standard for 32-bit kernels */
    . = 1M;

    .boot :
    {
        /* Multiboot header must be in the first 32 KiB of the image */
        KEEP(*(.multiboot_header))
    }

    .text :
    {
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    kernel_end = .;
}
