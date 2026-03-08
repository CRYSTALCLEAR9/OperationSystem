#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"
#include "sync_utils.h"

// _end is defined in kernel/kernel.lds, it marks the ending (virtual) address of PKE kernel
extern char _end[];
// g_mem_size is defined in spike_interface/spike_memory.c, it indicates the size of our
// (emulated) spike machine. g_mem_size's value is obtained when initializing HTIF.
extern uint64 g_mem_size;

static uint64 free_mem_start_addr;
static uint64 free_mem_end_addr;

#define MAX_PHYSICAL_PAGES (PKE_MAX_ALLOWABLE_RAM / PGSIZE)
static uint16 g_page_refcnt[MAX_PHYSICAL_PAGES];
int vm_alloc_stage[NCPU] = {0};

typedef struct node {
  struct node *next;
} list_node;

static list_node g_free_mem_list;
static volatile int g_pmm_lock = 0;

static inline int pa2page_idx(void *pa) {
  uint64 addr = (uint64)pa;
  if (addr < DRAM_BASE || addr >= DRAM_BASE + PKE_MAX_ALLOWABLE_RAM) return -1;
  return (int)((addr - DRAM_BASE) / PGSIZE);
}

static inline void free_list_push(void *pa) {
  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
}

static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE) {
    int idx = pa2page_idx((void *)p);
    if (idx >= 0) g_page_refcnt[idx] = 0;
    free_list_push((void *)p);
  }
}

void free_page(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr || (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  int idx = pa2page_idx(pa);
  if (idx < 0) panic("free_page: invalid page index for 0x%lx\n", pa);
  if (g_page_refcnt[idx] <= 0) panic("free_page: page 0x%lx has refcnt %d\n", pa, g_page_refcnt[idx]);

  spin_lock(&g_pmm_lock);
  g_page_refcnt[idx]--;
  if (g_page_refcnt[idx] == 0) free_list_push(pa);
  spin_unlock(&g_pmm_lock);
}

void *alloc_page(void) {
  uint64 hartid = read_tp();
  spin_lock(&g_pmm_lock);
  list_node *n = g_free_mem_list.next;
  if (n) g_free_mem_list.next = n->next;

  if (n) {
    int idx = pa2page_idx((void *)n);
    if (idx < 0) panic("alloc_page: invalid page index for 0x%lx\n", n);
    g_page_refcnt[idx] = 1;
    if (g_multicore_boot_mode && hartid < NCPU && vm_alloc_stage[hartid]) {
      sprint("hartid = %ld: alloc page 0x%x\n", hartid, n);
    }
  }
  spin_unlock(&g_pmm_lock);
  return (void *)n;
}

void inc_page_ref(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr || (uint64)pa >= free_mem_end_addr)
    panic("inc_page_ref 0x%lx\n", pa);

  int idx = pa2page_idx(pa);
  if (idx < 0) panic("inc_page_ref: invalid page index for 0x%lx\n", pa);
  spin_lock(&g_pmm_lock);
  if (g_page_refcnt[idx] <= 0) panic("inc_page_ref: page 0x%lx has refcnt %d\n", pa, g_page_refcnt[idx]);
  g_page_refcnt[idx]++;
  spin_unlock(&g_pmm_lock);
}

int get_page_ref(void *pa) {
  int idx = pa2page_idx(pa);
  if (idx < 0) return 0;
  return g_page_refcnt[idx];
}

void pmm_init() {
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;
  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;

  free_mem_start_addr = ROUNDUP(g_kernel_end, PGSIZE);

  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if (g_mem_size < pke_kernel_size)
    panic("Error when recomputing physical memory size (g_mem_size).\n");

  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
         g_kernel_start, g_kernel_end, pke_kernel_size);
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
         free_mem_end_addr - 1);
  sprint("kernel memory manager is initializing ...\n");
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
}
