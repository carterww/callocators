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
#define MAX_PAGES_FREE_LIST 16

struct palloc_page_head {
	struct dlink head;
	void *addr;
	size_t page_num;
};

struct __internal_page {
	size_t page_heads_cap;
	struct dlink head;
	struct palloc_page_head *pages;
};

struct palloc_state {
	pthread_mutex_t lock;
	struct dlink __head;
	struct dlink free_head;
	struct dlink used_head;
	size_t free_page_num;
};

static struct palloc_page_head static_page_heads[STATIC_PAGE_HEAD_NUM] = { 0 };
// TODO: maybe make this per thread. We'll see if global
// locking impacts performance significantly
static struct palloc_state state = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.__head = DLIST_INIT(state.__head),
	.free_head = DLIST_INIT(state.free_head),
	.used_head = DLIST_INIT(state.used_head),
	.free_page_num = 0,
};
static struct __internal_page static_internal_page = {
	.page_heads_cap = STATIC_PAGE_HEAD_NUM,
	.head = { 0 },
	.pages = static_page_heads,
};

static void __c_die(const char *str);
static void *find_free_pages(size_t pnum, struct palloc_page_head *extra);
static struct palloc_page_head *find_free_page_head();
static struct __internal_page *
find_page_head_container(struct palloc_page_head *head);
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
	struct __internal_page *__page;
	struct palloc_page_head *free_head = find_free_page_head();
	// No free slots
	if (free_head == NULL) {
		// Cannot pass NULL extra because we have no room in
		// internal pages for a split head
		struct palloc_page_head extra = { 0 };
		__page = find_free_pages(1, &extra);
		__page->page_heads_cap =
			(PAGE_SIZE - sizeof(struct __internal_page)) /
			sizeof(struct palloc_page_head);
		char *pages_offset =
			(char *)__page + sizeof(struct __internal_page);
		__page->pages = (struct palloc_page_head *)pages_offset;
		dlist_add(&__page->head, &state.__head);
		size_t start = 0;
		if (extra.addr != NULL) {
			__page->pages[start++] = extra;
		}
		free_head = &__page->pages[start];
	}
	// extra is NULL becaue we have at least one slot in current internal
	// page for a palloc_page_head
	void *pages = find_free_pages(pnum, NULL);
	dlist_add(&free_head->head, &state.used_head);
	free_head->addr = pages;
	free_head->page_num = pnum;
	pthread_mutex_unlock(&state.lock);

	return pages;
}

void pfree(void *pages)
{
	// Align pages to page boundary
	pages = (void *)((uintptr_t)pages & ~(PAGE_SIZE - 1));
	int found = 0;
	struct palloc_page_head *entry;
	pthread_mutex_lock(&state.lock);
	list_for_each(&state.used_head, entry, struct palloc_page_head, head) {
		if (entry->addr == pages) {
			found = 1;
			break;
		}
	}
	if (found == 0) {
		return;
	}
	dlist_del(&entry->head);
	if (state.free_page_num <= MAX_PAGES_FREE_LIST) {
		state.free_page_num += entry->page_num;
		dlist_add(&entry->head, &state.free_head);
		memset(pages, 0, PAGE_SIZE * entry->page_num);
		pthread_mutex_unlock(&state.lock);
		return;
	}
	entry->addr = NULL;
	struct __internal_page *container = find_page_head_container(entry);
	int should_free_container = 1;
	// Check if internal page is empty only if it isn't static page.
	// If all palloc_page_head spots are full, unmap the page
	if (container != &static_internal_page) {
		struct palloc_page_head *curr = container->pages;
		while (curr < container->pages + container->page_heads_cap) {
			if (curr->addr != NULL) {
				should_free_container = 0;
				break;
			}
			++curr;
		}
		if (should_free_container) {
			dlist_del(&container->head);
		}
	}
	pthread_mutex_unlock(&state.lock);
	__unmap_pages(entry->addr, entry->page_num * PAGE_SIZE);
	if (should_free_container) {
		__unmap_pages(container, PAGE_SIZE);
	}
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
		state.free_page_num -= entry->page_num;
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
		extra = find_free_page_head();
	}
	extra->page_num = entry->page_num - pnum;
	extra->addr = entry->addr + (PAGE_SIZE * pnum);
	entry->page_num = pnum;
	dlist_del(&entry->head);
	dlist_add(&entry->head, &state.used_head);
	state.free_page_num -= pnum;
	return entry->addr;
}

static struct palloc_page_head *find_free_page_head()
{
	struct __internal_page *entry;
	list_for_each(&state.__head, entry, struct __internal_page, head) {
		struct palloc_page_head *curr = entry->pages;
		while (curr < entry->page_heads_cap + entry->pages) {
			if (curr->addr != NULL) {
				++curr;
				continue;
			}
			return curr;
		}
	}
	return NULL;
}

static struct __internal_page *
find_page_head_container(struct palloc_page_head *head)
{
	struct __internal_page *entry;
	list_for_each(&state.__head, entry, struct __internal_page, head) {
		struct palloc_page_head *last =
			entry->pages + entry->page_heads_cap;
		uintptr_t end = (uintptr_t)last;
		uintptr_t start = (uintptr_t)entry;
		uintptr_t item = (uintptr_t)head;
		if (item < end && item > start) {
			return entry;
		}
	}
	__c_die("Finding containing internal page failed");
	return NULL;
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
