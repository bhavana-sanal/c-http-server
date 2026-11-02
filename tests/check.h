#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

static int checks, failures;

/* Non-fatal assertion; `what` names the case so table-driven failures are findable. */
#define CHECK(cond, what)                                                                    \
    do {                                                                                     \
        checks++;                                                                            \
        if (!(cond)) {                                                                       \
            failures++;                                                                      \
            fprintf(stderr, "%s:%d: [%s] check failed: %s\n", __FILE__, __LINE__, (what),   \
                    #cond);                                                                  \
        }                                                                                    \
    } while (0)

static inline int check_summary(const char *suite)
{
    printf("%-16s %5d checks, %d failed\n", suite, checks, failures);
    return failures ? 1 : 0;
}

#endif
