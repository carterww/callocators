#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "page.h"
#include "__utils.h"

// Probably inefficient, we should cache this value
#define PAGE_SIZE getpagesize()

struct palloc_page_head {
	struct palloc_page_head *prev;
	struct palloc_page_head *next;
	void *addr; // Must be aligned to page, mmap always returns this
	size_t len; // In bytes
};

struct __internal_page {
	size_t page_heads_cap;
	size_t page_heads_num;
	struct __internal_page *prev;
	struct __internal_page *next;
	struct palloc_page_head pages[];
};

struct palloc_state {
	struct __internal_page *__pages;
	struct palloc_page_head *free_list;
	struct palloc_page_head *used_list;
	pthread_mutex_t free_lock;
	pthread_mutex_t used_lock;
};

// TODO: maybe make this per thread. We'll see if global
// locking impacts performance significantly
struct palloc_state state = {
	.__pages = NULL,
	.free_list = NULL,
	.used_list = NULL,
	.free_lock = PTHREAD_MUTEX_INITIALIZER,
	.used_lock = PTHREAD_MUTEX_INITIALIZER,
};

static void __c_die(const char *str);
static void bootstrap_self();
static void *__map_pages(size_t pnum);
static int handle_mmap_fail(int err);

// Not focusing on multithreading support just yet
static int is_initialized = 0;

void *palloc(size_t pnum)
{
	if (pnum == 0) {
		errno = EINVAL;
		return NULL;
	}
	if (!is_initialized) {
		bootstrap_self();
		is_initialized = 1;
	}
	assert(state.__pages != NULL);
	struct __internal_page *curr_raw = state.__pages;
	if (curr_raw->page_heads_cap == curr_raw->page_heads_num) {
		// Need to pull from free list
		void *raw_page = __map_pages(1);
		struct __internal_page *rp = (struct __internal_page *)raw_page;
		rp->page_heads_cap = (PAGE_SIZE - sizeof(*state.__pages)) /
				     sizeof(*state.free_list);
		rp->page_heads_num = 0;
		state.__pages = rp;
		struct __internal_page *tmp = curr_raw->next;
		curr_raw->next = rp;
		rp->prev = curr_raw;
		rp->next = tmp;
		if (curr_raw->prev == curr_raw) {
			curr_raw->prev = rp;
		}
		curr_raw = rp;
	}
}

// Make sure we align pages to page size
void pfree(void *pages);

// Before we can
static void bootstrap_self()
{
	void *raw_pages = __map_pages(2);
	void *first_internal_page = raw_pages;
	void *first_free_page = raw_pages + PAGE_SIZE;
	struct __internal_page *fip =
		(struct __internal_page *)first_internal_page;
	struct palloc_page_head *free = &fip->pages[0];
	free->prev = free;
	free->next = free;
	free->addr = first_free_page;
	free->len = PAGE_SIZE;
	fip->page_heads_cap =
		(PAGE_SIZE - sizeof(*state.__pages)) / sizeof(*state.free_list);
	fip->page_heads_num = 1;
	fip->next = fip;
	fip->prev = fip;
	state.__pages = fip;
	state.free_list = free;
}

// Figure out how we can handle the real ones?
static int handle_mmap_fail(int err)
{
	assert(err != 1);
	switch (err) {
	// These are related to file mappings, not relevant to us
	case EACCES:
	case EAGAIN:
	case EBADF:
	case ENFILE:
	case ENODEV:
	case EPERM:
	case ETXTBSY:
	case EOVERFLOW:
		__c_die("Received an error related to file mapping");
	case EINVAL:
		return 1;
	case ENOMEM:
		return 1;
	}
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
