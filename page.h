#ifndef _PAGE_H
#define _PAGE_H

#include <stdlib.h>

// Get the page size
int page_size();

// Allocate pnum pages
void *palloc(size_t pnum);
// Free a previous page allocation. This pointer MUST be in the first
// page of the allocation. If it is not ... memory leak.
void pfree(void *pages);

#endif // _PAGE_H
