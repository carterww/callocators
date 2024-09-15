#ifndef _PAGE_H
#define _PAGE_H

#include <stdlib.h>

void *palloc(size_t pnum);
void pfree(void *pages);

#endif // _PAGE_H
