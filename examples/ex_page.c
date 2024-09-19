#include "page.h"

int main()
{
	int i = 0;
	while (i < 500) {
		void *page = palloc(1);
		int *a = (int *)page;
		*a = 5;
		pfree(page);
		++i;
	}
	return 0;
}
