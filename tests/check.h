#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

#define CHECK(cond, why) do {                                       \
	if (!(cond)) {                                                  \
		fprintf(stderr, "FAIL %s:%d\n  want: %s\n  expr: %s\n",     \
			__FILE__, __LINE__, (why), #cond);                      \
		return 1;                                                   \
	}                                                               \
} while (0)

/** Report a clean run. Last statement of main. */
#define CHECK_PASS(name) do {                                       \
	printf("%s: all checks passed\n", (name));                      \
	return 0;                                                       \
} while (0)

#endif
