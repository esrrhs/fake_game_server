// expect: 0
// vector_size(64) aligns to 64.  Passing may use ZMM or MEMORY.
package main;
typedef int V __attribute__((vector_size(64)));
struct S {
    char c;
    V v;
};
int main(void) {
    if (sizeof(V) != 64) return 1;
    if (__alignof__(V) != 64) return 2;
    if (__builtin_offsetof(struct S, v) != 64) return 3;
    if (sizeof(struct S) != 128) return 4;
    struct S s;
    s.c = 3;
    int i;
    for (i = 0; i < 16; i++) s.v[i] = i + 1;
    if (s.c != 3) return 5;
    if (s.v[0] != 1) return 6;
    if (s.v[15] != 16) return 7;
    if (((unsigned long)&s.v & 63ul) != 0) return 8;
    return 0;
}
