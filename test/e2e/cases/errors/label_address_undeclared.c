// expect_error
// &&label for a label that does not exist must be diagnosed: the IR lowering
// used to call exit(1), which kills any process embedding the compiler
// (found by fuzz_differential).
package main;
int main(void) { if (&&nope) return 1; return 0; }
