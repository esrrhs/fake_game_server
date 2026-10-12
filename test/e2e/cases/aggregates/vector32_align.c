// expect: 0
// vector_size(32) aligns to 32, so a leading char pads the vector to offset 32.
package main;
typedef int V __attribute__((vector_size(32)));
struct S {
    char c;
    V v;
};
int main(void) {
    if (sizeof(V) != 32) return 1;
    if (__alignof__(V) != 32) return 2;
    if (__builtin_offsetof(struct S, v) != 32) return 3;
    if (sizeof(struct S) != 64) return 4;
    struct S s;
    s.c = 7;
    V tmp = { 1, 2, 3, 4, 5, 6, 7, 8 };
    s.v = tmp;
    if (s.c != 7) return 5;
    if (s.v[0] != 1) return 6;
    if (s.v[7] != 8) return 7;
    if (((unsigned long)&s.v & 31ul) != 0) return 8;
    return 0;
}
