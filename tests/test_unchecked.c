#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// TC-01: plain malloc, no check
void tc01_no_check(void) {
    int *p = malloc(sizeof(int));
    *p = 1;  // warning here
}

// TC-02: malloc in loop
void tc02_loop(int n) {
    for (int i = 0; i < n; i++) {
        char *buf = malloc(256);
        strcpy(buf, "hello");  // warning
    }
}

// TC-03: calloc unchecked
void tc03_calloc(void) {
    int *arr = calloc(100, sizeof(int));
    arr[0] = 1;  // warning
}

// TC-04: realloc unchecked
void tc04_realloc(int *p) {
    p = realloc(p, 1024);
    p[0] = 0;  // warning
}

// TC-05: cast doesn't suppress
void tc05_cast(void) {
    char *s = (char *)malloc(64);
    s[0] = 'x';  // warning
}

// TC-06: direct dereference of malloc return (no named variable)
void tc06_direct_deref(void) {
    *(int *)malloc(sizeof(int)) = 99;  // warning
}

// TC-07: aligned_alloc unchecked (POSIX)
void tc07_aligned_alloc(void) {
    void *p = aligned_alloc(16, 64);
    ((char *)p)[0] = 'a';  // warning
}

// TC-08: unchecked use after assignment in inner scope
void tc08_inner_scope(void) {
    int *p;
    p = malloc(sizeof(int));
    {
        *p = 7;  // warning — no check before inner use
    }
}

// TC-09: null check on wrong variable
void tc09_wrong_var_check(void) {
    int *p = malloc(sizeof(int));
    int *q = NULL;
    if (q) return;   /* checks q, not p */
    *p = 3;  // warning
}
