#include <stdlib.h>
#include <string.h>

struct node {
    int *data;
    struct node *next;
};

// TC-20: nested malloc in struct field init — unchecked inner use
void tc20_struct_field(void) {
    struct node n;
    n.data = malloc(sizeof(int));
    *n.data = 42;  // warning
}

// TC-21: malloc result passed directly to function (callee might not check)
void helper(char *s);
void tc21_pass_to_func(void) {
    helper(malloc(64));  // escaped — no warning (callee may check)
}

// TC-22: multiple allocations, only some checked
void tc22_partial_check(void) {
    int *a = malloc(sizeof(int));
    int *b = malloc(sizeof(int));
    if (!a) return;
    *a = 1;
    *b = 2;  // warning on b
}

// TC-23: realloc reassigning same pointer — unchecked use
void tc23_realloc_reassign(int *p) {
    p = realloc(p, 2048);
    p[1] = 99;  // warning
}

// TC-24: custom allocator (requires -allocator=my_alloc plugin arg)
void *my_alloc(size_t n);
void tc24_custom(void) {
    char *buf = my_alloc(128);
    buf[0] = 'z';  // warning when my_alloc is registered
}

// TC-25: memset on unchecked buffer
void tc25_memset(void) {
    char *buf = malloc(128);
    memset(buf, 0, 128);  // warning
}

// TC-26: arrow / member dereference after field assign
struct wrapper {
    int *payload;
};
void tc26_member_arrow(void) {
    struct wrapper w;
    w.payload = malloc(sizeof(int));
    *w.payload = 10;  // warning
}

// TC-27: both pointers checked — no false positives
void tc27_both_checked(void) {
    int *a = malloc(sizeof(int));
    int *b = malloc(sizeof(int));
    if (!a || !b) return;
    *a = 1;
    *b = 2;
}

// TC-28: realloc result checked before use
void tc28_realloc_checked(int *p) {
    p = realloc(p, 512);
    if (!p) return;
    p[0] = 0;
}

// TC-29: unchecked calloc in switch default branch
void tc29_switch(int x) {
    switch (x) {
    default: {
        int *p = malloc(4);
        *p = x;  // warning
        break;
    }
    }
}

// TC-30: malloc in if-init style (C23) — unchecked body use
void tc30_if_init(void) {
#if __STDC_VERSION__ >= 202311L
    if (int *p = malloc(4)) {
        /* condition uses p as truthy — ok for branch body */
        *p = 1;
    }
#else
    int *p = malloc(4);
    if (p) *p = 1;
#endif
}

void helper(char *s) { (void)s; }
void *my_alloc(size_t n) { return malloc(n); }
