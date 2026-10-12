// expect_error
// The GCC integer bit builtins are expanded inline and read their argument
// directly, so a wrong argument count has to be diagnosed (found by
// fuzz_compile: `__builtin_clz()` dereferenced a NULL call_args).
package main;
int main(void) { return __builtin_clz(); }