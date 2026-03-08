#ifndef _ELF_H_
#define _ELF_H_

#include "util/types.h"
#include "process.h"

#define MAX_CMDLINE_ARGS 64

typedef struct elf_header_t {
  uint32 magic;
  uint8 elf[12];
  uint16 type;
  uint16 machine;
  uint32 version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint32 flags;
  uint16 ehsize;
  uint16 phentsize;
  uint16 phnum;
  uint16 shentsize;
  uint16 shnum;
  uint16 shstrndx;
} elf_header;

#define SEGMENT_READABLE   0x4
#define SEGMENT_EXECUTABLE 0x1
#define SEGMENT_WRITABLE   0x2

typedef struct elf_prog_header_t {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
} elf_prog_header;

typedef struct elf_sect_header_t {
  uint32 name;
  uint32 type;
  uint64 flags;
  uint64 addr;
  uint64 offset;
  uint64 size;
  uint32 link;
  uint32 info;
  uint64 addralign;
  uint64 entsize;
} elf_sect_header;

typedef struct elf_symbol_t {
  uint32 name;
  uint8 info;
  uint8 other;
  uint16 shndx;
  uint64 value;
  uint64 size;
} elf_symbol;

typedef struct __attribute__((packed)) debug_header_t {
  uint32 length;
  uint16 version;
  uint32 header_length;
  uint8 min_instruction_length;
  uint8 default_is_stmt;
  int8 line_base;
  uint8 line_range;
  uint8 opcode_base;
  uint8 std_opcode_lengths[12];
} debug_header;

#define ELF_MAGIC 0x464C457FU
#define ELF_PROG_LOAD 1
#define SHT_SYMTAB 2

#define STT_FUNC 2

typedef enum elf_status_t {
  EL_OK = 0,
  EL_EIO,
  EL_ENOMEM,
  EL_NOTELF,
  EL_ERR,
} elf_status;

typedef struct elf_ctx_t {
  void *info;
  elf_header ehdr;
} elf_ctx;

elf_status elf_init(elf_ctx *ctx, void *info);
elf_status elf_load(elf_ctx *ctx);

void load_bincode_from_host_elf(process *p, char *filename);
const char *find_symbol_name(uint64 addr);
void print_runtime_error(uint64 fault_pc);

#endif
