#include <stdlib.h>

void safe_malloc_1() {
    char *buf = malloc(256);

    if (buf == NULL)
        return;

    buf[0] = 'A';
    free(buf);
}

void safe_malloc_2() {
    int *arr = malloc(20 * sizeof(int));

    if (!arr)
        return;

    arr[5] = 10;
    free(arr);
}