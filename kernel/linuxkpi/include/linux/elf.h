/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
typedef u64 Elf64_Addr; typedef u16 Elf64_Half; typedef u64 Elf64_Off; typedef s32 Elf64_Sword; typedef u32 Elf64_Word; typedef u64 Elf64_Xword;
#define EI_NIDENT 16
typedef struct elf64_hdr { unsigned char e_ident[EI_NIDENT]; Elf64_Half e_type, e_machine; Elf64_Word e_version; Elf64_Addr e_entry;
	Elf64_Off e_phoff, e_shoff; Elf64_Word e_flags; Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct elf64_shdr { Elf64_Word sh_name, sh_type; Elf64_Xword sh_flags; Elf64_Addr sh_addr; Elf64_Off sh_offset; Elf64_Xword sh_size;
	Elf64_Word sh_link, sh_info; Elf64_Xword sh_addralign, sh_entsize; } Elf64_Shdr;
typedef struct elf64_phdr { Elf64_Word p_type, p_flags; Elf64_Off p_offset; Elf64_Addr p_vaddr, p_paddr; Elf64_Xword p_filesz, p_memsz, p_align; } Elf64_Phdr;
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
