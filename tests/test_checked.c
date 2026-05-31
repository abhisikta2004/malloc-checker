#include <stdlib.h>
#include <assert.h>

// TC-10: if-null guard
void tc10_if_null(void) {
    int *p = malloc(sizeof(int));
    if (!p) return;
    *p = 1;
}

// TC-11: assert
void tc11_assert(void) {
    int *p = malloc(sizeof(int));
    assert(p);
    *p = 1;
}

// TC-12: compare then use
void tc12_compare(void) {
    int *p = malloc(sizeof(int));
    if (p == NULL) { return; }
    *p = 42;
}

// TC-13: ternary guard
int *tc13_ternary(size_t n) {
    int *p = malloc(n);
    return p ? p : NULL;   // returning NULL is fine — not dereferenced
}

// TC-14: intentional discard
void tc14_discard(void) {
    (void)malloc(0);       // explicitly discarded
}

// TC-15: early return with error code
void tc15_early_return_code(void) {
    int *p = malloc(sizeof(int));
    if (!p) return;
    *p = 1;
}

// TC-16: checked first, then used after other statements
void tc16_later_use(void) {
    int *p = malloc(sizeof(int));
    if (!p) return;
    volatile int x = 1;
    (void)x;
    *p = 2;
}

// TC-17: pointer passed to free (escape — no dereference warning)
void tc17_free_escape(void) {
    int *p = malloc(sizeof(int));
    free(p);
}

// TC-18: pass-through return to caller
int *tc18_return_pass(size_t n) {
    return malloc(n);
}

// TC-19: guard inside compound then-branch
void tc19_then_branch(void) {
    int *p = malloc(sizeof(int));
    if (p == NULL) {
        return;
    }
    *p = 5;
}

// TC-20: calloc with null guard
void tc20_calloc_checked(void) {
    int *arr = calloc(10, sizeof(int));
    if (arr == NULL)
        return;
    arr[0] = 1;
}

// TC-21: realloc with null guard before use
void tc21_realloc_checked(int *old) {
    int *p = realloc(old, 64);
    if (!p)
        return;
    p[0] = 0;
}

// TC-22: aligned_alloc checked then used
void tc22_aligned_checked(void) {
    void *raw = aligned_alloc(16, 32);
    if (raw == NULL)
        return;
    ((unsigned char *)raw)[0] = 0xFF;
}

// TC-23: unchecked pointer handed off to callee (escape)
void sink(int *buf);
void tc23_handoff_escape(void) {
    int *p = malloc(32);
    sink(p);
}

// TC-24: two independent guarded allocations in sequence
void tc24_two_guarded(void) {
    char *a = malloc(16);
    if (!a) return;
    char *b = malloc(32);
    if (!b) return;
    a[0] = 'x';
    b[0] = 'y';
}

void sink(int *buf) { free(buf); }
