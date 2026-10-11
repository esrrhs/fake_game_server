// expect_error
// A \x escape with no argument to the classification builtins must be
// diagnosed before the inline expansion indexes past the argument array
// (found by fuzz_compile: NULL dereference).
package main;
int main(void) { return __builtin_isinf(); }
