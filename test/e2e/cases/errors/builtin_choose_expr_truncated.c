// expect_error
// A truncated __builtin_choose_expr leaves the argument expressions unparsed;
// the NULL that comes back must be reported, not dereferenced (found by
// fuzz_compile).
package main;
int main(void) { return __builtin_choose_expr(0); }
