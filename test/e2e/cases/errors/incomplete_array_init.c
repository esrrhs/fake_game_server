// expect_error
// Initializing a struct whose array member is incomplete has no dense array
// to build; the negative length used to reach calloc() and overflow it
// (found by fuzz_compile / fuzz_differential).
package main;
struct s { int f[]; };
int main(void) { struct s s = { 1, 4 }; return s.f[0]; }
