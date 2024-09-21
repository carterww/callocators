#ifndef _ARENA_H
#define _ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "kette.h"

struct arena_page {
	uintptr_t idx;
	uintptr_t end;
	struct slink pages_head;
};

struct arena {
	// !!!!!!! THIS MUST BE FIRST !!!!!!!
	struct arena_page page;
	struct slink head;
	size_t bytes_growth;
};

struct arena *arena_create();

struct arena *arena_create_ext(size_t initial_bytes, size_t bytes_growth);

void *arena_alloc(struct arena *arena, size_t bytes);

void arena_free(struct arena *arena);

#endif // _ARENA_H
