#ifndef _PMM_H_
#define _PMM_H_

#include "config.h"

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);
// Increase the reference count of a physical page
void inc_page_ref(void *pa);
// Query the reference count of a physical page
int get_page_ref(void *pa);
extern int vm_alloc_stage[NCPU];

#endif
