#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef _COMPILE_ASSERTS
#include <stdio.h>
#include <stdlib.h>
#define _assert(expr)                                                      \
	if (unlikely(!(expr))) {                                           \
		fprintf(stderr, "ERR: assert failed at %s:%d\n", __FILE__, \
			__LINE__);                                         \
		exit(1);                                                   \
	}
#else
#define _assert(expr)
#endif
