// expect_error
// A malformed declarator list that reuses a typedef name must not free the
// shared base type twice (found by fuzz_compile: double free).
package main;
typedef struct trio { int a, b, c; } trio;
int bar (int t), trio t) { return 0; }
