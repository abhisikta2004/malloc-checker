#include <stdlib.h>

void unsafe_malloc_1() {
    int *p = malloc(sizeof(int));

    *p = 5;
}

void unsafe_malloc_2() {
    char *buf = malloc(100);

    buf[0] = 'H';
}

void unsafe_malloc_3() {
    int *arr = malloc(50 * sizeof(int));

    arr[10] = 99;
}