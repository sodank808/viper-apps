/* Linker script for ELF app validation
 * This script mimics the ELF loader's memory layout to validate
 * that the app can be properly relocated at runtime.
 * 
 * Memory regions use virtual addresses that will be relocated
 * by the ELF loader at runtime.
 */

/* Define memory regions for ELF app sections */
MEMORY
{
    /* Code region: .text, .rodata, .data.rel.ro */
    /* Virtual address space - will be relocated at runtime */
    CODE_REGION : ORIGIN = 0x40000000, LENGTH = 512K
    
    /* Data region: .data, .bss */
    DATA_REGION : ORIGIN = 0x40080000, LENGTH = 256K
    
    /* Shared memory region: .shared_iram, .shared_dram */
    /* This maps to the 64KB shared memory pool */
    SHARED_MEM_REGION : ORIGIN = 0x400C0000, LENGTH = 64K
}

SECTIONS
{
    /* Code sections - will be loaded into CODE_REGION by ELF loader */
    .text : {
        *(.text)
        *(.text.*)
    } > CODE_REGION
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } > CODE_REGION
    
    .data.rel.ro : {
        *(.data.rel.ro)
        *(.data.rel.ro.*)
    } > CODE_REGION
    
    /* Data sections - will be loaded into DATA_REGION by ELF loader */
    .data : {
        *(.data)
        *(.data.*)
    } > DATA_REGION
    
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } > DATA_REGION
    
    /* Shared memory sections - will be loaded into SHARED_MEM_REGION by ELF loader */
    .shared_iram : {
        *(.shared_iram)
        *(.shared_iram.*)
    } > SHARED_MEM_REGION
    
    .shared_dram : {
        *(.shared_dram)
        *(.shared_dram.*)
    } > SHARED_MEM_REGION
    
    /* Other standard sections */
    .got : {
        *(.got)
        *(.got.*)
    } > CODE_REGION
    
    .got.plt : {
        *(.got.plt)
    } > CODE_REGION
    
    .dynamic : {
        *(.dynamic)
    } > CODE_REGION
    
    /* Discard other sections for validation */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.debug.*)
    }
}



