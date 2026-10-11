// expect_error
// __builtin_constant_p with no argument must be diagnosed, not dereferenced as
// a NULL Expr (found by fuzz_compile).
package main;
int main(void) { return __builtin_constant_p(); }
