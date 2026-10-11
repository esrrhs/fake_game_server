// expect_error
// An array declarator whose size expression never parses must be diagnosed,
// not dereferenced as a NULL Expr (found by fuzz_compile).
package main;
int main(void) { int a[1; return a[0]; }
