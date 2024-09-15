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
static int handle_mmap_fail(int err);

void *palloc(size_t pnum)
{
	if (pnum == 0) {
		errno = EINVAL;
		return NULL;
	}
}

// Make sure we align pages to page size
void pfree(void *pages);

// Before we can
static void bootstrap_self()
{
	// Allocate a single page with the following props:
	// 1. Read/Write perms
	// 2. Internal to the process
	// 3. Anonymous page (not backed by file)
	void *first_internal_page = mmap(NULL, PAGE_SIZE,
					 PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (first_internal_page == MAP_FAILED) {
		__c_die("Failed to boostrap page allocator");
	}
	struct __internal_page *fip =
		(struct __internal_page *)first_internal_page;
	fip->page_heads_cap =
		(PAGE_SIZE - sizeof(*state.__pages)) / sizeof(*state.free_list);
	fip->page_heads_num = 0;
	fip->next = fip;
	fip->prev = fip;
	state.__pages = fip;
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
