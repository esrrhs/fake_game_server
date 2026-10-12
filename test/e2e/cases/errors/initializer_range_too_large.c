// expect_error
// A designator range that expands to billions of elements must be diagnosed,
// not expanded (found by fuzz_compile: it asked for a petabyte).
package main;
int a[4] = { [0 ... 99999999] = 1 };
int main(void) { return a[0]; }
