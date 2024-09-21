#include <stdint.h>

#include "arena.h"
#include "kette.h"
#include "page.h"

#define INITIAL_BYTES_DEFAULT page_size()
#define BYTES_GROWTH_DEFAULT page_size()

static void arena_page_init(struct arena *arena, struct arena_page *page,
			    size_t struct_size, size_t page_size);
static void *alloc_in_page(struct arena_page *page, size_t bytes);
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
	slist_init(&a->head);
	a->bytes_growth = bytes_growth;
	arena_page_init(a, &a->page, sizeof(*a), num_pages * ps);
	return a;
}

void *arena_alloc(struct arena *arena, size_t bytes)
{
	struct arena_page *curr_page =
		list_entry(arena->head.next, struct arena_page, pages_head);
	size_t bytes_left = curr_page->end - curr_page->idx - 1;
	if (bytes <= bytes_left) {
		return alloc_in_page(curr_page, bytes);
	}
	size_t growth = bytes >= arena->bytes_growth ? bytes :
						       arena->bytes_growth;
	size_t ps = page_size();
	size_t num_pages = bytes_to_page(growth, ps);
	curr_page = palloc(num_pages);
	if (curr_page == NULL) {
		return NULL;
	}
	arena_page_init(arena, curr_page, sizeof(*curr_page), num_pages * ps);
	return alloc_in_page(curr_page, growth);
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

static void arena_page_init(struct arena *arena, struct arena_page *page,
			    size_t struct_size, size_t page_size)
{
	uintptr_t start = (uintptr_t)page;
	page->idx = start + struct_size;
	page->end = start + page_size;
	slist_add(&page->pages_head, &arena->head);
}

static void *alloc_in_page(struct arena_page *page, size_t bytes)
{
	void *ptr = (void *)page->idx;
	page->idx += bytes;
	return ptr;
}

static size_t bytes_to_page(size_t bytes, int ps)
{
	size_t num_pages = bytes / ps;
	if (bytes % ps != 0) {
		++num_pages;
	}
	return num_pages;
}
