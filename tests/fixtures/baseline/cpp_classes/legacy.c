#include <stdio.h>
#include "legacy.h"

typedef struct {
    int (*compare)(const void *a, const void *b);
    size_t width;
} sorter_t;

static int helper(int value)
{
    return value * 2;
}

int run(int argc, char **argv)
{
    printf("%d\n", helper(argc));
    return 0;
}
