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

// Grab the page size from sys call and store that value
int page_size()
{
	static int _page_size = -1;
	if (_page_size == -1) {
		_page_size = getpagesize();
	}
	return _page_size;
}

// Number of palloc_page_heads to store statically. This avoids internal
// fragmentation of internal pages for users with a small number of allocations.
#define STATIC_PAGE_HEAD_NUM 32
// The maximum number of pages palloc will keep in its free list.
#define MAX_PAGES_FREE_LIST 16
#define MOST_SIGNIFICANT_SIZE_T_BIT ((size_t)1 << (sizeof(size_t) * 8 - 1))

/* This structure stores the information for a single allocation. We store
 * the allocations in a doubly linked list on pages managed internally.
 */
struct palloc_page_head {
	struct dlink head;
	void *addr;
	size_t page_num;
};

/* This structure represents a page used to store palloc_page_heads. These
 * are allocated dynamically by the page allocator (except one statically allocated
 * page). 
 */
struct __internal_page {
	size_t page_heads_cap;
	size_t page_heads_num;
	struct dlink head;
};

/* The entire state of the page allocator.
 *
 * lock          -> Global lock for manipulating the state.
 * __head        -> Linked list head for __internal_pages
 * free_head     -> Linked list head for all freed pages that can be allocated
 *                  in the future.
 * used_head     -> Linked list head for all page allocations in use by users.
 * free_page_num -> Number of free pages in the free list. To avoid having
 *                  too many pages mapped at once, we keep this number below
 *                  a constant (MAX_PAGES_FREE_LIST).
 */
struct palloc_state {
	pthread_mutex_t lock;
	struct dlink __head;
	struct dlink free_head;
	struct dlink used_head;
	size_t free_page_num;
};

// Helper function to get pointer to palloc_page_heads in internal page
static inline struct palloc_page_head *
__internal_page_pages_ptr(struct __internal_page *page)
{
	uintptr_t page_ptr = (uintptr_t)page;
	return (struct palloc_page_head *)(page_ptr + sizeof(*page));
}

/* In the __internal_page structure, cap serves two purposes:
 * 1. It stores the maximum number of palloc_page_heads the __internal_page
 *    can hold.
 * 2. The most significant bit is used to implement a "second chance" page
 *    unmapping policy for __internal_pages.
 * There may be a scenario where a user is allocating then immediately freeing
 * a page. Let's say the palloc_page_head used for this is the only used
 * palloc_page_head in the internal page. If we free internal pages when they are
 * empty, this would cause a page to constantly get unmapped and mapped. To avoid
 * this, we set the MSb of page_heads_cap to 1 if the page is empty but the bit
 * was not set yet. On the next free, we can then free the page since its MSb is 1.
 */
static inline size_t __internal_page_get_cap(struct __internal_page *page)
{
	// MSb in page_heads_cap is a "second chance" bit for freeing internal pages
	return page->page_heads_cap & ~(MOST_SIGNIFICANT_SIZE_T_BIT);
}

// Mark the internal page as recently used by settings the page_heads_cap's
// MSb to 0. This will prevent the page from being freed right away.
static inline void __use_internal_page(struct __internal_page *page)
{
	page->page_heads_cap &= ~(MOST_SIGNIFICANT_SIZE_T_BIT);
}

// Return false if we should free the page. True if we shouldn't. If
// we return true, we mark the page to be freed next time. See the comment
// above "__internal_page_get_cap" for an explanation.
static inline int __give_second_chance(struct __internal_page *page)
{
	if (page->page_heads_cap & (MOST_SIGNIFICANT_SIZE_T_BIT)) {
		return 0;
	}
	page->page_heads_cap |= MOST_SIGNIFICANT_SIZE_T_BIT;
	return 1;
}

// Ensure palloc_page_heads is directly after __internal_page to be
// compatible with dynamic __internal_pages. This is a little ugly
// but it gets the job done and doesn't require any compiler extensions.
struct {
	struct __internal_page page;
	struct palloc_page_head heads[STATIC_PAGE_HEAD_NUM];
} __static_internal_page = {
	.page = { .page_heads_cap = STATIC_PAGE_HEAD_NUM, .page_heads_num = 0 },
	.heads = { 0 },
};
static struct __internal_page *static_internal_page =
	(struct __internal_page *)&__static_internal_page;

static struct palloc_state state = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.__head = DLIST_INIT(state.__head),
	.free_head = DLIST_INIT(state.free_head),
	.used_head = DLIST_INIT(state.used_head),
	.free_page_num = 0,
};

static void *find_free_pages(size_t pnum, struct palloc_page_head *extra);
static struct palloc_page_head *find_free_page_head();

// Find the internal page head is stored in
static struct __internal_page *
find_page_head_container(struct palloc_page_head *head);

// Second chance on internal pages. Loops through internal pages,
// checks if emtpy, if so mark the bit. If the bit is marked,
// return the page. If there are no pages to free, return NULL.
static struct __internal_page *find_internal_page_to_free();

// Internal functions to get and free pages. These can
// be altered to support a variety of platforms.
static void *__map_pages(size_t pnum);
static void __unmap_pages(void *addr, size_t len);

// Kill application. Used for debugging
static void __c_die(const char *str);

void *palloc(size_t pnum)
{
	if (pnum == 0) {
		errno = EINVAL;
		return NULL;
	}
	pthread_mutex_lock(&state.lock);
	// Add static page to list
	if (&state.__head == state.__head.next) {
		dlist_add(&static_internal_page->head, &state.__head);
	}
	struct __internal_page *__page;
	struct palloc_page_head *free_head = find_free_page_head();
	if (free_head != NULL) {
		__page = find_page_head_container(free_head);
	} else {
		// Cannot pass NULL extra because we have no room in
		// internal pages for a split head
		struct palloc_page_head extra = { 0 };
		__page = find_free_pages(1, &extra);
		__page->page_heads_cap =
			(page_size() - sizeof(struct __internal_page)) /
			sizeof(struct palloc_page_head);
		__page->page_heads_num = 0;
		dlist_add(&__page->head, &state.__head);
		struct palloc_page_head *pages =
			__internal_page_pages_ptr(__page);
		if (extra.addr != NULL) {
			pages[__page->page_heads_num++] = extra;
		}
		free_head = &pages[__page->page_heads_num];
	}
	// Mark the internal page as used recently
	__use_internal_page(__page);
	++__page->page_heads_num;
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
	pages = (void *)((uintptr_t)pages & ~(page_size() - 1));
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
		pthread_mutex_unlock(&state.lock);
		return;
	}
	// Delete from used list
	dlist_del(&entry->head);
	// Check if we should add it to free list
	if (state.free_page_num <= MAX_PAGES_FREE_LIST) {
		state.free_page_num += entry->page_num;
		dlist_add(&entry->head, &state.free_head);
		pthread_mutex_unlock(&state.lock);
		return;
	}
	// At this point, we know we are unmapping the user's page, check if
	// we should unmap __internal_page with old head
	struct __internal_page *container = find_page_head_container(entry);
	struct __internal_page *container_to_free =
		find_internal_page_to_free();
	if (container_to_free != NULL) {
		// Delete from internal page list
		dlist_del(&container_to_free->head);
		pthread_mutex_unlock(&state.lock);
		__unmap_pages(container_to_free, page_size());

	} else {
		entry->addr = NULL;
		--container->page_heads_num;
		pthread_mutex_unlock(&state.lock);
	}
	__unmap_pages(entry->addr, entry->page_num * page_size());
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
	// No free contiguous page found
	if (&entry->head == &state.free_head) {
		return __map_pages(pnum);
	}
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
		extra = find_free_page_head();
	}
	extra->page_num = entry->page_num - pnum;
	extra->addr = entry->addr + (page_size() * pnum);
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
		size_t cap = __internal_page_get_cap(entry);
		struct palloc_page_head *pages =
			__internal_page_pages_ptr(entry);
		struct palloc_page_head *curr = pages;
		while (curr < cap + pages) {
			if (curr->addr != NULL) {
				++curr;
				continue;
			}
			++entry->page_heads_num;
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
		size_t cap = __internal_page_get_cap(entry);
		struct palloc_page_head *last =
			__internal_page_pages_ptr(entry) + cap;
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

static struct __internal_page *find_internal_page_to_free()
{
	// Walk all the internal pages until we find an empty one that is marked
	// for deletion. If we find empty ones not marked, mark them for next time.
	struct __internal_page *entry;
	list_for_each(&state.__head, entry, struct __internal_page, head) {
		if (entry->page_heads_num == 0 &&
		    !__give_second_chance(entry)) {
			return entry;
		}
	}
	return NULL;
}

static void *__map_pages(size_t pnum)
{
	assert(pnum != 0);
	void *raw_pages = mmap(NULL, pnum * page_size(), PROT_READ | PROT_WRITE,
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
