/*
 * routines that scan and load an ELF file into memory.
 */

#include "elf.h"
#include "util/string.h"
#include "util/functions.h"
#include "riscv.h"
#include "vmm.h"
#include "pmm.h"
#include "sync_utils.h"
#include "spike_interface/spike_utils.h"
#include "spike_interface/spike_file.h"

typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

typedef struct code_file_t {
  uint64 dir;
  char *file;
} code_file;

typedef struct addr_line_t {
  uint64 addr;
  uint64 line;
  uint64 file;
} addr_line;

#define DEBUG_LINE_STORE_SIZE (1 << 20)
#define SHSTRTAB_STORE_SIZE   (1 << 15)
#define SYMTAB_STORE_SIZE     (1 << 18)
#define STRTAB_STORE_SIZE     (1 << 18)
#define MAX_DEBUG_DIRS 64
#define MAX_DEBUG_FILES 64
#define MAX_ADDR_LINES 4096
#define INVALID_DIR_INDEX ((uint64)-1)

static char g_debug_line_store[DEBUG_LINE_STORE_SIZE];
static char g_shstrtab_store[SHSTRTAB_STORE_SIZE];
static char g_symtab_store[SYMTAB_STORE_SIZE];
static char g_strtab_store[STRTAB_STORE_SIZE];
static char *g_debug_dirs[MAX_DEBUG_DIRS];
static code_file g_debug_files[MAX_DEBUG_FILES];
static addr_line g_addr_lines[MAX_ADDR_LINES];
static int g_dir_count;
static int g_file_count;
static int g_addr_line_count;
static elf_symbol *g_symbols;
static uint64 g_symbol_count;
static uint64 g_strtab_size;

static void reset_runtime_debug_info(void) {
  memset(g_debug_dirs, 0, sizeof(g_debug_dirs));
  memset(g_debug_files, 0, sizeof(g_debug_files));
  memset(g_addr_lines, 0, sizeof(g_addr_lines));
  g_dir_count = 0;
  g_file_count = 0;
  g_addr_line_count = 0;
  g_symbols = 0;
  g_symbol_count = 0;
  g_strtab_size = 0;
}

static void *elf_alloc_segment(elf_ctx *ctx, uint64 elf_va, uint64 size, uint64 perm) {
  elf_info *msg = (elf_info *)ctx->info;
  uint64 va_start = ROUNDDOWN(elf_va, PGSIZE);
  uint64 va_end = ROUNDUP(elf_va + size, PGSIZE);

  for (uint64 va = va_start; va < va_end; va += PGSIZE) {
    void *pa = alloc_page();
    if (pa == 0) panic("elf segment alloc failed\n");
    memset(pa, 0, PGSIZE);
    user_vm_map((pagetable_t)msg->p->pagetable, va, PGSIZE, (uint64)pa, perm);
  }

  return user_va_to_pa((pagetable_t)msg->p->pagetable, (void *)elf_va);
}

static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  return spike_file_pread(msg->f, dest, nb, offset);
}

elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;
  return EL_OK;
}

static void read_uleb128(uint64 *out, char **off) {
  uint64 value = 0;
  int shift = 0;
  uint8 byte;

  for (;;) {
    byte = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64)byte & 0x7F) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) break;
  }
  if (out) *out = value;
}

static void read_sleb128(int64 *out, char **off) {
  int64 value = 0;
  int shift = 0;
  uint8 byte;

  for (;;) {
    byte = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64)byte & 0x7F) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) break;
  }
  if (shift < 64 && (byte & 0x40)) value |= -(1L << shift);
  if (out) *out = value;
}

static void read_uint16(uint16 *out, char **off) {
  *out = 0;
  for (int i = 0; i < 2; i++) {
    *out |= (uint16)(uint8)(**off) << (i << 3);
    (*off)++;
  }
}

static void read_uint64(uint64 *out, char **off) {
  *out = 0;
  for (int i = 0; i < 8; i++) {
    *out |= (uint64)(uint8)(**off) << (i << 3);
    (*off)++;
  }
}

static void append_addr_line(addr_line entry, int file_base) {
  if (g_addr_line_count >= MAX_ADDR_LINES) return;
  if (g_addr_line_count > 0 && g_addr_lines[g_addr_line_count - 1].addr == entry.addr)
    g_addr_line_count--;

  entry.file += file_base - 1;
  g_addr_lines[g_addr_line_count++] = entry;
}

static void make_addr_line(char *debug_line, uint64 length) {
  char *off = debug_line;

  while (off < debug_line + length) {
    debug_header *dh = (debug_header *)off;
    int dir_base = g_dir_count;
    int file_base = g_file_count;
    off += sizeof(debug_header);

    while (*off != 0) {
      if (g_dir_count < MAX_DEBUG_DIRS) g_debug_dirs[g_dir_count++] = off;
      while (*off != 0) off++;
      off++;
    }
    off++;

    while (*off != 0) {
      char *file_name = off;
      while (*off != 0) off++;
      off++;

      uint64 dir = 0;
      read_uleb128(&dir, &off);
      read_uleb128(NULL, &off);
      read_uleb128(NULL, &off);

      if (g_file_count < MAX_DEBUG_FILES) {
        g_debug_files[g_file_count].file = file_name;
        g_debug_files[g_file_count].dir = dir == 0 ? INVALID_DIR_INDEX : dir - 1 + dir_base;
        g_file_count++;
      }
    }
    off++;

    addr_line regs;
    regs.addr = 0;
    regs.file = 1;
    regs.line = 1;

    for (;;) {
      uint8 op = *(off++);
      switch (op) {
        case 0: {
          read_uleb128(NULL, &off);
          op = *(off++);
          switch (op) {
            case 1:
              append_addr_line(regs, file_base);
              goto end_of_cu;
            case 2:
              read_uint64(&regs.addr, &off);
              break;
            case 4:
              read_uleb128(NULL, &off);
              break;
            default:
              break;
          }
          break;
        }
        case 1:
          append_addr_line(regs, file_base);
          break;
        case 2: {
          uint64 delta;
          read_uleb128(&delta, &off);
          regs.addr += delta * dh->min_instruction_length;
          break;
        }
        case 3: {
          int64 delta;
          read_sleb128(&delta, &off);
          regs.line += delta;
          break;
        }
        case 4:
          read_uleb128(&regs.file, &off);
          break;
        case 5:
          read_uleb128(NULL, &off);
          break;
        case 6:
        case 7:
          break;
        case 8: {
          int adjust = 255 - dh->opcode_base;
          regs.addr += (adjust / dh->line_range) * dh->min_instruction_length;
          break;
        }
        case 9: {
          uint16 delta;
          read_uint16(&delta, &off);
          regs.addr += delta;
          break;
        }
        default: {
          int adjust = op - dh->opcode_base;
          int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
          int line_delta = dh->line_base + (adjust % dh->line_range);
          regs.addr += addr_delta;
          regs.line += line_delta;
          append_addr_line(regs, file_base);
          break;
        }
      }
    }
end_of_cu:;
  }
}

static elf_status load_elf_runtime_sections(elf_ctx *ctx) {
  if (ctx->ehdr.shoff == 0 || ctx->ehdr.shnum == 0) return EL_OK;
  if (ctx->ehdr.shstrndx >= ctx->ehdr.shnum) return EL_ERR;

  elf_sect_header shstr_hdr;
  uint64 shstr_off = ctx->ehdr.shoff + (uint64)ctx->ehdr.shstrndx * sizeof(elf_sect_header);
  if (elf_fpread(ctx, &shstr_hdr, sizeof(shstr_hdr), shstr_off) != sizeof(shstr_hdr)) return EL_EIO;
  if (shstr_hdr.size > SHSTRTAB_STORE_SIZE) return EL_ERR;
  if (elf_fpread(ctx, g_shstrtab_store, shstr_hdr.size, shstr_hdr.offset) != shstr_hdr.size)
    return EL_EIO;

  elf_sect_header symtab_hdr;
  int symtab_found = 0;

  for (int i = 0; i < ctx->ehdr.shnum; i++) {
    elf_sect_header shdr;
    uint64 section_off = ctx->ehdr.shoff + (uint64)i * sizeof(elf_sect_header);
    if (elf_fpread(ctx, &shdr, sizeof(shdr), section_off) != sizeof(shdr)) return EL_EIO;
    if (shdr.name >= shstr_hdr.size) continue;

    char *section_name = g_shstrtab_store + shdr.name;
    if (strcmp(section_name, ".debug_line") == 0) {
      if (shdr.size > DEBUG_LINE_STORE_SIZE) return EL_ERR;
      if (elf_fpread(ctx, g_debug_line_store, shdr.size, shdr.offset) != shdr.size) return EL_EIO;
      make_addr_line(g_debug_line_store, shdr.size);
    } else if (strcmp(section_name, ".symtab") == 0) {
      symtab_hdr = shdr;
      symtab_found = 1;
    }
  }

  if (!symtab_found) return EL_OK;
  if (symtab_hdr.size > SYMTAB_STORE_SIZE) return EL_ERR;
  if (elf_fpread(ctx, g_symtab_store, symtab_hdr.size, symtab_hdr.offset) != symtab_hdr.size)
    return EL_EIO;

  elf_sect_header strtab_hdr;
  uint64 strtab_hdr_off = ctx->ehdr.shoff + (uint64)symtab_hdr.link * sizeof(elf_sect_header);
  if (elf_fpread(ctx, &strtab_hdr, sizeof(strtab_hdr), strtab_hdr_off) != sizeof(strtab_hdr))
    return EL_EIO;
  if (strtab_hdr.size > STRTAB_STORE_SIZE) return EL_ERR;
  if (elf_fpread(ctx, g_strtab_store, strtab_hdr.size, strtab_hdr.offset) != strtab_hdr.size)
    return EL_EIO;

  g_symbols = (elf_symbol *)g_symtab_store;
  g_symbol_count = symtab_hdr.size / sizeof(elf_symbol);
  g_strtab_size = strtab_hdr.size;
  return EL_OK;
}

elf_status elf_load(elf_ctx *ctx) {
  elf_prog_header ph_addr;

  for (int i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    uint64 perm = 0;
    if (ph_addr.flags & SEGMENT_READABLE) perm |= PROT_READ;
    if (ph_addr.flags & SEGMENT_WRITABLE) perm |= PROT_WRITE;
    if (ph_addr.flags & SEGMENT_EXECUTABLE) perm |= PROT_EXEC;

    process *proc = ((elf_info *)ctx->info)->p;
    uint64 seg_va = ROUNDDOWN(ph_addr.vaddr, PGSIZE);
    uint64 seg_sz = ROUNDUP(ph_addr.vaddr + ph_addr.memsz, PGSIZE) - seg_va;
    elf_alloc_segment(ctx, ph_addr.vaddr, ph_addr.memsz, prot_to_type(perm, 1));

    uint64 file_left = ph_addr.filesz;
    uint64 file_off = ph_addr.off;
    uint64 load_va = ph_addr.vaddr;
    while (file_left > 0) {
      uint64 page_off = load_va & (PGSIZE - 1);
      uint64 copy_bytes = file_left < PGSIZE - page_off ? file_left : PGSIZE - page_off;
      void *dest = user_va_to_pa((pagetable_t)proc->pagetable, (void *)load_va);
      if (dest == 0) return EL_ERR;
      if (elf_fpread(ctx, dest, copy_bytes, file_off) != copy_bytes) return EL_EIO;
      load_va += copy_bytes;
      file_off += copy_bytes;
      file_left -= copy_bytes;
    }

    int slot = proc->total_mapped_region;
    proc->mapped_info[slot].va = seg_va;
    proc->mapped_info[slot].npages = seg_sz / PGSIZE;

    if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_EXECUTABLE)) {
      proc->mapped_info[slot].seg_type = CODE_SEGMENT;
      if (proc->pid != 0) sprint("CODE_SEGMENT added at mapped info offset:%d\n", slot);
    } else if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_WRITABLE)) {
      proc->mapped_info[slot].seg_type = DATA_SEGMENT;
      if (proc->pid != 0) sprint("DATA_SEGMENT added at mapped info offset:%d\n", slot);
    } else {
      panic("unknown program segment encountered, segment flag:%d.\n", ph_addr.flags);
    }
    proc->total_mapped_region++;
  }

  return EL_OK;
}

const char *find_symbol_name(uint64 addr) {
  if (g_symbols == 0 || g_symbol_count == 0 || g_strtab_size == 0) return 0;

  uint64 pc = addr > 0 ? addr - 1 : addr;
  elf_symbol *best = 0;
  for (uint64 i = 0; i < g_symbol_count; i++) {
    elf_symbol *sym = &g_symbols[i];
    if ((sym->info & 0xF) != STT_FUNC) continue;
    if (sym->name >= g_strtab_size) continue;
    if (sym->value > pc) continue;
    if (best == 0 || sym->value > best->value) best = sym;
  }
  return best ? g_strtab_store + best->name : 0;
}

static int find_addr_line_entry(uint64 fault_pc, addr_line *entry) {
  int best = -1;
  for (int i = 0; i < g_addr_line_count; i++) {
    if (g_addr_lines[i].addr > fault_pc) break;
    best = i;
  }
  if (best < 0) return 0;
  *entry = g_addr_lines[best];
  return 1;
}

static void print_source_line(const char *path, uint64 target_line) {
  if (!path || target_line == 0) return;

  spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) return;

  char buf[256];
  char source_line[256];
  uint64 line_no = 1;
  uint64 line_pos = 0;
  uint64 offset = 0;
  int found = 0;

  for (;;) {
    ssize_t n = spike_file_pread(f, buf, sizeof(buf), offset);
    if (n <= 0) break;
    offset += n;

    for (ssize_t i = 0; i < n; i++) {
      char ch = buf[i];
      if (line_no == target_line && ch != '\n' && ch != '\r' && line_pos < sizeof(source_line) - 1)
        source_line[line_pos++] = ch;

      if (ch == '\n') {
        if (line_no == target_line) {
          found = 1;
          break;
        }
        line_no++;
      }
    }

    if (found) break;
  }

  if (line_no == target_line && line_pos > 0) found = 1;
  if (found) {
    source_line[line_pos] = '\0';
    sprint("%s\n", source_line);
  }

  spike_file_close(f);
}

void print_runtime_error(uint64 fault_pc) {
  addr_line entry;
  if (!find_addr_line_entry(fault_pc, &entry)) return;
  if (entry.file >= (uint64)g_file_count) return;

  code_file cf = g_debug_files[entry.file];
  const char *file = cf.file;
  const char *dir = cf.dir == INVALID_DIR_INDEX || cf.dir >= (uint64)g_dir_count
                      ? 0
                      : g_debug_dirs[cf.dir];
  char full_path[256];

  if (file == 0) return;
  if (dir && dir[0] != '\0') {
    sprint("Runtime error at %s/%s:%ld\n", dir, file, entry.line);
    safestrcpy(full_path, dir, sizeof(full_path));
    if (strlen(full_path) + 1 < sizeof(full_path)) strcat(full_path, "/");
    if (strlen(full_path) + strlen(file) < sizeof(full_path)) strcat(full_path, file);
    print_source_line(full_path, entry.line);
  } else {
    sprint("Runtime error at %s:%ld\n", file, entry.line);
    print_source_line(file, entry.line);
  }
}

static void resolve_elf_host_path(const char *filename, char *host_path, uint64 size) {
  if (filename == 0 || host_path == 0 || size == 0) panic("invalid ELF path.\n");

  if (filename[0] == '/') {
    safestrcpy(host_path, "hostfs_root", size);
    if (strlen(host_path) + strlen(filename) + 1 > size) panic("ELF path too long: %s\n", filename);
    strcat(host_path, filename);
  } else {
    safestrcpy(host_path, filename, size);
  }
}

void load_bincode_from_host_elf(process *p, char *filename) {
  if (g_multicore_boot_mode)
    sprint("hartid = %ld: Application: %s\n", read_tp(), filename);
  else
    sprint("Application: %s\n", filename);

  reset_runtime_debug_info();

  elf_ctx elfloader;
  elf_info info;
  char host_path[256];
  resolve_elf_host_path(filename, host_path, sizeof(host_path));
  info.f = spike_file_open(host_path, O_RDONLY, 0);
  info.p = p;
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  if (elf_init(&elfloader, &info) != EL_OK) panic("fail to init elfloader.\n");
  if (elf_load(&elfloader) != EL_OK) panic("Fail on loading elf.\n");
  if (load_elf_runtime_sections(&elfloader) != EL_OK) panic("Fail on loading ELF runtime sections.\n");

  p->trapframe->epc = elfloader.ehdr.entry;
  spike_file_close(info.f);

  if (g_multicore_boot_mode)
    sprint("hartid = %ld: Application program entry point (virtual address): 0x%lx\n",
           read_tp(), p->trapframe->epc);
  else
    sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}
