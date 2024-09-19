#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kette.h"
#include "page.h"
#include "__utils.h"

// Probably inefficient, we should cache this value
#define PAGE_SIZE getpagesize()
#define STATIC_PAGE_HEAD_NUM 32

struct palloc_page_head {
	struct dlink head;
	void *addr;
	size_t page_num;
};

struct __internal_page {
	size_t page_heads_cap;
	size_t page_heads_num;
	struct dlink head;
	struct palloc_page_head *pages;
};

struct palloc_state {
	pthread_mutex_t lock;
	struct dlink __head;
	struct dlink free_head;
	struct dlink used_head;
};

static size_t initial_free_page_num = 4;
static struct palloc_page_head static_page_heads[STATIC_PAGE_HEAD_NUM] = { 0 };
// TODO: maybe make this per thread. We'll see if global
// locking impacts performance significantly
static struct palloc_state state = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.__head = DLIST_INIT(state.__head),
	.free_head = DLIST_INIT(state.free_head),
	.used_head = DLIST_INIT(state.used_head),
};
static struct __internal_page static_internal_page = {
	.page_heads_cap = STATIC_PAGE_HEAD_NUM,
	.page_heads_num = 0,
	.head = { 0 },
	.pages = static_page_heads,
};

static void __c_die(const char *str);
static void *find_free_pages(size_t pnum, struct palloc_page_head *extra);
static void *__map_pages(size_t pnum);
static void __unmap_pages(void *addr, size_t len);

// Not focusing on multithreading support just yet
static int is_initialized = 0;

void *palloc(size_t pnum)
{
	if (pnum == 0) {
		errno = EINVAL;
		return NULL;
	}
	pthread_mutex_lock(&state.lock);
	if (&state.__head == state.__head.next) {
		dlist_add(&static_internal_page.head, &state.__head);
	}
	struct __internal_page *__page =
		list_entry(state.__head.next, struct __internal_page, head);
	// Static "page" not added to linked list yet
	// Current internal page is full, need to get another
	if (__page->page_heads_cap == __page->page_heads_num) {
		// Cannot pass NULL extra because we have no room in
		// internal pages for a split head
		struct palloc_page_head extra = { 0 };
		__page = find_free_pages(1, &extra);
		__page->page_heads_cap =
			(PAGE_SIZE - sizeof(struct __internal_page)) /
			sizeof(struct palloc_page_head);
		__page->page_heads_num = 0;
		char *pages_offset =
			(char *)__page + sizeof(struct __internal_page);
		__page->pages = (struct palloc_page_head *)pages_offset;
		dlist_add(&__page->head, &state.__head);
		if (extra.addr != NULL) {
			__page->pages[0] = extra;
			__page->page_heads_num = 1;
		}
	}
	// extra is NULL becaue we have at least one slot in current internal
	// page for a palloc_page_head
	void *pages = find_free_pages(pnum, NULL);
	struct palloc_page_head *new_head =
		__page->pages + __page->page_heads_num;
	++__page->page_heads_num;
	dlist_add(&new_head->head, &state.used_head);
	new_head->addr = pages;
	new_head->page_num = pnum;
	pthread_mutex_unlock(&state.lock);

	return pages;
}

void pfree(void *pages)
{
	// Align pages to page boundary
	pages = (void *)((uintptr_t)pages & ~(PAGE_SIZE - 1));
	int found = 0;
	struct palloc_page_head *entry;
	list_for_each(&state.used_head, entry, struct palloc_page_head, head) {
		printf("param: %p, entry: %p\n", pages, entry->addr);
		if (entry->addr == pages) {
			found = 1;
			break;
		}
	}
	if (found == 0) {
		// For debugging
		__c_die("Tried to free page not managed by palloc");
	}
	// For now, we'll ignore freeing palloc_page_head from internal pages
	dlist_del(&entry->head);
	__unmap_pages(entry->addr, entry->page_num * PAGE_SIZE);
}

// Find pages in free list or by allocating new ones
static void *find_free_pages(size_t pnum, struct palloc_page_head *extra)
{
	struct palloc_page_head *entry;
	list_for_each(&state.free_head, entry, struct palloc_page_head, head) {
		if (entry->page_num >= pnum) {
			break;
		}
	}
	if (&entry->head == &state.free_head) {
		return __map_pages(pnum);
	}
	// No free contiguous page found
	if (entry->page_num < pnum) {
		return __map_pages(pnum);
	} else if (entry->page_num == pnum) {
		dlist_del(&entry->head);
		dlist_add(&entry->head, &state.used_head);
		return entry->addr;
	}

	// At this point we know we have a page in the free list, but
	// it is too big. If extra == NULL, we can assume caller is
	// telling us the current internal page has room. If it is not
	// null, the caller is trying to find an internal page, so we
	// must put the split palloc_page_head in extra.
	if (extra == NULL) {
		struct __internal_page *__page = list_entry(
			&state.__head.next, struct __internal_page, head);
		extra = &__page->pages[__page->page_heads_num++];
	}
	extra->page_num = entry->page_num - pnum;
	extra->addr = entry->addr + (PAGE_SIZE * pnum);
	entry->page_num = pnum;
	dlist_del(&entry->head);
	dlist_add(&entry->head, &state.used_head);
	return entry->addr;
}

static void *__map_pages(size_t pnum)
{
	assert(pnum != 0);
	void *raw_pages = mmap(NULL, pnum * PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	// Die for now on error
	if (raw_pages == MAP_FAILED) {
		__c_die("Failed to boostrap page allocator");
	}
	return raw_pages;
}

static void __unmap_pages(void *addr, size_t len)
{
	assert(addr != 0);
	assert(len != 0);
	int err = munmap(addr, len);
	if (err != 0) {
		perror(NULL);
		exit(1);
	}
}

static void __c_die(const char *str)
{
	size_t len = strnlen(str, 512);
	int print_nl = 1;
	for (int i = 0; i < len; ++i) {
		if (str[i] == '\n') {
			print_nl = 0;
			break;
		}
	}
	write(STDERR_FILENO, str, len);
	if (print_nl) {
		write(STDERR_FILENO, "\n", 1);
	}
	exit(1);
}
