#include <stdint.h>

#include "arena.h"
#include "kette.h"
#include "page.h"

#define INITIAL_BYTES_DEFAULT page_size()
#define BYTES_GROWTH_DEFAULT page_size()

static size_t bytes_to_page(size_t bytes, int page_size);

struct arena *arena_create()
{
	return arena_create_ext(INITIAL_BYTES_DEFAULT, BYTES_GROWTH_DEFAULT);
}

struct arena *arena_create_ext(size_t initial_bytes, size_t bytes_growth)
{
	int ps = page_size();
	size_t num_pages = bytes_to_page(initial_bytes, ps);
	struct arena *a = palloc(num_pages);
	if (a == NULL) {
		return NULL;
	}
	uintptr_t start = (uintptr_t)a;
	slist_init(&a->head);
	a->bytes_growth = bytes_growth;
	a->page.idx = start + sizeof(*a);
	a->page.end = start + (num_pages * ps);
	slist_add(&a->page.pages_head, &a->head);
	return a;
}

void *arena_alloc(struct arena *arena, size_t bytes)
{
	void *ptr = NULL;
	struct arena_page *curr_page =
		list_entry(arena->head.next, struct arena_page, pages_head);
	size_t bytes_left = curr_page->end - curr_page->idx - 1;
	if (bytes <= bytes_left) {
		ptr = (void *)curr_page->idx;
		curr_page->idx += bytes;
		return ptr;
	}
	size_t growth = bytes >= arena->bytes_growth ? bytes :
						       arena->bytes_growth;
	size_t ps = page_size();
	size_t num_pages = bytes_to_page(growth, ps);
	curr_page = palloc(num_pages);
	if (curr_page == NULL) {
		return NULL;
	}
	uintptr_t start = (uintptr_t)curr_page;
	curr_page->idx = start + sizeof(*curr_page);
	curr_page->end = start + (num_pages * ps);
	slist_add(&curr_page->pages_head, &arena->head);
	ptr = (void *)curr_page->idx;
	curr_page->idx += bytes;
	return ptr;
}

void arena_free(struct arena *arena)
{
	// This is the page arena is allocated on
	struct arena_page *free_last =
		list_entry(arena->head.next, struct arena_page, pages_head);
	struct arena_page *entry;
	list_for_each(&arena->head, entry, struct arena_page, pages_head) {
		if (entry != free_last) {
			pfree(entry);
		}
	}
	pfree(free_last);
}

static size_t bytes_to_page(size_t bytes, int ps)
{
	size_t num_pages = bytes / ps;
	if (bytes % ps != 0) {
		++num_pages;
	}
	return num_pages;
}
