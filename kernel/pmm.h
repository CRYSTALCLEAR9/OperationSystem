#ifndef _PMM_H_
#define _PMM_H_

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);
// Increase the reference count of a mapped physical page.
void inc_page_ref(void* pa);
// Read current reference count of a mapped physical page.
int get_page_ref(void* pa);

#endif